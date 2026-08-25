/**
 * @file src/meow/viewport_runtime.h
 * @brief Runtime plumbing for viewport-following ("foveated") streaming.
 *
 * The geometry, the wire format and every policy decision live in `src/meow/viewport.h`
 * and are pure. This header is the thin, ugly half: the process-wide state that carries a
 * rectangle from the control thread to the encode thread, the config gate, the
 * control-stream registration, and the FFmpeg scaler adapter.
 *
 * ## Threading
 *
 * Exactly two threads touch this state per session:
 *
 *  - the **control thread** (`stream::controlBroadcastThread`) writes the requested
 *    rectangle when a viewport packet arrives, at most ~20 Hz (the client rate-limits to
 *    one update per 50 ms);
 *  - the **encode thread** (`video::encode_run` -> `convert()`) reads it once per frame,
 *    at up to 180 Hz.
 *
 * That is a single-writer/single-reader hand-off of 8 bytes, so it is a relaxed
 * `std::atomic<std::uint64_t>` rather than a lock. The encode thread's per-frame cost is
 * one atomic load and a few dozen integer operations — see the `PerFramePlanningIsCheap`
 * test, which asserts a measured bound.
 *
 * ## Ownership, and why a stale crop cannot leak
 *
 * The state is process-wide, but it is *claimed*. `on_scaler_init()` records the address
 * of the scaler that most recently initialised and clears any pending rectangle;
 * `plan_for_frame()` refuses to crop unless the caller is that owner **and** the captured
 * and encode-surface dimensions still match what was recorded. So:
 *
 *  - a new session always starts uncropped, because its own `on_scaler_init()` cleared the
 *    previous session's rectangle before its first frame;
 *  - a display mode change or an encoder reinit re-runs `on_scaler_init()` and therefore
 *    also resets to full desktop;
 *  - with two concurrent sessions (`channels > 1`, off by default) only the most recently
 *    initialised one follows its viewport; the other streams the full desktop. Wrong-ish,
 *    but deterministic and never corrupt.
 */
#pragma once

// standard includes
#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

// lib includes
extern "C" {
#include <libavutil/frame.h>
#include <libavutil/imgutils.h>
#include <libavutil/pixfmt.h>
}

// local includes
#include "src/config.h"
#include "src/meow/viewport.h"

namespace meow::viewport {

  namespace detail {

    /**
     * @brief Pack a rectangle into 64 bits so it can be published atomically.
     *
     * Every field has already been clamped into a captured frame no larger than
     * `meow::display_union::max_pixel_width` x `max_pixel_height` (8192x4096), so 16 bits
     * per field is ample. The clamp is applied anyway, because a packed value that
     * silently wrapped would be far worse than one that saturates.
     *
     * @param rect Rectangle to pack.
     * @return The packed representation; `0` when the rectangle is empty.
     */
    [[nodiscard]] inline std::uint64_t pack(const rect_t &rect) noexcept {
      if (rect.width <= 0 || rect.height <= 0) {
        return 0;
      }
      const auto f = [](const int v) noexcept {
        return static_cast<std::uint64_t>(static_cast<std::uint16_t>(std::clamp(v, 0, 0xFFFF)));
      };
      return f(rect.x) | (f(rect.y) << 16) | (f(rect.width) << 32) | (f(rect.height) << 48);
    }

    /**
     * @brief Unpack a rectangle published by `pack()`.
     *
     * @param packed Packed representation.
     * @return The rectangle, or `std::nullopt` when nothing is published.
     */
    [[nodiscard]] inline std::optional<rect_t> unpack(const std::uint64_t packed) noexcept {
      if (packed == 0) {
        return std::nullopt;
      }
      rect_t r;
      r.x = static_cast<int>(packed & 0xFFFF);
      r.y = static_cast<int>((packed >> 16) & 0xFFFF);
      r.width = static_cast<int>((packed >> 32) & 0xFFFF);
      r.height = static_cast<int>((packed >> 48) & 0xFFFF);
      if (r.width <= 0 || r.height <= 0) {
        return std::nullopt;
      }
      return r;
    }

    /**
     * @brief The rectangle the client most recently asked for, packed by `pack()`.
     */
    inline std::atomic<std::uint64_t> requested {0};

