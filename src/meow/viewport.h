/**
 * @file src/meow/viewport.h
 * @brief Pure geometry and wire format for viewport-following ("foveated") streaming.
 *
 * The problem this solves: a 5360x1440 two-monitor desktop scaled into a 1280x720 encode
 * surface at 5-8 Mbps is unreadable, and pinch-zooming on the client only magnifies an
 * already-destroyed image. If the client tells the host *which rectangle it is currently
 * displaying*, the host can crop to that rectangle before scaling into the encoder. The
 * encode resolution and the bitrate never change, so the same bits describe far fewer
 * pixels.
 *
 * Everything in this header is a pure function over integers so it can be unit tested
 * without a GPU, a display, a client, or FFmpeg (CLAUDE.md §5.5). The runtime plumbing —
 * the atomic session state, the control-stream registration and the FFmpeg scaler
 * adapter — lives in `src/meow/viewport_runtime.h`.
 *
 * ## The compatibility floor
 *
 * `plan()` called with no request returns *exactly* the numbers upstream's
 * `avcodec_software_encode_device_t::init()` computes today:
 *
 * ```
 * scalar = min(surface_w / capture_w, surface_h / capture_h)
 * out    = capture * scalar                       (float multiply, truncated to int)
 * offset = (surface - out) / 2                    (integer divide)
 * ```
 *
 * That is asserted by `NoRequestReproducesUpstreamFormula` in
 * `tests/unit/meow/test_viewport.cpp`, so a client that never sends a viewport packet —
 * stock Moonlight, an older moonmeow, anything — gets a bit-identical full-desktop
 * stream. Every rejection path in this header funnels back to that same plan.
 *
 * ## Coordinate systems -- the easiest thing to get wrong here
 *
 * There are two, and the wire uses the *other* one from the one the crop needs.
 *
 *  - **Captured-desktop pixels.** Origin top-left of the captured frame. For unified
 *    desktop capture (`output_name = all`) that is the union of every output, so a
 *    rectangle spans monitors naturally. `plan()`, `sanitize()` and everything downstream
 *    of them work here.
 *  - **Reference-frame pixels.** The negotiated stream resolution -- the encode surface --
 *    as the client sees it *before any crop is applied*. This is what arrives on the wire,
 *    and what the echo is expressed in.
 *
 * **The wire is NOT desktop pixels, whatever `Limelight.h` says.** `LiSendViewportEvent`'s
 * documentation calls them host desktop coordinates, but the client provably cannot honour
 * that: nothing in the handshake tells it the host's desktop size -- `serverinfo` does not
 * carry it. So the client sends the rectangle against the negotiated stream resolution,
 * which is the same reference space `LiSendMousePositionEvent` already uses and the only
 * one both ends can compute. The client half reached this conclusion independently. Treat
 * the `Limelight.h` comment as wrong.
 *
 * This is not a rescale-by-a-constant. Sunshine **pads to preserve aspect ratio**: a
 * 5360x1440 desktop in a 1280x720 surface occupies only 1280x343 of it, centred, with 188
 * rows of black above and below. "40% across the encoded frame" is therefore not "40%
 * across the desktop", and a naive proportional mapping is wrong by 188 rows in exactly
 * the configuration this feature exists for. `to_desktop()` and `to_reference()` undo and
 * redo that padding transform, once, at the boundary.
 *
 * The reference frame is deliberately the **uncropped** framing, and stays that way while
 * a crop is active. It is a fixed coordinate system, not "whatever is on screen right
 * now": the same rectangle always means the same desktop region regardless of what is
 * currently applied. Defining it relative to the current crop would make successive pans
 * compose multiplicatively and walk the view off the desktop.
 */
#pragma once

// standard includes
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <optional>
#include <string>
#include <string_view>

namespace meow::viewport {

