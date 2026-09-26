/**
 * @file src/meow/cursor_pipewire.h
 * @brief PipeWire glue for host cursor reporting: request `SPA_META_Cursor`, read it, and
 *        draw the cursor into memory frames so the stream still shows one.
 *
 * Used only when the capture stream was opened in KWin's `metadata` pointer mode, which
 * `src/platform/linux/kwingrab.cpp` requests only for a stream that delivers **memory
 * buffers** (`metadata_mode_wanted()`): those frames pass through the CPU before
 * `cuda_ram_t::load_ram()` (or the software scaler) reads them, so the cursor can be painted
 * back in with the pure blend in `src/meow/cursor.h`. A DMA-BUF frame never reaches the CPU,
 * so a DMA-BUF stream keeps the embedded cursor and reports no positions. With metadata mode
 * off, nothing in this file runs and the upstream PipeWire path is untouched.
 *
 * ## Why this path does its own buffer handling in metadata mode
 *
 * In metadata mode KWin sends a buffer on every pointer motion, flagged
 * `SPA_CHUNK_FLAG_CORRUPTED` ("do not look at the frame contents") because it carries only the
 * cursor. Upstream's memory path would `memcpy` that garbage into the staging buffer and swap
 * it in, and its `fill_img()` reads the chunk flags of a buffer it has already given back to
 * PipeWire - which KWin reuses for exactly these cursor-only updates. So in metadata mode the
 * memory path is handled here: a data frame is copied and its header/damage metadata recorded
 * at copy time; a cursor-only buffer only updates the cursor and wakes the capture thread,
 * which re-draws the cursor into the frame it already has.
 */
#pragma once

// standard includes
#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <optional>
#include <vector>

// lib includes
#include <pipewire/pipewire.h>
#include <spa/buffer/meta.h>
#include <spa/param/video/raw.h>
#include <spa/pod/builder.h>

// local includes
#include "src/logging.h"
#include "src/meow/cursor.h"
#include "src/meow/cursor_runtime.h"
#include "src/platform/common.h"

namespace meow::cursor::pipewire {

  using namespace std::literals;

  static_assert(sizeof(spa_meta_cursor) == sizeof(spa_meta_cursor_layout_t), "src/meow/cursor.h mirrors struct spa_meta_cursor");
  static_assert(offsetof(spa_meta_cursor, position) == offsetof(spa_meta_cursor_layout_t, position_x));
  static_assert(offsetof(spa_meta_cursor, hotspot) == offsetof(spa_meta_cursor_layout_t, hotspot_x));
  static_assert(offsetof(spa_meta_cursor, bitmap_offset) == offsetof(spa_meta_cursor_layout_t, bitmap_offset));
  static_assert(sizeof(spa_meta_bitmap) == sizeof(spa_meta_bitmap_layout_t), "src/meow/cursor.h mirrors struct spa_meta_bitmap");
  static_assert(offsetof(spa_meta_bitmap, size) == offsetof(spa_meta_bitmap_layout_t, width));
  static_assert(offsetof(spa_meta_bitmap, stride) == offsetof(spa_meta_bitmap_layout_t, stride));
  static_assert(offsetof(spa_meta_bitmap, offset) == offsetof(spa_meta_bitmap_layout_t, offset));

  /**
   * @brief Map a SPA video format to a pixel byte order.
   * @param format `enum spa_video_format`.
   * @return The byte order, or `unknown`.
   */
  [[nodiscard]] inline pixel_format_t from_spa(const std::uint32_t format) noexcept {
    switch (format) {
      case SPA_VIDEO_FORMAT_BGRA:
        return pixel_format_t::bgra;
      case SPA_VIDEO_FORMAT_BGRx:
        return pixel_format_t::bgrx;
      case SPA_VIDEO_FORMAT_RGBA:
        return pixel_format_t::rgba;
      case SPA_VIDEO_FORMAT_RGBx:
        return pixel_format_t::rgbx;
      case SPA_VIDEO_FORMAT_ARGB:
        return pixel_format_t::argb;
      case SPA_VIDEO_FORMAT_ABGR:
        return pixel_format_t::abgr;
      default:
        return pixel_format_t::unknown;
    }
  }

  /**
   * @brief Size of a cursor metadata block holding a `w` x `h` RGBA bitmap.
   * @param w Bitmap width.
   * @param h Bitmap height.
   * @return Bytes.
   */
  [[nodiscard]] inline constexpr int meta_size(const int w, const int h) noexcept {
    return static_cast<int>(sizeof(spa_meta_cursor) + sizeof(spa_meta_bitmap)) + w * h * 4;
  }

