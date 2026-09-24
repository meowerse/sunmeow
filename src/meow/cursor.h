/**
 * @file src/meow/cursor.h
 * @brief Pure logic for host cursor reporting: wire format, PipeWire cursor metadata,
 *        software cursor blending, reference-frame mapping and send coalescing.
 *
 * The client follows the host's real mouse cursor with its zoomed view. To do that it needs
 * the cursor position, which a stream with the cursor painted into the pixels does not
 * carry. KWin can instead deliver the cursor as PipeWire metadata (`SPA_META_Cursor`:
 * position, hotspot and, when the shape changes, a bitmap). Using that mode means the stream
 * no longer contains a cursor, so the host paints the bitmap back in itself before the frame
 * is uploaded, and publishes the position for the control thread to send as `0x3004`.
 *
 * Everything in this header is a pure function over integers and byte buffers - no PipeWire,
 * no GPU, no network - so the parsing, the blend and the mapping are unit tested pixel for
 * pixel (CLAUDE.md §5.5). The PipeWire glue is `src/meow/cursor_pipewire.h`; the
 * cross-thread state and the control-stream side are `src/meow/cursor_runtime.h`.
 */
#pragma once

// standard includes
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

// local includes
#include "src/meow/viewport.h"

namespace meow::cursor {

  /**
   * @brief Control-stream packet type of the cursor messages (both directions).
   *
   * Must equal `packetTypesGen7Enc[IDX_CURSOR]` on branch `meow` of
   * `meowerse/moonlight-common-c`. Registration is refused on a collision with the host's own
   * `packetTypes` table (see `src/meow/control_stream.h`).
   */
  inline constexpr std::uint16_t control_packet_type = 0x3004;

  /**
   * @brief Version byte of both cursor payloads.
   */
  inline constexpr std::uint8_t payload_version = 1;

  /**
   * @brief Exact length of a client -> host SUBSCRIBE.
   */
  inline constexpr std::size_t subscribe_length = 2;

  /**
   * @brief SUBSCRIBE flag bit: 1 subscribes, 0 unsubscribes.
   */
  inline constexpr std::uint8_t flag_subscribe = 0x01;

  /**
   * @brief Exact length of a host -> client POSITION.
   */
  inline constexpr std::size_t position_length = 8;

  /**
   * @brief POSITION flag bit: the cursor is visible.
   */
  inline constexpr std::uint8_t flag_visible = 0x01;

  /**
   * @brief Largest cursor bitmap accepted from the compositor, per axis.
   *
   * Matches the upper bound KWin advertises for `SPA_META_Cursor`. Anything larger is a
   * corrupt or hostile buffer, not a cursor.
   */
  inline constexpr int max_bitmap_extent = 1024;

  /**
   * @brief Bitmap size requested by default: enough for a 4x-scaled 64 px cursor.
   */
  inline constexpr int default_bitmap_extent = 256;

  /**
   * @brief Configuration key that enables cursor reporting.
   */
  inline constexpr std::string_view reporting_config_key = "meow_cursor_reporting";

  /**
   * @brief Parse a client SUBSCRIBE payload.
   *
   * @param payload Control-stream payload, excluding the header.
   * @return `true` to subscribe, `false` to unsubscribe, `std::nullopt` when the payload is
   *         short, oversize or of an unknown version and must be dropped.
   */
  [[nodiscard]] inline std::optional<bool> parse_subscribe(const std::string_view payload) noexcept {
    if (payload.size() != subscribe_length || static_cast<std::uint8_t>(payload[0]) != payload_version) {
      return std::nullopt;
    }
    return (static_cast<std::uint8_t>(payload[1]) & flag_subscribe) != 0;
  }

  /**
   * @brief Serialize a POSITION payload.
   *
   * Layout (little endian): `u8 version, u8 flags (bit0 visible), u16 seq, u16 x, u16 y`.
   *
   * @param x Hotspot x in the reference frame.
   * @param y Hotspot y in the reference frame.
   * @param visible Whether the cursor is visible.
   * @param seq Sequence number of this send.
   * @param out Destination, at least `position_length` bytes.
   */
  inline void write_position(const std::uint16_t x, const std::uint16_t y, const bool visible, const std::uint16_t seq, std::uint8_t *const out) noexcept {
    out[0] = payload_version;
    out[1] = visible ? flag_visible : 0;
    out[2] = static_cast<std::uint8_t>(seq & 0xFF);
    out[3] = static_cast<std::uint8_t>(seq >> 8);
    out[4] = static_cast<std::uint8_t>(x & 0xFF);
    out[5] = static_cast<std::uint8_t>(x >> 8);
    out[6] = static_cast<std::uint8_t>(y & 0xFF);
    out[7] = static_cast<std::uint8_t>(y >> 8);
  }