  /**
   * @brief Control-stream packet type carrying a viewport rectangle.
   *
   * This value is the whole interoperability contract with the client. It must equal
   * `packetTypesGen7Enc[IDX_VIEWPORT]` on branch `meow` of
   * `https://github.com/meowerse/moonlight-common-c` (`src/ControlStream.c`), which is
   * `0x3003, // Viewport event (Apollo protocol extension)`.
   *
   * Sunshine does **not** consume that header: `third-party/moonlight-common-c` is pinned
   * to upstream `moonlight-stream/moonlight-common-c`, which has no `IDX_VIEWPORT`, and
   * `src/stream.cpp` carries its own `packetTypes` table. The two definitions are
   * therefore independent, and a drift between them is a real failure mode rather than a
   * theoretical one. Three things make the agreement checkable instead of trusted:
   *
   *  1. The host never indexes `packetTypes` for this feature — it uses this constant
   *     directly — so there is no table entry to forget and no way to read off the end.
   *  2. `packet_type_collision()` is called at registration time with the real
   *     `packetTypes` table. If any upstream message ever takes `0x3003`, registration is
   *     refused, the host logs why, and the stream falls back to full desktop rather than
   *     dispatching a foreign packet into this handler.
   *  3. `PayloadRoundTripMatchesTheWireContract` pins the byte layout, the version and the
   *     packet number as literals, so changing any of them is a deliberate, reviewed edit
   *     rather than a silent one.
   *
   * If the client ever changes its number, symptom and diagnosis are both cheap: the host
   * logs the unknown type at debug level in `control_server_t::call()` and viewport
   * following simply never engages. Nothing is mis-dispatched, because 3 different numbers
   * cannot collide by accident.
   */
  inline constexpr std::uint16_t control_packet_type = 0x3003;

  /**
   * @brief Version byte of the viewport payload.
   *
   * A receiver that does not recognise the version discards the whole message rather than
   * parsing it field by field, matching the client. So a bump turns viewport following
   * *off* against an older peer instead of degrading it — prefer the reserved flags byte
   * for additive changes.
   */
  inline constexpr std::uint8_t payload_version = 1;

  /**
   * @brief Exact size of the viewport payload in bytes.
   *
   * `uint8 version, uint8 flags, uint16 x, uint16 y, uint16 width, uint16 height`,
   * little endian, matching every other control-stream payload.
   */
  inline constexpr std::size_t payload_length = 10;

  /**
   * @brief Flag bit meaning "the captured desktop size follows the rectangle".
   *
   * Set only on the echo, never on a request. The client's version-1 parser reads the
   * first ten bytes and ignores both unknown flag bits and trailing bytes, so a longer
   * payload reaches an existing client harmlessly -- it simply sees the rectangle. A
   * client that understands the bit gets the two extra `uint16`s and can then compute the
   * host's padding transform itself, which closes the coordinate-space gap permanently
   * without a version bump.
   */
  inline constexpr std::uint8_t flag_desktop_extent = 0x01;

  /**
   * @brief Size of the echo payload: the request layout plus `uint16 desktop_w, desktop_h`.
   */
  inline constexpr std::size_t echo_payload_length = 14;

  /**
   * @brief Smallest crop we will scale from, on either axis, in captured pixels.
   *
   * Guards against a client asking for a 1x1 rectangle and driving swscale into an
   * enormous upscale factor. Requests below this are *grown*, not rejected, because a
   * user pinching in hard should hit a floor rather than snap back to the full desktop.
   */
  inline constexpr int min_source_extent = 64;

  /**
   * @brief Smallest scaled image we will place into the encode surface, on either axis.
   *
   * This is the "aspect ratio wildly different from the encode surface" guard. A
   * 5360x64 request against a 1280x720 surface scales to 1280x15 — technically valid,
   * visually useless, and a waste of the whole surface. Anything that degenerate is
   * refused and the full desktop is streamed instead; the echo tells the client exactly
   * what it got.
   */
  inline constexpr int min_output_extent = 32;

  /**
   * @brief Configuration key that enables viewport following.
   *
   * Parsed out of Sunshine's own configuration file by `viewport_runtime.h` rather than
   * registered through `config.cpp`. Adding a key to `config.cpp` drags
   * `config.h`, `configuration.md`, `config.html` and `en.json` along with it —
   * `tests/integration/test_config_consistency.cpp` enforces that — for a setting that
   * needs no UI. This is the same trade `meow::display_union` made for `output_name`.
   *
   * The `meow_` prefix guarantees it can never collide with an upstream key, and unknown
   * keys are ignored by `config::parse_config()`, so an unpatched Sunshine reading this
   * file is unaffected.
   */
  inline constexpr std::string_view following_config_key = "meow_viewport_following";

