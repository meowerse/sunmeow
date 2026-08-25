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
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

// lib includes
extern "C" {
#include <libavutil/frame.h>
}

// local includes
#include "src/config.h"
#include "src/file_handler.h"
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
     * Compared, never dereferenced. See the ownership note at the top of this file.
     */
    inline std::atomic<const void *> owner {nullptr};

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
   *  3. It only takes effect on the software scaling path today (see the coverage note in
   *     `configure_scaler()`), so defaulting it on would advertise a feature that silently
   *     does nothing on the VA-API and CUDA paths.
   *
   * Read once, on first use. The config file is not reloaded while streaming, and neither
   * is anything else in `config::`.
   *
   * @return `true` when `meow_viewport_following` is enabled.
   */
  [[nodiscard]] inline bool following_enabled() {
    static const bool enabled = [] {
      const auto &path = config::sunshine.config_file;
      if (path.empty()) {
        return false;
      }
      const auto contents = file_handler::read_file(path.c_str());
      if (contents.empty()) {
        return false;
      }
      const auto vars = config::parse_config(contents);
      const auto it = vars.find(std::string {following_config_key});
      if (it == std::end(vars)) {
        return false;
      }
      return parse_following_value(it->second);
    }();
    return enabled;
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
    detail::requested.store(0, std::memory_order_relaxed);
    detail::geometry.store(f(capture_width) | (f(capture_height) << 16) | (f(surface_width) << 32) | (f(surface_height) << 48), std::memory_order_relaxed);
    detail::owner.store(token, std::memory_order_release);
  }

  /**
   * @brief Forget everything, so the next frame is a full desktop frame.
   *
   * Called when the control broadcast ends. Belt and braces: `on_scaler_init()` already
   * clears the rectangle at the start of the next session, but a session that ends and is
   * never followed by another should not sit holding a rectangle either.
   */
  inline void reset() noexcept {
    detail::requested.store(0, std::memory_order_relaxed);
    detail::geometry.store(0, std::memory_order_relaxed);
    detail::owner.store(nullptr, std::memory_order_release);
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
    // Acquire on `owner` first, then read `geometry`. `on_scaler_init()` writes the geometry
    // before releasing the owner, so this ordering is what guarantees the two are a matched
    // pair rather than a torn read across an encoder reinit.
    if (detail::owner.load(std::memory_order_acquire) == nullptr) {
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
    detail::requested.store(outcome.publish ? detail::pack(*outcome.publish) : 0, std::memory_order_relaxed);
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
   * @brief Decide what this frame's scaler configuration should be.
   *
   * Pure apart from two relaxed atomic loads. Called once per frame on the encode thread.
   *
   * The captured size used here is the one recorded at `init()`, **not** the size of the
   * frame in hand. That is deliberate: upstream configures the scaler from the size it was
   * initialised with and never looks at `img.width`/`img.height` again, so deriving the
   * uncropped plan from anything else would silently resize the scaler for a backend whose
   * captured images do not match. The frame in hand is used only to *tighten* the clamp on
   * the requested rectangle, so the pointer arithmetic can never leave a short buffer.
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
   * backend hands system memory to an encoder that does not scale on the GPU. The VA-API
   * (`src/platform/linux/graphics.cpp`, `egl::sws_t`) and CUDA
   * (`src/platform/linux/cuda.cu`) scalers have their own source-to-destination mapping
   * and are **not** cropped by this change. `plan_t` is shaped to drive them too -- both
   * already carry a destination viewport and a scale factor -- but wiring them up means
   * editing a GLSL shader and a CUDA kernel, which is a separate change.
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
   * @return What happened, and what to log about it.
   */
  template<class Server, class Echo>
  [[nodiscard]] registration_t map_request_handler(Server &server, const short *const table, const std::size_t count, Echo echo) {
    if (packet_type_collision(table, count)) {
      return {false, following_status(false), packet_type_collision_warning()};
    }

    server.map(control_packet_type, [echo](auto *session, const std::string_view &payload) {
      if (const auto applied = on_request(payload)) {
        echo(session, *applied);
      }
    });
    return {true, following_status(following_enabled()), {}};
  }

}  // namespace meow::viewport
