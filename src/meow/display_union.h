/**
 * @file src/meow/display_union.h
 * @brief Pure geometry for "capture every output as one region" (unified desktop capture).
 *
 * KWin's `zkde_screencast_unstable_v1::stream_region` (protocol version >= 3) hands back a
 * single PipeWire stream containing an arbitrary rectangle of the *logical* workspace,
 * composited by KWin itself. That is what makes multi-GPU multi-monitor capture tractable:
 * we never composite across GPUs, the compositor does.
 *
 * Everything in this header is a pure function over output descriptions so it can be unit
 * tested without a compositor, a GPU, or a Wayland connection (see CLAUDE.md §5.5).
 *
 * Coordinate systems — the easiest thing to get wrong here:
 *   - `stream_region` takes **logical** coordinates (what the user arranges in System Settings).
 *   - `wl_output::mode` reports **device pixels** of the current mode.
 *   - `xdg_output::logical_size` reports the logical size directly, and is the only
 *     source that stays correct under fractional scaling.
 *   - The per-output scale is therefore derived as `pixel_width / logical_width`, which
 *     yields 1.0 at 100%, 1.5 at 150%, and so on — no integer-scale assumption anywhere.
 */
#pragma once

// standard includes
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace meow::display_union {

  /**
   * @brief Reserved value for the `output_name` setting that selects unified desktop capture.
   *
   * Chosen over a brand-new config key because Sunshine already routes `output_name` through
   * `platf::display_names()` -> `refresh_displays()` -> `platf::display()`, which is exactly
   * the plumbing a pseudo-display needs. A new key would have required editing `config.cpp`,
   * `config.h` and the web UI (all upstream files) for no behavioural gain.
   *
   * A real output can never be called this — DRM connector names are of the form
   * `eDP-1` / `HDMI-A-1` / `DP-2` — but callers must still resolve real outputs first so a
   * hypothetical collision can never shadow real hardware.
   */
  inline constexpr std::string_view union_output_name = "all";

  /**
   * @brief Largest frame the PipeWire capture path advertises as acceptable.
   *
   * Mirrors the `SPA_RECTANGLE(8192, 4096)` upper bound in
   * `src/platform/linux/pipewire.cpp::build_format_parameter`. Kept here so a union that
   * cannot possibly be negotiated is rejected loudly instead of silently renegotiated to
   * some other size.
   */
  inline constexpr std::int32_t max_pixel_width = 8192;
  inline constexpr std::int32_t max_pixel_height = 4096;  ///< @see max_pixel_width

  /**
   * @brief One output as the compositor describes it.
   */
  struct output_geometry_t {
    std::string name;  ///< Connector name, e.g. `eDP-2`.
    std::int32_t logical_x = 0;  ///< Logical left edge in the workspace.
    std::int32_t logical_y = 0;  ///< Logical top edge in the workspace.
    std::int32_t logical_width = 0;  ///< Logical width (xdg_output); 0 when unknown.
    std::int32_t logical_height = 0;  ///< Logical height (xdg_output); 0 when unknown.
    std::int32_t pixel_width = 0;  ///< Current mode width in device pixels (wl_output).
    std::int32_t pixel_height = 0;  ///< Current mode height in device pixels (wl_output).
    std::int32_t wl_scale = 1;  ///< Integer `wl_output::scale`; only used as a fallback.
    std::int32_t refresh_mhz = 0;  ///< Current mode refresh rate in mHz (wl_output).
    bool enabled = true;  ///< False for outputs that must not contribute.
  };

  /**
   * @brief The rectangle to hand to `stream_region`, plus what it will produce.
   */
  struct union_region_t {
    std::int32_t x = 0;  ///< Logical left of the union.
    std::int32_t y = 0;  ///< Logical top of the union.
    std::int32_t width = 0;  ///< Logical width of the union.
    std::int32_t height = 0;  ///< Logical height of the union.
    double scale = 1.0;  ///< Capture scale to request; the highest scale among contributors.
    std::int32_t pixel_width = 0;  ///< Expected captured width in device pixels.
    std::int32_t pixel_height = 0;  ///< Expected captured height in device pixels.
    std::int32_t refresh_mhz = 0;  ///< Highest refresh among contributors, in mHz.
    std::size_t contributing_outputs = 0;  ///< How many outputs were folded in.
    bool covers_whole_region = false;  ///< False when the arrangement leaves dead space.
    bool exceeds_capture_limits = false;  ///< True when pixel size exceeds the PipeWire bound.
    bool valid = false;  ///< False when no usable output was supplied.
  };

  /**
   * @brief Case-insensitive match against the reserved output name.
   * @param name Configured output name.
   * @return True when unified desktop capture was requested.
   */
  [[nodiscard]] inline bool is_union_output_name(const std::string_view name) {
    if (name.size() != union_output_name.size()) {
      return false;
    }
    return std::equal(name.begin(), name.end(), union_output_name.begin(), [](const char a, const char b) {
      return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b));
    });
  }

  /**
   * @brief Logical width of an output, falling back to mode/scale when xdg_output is absent.
   * @param output Output description.
   * @return Logical width, or 0 when it cannot be determined.
   */
  [[nodiscard]] inline std::int32_t effective_logical_width(const output_geometry_t &output) {
    if (output.logical_width > 0) {
      return output.logical_width;
    }
    const auto scale = std::max<std::int32_t>(1, output.wl_scale);
    return output.pixel_width > 0 ? output.pixel_width / scale : 0;
  }

  /**
   * @brief Logical height of an output, falling back to mode/scale when xdg_output is absent.
   * @param output Output description.
   * @return Logical height, or 0 when it cannot be determined.
   */
  [[nodiscard]] inline std::int32_t effective_logical_height(const output_geometry_t &output) {
    if (output.logical_height > 0) {
      return output.logical_height;
    }
    const auto scale = std::max<std::int32_t>(1, output.wl_scale);
    return output.pixel_height > 0 ? output.pixel_height / scale : 0;
  }

  /**
   * @brief Scale factor of an output, derived from device pixels over logical size.
   * @param output Output description.
   * @return Scale factor; 1.0 when it cannot be determined.
   */
  [[nodiscard]] inline double effective_scale(const output_geometry_t &output) {
    const auto logical_w = effective_logical_width(output);
    const auto logical_h = effective_logical_height(output);
    double scale = 0.0;
    if (logical_w > 0 && output.pixel_width > 0) {
      scale = static_cast<double>(output.pixel_width) / static_cast<double>(logical_w);
    }
    if (logical_h > 0 && output.pixel_height > 0) {
      scale = std::max(scale, static_cast<double>(output.pixel_height) / static_cast<double>(logical_h));
    }
    if (scale <= 0.0) {
      scale = static_cast<double>(std::max<std::int32_t>(1, output.wl_scale));
    }
    return scale;
  }

  /**
   * @brief Whether an output can contribute to the union.
   * @param output Output description.
   * @return True when the output is enabled and has a usable logical size.
   */
  [[nodiscard]] inline bool contributes(const output_geometry_t &output) {
    return output.enabled && effective_logical_width(output) > 0 && effective_logical_height(output) > 0;
  }

  /**
   * @brief Compute the bounding box of every contributing output, in logical coordinates.
   *
   * Design decisions baked in here, all of which are observable in the returned struct:
   *
   * - **Scale**: we request the *highest* scale among the contributing outputs so the
   *   sharpest monitor is captured at its native pixel density and the others are upscaled.
   *   Downscaling the sharp one instead would throw away detail that cannot be recovered.
   *   (KWin >= protocol v5 does the same when handed a scale of 0.0; we compute it
   *   explicitly so v3 and v4 behave identically.)
   * - **Dead space**: a non-rectangular arrangement leaves parts of the bounding box
   *   uncovered. KWin renders those as black. `covers_whole_region` reports whether that
   *   happened so the caller can warn rather than leaving the user guessing.
   * - **Refresh**: a region spanning outputs has no single refresh rate. We report the
   *   highest, which is the ceiling KWin can deliver; the PipeWire stream itself is
   *   negotiated at variable rate, so actual pacing is driven by damage, not by this value.
   *
   * @param outputs Every output the compositor advertised.
   * @return The union region; `valid` is false when nothing usable was supplied.
   */
  [[nodiscard]] inline union_region_t compute_union(const std::vector<output_geometry_t> &outputs) {
    union_region_t region;

    std::int32_t min_x = 0;
    std::int32_t min_y = 0;
    std::int32_t max_x = 0;
    std::int32_t max_y = 0;
    std::int64_t covered_area = 0;
    bool first = true;

    for (const auto &output : outputs) {
      if (!contributes(output)) {
        continue;
      }
      const auto logical_w = effective_logical_width(output);
      const auto logical_h = effective_logical_height(output);
      const auto left = output.logical_x;
      const auto top = output.logical_y;
      const auto right = left + logical_w;
      const auto bottom = top + logical_h;

      if (first) {
        min_x = left;
        min_y = top;
        max_x = right;
        max_y = bottom;
        region.scale = effective_scale(output);
        first = false;
      } else {
        min_x = std::min(min_x, left);
        min_y = std::min(min_y, top);
        max_x = std::max(max_x, right);
        max_y = std::max(max_y, bottom);
        region.scale = std::max(region.scale, effective_scale(output));
      }

      covered_area += static_cast<std::int64_t>(logical_w) * static_cast<std::int64_t>(logical_h);
      region.refresh_mhz = std::max(region.refresh_mhz, output.refresh_mhz);
      ++region.contributing_outputs;
    }

    if (region.contributing_outputs == 0) {
      return region;
    }

    region.x = min_x;
    region.y = min_y;
    region.width = max_x - min_x;
    region.height = max_y - min_y;
    region.valid = region.width > 0 && region.height > 0;
    if (!region.valid) {
      return region;
    }

    // Overlapping (mirrored) outputs make covered_area exceed the bounding box; that is not
    // dead space, so compare with >= rather than ==.
    const auto bounding_area = static_cast<std::int64_t>(region.width) * static_cast<std::int64_t>(region.height);
    region.covers_whole_region = covered_area >= bounding_area;

    region.pixel_width = static_cast<std::int32_t>(std::lround(static_cast<double>(region.width) * region.scale));
    region.pixel_height = static_cast<std::int32_t>(std::lround(static_cast<double>(region.height) * region.scale));
    region.exceeds_capture_limits = region.pixel_width > max_pixel_width || region.pixel_height > max_pixel_height;

    return region;
  }

}  // namespace meow::display_union