  /**
   * @brief A rectangle in captured-desktop pixels.
   */
  struct rect_t {
    int x {};  ///< Left edge, captured-desktop pixels.
    int y {};  ///< Top edge, captured-desktop pixels.
    int width {};  ///< Width in captured-desktop pixels; `<= 0` means "no rectangle".
    int height {};  ///< Height in captured-desktop pixels; `<= 0` means "no rectangle".

    bool operator==(const rect_t &) const = default;
  };

  /**
   * @brief Everything the software scaler needs in order to honour a viewport.
   *
   * `source` is what is read from the captured frame; `out_width`/`out_height` is what
   * swscale writes; `offset_w`/`offset_h` is where that lands inside the encode surface.
   * The encode surface itself never changes size — that is the point.
   */
  struct plan_t {
    rect_t source {};  ///< Rectangle of the captured frame to scale from.
    int out_width {};  ///< Scaled width written into the encode surface.
    int out_height {};  ///< Scaled height written into the encode surface.
    int offset_w {};  ///< Horizontal placement of the scaled image in the encode surface.
    int offset_h {};  ///< Vertical placement of the scaled image in the encode surface.
    bool cropped {};  ///< Whether a viewport was actually applied (false == today's behaviour).

    bool operator==(const plan_t &) const = default;
  };

  /**
   * @brief Round `value` down to the nearest multiple of two.
   *
   * Chroma planes are subsampled 2x2 in NV12 and YUV420. An odd source origin makes the
   * UV pointer land half a chroma sample early, which shows up as colour fringing along
   * the crop edge; an odd destination offset does the same at the letterbox seam. Every
   * cropped coordinate this header emits is therefore even. The uncropped plan is left
   * exactly as upstream computes it, odd values included, because reproducing today's
   * behaviour bit-for-bit outranks tidying it.
   *
   * @param value Value to round.
   * @return The largest even number `<= value`, or 0 for negative input.
   */
  [[nodiscard]] inline constexpr int floor_even(const int value) noexcept {
    return value <= 0 ? 0 : value & ~1;
  }

  /**
   * @brief The uncropped plan: scale the whole captured frame, exactly as upstream does.
   *
   * @param capture_width Width of the captured frame in pixels.
   * @param capture_height Height of the captured frame in pixels.
   * @param surface_width Width of the encode surface in pixels.
   * @param surface_height Height of the encode surface in pixels.
   * @return The full-frame plan.
   */
  [[nodiscard]] inline plan_t full_frame_plan(const int capture_width, const int capture_height, const int surface_width, const int surface_height) noexcept {
    plan_t plan;
    plan.source = {0, 0, capture_width, capture_height};

    // Byte-for-byte the arithmetic in avcodec_software_encode_device_t::init(): a float
    // scalar, a float multiply truncated to int, and an integer halving for the offset.
    // Do not "clean this up" -- the difference would be a silent behaviour change for
    // every client that never sends a viewport.
    const auto scalar = std::min(static_cast<float>(surface_width) / static_cast<float>(capture_width), static_cast<float>(surface_height) / static_cast<float>(capture_height));
    plan.out_width = static_cast<int>(static_cast<float>(capture_width) * scalar);
    plan.out_height = static_cast<int>(static_cast<float>(capture_height) * scalar);
    plan.offset_w = (surface_width - plan.out_width) / 2;
    plan.offset_h = (surface_height - plan.out_height) / 2;
    plan.cropped = false;
    return plan;
  }