    /**
     * @brief Captured frame size and encode surface size of the owning scaler, packed as
     *        `capture_w | capture_h << 16 | surface_w << 32 | surface_h << 48`.
     */
    inline std::atomic<std::uint64_t> geometry {0};

    /**
     * @brief Address of the scaler entitled to act on `requested`.
     *
     * Compared, never dereferenced -- which matters more than it looks:
     * `make_avcodec_encode_session()` also runs during encoder probing at startup, so this
     * routinely holds the address of a device that has already been destroyed.
     */
    inline std::atomic<const void *> owner {nullptr};

    /**
     * @brief Whether the host is currently willing to act on a request.
     *
     * Separate from `owner` so `reset()` can stop answering the client while still serving
     * the running scaler a plan -- which is what makes a cropped scaler actually *revert*
     * rather than freeze on its last crop.
     */
    inline std::atomic<bool> accepting {false};

    /**
     * @brief Set when the host dropped a crop on its own, and has not told the client yet.
     */
    inline std::atomic<bool> revoked {false};

  }  // namespace detail

  /**
   * @brief Everything the client needs in order to reconcile its view with the host's.
   */
  struct echo_t {
    rect_t applied {};  ///< Applied rectangle, in reference-frame (negotiated stream) pixels.
    int capture_width {};  ///< Captured desktop width, so the client can derive the padding transform.
    int capture_height {};  ///< Captured desktop height.

    bool operator==(const echo_t &) const = default;
  };

  /**
   * @brief Whether viewport following is enabled in the host configuration.
   *
   * Reads `config::video.viewport_following`, which `config.cpp` fills from the
   * `meow_viewport_following` key like every other Sunshine setting.
   *
   * An earlier revision parsed the key out of the configuration file here instead, to avoid
   * an upstream edit. That was the wrong trade: Sunshine's own parser never learned the key
   * existed, so it logged `Unrecognized configurable option [meow_viewport_following]` at
   * every startup — telling a user who had just enabled the feature that the setting does
   * not exist. Registering it properly also lets it appear in the web UI, which an
   * out-of-band read never can.
   *
   * **Defaults to off.** Three reasons, in descending order of importance:
   *
   *  1. Client-supplied absolute pointer and touch coordinates are still mapped through
   *     `video::make_port()`, which knows only the *full* captured desktop. While a crop
   *     is active those coordinates land in the wrong place. Cropping is therefore an
   *     opt-in trade — dramatically more readable text in exchange for pointer input that
   *     needs the matching host-side remap, which this change does not yet include.
   *  2. CLAUDE.md's compatibility floor: an existing working setup must not change
   *     behaviour on upgrade. A user who never edits their config gets exactly the stream
   *     they had yesterday.
   *  3. It does not cover every scaling path (see the coverage note in `configure_scaler()`).
   *     The software and CUDA/NVENC scalers crop; VA-API does not, so on those hosts the
   *     setting would advertise a feature that silently does nothing.
   *
   * @return `true` when `meow_viewport_following` is enabled.
   */
  [[nodiscard]] inline bool following_enabled() {
    return config::video.viewport_following;
  }

  /**
   * @brief Publish the geometry of a freshly initialised scaler and reset to full desktop.
   *
   * Called from `avcodec_software_encode_device_t::init()`. Claims ownership for `token`
   * and drops any rectangle left over from a previous session, a previous display mode or
   * a previous encoder reinit — which is what makes a stale crop unable to leak forward.
   *
   * @param token Address identifying the scaler; compared, never dereferenced.
   * @param capture_width Width of the captured frame in pixels.
   * @param capture_height Height of the captured frame in pixels.
   * @param surface_width Width of the encode surface in pixels.
   * @param surface_height Height of the encode surface in pixels.
   */
  inline void on_scaler_init(const void *const token, const int capture_width, const int capture_height, const int surface_width, const int surface_height) noexcept {
    const auto f = [](const int v) noexcept {
      return static_cast<std::uint64_t>(static_cast<std::uint16_t>(std::clamp(v, 0, 0xFFFF)));
    };
    // Dropping a crop the client still believes in is exactly the case that leaves it
    // showing the whole desktop at 1:1 after it reset its own zoom, so remember to say so.
    if (detail::requested.exchange(0, std::memory_order_relaxed) != 0) {
      detail::revoked.store(true, std::memory_order_relaxed);
    }
    detail::geometry.store(f(capture_width) | (f(capture_height) << 16) | (f(surface_width) << 32) | (f(surface_height) << 48), std::memory_order_relaxed);
    detail::owner.store(token, std::memory_order_release);
    detail::accepting.store(true, std::memory_order_release);
  }