  /**
   * @brief Byte order of a 32-bit pixel, as it lies in memory.
   */
  enum class pixel_format_t {
    unknown,  ///< Not something this code can read or write.
    bgra,  ///< B, G, R, A.
    bgrx,  ///< B, G, R, unused.
    rgba,  ///< R, G, B, A.
    rgbx,  ///< R, G, B, unused.
    argb,  ///< A, R, G, B.
    abgr  ///< A, B, G, R.
  };

  /**
   * @brief Whether frames of this format can have a cursor blended into them.
   * @param format Frame format.
   * @return True for the 8-bit, 4-byte formats with R, G, B in the first three bytes.
   */
  [[nodiscard]] inline constexpr bool blendable_frame_format(const pixel_format_t format) noexcept {
    return format == pixel_format_t::bgra || format == pixel_format_t::bgrx || format == pixel_format_t::rgba || format == pixel_format_t::rgbx;
  }

  /**
   * @brief Mirror of `struct spa_meta_cursor` (`spa/buffer/meta.h`).
   *
   * Declared here so this header needs no PipeWire headers; `src/meow/cursor_pipewire.h`
   * `static_assert`s that the real struct has the same size and offsets.
   */
  struct spa_meta_cursor_layout_t {
    std::uint32_t id;  ///< Cursor id; 0 means no cursor / invalid.
    std::uint32_t flags;  ///< Unused.
    std::int32_t position_x;  ///< Hotspot position in the frame.
    std::int32_t position_y;  ///< Hotspot position in the frame.
    std::int32_t hotspot_x;  ///< Hotspot offset inside the bitmap.
    std::int32_t hotspot_y;  ///< Hotspot offset inside the bitmap.
    std::uint32_t bitmap_offset;  ///< Offset of a `spa_meta_bitmap` from this struct; 0 = unchanged.
  };

  /**
   * @brief Mirror of `struct spa_meta_bitmap` (`spa/buffer/meta.h`).
   */
  struct spa_meta_bitmap_layout_t {
    std::uint32_t format;  ///< `enum spa_video_format`.
    std::uint32_t width;  ///< Bitmap width.
    std::uint32_t height;  ///< Bitmap height.
    std::int32_t stride;  ///< Bytes per row.
    std::uint32_t offset;  ///< Offset of the pixels from this struct.
  };

  /**
   * @brief A validated view of a cursor bitmap inside a metadata block.
   */
  struct bitmap_view_t {
    pixel_format_t format = pixel_format_t::unknown;  ///< Pixel byte order.
    int width = 0;  ///< Width in pixels; 0 means "no cursor image" (hidden).
    int height = 0;  ///< Height in pixels.
    int stride = 0;  ///< Bytes per row.
    const std::uint8_t *pixels = nullptr;  ///< First pixel; valid for `stride * (height - 1) + width * 4` bytes.
  };

  /**
   * @brief One parsed `SPA_META_Cursor` block.
   */
  struct meta_t {
    bool visible = false;  ///< Whether the compositor reports a cursor at all.
    int x = 0;  ///< Hotspot x, in frame pixels.
    int y = 0;  ///< Hotspot y, in frame pixels.
    int hotspot_x = 0;  ///< Hotspot offset inside the bitmap.
    int hotspot_y = 0;  ///< Hotspot offset inside the bitmap.
    std::optional<bitmap_view_t> bitmap;  ///< A new cursor image, when this block carries one.
  };