  /**
   * @brief Clamp a network-supplied rectangle into something safe to read from.
   *
   * This is the hostile-input boundary. The rectangle arrives from a paired but otherwise
   * untrusted client, and its only job here is to come out inside `[0, capture)` on both
   * axes with even, non-degenerate extents — so that the pointer arithmetic downstream
   * cannot leave the captured buffer no matter what was sent.
   *
   * Order matters: clamp the origin first, then trim the extent to what is left, then
   * grow undersized extents back toward the minimum (pulling the origin in if the
   * rectangle would overhang), then even-align. Even-aligning before trimming would let a
   * rectangle grow back past the right or bottom edge.
   *
   * @param requested Rectangle as received.
   * @param capture_width Width of the captured frame in pixels.
   * @param capture_height Height of the captured frame in pixels.
   * @return The usable rectangle, or `std::nullopt` when nothing usable remains.
   */
  [[nodiscard]] inline std::optional<rect_t> sanitize(const rect_t &requested, const int capture_width, const int capture_height) noexcept {
    if (capture_width < min_source_extent || capture_height < min_source_extent) {
      // The captured frame is smaller than the smallest crop we allow; there is no
      // meaningful sub-rectangle of it.
      return std::nullopt;
    }
    if (requested.width <= 0 || requested.height <= 0 || requested.x < 0 || requested.y < 0) {
      return std::nullopt;
    }
    if (requested.x >= capture_width || requested.y >= capture_height) {
      // Wholly outside the desktop. Sliding it back inside would silently show the user
      // somewhere they did not ask for, which is worse than refusing.
      return std::nullopt;
    }

    rect_t r = requested;
    r.width = std::min(r.width, capture_width - r.x);
    r.height = std::min(r.height, capture_height - r.y);

    if (r.width < min_source_extent) {
      r.width = min_source_extent;
      r.x = std::min(r.x, capture_width - r.width);
    }
    if (r.height < min_source_extent) {
      r.height = min_source_extent;
      r.y = std::min(r.y, capture_height - r.height);
    }

    r.x = floor_even(r.x);
    r.y = floor_even(r.y);
    r.width = floor_even(std::min(r.width, capture_width - r.x));
    r.height = floor_even(std::min(r.height, capture_height - r.y));

    if (r.width < min_source_extent || r.height < min_source_extent) {
      return std::nullopt;
    }
    return r;
  }

  /**
   * @brief Decide what the scaler should do this frame.
   *
   * The single decision point for the whole feature. Every way of saying "no" — no
   * request, a hostile rectangle, a degenerate aspect ratio, a capture or surface size
   * that makes no sense — returns the full-frame plan, which is today's behaviour exactly.
   *
   * @param capture_width Width of the captured frame in pixels.
   * @param capture_height Height of the captured frame in pixels.
   * @param surface_width Width of the encode surface in pixels.
   * @param surface_height Height of the encode surface in pixels.
   * @param requested Rectangle the client asked for, if any.
   * @return The plan to apply.
   */
  [[nodiscard]] inline plan_t plan(const int capture_width, const int capture_height, const int surface_width, const int surface_height, const std::optional<rect_t> &requested) noexcept {
    if (capture_width <= 0 || capture_height <= 0 || surface_width <= 0 || surface_height <= 0) {
      // Nothing sane to compute. Hand back an all-zero plan; the caller treats a plan
      // whose source is empty as "leave the scaler alone".
      return {};
    }

    const auto full = full_frame_plan(capture_width, capture_height, surface_width, surface_height);
    if (!requested) {
      return full;
    }

    const auto source = sanitize(*requested, capture_width, capture_height);
    if (!source || *source == full.source) {
      return full;
    }

    plan_t cropped;
    cropped.source = *source;

    const auto scalar = std::min(static_cast<float>(surface_width) / static_cast<float>(source->width), static_cast<float>(surface_height) / static_cast<float>(source->height));
    cropped.out_width = floor_even(static_cast<int>(static_cast<float>(source->width) * scalar));
    cropped.out_height = floor_even(static_cast<int>(static_cast<float>(source->height) * scalar));

    if (cropped.out_width < min_output_extent || cropped.out_height < min_output_extent) {
      // Aspect ratio so far from the encode surface that the result would be a sliver.
      return full;
    }

    cropped.offset_w = floor_even((surface_width - cropped.out_width) / 2);
    cropped.offset_h = floor_even((surface_height - cropped.out_height) / 2);
    cropped.cropped = true;
    return cropped;
  }

  /**
   * @brief Where the uncropped desktop image sits inside the encode surface.
   *
   * This is the coordinate system the client expresses viewport rectangles in: the
   * negotiated stream resolution, with the desktop letterboxed into it exactly as the
   * uncropped stream presents it. `content_*` is the part of the surface that actually
   * shows desktop; everything outside it is padding and maps to nothing.
   */
  struct reference_frame_t {
    int surface_width {};  ///< Negotiated stream width -- the full reference space.
    int surface_height {};  ///< Negotiated stream height.
    int content_x {};  ///< Left edge of the desktop image within the surface.
    int content_y {};  ///< Top edge of the desktop image within the surface.
    int content_width {};  ///< Width of the desktop image within the surface.
    int content_height {};  ///< Height of the desktop image within the surface.