  /**
   * @brief Whether the memory path would be used for a stream, mirroring
   *        `pipewire_t::ensure_stream()`'s `use_dmabuf` rule.
   *
   * Duplicated on purpose, because the pointer mode must be chosen before the stream exists.
   * If the two ever drift, `on_format()` notices a DMA-BUF stream in metadata mode, gives up on
   * metadata mode and restarts the capture, so the failure mode is one reinit, not a stream
   * without a cursor.
   *
   * @param mem_type Encoder memory type.
   * @param n_dmabuf_infos DMA-BUF formats the EGL display supports.
   * @param display_is_nvidia Whether the display GPU is NVIDIA.
   * @return True when frames will arrive in memory buffers.
   */
  [[nodiscard]] inline bool memory_path(const platf::mem_type_e mem_type, const int n_dmabuf_infos, const bool display_is_nvidia) noexcept {
    const bool use_dmabuf = n_dmabuf_infos > 0 && (mem_type == platf::mem_type_e::vaapi || mem_type == platf::mem_type_e::vulkan || (mem_type == platf::mem_type_e::cuda && display_is_nvidia));
    return !use_dmabuf;
  }

  /**
   * @brief Header/damage metadata of a data frame, recorded when it was copied.
   */
  struct frame_meta_t {
    std::optional<std::uint64_t> pts;  ///< `spa_meta_header::pts`.
    std::optional<std::uint64_t> seq;  ///< `spa_meta_header::seq`.
    bool damaged = false;  ///< Whether `SPA_META_VideoDamage` reported a region.
  };

  /**
   * @brief Per-stream cursor state, a member of the PipeWire stream data.
   *
   * Written on the PipeWire thread and read on the capture thread, always under the stream's
   * `frame_mutex`.
   */
  struct stream_t {
    stream_t() = default;
    stream_t(const stream_t &) = delete;
    stream_t &operator=(const stream_t &) = delete;

    ~stream_t() {
      if (enabled) {
        deactivate();
      }
    }

    bool enabled = false;  ///< The stream was opened in metadata pointer mode.
    pixel_format_t frame_format = pixel_format_t::unknown;  ///< Negotiated frame byte order.
    image_t image;  ///< Current cursor image.
    bool visible = false;  ///< Whether a cursor is shown.
    int x = 0;  ///< Hotspot x, frame pixels.
    int y = 0;  ///< Hotspot y, frame pixels.
    save_under_t save;  ///< What the cursor covers in the front buffer.
    bool front_has_cursor = false;  ///< Whether the front buffer currently has a cursor drawn in it.
    bool have_frame = false;  ///< Whether a data frame has been copied yet.
    frame_meta_t front_meta;  ///< Metadata of the frame in the front buffer.
    std::uint64_t refreshes = 0;  ///< Cursor-only updates, counted so each yields a distinct image.
    std::chrono::steady_clock::time_point refresh_stamp {};  ///< Timestamp of the last cursor-only refresh handed out; no later frame is stamped earlier.
  };

  /**
   * @brief Add the `SPA_META_Cursor` request to a buffer-parameter list.
   *
   * @param builder Pod builder with room for one more object.
   * @return The parameter pod.
   */
  [[nodiscard]] inline const spa_pod *meta_param(spa_pod_builder *builder) {
    return static_cast<const spa_pod *>(spa_pod_builder_add_object(
      builder,
      SPA_TYPE_OBJECT_ParamMeta,
      SPA_PARAM_Meta,
      SPA_PARAM_META_type,
      SPA_POD_Id(SPA_META_Cursor),
      SPA_PARAM_META_size,
      SPA_POD_CHOICE_RANGE_Int(meta_size(default_bitmap_extent, default_bitmap_extent), meta_size(1, 1), meta_size(max_bitmap_extent, max_bitmap_extent))
    ));
  }