  /**
   * @brief Parse a `SPA_META_Cursor` block without ever reading outside it.
   *
   * The block comes from the compositor through shared memory, so every offset and size in it
   * is checked against `size` before it is followed. A block that is too short is rejected
   * entirely; a bitmap that does not fit, has an absurd size or an unknown format is dropped
   * while the position is kept (the previous cursor image stays in use).
   *
   * @tparam FormatMapper Callable `pixel_format_t(std::uint32_t spa_video_format)`.
   * @param data Start of the metadata block.
   * @param size Size of the metadata block in bytes.
   * @param map_format Maps a SPA video format to a pixel byte order.
   * @return The parsed block, or `std::nullopt` when it is unusable.
   */
  template<class FormatMapper>
  [[nodiscard]] std::optional<meta_t> parse_meta(const std::uint8_t *const data, const std::size_t size, FormatMapper map_format) noexcept {
    if (!data || size < sizeof(spa_meta_cursor_layout_t)) {
      return std::nullopt;
    }
    spa_meta_cursor_layout_t cursor;
    std::memcpy(&cursor, data, sizeof(cursor));

    meta_t meta;
    if (cursor.id == 0) {
      // `spa_meta_cursor_is_valid()` is false. Consumers (OBS, and KWin's own tests against
      // them) treat that as "no cursor over this stream", so it is reported as hidden.
      return meta;
    }
    meta.visible = true;
    meta.x = cursor.position_x;
    meta.y = cursor.position_y;
    meta.hotspot_x = cursor.hotspot_x;
    meta.hotspot_y = cursor.hotspot_y;

    if (cursor.bitmap_offset == 0) {
      return meta;  // Same image as before.
    }
    const std::size_t bitmap_at = cursor.bitmap_offset;
    if (bitmap_at < sizeof(spa_meta_cursor_layout_t) || bitmap_at > size || size - bitmap_at < sizeof(spa_meta_bitmap_layout_t)) {
      return meta;
    }
    spa_meta_bitmap_layout_t bitmap;
    std::memcpy(&bitmap, data + bitmap_at, sizeof(bitmap));

    bitmap_view_t view;
    if (bitmap.format == 0) {
      return meta;  // `spa_meta_bitmap_is_valid()` is false: no new image information.
    }
    if (bitmap.offset == 0 || bitmap.width == 0 || bitmap.height == 0) {
      // No image data: the pointer has no visible shape (e.g. an application hid it).
      meta.bitmap = view;
      return meta;
    }
    if (bitmap.width > static_cast<std::uint32_t>(max_bitmap_extent) || bitmap.height > static_cast<std::uint32_t>(max_bitmap_extent) || bitmap.stride < 0) {
      return meta;
    }
    view.format = map_format(bitmap.format);
    if (view.format == pixel_format_t::unknown) {
      return meta;
    }
    view.width = static_cast<int>(bitmap.width);
    view.height = static_cast<int>(bitmap.height);
    view.stride = bitmap.stride;
    if (view.stride < view.width * 4) {
      return meta;
    }
    // All in 64 bits: nothing here can overflow, whatever the compositor wrote.
    const std::uint64_t pixels_at = static_cast<std::uint64_t>(bitmap_at) + bitmap.offset;
    const std::uint64_t pixels_len = static_cast<std::uint64_t>(view.stride) * static_cast<std::uint64_t>(view.height - 1) + static_cast<std::uint64_t>(view.width) * 4;
    if (bitmap.offset < sizeof(spa_meta_bitmap_layout_t) || pixels_at > size || pixels_len > size - pixels_at) {
      return meta;
    }
    view.pixels = data + pixels_at;
    meta.bitmap = view;
    return meta;
  }

  /**
   * @brief A cursor image in a canonical form: premultiplied B, G, R, A, tightly packed.
   *
   * Converted once when the compositor sends a new shape (rare), so the per-frame blend never
   * has to look at the source format.
   */
  struct image_t {
    std::vector<std::uint8_t> bgra;  ///< Premultiplied BGRA, `width * height * 4` bytes.
    int width = 0;  ///< Width in pixels; 0 = no image.
    int height = 0;  ///< Height in pixels.
    int hotspot_x = 0;  ///< Hotspot offset inside the image.
    int hotspot_y = 0;  ///< Hotspot offset inside the image.

    /**
     * @brief Whether there is anything to draw.
     * @return True when the image is non-empty.
     */
    [[nodiscard]] bool empty() const noexcept {
      return width <= 0 || height <= 0;
    }
  };