    bool operator==(const reference_frame_t &) const = default;
  };

  /**
   * @brief The reference frame for a given capture and encode surface.
   *
   * Derived from `full_frame_plan()` on purpose: the client is looking at the *uncropped*
   * stream's framing, so the reference space must be the uncropped placement, and it must
   * stay the uncropped placement while a crop is active.
   *
   * @param capture_width Width of the captured frame in pixels.
   * @param capture_height Height of the captured frame in pixels.
   * @param surface_width Width of the encode surface in pixels.
   * @param surface_height Height of the encode surface in pixels.
   * @return The reference frame; all-zero when the inputs are degenerate.
   */
  [[nodiscard]] inline reference_frame_t reference_frame(const int capture_width, const int capture_height, const int surface_width, const int surface_height) noexcept {
    if (capture_width <= 0 || capture_height <= 0 || surface_width <= 0 || surface_height <= 0) {
      return {};
    }
    const auto full = full_frame_plan(capture_width, capture_height, surface_width, surface_height);
    return {surface_width, surface_height, full.offset_w, full.offset_h, full.out_width, full.out_height};
  }

  /**
   * @brief Map a rectangle from the reference frame into captured-desktop pixels.
   *
   * The hostile-input boundary for the coordinate transform. The rectangle arrives from
   * the network in surface coordinates and may lie partly or wholly in the padding, where
   * there is no desktop to map to.
   *
   * The origin is floored and the far edge ceiled, so the returned rectangle covers at
   * least the region the client asked about rather than shaving a pixel off each edge at
   * every mapping. Both axes are scaled by their own ratio: `full_frame_plan()` truncates
   * `out_width` and `out_height` independently, so the two are equal only up to that
   * truncation, and using one for both would drift on a tall crop.
   *
   * @param in_frame Rectangle in reference-frame (negotiated stream resolution) pixels.
   * @param capture_width Width of the captured frame in pixels.
   * @param capture_height Height of the captured frame in pixels.
   * @param surface_width Width of the encode surface in pixels.
   * @param surface_height Height of the encode surface in pixels.
   * @return The rectangle in captured-desktop pixels, or `std::nullopt` when it does not
   *         intersect the part of the surface that shows desktop.
   */
  [[nodiscard]] inline std::optional<rect_t> to_desktop(const rect_t &in_frame, const int capture_width, const int capture_height, const int surface_width, const int surface_height) noexcept {
    const auto ref = reference_frame(capture_width, capture_height, surface_width, surface_height);
    if (ref.content_width <= 0 || ref.content_height <= 0) {
      return std::nullopt;
    }
    if (in_frame.width <= 0 || in_frame.height <= 0) {
      return std::nullopt;
    }

    // Intersect with the part of the surface that actually shows desktop. A request that
    // is entirely padding is refused rather than being slid onto the nearest real pixels.
    //
    // Widened to 64 bits for the far edges. Everything the wire can produce fits in an int
    // (`parse_payload` yields `uint16`s), but this is a public entry point on a hostile-input
    // path, and `x + width` on two `INT_MAX`s is undefined behaviour rather than a big
    // number. The intersection below then bounds every value back into the surface.
    const auto left = std::max<std::int64_t>(in_frame.x, ref.content_x);
    const auto top = std::max<std::int64_t>(in_frame.y, ref.content_y);
    const auto right = std::min<std::int64_t>(static_cast<std::int64_t>(in_frame.x) + in_frame.width, static_cast<std::int64_t>(ref.content_x) + ref.content_width);
    const auto bottom = std::min<std::int64_t>(static_cast<std::int64_t>(in_frame.y) + in_frame.height, static_cast<std::int64_t>(ref.content_y) + ref.content_height);
    if (right <= left || bottom <= top) {
      return std::nullopt;
    }

    const auto sx = static_cast<double>(capture_width) / static_cast<double>(ref.content_width);
    const auto sy = static_cast<double>(capture_height) / static_cast<double>(ref.content_height);

    const auto dx = static_cast<int>(std::floor(static_cast<double>(left - ref.content_x) * sx));
    const auto dy = static_cast<int>(std::floor(static_cast<double>(top - ref.content_y) * sy));
    const auto dr = static_cast<int>(std::ceil(static_cast<double>(right - ref.content_x) * sx));
    const auto db = static_cast<int>(std::ceil(static_cast<double>(bottom - ref.content_y) * sy));

    rect_t out;
    out.x = std::clamp(dx, 0, capture_width - 1);
    out.y = std::clamp(dy, 0, capture_height - 1);
    out.width = std::clamp(dr, out.x + 1, capture_width) - out.x;
    out.height = std::clamp(db, out.y + 1, capture_height) - out.y;
    return out;
  }

