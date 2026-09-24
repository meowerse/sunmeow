/**
 * @file src/meow/cursor_runtime.h
 * @brief Cross-thread state for host cursor reporting.
 *
 * Exactly two parties touch this state:
 *
 *  - the **PipeWire thread** publishes the cursor position whenever a buffer carries
 *    `SPA_META_Cursor` (`src/meow/cursor_pipewire.h`);
 *  - the **control thread** reads it for every subscribed session and sends `0x3004`
 *    POSITION messages (`src/meow/control_stream.h`).
 *
 * That is a single-writer hand-off of a few integers, so it is one 64-bit atomic word
 * rather than a lock: x and y in 16 bits each, a visible bit, an active bit and a 30-bit
 * change counter. The writer never blocks on the network and the reader never blocks on
 * capture.
 */
#pragma once

// standard includes
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <optional>

// local includes
#include "src/config.h"
#include "src/meow/cursor.h"

namespace meow::cursor {

  namespace detail {

    /**
     * @brief The published cursor: `x | y << 16 | visible << 32 | active << 33 | generation << 34`.
     */
    inline std::atomic<std::uint64_t> published {0};

    /**
     * @brief Number of sessions currently subscribed to POSITION messages.
     */
    inline std::atomic<int> subscribers {0};

    /**
     * @brief Set once blending turned out to be impossible for this host; sticky until restart.
     *
     * When the compositor negotiates a frame format the blend cannot write (10-bit, or DMA-BUF),
     * the stream would otherwise show no cursor at all. The capture is restarted instead, and
     * every later stream of this process asks for the cursor embedded in the pixels, with no
     * positions. Sticky on purpose: the negotiated format depends on the compositor and the
     * display mode, and flapping between modes would restart the capture every session.
     */
    inline std::atomic<bool> metadata_refused {false};

    /**
     * @brief Bit of `published` meaning "the cursor is visible".
     */
    inline constexpr std::uint64_t visible_bit = std::uint64_t {1} << 32;

    /**
     * @brief Bit of `published` meaning "a metadata stream is feeding positions".
     */
    inline constexpr std::uint64_t active_bit = std::uint64_t {1} << 33;

    /**
     * @brief Shift of the change counter in `published`.
     */
    inline constexpr int generation_shift = 34;

  }  // namespace detail

  /**
   * @brief Whether cursor reporting is enabled in the host configuration.
   *
   * Reads `config::video.cursor_reporting`, filled from `meow_cursor_reporting` (default on).
   * With it off the compositor is asked for an embedded cursor exactly as before, and no
   * position is ever sent.
   *
   * @return True when enabled.
   */
  [[nodiscard]] inline bool reporting_enabled() {
    return config::video.cursor_reporting;
  }

  /**
   * @brief Whether a new capture stream should ask the compositor for cursor metadata.
   *
   * Only a path whose frames arrive in system memory can have the cursor blended back in
   * (`src/meow/cursor_pipewire.h`); DMA-BUF frames never reach the CPU, so a DMA-BUF path keeps
   * the embedded cursor and reports no positions.
   *
   * @param memory_path Whether the stream will deliver memory buffers.
   * @return True to request `metadata` pointer mode.
   */
  [[nodiscard]] inline bool metadata_mode_wanted(const bool memory_path) {
    return memory_path && reporting_enabled() && !detail::metadata_refused.load(std::memory_order_relaxed);
  }

  /**
   * @brief Give up on metadata mode for the rest of this process.
   */
  inline void refuse_metadata() noexcept {
    detail::metadata_refused.store(true, std::memory_order_relaxed);
  }

  /**
   * @brief Publish a cursor state. Called by the single writer (the PipeWire thread).
   *
   * @param x Hotspot x in captured pixels.
   * @param y Hotspot y in captured pixels.
   * @param visible Whether the cursor is shown.
   */
  inline void publish(const int x, const int y, const bool visible) noexcept {
    const auto previous = detail::published.load(std::memory_order_relaxed);
    const auto cx = static_cast<std::uint64_t>(std::clamp(x, 0, 0xFFFF));
    const auto cy = static_cast<std::uint64_t>(std::clamp(y, 0, 0xFFFF));
    const auto next_state = cx | (cy << 16) | (visible ? detail::visible_bit : 0) | detail::active_bit;
    const auto previous_state = previous & ((std::uint64_t {1} << detail::generation_shift) - 1);
    if (previous_state == next_state) {
      return;  // Nothing moved; do not wake the control thread for it.
    }
    const auto generation = ((previous >> detail::generation_shift) + 1) & ((std::uint64_t {1} << (64 - detail::generation_shift)) - 1);
    detail::published.store(next_state | (generation << detail::generation_shift), std::memory_order_release);
  }

  /**
   * @brief Mark the position source as gone (the metadata stream ended).
   */
  inline void deactivate() noexcept {
    detail::published.store(detail::published.load(std::memory_order_relaxed) & ~detail::active_bit, std::memory_order_release);
  }

  /**
   * @brief The latest published state.
   * @return The state, or `std::nullopt` when no metadata stream is feeding positions.
   */
  [[nodiscard]] inline std::optional<state_t> current() noexcept {
    const auto word = detail::published.load(std::memory_order_acquire);
    if (!(word & detail::active_bit)) {
      return std::nullopt;
    }
    return state_t {static_cast<int>(word & 0xFFFF), static_cast<int>((word >> 16) & 0xFFFF), (word & detail::visible_bit) != 0, static_cast<std::uint32_t>(word >> detail::generation_shift)};
  }

  /**
   * @brief Whether any session is subscribed and a position source is active.
   * @return True when the control thread should poll for positions at 60 Hz.
   */
  [[nodiscard]] inline bool polling_wanted() noexcept {
    return detail::subscribers.load(std::memory_order_relaxed) > 0 && (detail::published.load(std::memory_order_relaxed) & detail::active_bit) != 0;
  }

}  // namespace meow::cursor