  /**
   * @brief Copy a validated bitmap into an `image_t`.
   *
   * KWin writes its cursor as premultiplied RGBA (`QImage::Format_RGBA8888_Premultiplied`);
   * `premultiplied = false` converts straight alpha for compositors that do not.
   *
   * Reuses `out`'s storage, so it allocates only when a cursor grows past the largest seen.
   *
   * @param view Bitmap from `parse_meta()`.
   * @param hotspot_x Hotspot offset inside the bitmap.
   * @param hotspot_y Hotspot offset inside the bitmap.
   * @param premultiplied Whether the source alpha is already premultiplied.
   * @param out Destination image.
   */
  inline void import_bitmap(const bitmap_view_t &view, const int hotspot_x, const int hotspot_y, const bool premultiplied, image_t &out) {
    out.hotspot_x = hotspot_x;
    out.hotspot_y = hotspot_y;
    if (view.width <= 0 || view.height <= 0 || !view.pixels) {
      out.width = 0;
      out.height = 0;
      return;
    }
    out.width = view.width;
    out.height = view.height;
    out.bgra.resize(static_cast<std::size_t>(view.width) * static_cast<std::size_t>(view.height) * 4);

    // Byte index of B, G, R, A within a source pixel, and whether A exists.
    int ib = 0;
    int ig = 1;
    int ir = 2;
    int ia = 3;
    bool has_alpha = true;
    switch (view.format) {
      case pixel_format_t::bgra:
        break;
      case pixel_format_t::bgrx:
        has_alpha = false;
        break;
      case pixel_format_t::rgba:
        ib = 2;
        ir = 0;
        break;
      case pixel_format_t::rgbx:
        ib = 2;
        ir = 0;
        has_alpha = false;
        break;
      case pixel_format_t::argb:
        ia = 0;
        ir = 1;
        ig = 2;
        ib = 3;
        break;
      case pixel_format_t::abgr:
        ia = 0;
        ib = 1;
        ig = 2;
        ir = 3;
        break;
      case pixel_format_t::unknown:
      default:
        out.width = 0;
        out.height = 0;
        return;
    }

    for (int row = 0; row < view.height; ++row) {
      const auto *src = view.pixels + static_cast<std::ptrdiff_t>(row) * view.stride;
      auto *dst = out.bgra.data() + static_cast<std::ptrdiff_t>(row) * view.width * 4;
      for (int col = 0; col < view.width; ++col, src += 4, dst += 4) {
        const std::uint32_t a = has_alpha ? src[ia] : 255u;
        std::uint32_t b = src[ib];
        std::uint32_t g = src[ig];
        std::uint32_t r = src[ir];
        if (!premultiplied && a != 255u) {
          b = (b * a + 127u) / 255u;
          g = (g * a + 127u) / 255u;
          r = (r * a + 127u) / 255u;
        }
        // A premultiplied channel can never exceed alpha; clamp so a malformed image cannot
        // wrap in the blend below.
        dst[0] = static_cast<std::uint8_t>(std::min(b, a));
        dst[1] = static_cast<std::uint8_t>(std::min(g, a));
        dst[2] = static_cast<std::uint8_t>(std::min(r, a));
        dst[3] = static_cast<std::uint8_t>(a);
      }
    }
  }

  /**
   * @brief The frame pixels a blended cursor covered, so they can be put back.
   *
   * A cursor that moves while the desktop is idle is re-drawn into the *same* captured frame,
   * so what it covered last time must be restored first. Storage is reused, so this does not
   * allocate after the first (largest) cursor.
   */
  struct save_under_t {
    std::vector<std::uint8_t> pixels;  ///< Saved rows, `width * 4` bytes each.
    int x = 0;  ///< Left edge of the saved rectangle in the frame.
    int y = 0;  ///< Top edge of the saved rectangle in the frame.
    int width = 0;  ///< Width of the saved rectangle; 0 = nothing saved.
    int height = 0;  ///< Height of the saved rectangle.
  };

  /**
   * @brief Put back what the last blend covered.
   *
   * @param frame First byte of the frame.
   * @param stride Bytes per frame row.
   * @param save What the last blend saved; cleared by this call.
   */
  inline void restore(std::uint8_t *const frame, const int stride, save_under_t &save) noexcept {
    if (!frame || save.width <= 0 || save.height <= 0) {
      save.width = 0;
      save.height = 0;
      return;
    }
    const auto row_bytes = static_cast<std::size_t>(save.width) * 4;
    for (int row = 0; row < save.height; ++row) {
      std::memcpy(frame + static_cast<std::ptrdiff_t>(save.y + row) * stride + static_cast<std::ptrdiff_t>(save.x) * 4, save.pixels.data() + static_cast<std::size_t>(row) * row_bytes, row_bytes);
    }
    save.width = 0;
    save.height = 0;
  }