  /**
   * @brief Map a captured-desktop rectangle back into the reference frame.
   *
   * The inverse of `to_desktop()`, used for the echo so the client is answered in the
   * coordinate system it asked in. Rounds to nearest and clamps into the content area, so
   * the answer is always a rectangle the client could itself have sent.
   *
   * @param desktop Rectangle in captured-desktop pixels.
   * @param capture_width Width of the captured frame in pixels.
   * @param capture_height Height of the captured frame in pixels.
   * @param surface_width Width of the encode surface in pixels.
   * @param surface_height Height of the encode surface in pixels.
   * @return The rectangle in reference-frame pixels; all-zero when the inputs are degenerate.
   */
  [[nodiscard]] inline rect_t to_reference(const rect_t &desktop, const int capture_width, const int capture_height, const int surface_width, const int surface_height) noexcept {
    const auto ref = reference_frame(capture_width, capture_height, surface_width, surface_height);
    if (ref.content_width <= 0 || ref.content_height <= 0 || desktop.width <= 0 || desktop.height <= 0) {
      return {};
    }

    const auto sx = static_cast<double>(ref.content_width) / static_cast<double>(capture_width);
    const auto sy = static_cast<double>(ref.content_height) / static_cast<double>(capture_height);

    const auto left = ref.content_x + static_cast<int>(std::lround(desktop.x * sx));
    const auto top = ref.content_y + static_cast<int>(std::lround(desktop.y * sy));
    const auto right = ref.content_x + static_cast<int>(std::lround((desktop.x + desktop.width) * sx));
    const auto bottom = ref.content_y + static_cast<int>(std::lround((desktop.y + desktop.height) * sy));

    rect_t out;
    out.x = std::clamp(left, ref.content_x, ref.content_x + ref.content_width - 1);
    out.y = std::clamp(top, ref.content_y, ref.content_y + ref.content_height - 1);
    out.width = std::clamp(right, out.x + 1, ref.content_x + ref.content_width) - out.x;
    out.height = std::clamp(bottom, out.y + 1, ref.content_y + ref.content_height) - out.y;
    return out;
  }

  /**
   * @brief Read a little-endian 16-bit value out of a payload.
   *
   * Hand-rolled rather than `memcpy` + `endian` so this header stays dependency-free and
   * behaves identically on a big-endian host.
   *
   * @param payload Payload bytes.
   * @param offset Byte offset of the value.
   * @return The decoded value.
   */
  [[nodiscard]] inline std::uint16_t read_le16(const std::string_view payload, const std::size_t offset) noexcept {
    return static_cast<std::uint16_t>(static_cast<std::uint8_t>(payload[offset]) | (static_cast<std::uint16_t>(static_cast<std::uint8_t>(payload[offset + 1])) << 8));
  }

  /**
   * @brief Parse a viewport payload off the control stream.
   *
   * Rejects anything it cannot fully account for rather than acting on a partially
   * understood message. `width`/`height` arrive as `uint16` so they are never negative,
   * but zero is meaningless and is refused here so that a zero rectangle can be used
   * unambiguously as "no viewport" everywhere else.
   *
   * A payload *longer* than `payload_length` is accepted and the tail ignored: that is how
   * a future client adds fields under the reserved flags byte without a version bump.
   *
   * @param payload Payload bytes, excluding the control header.
   * @return The requested rectangle, or `std::nullopt` when the message is unusable.
   */
  [[nodiscard]] inline std::optional<rect_t> parse_payload(const std::string_view payload) noexcept {
    if (payload.size() < payload_length) {
      return std::nullopt;
    }
    if (static_cast<std::uint8_t>(payload[0]) != payload_version) {
      return std::nullopt;
    }
    // payload[1] is the reserved flags byte. No flags are defined for version 1, so any
    // that are set are ignored: a peer that needs us to understand a new field must bump
    // the version, which we reject above.

    rect_t r;
    r.x = read_le16(payload, 2);
    r.y = read_le16(payload, 4);
    r.width = read_le16(payload, 6);
    r.height = read_le16(payload, 8);

    if (r.width == 0 || r.height == 0) {
      return std::nullopt;
    }
    return r;
  }