  /**
   * @brief Forget everything, so the next frame is a full desktop frame.
   *
   * Called when the control broadcast ends. `on_scaler_init()` already clears the rectangle
   * at the start of the next session, so this is the belt to that pair of braces -- but it
   * is also what stops a host whose next session does *not* run through the software scaler
   * from being answered against this session's geometry.
   */
  inline void reset() noexcept {
    // Order matters. Clearing the rectangle first means a scaler that is still running gets
    // the full-frame plan on its very next frame and actually reverts; clearing `owner` or
    // `geometry` first would make `plan_for_frame()` return "do not touch", freezing it on
    // its last crop instead. `accepting` then stops any further request being answered, so
    // nothing can re-crop on the way out.
    detail::requested.store(0, std::memory_order_relaxed);
    detail::revoked.store(false, std::memory_order_relaxed);
    detail::accepting.store(false, std::memory_order_release);
  }

  /**
   * @brief Handle a viewport packet from the control stream.
   *
   * Parses the request, maps it out of the client's coordinate system, validates it,
   * stores it, and reports back what to tell the client. The applied rectangle is
   * frequently *not* the requested one — it is clamped to the desktop, grown to a minimum
   * size, even-aligned for chroma, and refused outright when its aspect ratio would scale
   * to a sliver.
   *
   * Runs on the control thread.
   *
   * Split from `on_request()` so the state machine can be driven in a unit test. The
   * config gate reads a file once into a function-local static, which a test cannot
   * influence — folding it in here would make every test of this function pass
   * vacuously, which is worse than not testing it (CLAUDE.md §5).
   *
   * @param payload Control-stream payload, excluding the header.
   * @return What to send back, or `std::nullopt` when nothing can be reported — no scaler
   *         has published its geometry, in which case we do not even know the coordinate
   *         system an answer would be in.
   */
  [[nodiscard]] inline std::optional<echo_t> apply_request(const std::string_view payload) {
    // Acquire on `accepting` first, then read `geometry`. `on_scaler_init()` writes the
    // geometry before releasing this flag, so the ordering is what guarantees the two are a
    // matched pair rather than a torn read across an encoder reinit. It is also false on a
    // host whose encoder never takes the software scaling path, which is how such a host
    // stays silent instead of promising a crop it will not apply.
    if (!detail::accepting.load(std::memory_order_acquire)) {
      return std::nullopt;
    }

    const auto packed_geometry = detail::geometry.load(std::memory_order_relaxed);
    if (packed_geometry == 0) {
      // No scaler has initialised, or this session does not run through the software
      // scaling path at all. Without the captured desktop size we cannot even name the
      // coordinate system an answer would be in, so say nothing: the client treats the
      // absence of an echo as "not applied".
      return std::nullopt;
    }

    const auto capture_width = static_cast<int>(packed_geometry & 0xFFFF);
    const auto capture_height = static_cast<int>((packed_geometry >> 16) & 0xFFFF);
    const auto surface_width = static_cast<int>((packed_geometry >> 32) & 0xFFFF);
    const auto surface_height = static_cast<int>((packed_geometry >> 48) & 0xFFFF);

    const auto outcome = evaluate_request(payload, capture_width, capture_height, surface_width, surface_height);
    if (!outcome.understood) {
      // Change nothing and say nothing. See `request_outcome_t::understood`.
      return std::nullopt;
    }

    detail::requested.store(outcome.publish ? detail::pack(*outcome.publish) : 0, std::memory_order_relaxed);
    // Whatever we just decided, the client is about to be told it, so there is no
    // outstanding revocation left to report.
    detail::revoked.store(false, std::memory_order_relaxed);
    if (!outcome.echo) {
      return std::nullopt;
    }
    return echo_t {*outcome.echo, capture_width, capture_height};
  }