  /**
   * @brief Draw a cursor into a 32-bit frame, premultiplied "over", clipped to the frame.
   *
   * The image's top-left lands at `(x - hotspot_x, y - hotspot_y)`. Pixels outside the frame
   * are skipped, so a cursor half off an edge is drawn half. What is covered is saved into
   * `save` first (when given) so `restore()` can undo it exactly.
   *
   * Per channel: `out = src + dst * (255 - a) / 255`, rounded to nearest. Alpha of an `x`
   * format is left untouched.
   *
   * @param frame First byte of the frame.
   * @param width Frame width in pixels.
   * @param height Frame height in pixels.
   * @param stride Bytes per frame row (at least `width * 4`).
   * @param format Frame byte order; must be `blendable_frame_format()`.
   * @param image Cursor image.
   * @param x Hotspot x in the frame.
   * @param y Hotspot y in the frame.
   * @param save Receives what was covered; may be null.
   * @return True when anything was drawn.
   */
  inline bool blend(std::uint8_t *const frame, const int width, const int height, const int stride, const pixel_format_t format, const image_t &image, const int x, const int y, save_under_t *const save) {
    if (save) {
      save->width = 0;
      save->height = 0;
    }
    if (!frame || image.empty() || width <= 0 || height <= 0 || stride < width * 4 || !blendable_frame_format(format)) {
      return false;
    }

    // Clip in 64 bits: the position comes from the compositor and may be anywhere.
    const std::int64_t left = static_cast<std::int64_t>(x) - image.hotspot_x;
    const std::int64_t top = static_cast<std::int64_t>(y) - image.hotspot_y;
    const std::int64_t x0 = std::max<std::int64_t>(left, 0);
    const std::int64_t y0 = std::max<std::int64_t>(top, 0);
    const std::int64_t x1 = std::min<std::int64_t>(left + image.width, width);
    const std::int64_t y1 = std::min<std::int64_t>(top + image.height, height);
    if (x1 <= x0 || y1 <= y0) {
      return false;
    }
    const int cw = static_cast<int>(x1 - x0);
    const int ch = static_cast<int>(y1 - y0);

    if (save) {
      save->pixels.resize(static_cast<std::size_t>(cw) * static_cast<std::size_t>(ch) * 4);
      save->x = static_cast<int>(x0);
      save->y = static_cast<int>(y0);
      save->width = cw;
      save->height = ch;
    }

    const bool swap_rb = format == pixel_format_t::rgba || format == pixel_format_t::rgbx;
    const bool write_alpha = format == pixel_format_t::bgra || format == pixel_format_t::rgba;
    const auto blend_channel = [](const std::uint32_t src, const std::uint32_t dst, const std::uint32_t inverse) {
      return static_cast<std::uint8_t>(std::min<std::uint32_t>(255u, src + (dst * inverse + 127u) / 255u));
    };

    for (int row = 0; row < ch; ++row) {
      auto *dst = frame + (y0 + row) * static_cast<std::int64_t>(stride) + x0 * 4;
      if (save) {
        std::memcpy(save->pixels.data() + static_cast<std::size_t>(row) * static_cast<std::size_t>(cw) * 4, dst, static_cast<std::size_t>(cw) * 4);
      }
      const auto *src = image.bgra.data() + ((y0 - top + row) * image.width + (x0 - left)) * 4;
      for (int col = 0; col < cw; ++col, src += 4, dst += 4) {
        const std::uint32_t a = src[3];
        if (a == 0) {
          continue;
        }
        const std::uint32_t inverse = 255u - a;
        const std::uint32_t sb = swap_rb ? src[2] : src[0];
        const std::uint32_t sr = swap_rb ? src[0] : src[2];
        dst[0] = blend_channel(sb, dst[0], inverse);
        dst[1] = blend_channel(src[1], dst[1], inverse);
        dst[2] = blend_channel(sr, dst[2], inverse);
        if (write_alpha) {
          dst[3] = blend_channel(a, dst[3], inverse);
        }
      }
    }
    return true;
  }