  /**
   * @brief Serialize a rectangle into the viewport payload the client expects.
   *
   * Used for the echo: the host tells the client which rectangle it actually applied,
   * which is frequently not the one that was asked for.
   *
   * Values are truncated to 16 bits. They cannot exceed that in practice — every
   * rectangle written here has been through `sanitize()` against a captured frame, and
   * `meow::display_union` caps capture at 8192x4096 — but the truncation is explicit so
   * the wire format cannot be corrupted by a future caller with a larger surface.
   *
   * @param rect Rectangle to serialize.
   * @param out Destination buffer, at least `payload_length` bytes.
   */
  inline void write_payload(const rect_t &rect, std::uint8_t *const out) noexcept {
    const auto put16 = [out](const std::size_t offset, const int value) noexcept {
      const auto v = static_cast<std::uint16_t>(std::clamp(value, 0, 0xFFFF));
      out[offset] = static_cast<std::uint8_t>(v & 0xFF);
      out[offset + 1] = static_cast<std::uint8_t>((v >> 8) & 0xFF);
    };

    out[0] = payload_version;
    out[1] = 0;  // Flags (reserved)
    put16(2, rect.x);
    put16(4, rect.y);
    put16(6, rect.width);
    put16(8, rect.height);
  }

  /**
   * @brief Serialize the echo: the applied rectangle plus the captured desktop size.
   *
   * The rectangle is in **reference-frame** pixels -- the same coordinate system the
   * request arrived in -- because that is the only space the client can act on without
   * already knowing the answer.
   *
   * The two extra `uint16`s are what make the whole coordinate question decidable at the
   * other end: with the negotiated stream resolution (which the client already has) and
   * the captured desktop size, the client can compute the host's `min()` scalar and
   * padding offsets itself. `flag_desktop_extent` announces them. An existing client reads
   * the first ten bytes, ignores the flag it does not know and the four bytes it does not
   * expect, and behaves exactly as before -- no version bump, no break.
   *
   * @param applied_in_reference Applied rectangle, in reference-frame pixels.
   * @param capture_width Width of the captured desktop in pixels.
   * @param capture_height Height of the captured desktop in pixels.
   * @param out Destination buffer, at least `echo_payload_length` bytes.
   */
  inline void write_echo_payload(const rect_t &applied_in_reference, const int capture_width, const int capture_height, std::uint8_t *const out) noexcept {
    const auto put16 = [out](const std::size_t offset, const int value) noexcept {
      const auto v = static_cast<std::uint16_t>(std::clamp(value, 0, 0xFFFF));
      out[offset] = static_cast<std::uint8_t>(v & 0xFF);
      out[offset + 1] = static_cast<std::uint8_t>((v >> 8) & 0xFF);
    };

    write_payload(applied_in_reference, out);
    out[1] = flag_desktop_extent;
    put16(10, capture_width);
    put16(12, capture_height);
  }

  /**
   * @brief What to do about a viewport packet that just arrived.
   */
  struct request_outcome_t {
    /**
     * @brief Rectangle to echo to the client, in **reference-frame** pixels.
     *
     * `std::nullopt` when there is nothing to say. This is the *applied* rectangle, which
     * is frequently not the requested one -- it is clamped to the desktop, grown to a
     * minimum size, even-aligned for chroma, or refused outright. It is the full content
     * area when the request was refused, so the client can always tell "refused" apart
     * from "lost in transit".
     */
    std::optional<rect_t> echo {};

    /**
     * @brief Rectangle to hand to the encode thread, in **captured-desktop** pixels.
     *
     * `std::nullopt` means "stream the full desktop". This is the mapped request rather
     * than the planned source, because the encode thread re-derives the plan against the
     * frame actually in hand; publishing a pre-computed source would bake in a capture
     * size that may already be stale.
     */
    std::optional<rect_t> publish {};

    bool operator==(const request_outcome_t &) const = default;
  };