  /**
   * @brief Check a freshly negotiated format; give up on metadata mode if it cannot work.
   *
   * @param stream Cursor state.
   * @param spa_format Negotiated `enum spa_video_format`.
   * @param dmabuf Whether the stream negotiated DMA-BUF buffers.
   * @return False when metadata mode was refused and the capture must restart (embedded).
   */
  inline bool on_format(stream_t &stream, const std::uint32_t spa_format, const bool dmabuf) {
    if (!stream.enabled) {
      return true;
    }
    stream.frame_format = from_spa(spa_format);
    stream.have_frame = false;
    stream.front_has_cursor = false;
    if (dmabuf || !blendable_frame_format(stream.frame_format)) {
      BOOST_LOG(warning) << "[meow cursor] the compositor negotiated "sv << (dmabuf ? "DMA-BUF frames"sv : "a frame format the cursor cannot be drawn into"sv)
                         << " (spa format "sv << spa_format << "); restarting capture with the cursor embedded by the compositor, without position reports"sv;
      refuse_metadata();
      stream.enabled = false;
      deactivate();
      return false;
    }
    return true;
  }

  /**
   * @brief Read the cursor metadata of a buffer into the stream state.
   *
   * @param stream Cursor state (caller holds the frame mutex).
   * @param buffer The PipeWire buffer.
   * @return True when the cursor changed (moved, changed shape or visibility).
   */
  inline bool read_cursor(stream_t &stream, spa_buffer *buffer) {
    const auto *meta = spa_buffer_find_meta(buffer, SPA_META_Cursor);
    if (!meta || !meta->data) {
      return false;
    }
    const auto parsed = parse_meta(static_cast<const std::uint8_t *>(meta->data), meta->size, from_spa);
    if (!parsed) {
      return false;
    }
    bool changed = parsed->visible != stream.visible;
    stream.visible = parsed->visible;
    if (parsed->visible) {
      changed = changed || parsed->x != stream.x || parsed->y != stream.y;
      stream.x = parsed->x;
      stream.y = parsed->y;
      if (parsed->bitmap) {
        // KWin writes premultiplied RGBA (QImage::Format_RGBA8888_Premultiplied).
        import_bitmap(*parsed->bitmap, parsed->hotspot_x, parsed->hotspot_y, true, stream.image);
        changed = true;
      }
    }
    publish(stream.x, stream.y, stream.visible && !stream.image.empty());
    return changed;
  }

  /**
   * @brief The metadata-mode memory path of `on_process()`.
   *
   * A data frame is copied into the back staging buffer and swapped in, with its metadata
   * recorded now (the buffer goes back to PipeWire immediately). A cursor-only buffer
   * (`SPA_CHUNK_FLAG_CORRUPTED`, or empty) only updates the cursor. Either way the capture
   * thread is woken when there is something new to show.
   *
   * @tparam StreamData `pipewire::stream_data_t`.
   * @tparam Requeue Callable `void(pw_buffer *)` returning a buffer to PipeWire.
   * @param d Stream data.
   * @param b The newest dequeued buffer; always handed to `requeue` before returning.
   * @param requeue Returns the buffer to PipeWire (`pw_stream_queue_buffer`).
   */
  template<class StreamData, class Requeue>
  void process_memory(StreamData *d, pw_buffer *b, Requeue &&requeue) {
    auto *buffer = b->buffer;
    const auto &data = buffer->datas[0];
    const bool cursor_only = !data.data || !data.chunk || data.chunk->size == 0 || (data.chunk->flags & SPA_CHUNK_FLAG_CORRUPTED);

    frame_meta_t frame_meta;
    if (!cursor_only) {
      const std::size_t size = data.chunk->size;
      if (d->back_buffer->size() < size) {
        d->back_buffer->resize(size);
      }
      std::memcpy(d->back_buffer->data(), data.data, size);
      if (const auto *h = static_cast<const spa_meta_header *>(spa_buffer_find_meta_data(buffer, SPA_META_Header, sizeof(spa_meta_header)))) {
        // Same rule as upstream's fill_img_metadata(): a pts of 0 (or less) is not a usable
        // timestamp, so the frame carries none rather than a stale or bogus one.
        if (h->pts > 0) {
          frame_meta.pts = static_cast<std::uint64_t>(h->pts);
        }
        frame_meta.seq = h->seq;
      }
      const auto *damage = static_cast<const spa_meta_region *>(spa_buffer_find_meta_data(buffer, SPA_META_VideoDamage, sizeof(spa_meta_region)));
      frame_meta.damaged = damage && damage->region.size.width > 0 && damage->region.size.height > 0;
    }

    bool wake = false;
    {
      std::scoped_lock lock(d->frame_mutex);
      auto &stream = d->meow_cursor;
      const bool cursor_changed = read_cursor(stream, buffer);
      if (!cursor_only) {
        std::swap(d->front_buffer, d->back_buffer);
        d->local_stride = data.chunk->stride;
        stream.front_meta = frame_meta;
        stream.refreshes = 0;  // The fresh frame supersedes any refresh not handed out yet, and keeps its own pts.
        stream.front_has_cursor = false;  // A fresh frame from the compositor has no cursor in it.
        stream.have_frame = true;
        wake = true;
      } else if (cursor_changed && stream.have_frame) {
        ++stream.refreshes;
        wake = true;
      }
      if (wake) {
        d->frame_ready = true;
      }
    }

    requeue(b);
    if (wake) {
      d->frame_cv.notify_one();
    }
  }