  /**
   * @brief `apply_request()`, gated on the host configuration.
   *
   * The entry point `src/stream.cpp` registers. When the feature is off this does nothing
   * at all — no state is written and no echo is sent — so a client that speaks the
   * extension against a host that has not opted in gets today's behaviour and can tell,
   * from the absence of an echo, that its request was not honoured.
   *
   * @param payload Control-stream payload, excluding the header.
   * @return What to send back, or `std::nullopt`.
   */
  [[nodiscard]] inline std::optional<echo_t> on_request(const std::string_view payload) {
    if (!following_enabled()) {
      return std::nullopt;
    }
    return apply_request(payload);
  }

  /**
   * @brief Claim an unreported revocation, if the host dropped a crop on its own.
   *
   * The echo is load-bearing: a client that has reset its local zoom to 1:1 on the strength
   * of an earlier echo, and is then silently handed the full desktop again, shows it at 1:1
   * with no way to know. The host drops a crop by itself on an encoder reinit or a display
   * mode change mid-session (`on_scaler_init()`), and this is how the client finds out.
   *
   * One-shot: the flag is cleared by the read, so a caller that loses the returned value
   * loses the notification. That is deliberate -- retrying forever would spam the control
   * channel, and the next request the client sends is answered anyway.
   *
   * @return The full-content-area echo to send, or `std::nullopt` when nothing is pending.
   */
  [[nodiscard]] inline std::optional<echo_t> take_revocation_echo() noexcept {
    if (!detail::revoked.exchange(false, std::memory_order_relaxed)) {
      return std::nullopt;
    }
    if (!detail::accepting.load(std::memory_order_acquire)) {
      return std::nullopt;
    }

    const auto packed_geometry = detail::geometry.load(std::memory_order_relaxed);
    if (packed_geometry == 0) {
      return std::nullopt;
    }
    const auto capture_width = static_cast<int>(packed_geometry & 0xFFFF);
    const auto capture_height = static_cast<int>((packed_geometry >> 16) & 0xFFFF);
    const auto surface_width = static_cast<int>((packed_geometry >> 32) & 0xFFFF);
    const auto surface_height = static_cast<int>((packed_geometry >> 48) & 0xFFFF);

    const auto full = to_reference({0, 0, capture_width, capture_height}, capture_width, capture_height, surface_width, surface_height);
    if (full.width <= 0 || full.height <= 0) {
      return std::nullopt;
    }
    return echo_t {full, capture_width, capture_height};
  }

  /**
   * @brief Decide what this frame's scaler configuration should be.
   *
   * Pure apart from two relaxed atomic loads. Called once per frame on the encode thread.
   *
   * The captured size used here is the one recorded at `init()`, **not** the size of the
   * frame in hand. That is deliberate: upstream configures the scaler from the size it was
   * initialised with and never looks at `img.width`/`img.height` again, so deriving the
   * uncropped plan from anything else would silently resize the scaler for a backend whose
   * captured images do not match. The frame in hand is used only to *tighten* the clamp on
   * the requested rectangle, so the pointer arithmetic -- or, on the CUDA path, the texture
   * sampling -- can never leave a short buffer.
   *
   * Returns `std::nullopt` for "do not touch this scaler at all":
   *
   *  - no geometry recorded (this session never ran through the software scaling path);
   *  - the caller is not the owning scaler -- see the ownership note at the top of this
   *    file. A scaler displaced by a newer session keeps whatever it had; with the default
   *    `channels = 1` there is never more than one, so this cannot arise;
   *  - the caller's encode surface disagrees with the recorded one, which means the state
   *    describes some other scaler and is not trustworthy.
   *
   * When the caller *is* the owner and no rectangle is pending, the returned plan is the
   * full-frame plan built from the recorded geometry -- identical to what `init()`
   * configured -- so `configure_scaler()` finds nothing to change in the steady state and
   * a scaler that was cropped reverts on the very next frame.
   *
   * @param token Address identifying the calling scaler.
   * @param frame_width Width of the captured frame actually in hand.
   * @param frame_height Height of the captured frame actually in hand.
   * @param surface_width Width of the encode surface.
   * @param surface_height Height of the encode surface.
   * @return The plan, or `std::nullopt` to leave the scaler untouched.
   */
  [[nodiscard]] inline std::optional<plan_t> plan_for_frame(const void *const token, const int frame_width, const int frame_height, const int surface_width, const int surface_height) noexcept {
    if (detail::owner.load(std::memory_order_acquire) != token) {
      return std::nullopt;
    }

    const auto packed_geometry = detail::geometry.load(std::memory_order_relaxed);
    if (packed_geometry == 0) {
      return std::nullopt;
    }

    const auto capture_width = static_cast<int>(packed_geometry & 0xFFFF);
    const auto capture_height = static_cast<int>((packed_geometry >> 16) & 0xFFFF);
    if (static_cast<int>((packed_geometry >> 32) & 0xFFFF) != surface_width || static_cast<int>((packed_geometry >> 48) & 0xFFFF) != surface_height) {
      return std::nullopt;
    }

    auto requested = detail::unpack(detail::requested.load(std::memory_order_relaxed));
    if (requested && (frame_width < capture_width || frame_height < capture_height)) {
      // The frame in hand is smaller than the one we were initialised for. Tighten the
      // clamp so the crop cannot address rows or columns that are not there.
      requested = sanitize(*requested, std::min(capture_width, std::max(frame_width, 0)), std::min(capture_height, std::max(frame_height, 0)));
    }

    return plan(capture_width, capture_height, surface_width, surface_height, requested);
  }