  /**
   * @brief Decide what a viewport packet means, without touching any state.
   *
   * The whole hostile-input path in one pure function: parse, map out of the client's
   * coordinate system, validate, plan, and report both what the client should be told and
   * what the encoder should be given.
   *
   * @param payload Control-stream payload, excluding the header.
   * @param capture_width Width of the captured frame in pixels.
   * @param capture_height Height of the captured frame in pixels.
   * @param surface_width Width of the encode surface in pixels.
   * @param surface_height Height of the encode surface in pixels.
   * @return What to echo and what to publish.
   */
  [[nodiscard]] inline request_outcome_t evaluate_request(const std::string_view payload, const int capture_width, const int capture_height, const int surface_width, const int surface_height) noexcept {
    const auto in_frame = parse_payload(payload);
    const auto requested = in_frame ? to_desktop(*in_frame, capture_width, capture_height, surface_width, surface_height) : std::nullopt;
    const auto applied = plan(capture_width, capture_height, surface_width, surface_height, requested);
    if (applied.source.width <= 0 || applied.source.height <= 0) {
      // Degenerate capture or surface size; there is nothing truthful to report.
      return {};
    }
    return {to_reference(applied.source, capture_width, capture_height, surface_width, surface_height), applied.cropped ? requested : std::nullopt};
  }

  /**
   * @brief Whether `control_packet_type` collides with an existing control message.
   *
   * Called with `src/stream.cpp`'s real `packetTypes` table at registration time. A
   * collision would make `control_server_t::call()` dispatch a genuine upstream message
   * into the viewport handler — silent, and catastrophic for whatever feature owned the
   * number. Registration is refused instead.
   *
   * @param table The host's control-stream packet type table.
   * @param count Number of entries in `table`.
   * @return `true` when the table already uses `control_packet_type`.
   */
  [[nodiscard]] inline constexpr bool packet_type_collision(const short *const table, const std::size_t count) noexcept {
    for (std::size_t i = 0; i < count; ++i) {
      if (static_cast<std::uint16_t>(table[i]) == control_packet_type) {
        return true;
      }
    }
    return false;
  }

  /**
   * @brief The message logged when registration is refused because of a collision.
   *
   * Returned rather than logged so this header stays free of Boost and remains unit
   * testable without a logging sink (CLAUDE.md §5.5).
   *
   * @return A human-readable explanation.
   */
  [[nodiscard]] inline std::string packet_type_collision_warning() {
    return "meow viewport: control packet type 0x3003 is already used by an upstream control "
           "message in src/stream.cpp. Viewport following is disabled for this build so the "
           "upstream message keeps working; the full desktop will be streamed. Pick a new "
           "number here and in packetTypesGen7Enc[IDX_VIEWPORT] on branch 'meow' of "
           "meowerse/moonlight-common-c -- they must match.";
  }

  /**
   * @brief One line describing the state of the feature, for the host log.
   *
   * Written here rather than at the call site so the wording is testable and there is one
   * definition of it. Without this line, a user who set the key and mistyped it has no way
   * to tell the difference between "off" and "on but nothing is asking for a crop".
   *
   * @param enabled Whether viewport following is enabled.
   * @return The line to log.
   */
  [[nodiscard]] inline std::string following_status(const bool enabled) {
    if (enabled) {
      return std::string(
        "meow viewport following: enabled. The client may request a crop of the desktop; "
        "only the software scaling path honours it, and absolute pointer coordinates are not remapped."
      );
    }
    return std::string("meow viewport following: disabled. Set '").append(following_config_key).append(" = enabled' in sunshine.conf to allow the client to crop the streamed desktop.");
  }

  /**
   * @brief Interpret the value of `meow_viewport_following` from a config file.
   *
   * Mirrors `config::to_bool()` in accepting `true`/`yes`/`enable`/`enabled`/`on` and any
   * non-zero integer, so the key behaves like every other boolean in the same file.
   *
   * @param value Raw value text.
   * @return The parsed setting.
   */
  [[nodiscard]] inline bool parse_following_value(std::string_view value) {
    std::string lowered;
    lowered.reserve(value.size());
    for (const auto ch : value) {
      lowered.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
    }

    if (lowered == "true" || lowered == "yes" || lowered == "enable" || lowered == "enabled" || lowered == "on") {
      return true;
    }
    if (lowered == "false" || lowered == "no" || lowered == "disable" || lowered == "disabled" || lowered == "off") {
      return false;
    }
    return std::atoi(lowered.c_str()) != 0;
  }

}  // namespace meow::viewport