  /**
   * @brief The capture timestamp of a handed-out frame, chosen exactly as upstream's
   *        `fill_img_metadata()` chooses it for the paths this file replaces.
   *
   * PipeWire's pts is `CLOCK_MONOTONIC` nanoseconds, the clock `std::chrono::steady_clock`
   * reads on Linux, so a trusted pts converts directly. Whether it is trusted is upstream's
   * compositor whitelist (`pipewire_t::prefer_pipewire_pts`); a frame without a usable pts —
   * including a cursor-only refresh, whose image is new *now* — is stamped with `now`.
   *
   * @param pts The frame's PipeWire pts, if it carried a usable one.
   * @param prefer_pipewire_pts Whether upstream trusts PipeWire pts for this session.
   * @param now The current time.
   * @return The timestamp to hand to the encoder.
   */
  [[nodiscard]] inline std::chrono::steady_clock::time_point frame_timestamp(const std::optional<std::uint64_t> &pts, const bool prefer_pipewire_pts, const std::chrono::steady_clock::time_point now) noexcept {
    if (pts.has_value() && prefer_pipewire_pts) {
      return std::chrono::steady_clock::time_point(std::chrono::nanoseconds(*pts));
    }
    return now;
  }

  /**
   * @brief The metadata-mode memory path of `fill_img()`: hand out the front buffer with the
   *        cursor drawn in.
   *
   * Runs on the capture thread with the frame mutex held. Restores what the previous cursor
   * covered in this same frame before drawing at the new position, so an idle desktop with a
   * moving pointer shows one cursor, not a trail.
   *
   * @tparam Image `pipewire::img_descriptor_t`.
   * @tparam StreamData `pipewire::stream_data_t`.
   * @param d Stream data.
   * @param img Image to fill.
   * @param prefer_pipewire_pts Upstream's `pipewire_t::prefer_pipewire_pts` for this session.
   */
  template<class Image, class StreamData>
  void fill_memory_img(StreamData &d, Image &img, const bool prefer_pipewire_pts = false) {
    auto &stream = d.meow_cursor;
    if (!stream.have_frame || d.front_buffer->empty()) {
      img.data = nullptr;
      return;
    }
    auto *frame = d.front_buffer->data();
    const auto stride = static_cast<int>(d.local_stride);

    if (stream.front_has_cursor) {
      restore(frame, stride, stream.save);
      stream.front_has_cursor = false;
    }
    if (stream.visible && stride >= img.width * 4 && d.front_buffer->size() >= static_cast<std::size_t>(stride) * static_cast<std::size_t>(img.height)) {
      stream.front_has_cursor = blend(frame, img.width, img.height, stride, stream.frame_format, stream.image, stream.x, stream.y, &stream.save);
    }

    img.data = frame;
    img.data_owned = false;
    img.row_pitch = stride;
    img.pixel_pitch = 4;
    // Metadata recorded when the frame was copied, never read from a buffer PipeWire owns
    // again. A cursor-only refresh has no header of its own, so it carries none, which keeps
    // the duplicate filter from discarding it.
    const bool refresh = stream.refreshes != 0;
    img.pts = refresh ? std::nullopt : stream.front_meta.pts;
    // A refresh is stamped now(), while a frame composed before that refresh was handed out
    // can carry an earlier compositor pts. Never let the stream's timestamps go backwards:
    // the RTP timestamp is derived from them and the client paces by it.
    const auto stamp = std::max(frame_timestamp(img.pts, prefer_pipewire_pts, std::chrono::steady_clock::now()), stream.refresh_stamp);
    if (refresh) {
      stream.refresh_stamp = stamp;
    }
    img.frame_timestamp = stamp;
    img.seq = stream.front_meta.seq;
    img.pw_flags = 0;
    img.pw_damage = stream.front_meta.damaged ? std::optional<bool>(true) : std::nullopt;
    stream.refreshes = 0;
  }

}  // namespace meow::cursor::pipewire