  /**
   * @brief Apply a plan to the FFmpeg scaler frames, and say whether anything changed.
   *
   * The adapter between the pure geometry and `avcodec_software_encode_device_t`. It only
   * ever writes the four dimension fields and the two offsets; the caller owns the
   * decision to reinitialise swscale, because only the caller can do that.
   *
   * Returning `false` in the steady state is what keeps requirement 6 honest: with the
   * viewport unchanged this is six integer comparisons and nothing else, so swscale's
   * filter tables are not rebuilt on a frame where nothing moved.
   *
   * **Coverage.** This is the software scaling path -- the one taken when the capture
   * backend hands system memory to an encoder that does not scale on the GPU. The CUDA /
   * NVENC scaler (`src/platform/linux/cuda.cu`) honours the same `plan_t` through
   * `src/meow/viewport_cuda.h`, which turns it into a source origin and a per-axis step for
   * the conversion kernel; that path calls `plan_for_frame()` directly rather than coming
   * through here, because it has no `AVFrame`s to reconfigure. The VA-API scaler
   * (`src/platform/linux/graphics.cpp`, `egl::sws_t`) has its own source-to-destination
   * mapping and is still **not** cropped: it needs the same treatment applied to a GLSL
   * shader, which is a separate change.
   *
   * @param planned Plan to apply; `std::nullopt` leaves the scaler untouched.
   * @param sws_input Scaler input frame; its width and height are updated.
   * @param sws_output Scaler output frame; its width and height are updated.
   * @param offset_w Horizontal padding offset; updated.
   * @param offset_h Vertical padding offset; updated.
   * @return `true` when any field changed, meaning swscale must be reinitialised and the
   *         padding re-blacked.
   */
  [[nodiscard]] inline bool configure_scaler(const std::optional<plan_t> &planned, AVFrame &sws_input, AVFrame &sws_output, int &offset_w, int &offset_h) noexcept {
    if (!planned) {
      return false;
    }
    const auto &plan = *planned;
    if (plan.source.width <= 0 || plan.source.height <= 0 || plan.out_width <= 0 || plan.out_height <= 0) {
      // Nothing usable was planned (degenerate capture or surface size). Leave the scaler
      // exactly as it is; the existing configuration is still valid.
      return false;
    }

    if (sws_input.width == plan.source.width && sws_input.height == plan.source.height && sws_output.width == plan.out_width && sws_output.height == plan.out_height && offset_w == plan.offset_w && offset_h == plan.offset_h) {
      return false;
    }

    sws_input.width = plan.source.width;
    sws_input.height = plan.source.height;

    if (sws_output.width != plan.out_width || sws_output.height != plan.out_height) {
      // The intermediate output frame is left unallocated by upstream so that
      // `sws_scale_frame()` allocates it on first use -- and it keeps that buffer for the
      // life of the frame. Changing the dimensions without dropping the buffer would leave
      // a frame that *claims* the new size while owning storage for the old one, which the
      // scaler would then write past. Drop it and let the scaler allocate the right size
      // again on the next scale; `format` is the only property worth carrying across.
      const auto format = sws_output.format;
      av_frame_unref(&sws_output);
      sws_output.format = format;
      sws_output.width = plan.out_width;
      sws_output.height = plan.out_height;
    }

    offset_w = plan.offset_w;
    offset_h = plan.offset_h;
    return true;
  }