  /**
   * @brief Map a cursor hotspot from captured-frame pixels into the reference frame.
   *
   * The reference frame is the negotiated stream resolution *uncropped*, with Sunshine's
   * aspect-ratio padding - the same space `0x3003` uses (`viewport::to_reference()`). The
   * point is first clamped into the captured frame, then mapped and clamped into the content
   * area, so the client is never sent a position in the padding.
   *
   * @param x Hotspot x in captured pixels.
   * @param y Hotspot y in captured pixels.
   * @param capture_width Captured frame width.
   * @param capture_height Captured frame height.
   * @param surface_width Negotiated stream width.
   * @param surface_height Negotiated stream height.
   * @return The point in the reference frame, or `std::nullopt` when the geometry is degenerate.
   */
  [[nodiscard]] inline std::optional<std::pair<int, int>> to_reference(const int x, const int y, const int capture_width, const int capture_height, const int surface_width, const int surface_height) noexcept {
    const auto ref = viewport::reference_frame(capture_width, capture_height, surface_width, surface_height);
    if (ref.content_width <= 0 || ref.content_height <= 0) {
      return std::nullopt;
    }
    const auto cx = std::clamp(x, 0, capture_width - 1);
    const auto cy = std::clamp(y, 0, capture_height - 1);
    const auto sx = static_cast<double>(ref.content_width) / static_cast<double>(capture_width);
    const auto sy = static_cast<double>(ref.content_height) / static_cast<double>(capture_height);
    const auto rx = ref.content_x + static_cast<int>(std::lround(cx * sx));
    const auto ry = ref.content_y + static_cast<int>(std::lround(cy * sy));
    return std::pair<int, int> {std::clamp(rx, ref.content_x, ref.content_x + ref.content_width - 1), std::clamp(ry, ref.content_y, ref.content_y + ref.content_height - 1)};
  }

  /**
   * @brief A cursor state as published by the capture thread.
   */
  struct state_t {
    int x = 0;  ///< Hotspot x in captured pixels.
    int y = 0;  ///< Hotspot y in captured pixels.
    bool visible = false;  ///< Whether a cursor is shown.
    std::uint32_t generation = 0;  ///< Incremented on every change.

    bool operator==(const state_t &) const = default;
  };

  /**
   * @brief Decides when a subscribed client is sent a POSITION.
   *
   * Sends on change, at most every `min_interval` (60 Hz), and immediately - ignoring the
   * interval - on subscribe and when visibility changes, so a cursor that disappears or
   * reappears is never late. A change that arrives inside the interval is not lost: it is
   * sent as soon as the interval has passed (coalesced to the latest state).
   */
  class coalescer_t {
  public:
    /**
     * @brief Minimum time between two sends: 1/60 s.
     */
    static constexpr std::chrono::nanoseconds min_interval {16'666'667};

    /**
     * @brief Force the next send regardless of change or interval (subscribe).
     */
    void force() noexcept {
      forced_ = true;
    }

    /**
     * @brief Whether a send is owed.
     *
     * @param state Latest published state.
     * @param now Current time.
     * @return True when the caller should send `state` now (and then call `sent()`).
     */
    [[nodiscard]] bool due(const state_t &state, const std::chrono::steady_clock::time_point now) const noexcept {
      if (forced_ || !last_) {
        return true;
      }
      if (state.visible != last_->visible) {
        return true;
      }
      if (state.generation == last_->generation) {
        return false;
      }
      if (!state.visible && !last_->visible) {
        return false;  // Moved while hidden: nothing the client can use.
      }
      return now - last_send_ >= min_interval;
    }

    /**
     * @brief Record that `state` was sent at `now`.
     * @param state The state that was sent.
     * @param now Send time.
     */
    void sent(const state_t &state, const std::chrono::steady_clock::time_point now) noexcept {
      last_ = state;
      last_send_ = now;
      forced_ = false;
    }

  private:
    std::optional<state_t> last_;  ///< Last state sent.
    std::chrono::steady_clock::time_point last_send_ {};  ///< When it was sent.
    bool forced_ = false;  ///< Send on the next check regardless.
  };

}  // namespace meow::cursor