  /**
   * @brief Re-blacken the encode surface without reallocating it.
   *
   * When a crop shrinks, the scaled image no longer covers everything the previous one did,
   * and the uncovered border keeps the old pixels -- a visibly corrupt frame. Upstream's
   * `prefill()` solves that at init time, but it cannot be reused per frame:
   * `av_frame_get_buffer()` is documented "if frame already has been allocated, calling this
   * function will leak memory", and the surface has been allocated since `init()`. At
   * 1280x720 NV12 that is ~1.35 MB leaked per crop change, up to twenty times a second while
   * a user is pinch-zooming.
   *
   * So this does only the half that is wanted: fill black, no allocation. Upstream already
   * assumes exclusive ownership of this frame -- the padding `memcpy` in `convert()` writes
   * into it every frame with no `av_frame_make_writable()` -- so this makes no new
   * assumption.
   *
   * @param surface Encode surface to blacken.
   */
  inline void reblack(AVFrame &surface) noexcept {
    if (!surface.data[0]) {
      return;
    }
    const std::array<std::ptrdiff_t, 4> linesize {surface.linesize[0], surface.linesize[1], surface.linesize[2], surface.linesize[3]};
    av_image_fill_black(surface.data, linesize.data(), static_cast<AVPixelFormat>(surface.format), surface.color_range, surface.width, surface.height);
  }

  /**
   * @brief Read back the plan a scaler is currently configured for.
   *
   * Used to restore a working configuration when a reconfiguration fails. The source origin
   * is not stored anywhere on the scaler -- it lives only in the plane pointers, which are
   * rebuilt from scratch every frame -- so it comes back as `{0, 0}`. That is exactly right
   * for a restore: the pointers are re-derived from the restored plan on the same frame.
   *
   * @param sws_input Scaler input frame.
   * @param sws_output Scaler output frame.
   * @param offset_w Current horizontal padding offset.
   * @param offset_h Current vertical padding offset.
   * @return The plan describing the current configuration.
   */
  [[nodiscard]] inline plan_t current_scaler_plan(const AVFrame &sws_input, const AVFrame &sws_output, const int offset_w, const int offset_h) noexcept {
    plan_t p;
    p.source = {0, 0, sws_input.width, sws_input.height};
    p.out_width = sws_output.width;
    p.out_height = sws_output.height;
    p.offset_w = offset_w;
    p.offset_h = offset_h;
    p.cropped = false;
    return p;
  }

  /**
   * @brief Apply a plan to the scaler, reinitialising it, and fall back if that fails.
   *
   * `reinit` is the caller's swscale reinitialisation -- it can only be done from inside
   * `avcodec_software_encode_device_t`, whose scaler members are private.
   *
   * The fallback exists because the failure mode changed. Upstream reinitialised swscale at
   * most twice per session, so a failure there was effectively unreachable after startup.
   * A crop makes it reachable on every zoom, driven by network input, and `convert()`
   * returning nonzero ends the session. A transient allocation failure must therefore drop
   * the *crop*, not the stream.
   *
   * @tparam Reinit Callable returning `< 0` on failure.
   * @param planned Plan to apply; `std::nullopt` leaves the scaler untouched.
   * @param sws_input Scaler input frame.
   * @param sws_output Scaler output frame.
   * @param offset_w Horizontal padding offset; updated.
   * @param offset_h Vertical padding offset; updated.
   * @param surface Encode surface, re-blackened when the configuration changes.
   * @param reinit Reinitialises swscale for the new dimensions.
   * @return `false` only when even the previous configuration cannot be restored, which is
   *         genuinely fatal to the session.
   */
  template<class Reinit>
  [[nodiscard]] bool apply_plan(const std::optional<plan_t> &planned, AVFrame &sws_input, AVFrame &sws_output, int &offset_w, int &offset_h, AVFrame &surface, Reinit reinit) {
    const auto previous = current_scaler_plan(sws_input, sws_output, offset_w, offset_h);
    if (!configure_scaler(planned, sws_input, sws_output, offset_w, offset_h)) {
      return true;
    }

    reblack(surface);
    if (reinit() >= 0) {
      return true;
    }

    // Put back what was working. `configure_scaler` will report a change (we just moved
    // away from it), so the reinit below is required, not optional.
    static_cast<void>(configure_scaler(previous, sws_input, sws_output, offset_w, offset_h));
    reblack(surface);
    return reinit() >= 0;
  }

  /**
   * @brief Move the scaler's plane pointers to the crop origin.
   *
   * Called after the caller has pointed the input frame at the captured buffer. This is
   * the entire per-frame cost of a pan: two pointer additions, no allocation and no copy.
   *
   * The origin is always even (`sanitize()` guarantees it), so the NV12 chroma plane
   * offset is exact: chroma is subsampled 2x2 and interleaved two bytes per sample pair,
   * so a luma column offset of `x` is a chroma byte offset of `x`, and a luma row offset
   * of `y` is a chroma row offset of `y / 2`.
   *
   * @param sws_input Scaler input frame whose `data` pointers are shifted in place.
   * @param planned Plan produced by `plan_for_frame()`; `std::nullopt` is a no-op.
   * @param row_pitch Bytes between consecutive rows of the captured buffer.
   * @param pixel_pitch Bytes per pixel of the captured buffer's first plane.
   * @param nv12 Whether the capture is NV12 and therefore has a second (chroma) plane.
   */
  inline void offset_source_planes(AVFrame &sws_input, const std::optional<plan_t> &planned, const int row_pitch, const int pixel_pitch, const bool nv12) noexcept {
    if (!planned) {
      return;
    }
    const auto &source = planned->source;
    if (source.x <= 0 && source.y <= 0) {
      return;
    }
    if (sws_input.data[0]) {
      sws_input.data[0] += static_cast<std::ptrdiff_t>(source.y) * row_pitch + static_cast<std::ptrdiff_t>(source.x) * pixel_pitch;
    }
    if (nv12 && sws_input.data[1]) {
      sws_input.data[1] += static_cast<std::ptrdiff_t>(source.y / 2) * row_pitch + static_cast<std::ptrdiff_t>(source.x);
    }
  }

  /**
   * @brief Outcome of trying to register the viewport control-stream handler.
   */
  struct registration_t {
    bool registered {};  ///< Whether the handler was installed.
    std::string note {};  ///< One line for the host log describing the state of the feature.
    std::string warning {};  ///< Empty on success; the reason to log otherwise.
  };

  /**
   * @brief Register the viewport handler on the control stream, refusing on a collision.
   *
   * Templated on the server and the echo callback so this header needs to know nothing
   * about `stream::session_t` or `stream::control_server_t`, both of which are private to
   * `src/stream.cpp`.
   *
   * `table`/`count` are the host's real `packetTypes` array. If `control_packet_type` ever
   * appears in it, the handler is *not* installed: dispatching a genuine upstream message
   * into this handler would break whatever feature owned the number, silently.
   *
   * @tparam Server Control server type exposing `map(type, callback)`.
   * @tparam Echo Callable `(session *, const rect_t &)` that sends the applied rectangle back.
   * @param server Control server to register on.
   * @param table The host's control-stream packet type table.
   * @param count Number of entries in `table`.
   * @param echo Callback used to echo the applied rectangle to the client.
   * @param enabled Whether the host configuration opts in; passed rather than read here so
   *        the whole decision is testable (the config gate reads a file once into a
   *        function-local static that a test cannot influence).
   * @return What happened, and what to log about it.
   */
  template<class Server, class Echo>
  [[nodiscard]] registration_t map_request_handler(Server &server, const short *const table, const std::size_t count, Echo echo, const bool enabled) {
    if (packet_type_collision(table, count)) {
      return {false, following_status(false), packet_type_collision_warning()};
    }
    if (!enabled) {
      // Install nothing. The feature is then genuinely inert rather than merely quiet: a
      // viewport packet from a client that speaks the extension falls through to
      // `control_server_t::call()`'s unknown-type path, which logs at debug and returns.
      return {false, following_status(false), {}};
    }

    server.map(control_packet_type, [echo](auto *session, const std::string_view &payload) {
      if (const auto applied = apply_request(payload)) {
        echo(session, *applied);
      }
    });
    return {true, following_status(true), {}};
  }

}  // namespace meow::viewport
