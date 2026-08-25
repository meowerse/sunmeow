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
 *   - `wl_output::mode` is reported **before** the output transform is applied, while
 *     `xdg_output::logical_size` is reported **after** it. On a 90/270-rotated output the
 *     two therefore describe swapped axes, and dividing one by the other without undoing
 *     the rotation produces a nonsense scale (1.78 instead of 1.0 for a rotated 1080p
 *     panel). `oriented_pixel_width()`/`oriented_pixel_height()` undo it exactly once, at
 *     the boundary, so every ratio downstream compares like with like.
 */
#pragma once

// standard includes
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <ranges>
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
   * @brief Output transform, mirroring `enum wl_output_transform`.
   *
   * Duplicated rather than included so this header stays free of any Wayland dependency and
   * can be unit tested on a machine with no compositor (CLAUDE.md §5.5). The values are
   * pinned to the protocol's by `static_assert`s at the one place that converts between the
   * two (`src/platform/linux/kwingrab.cpp`), so a drift is a compile error, not a bug.
   */
  enum class output_transform_t : std::int32_t {
    normal = 0,  ///< `WL_OUTPUT_TRANSFORM_NORMAL`
    rotate_90 = 1,  ///< `WL_OUTPUT_TRANSFORM_90`
    rotate_180 = 2,  ///< `WL_OUTPUT_TRANSFORM_180`
    rotate_270 = 3,  ///< `WL_OUTPUT_TRANSFORM_270`
    flipped = 4,  ///< `WL_OUTPUT_TRANSFORM_FLIPPED`
    flipped_90 = 5,  ///< `WL_OUTPUT_TRANSFORM_FLIPPED_90`
    flipped_180 = 6,  ///< `WL_OUTPUT_TRANSFORM_FLIPPED_180`
    flipped_270 = 7,  ///< `WL_OUTPUT_TRANSFORM_FLIPPED_270`
  };

  /**
   * @brief Whether a transform swaps the output's width and height axes.
   *
   * The four quarter-turn transforms do; `normal`, `180` and the two unrotated flips do not.
   *
   * @param transform Output transform.
   * @return True when logical width corresponds to the mode's height and vice versa.
   */
  [[nodiscard]] inline bool swaps_axes(const output_transform_t transform) {
    switch (transform) {
      case output_transform_t::rotate_90:
      case output_transform_t::rotate_270:
      case output_transform_t::flipped_90:
      case output_transform_t::flipped_270:
        return true;
      case output_transform_t::normal:
      case output_transform_t::rotate_180:
      case output_transform_t::flipped:
      case output_transform_t::flipped_180:
        return false;
    }
    // An unknown transform is treated as unrotated: guessing a rotation we do not understand
    // would be worse than reporting the mode as-is.
    return false;
  }

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
    /**
     * @brief Output transform (`wl_output::geometry`).
     *
     * `pixel_width`/`pixel_height` are the *pre-transform* mode, so this is required to
     * relate them to the *post-transform* logical size. @see oriented_pixel_width.
     */
    output_transform_t transform = output_transform_t::normal;
    std::int32_t wl_scale = 1;  ///< Integer `wl_output::scale`; only used as a fallback.
    std::int32_t refresh_mhz = 0;  ///< Current mode refresh rate in mHz (wl_output).
    bool enabled = true;  ///< False for outputs that must not contribute.
  };

  /**
   * @brief Everything a compositor told us about one output, before any policy is applied.
   *
   * This is deliberately a *record of events received*, not a merged view: `wl_output` and
   * `xdg_output` are separate protocols that report overlapping facts, and the whole point of
   * keeping them apart is that single-output capture must go on using exactly the values it
   * used before `xdg_output` was bound at all. @see describe_output, single_output_pos_x.
   */
  struct output_report_t {
    std::string name;  ///< Connector name from `wl_output::name`.
    std::int32_t wl_x = 0;  ///< X from `wl_output::geometry`.
    std::int32_t wl_y = 0;  ///< Y from `wl_output::geometry`.
    std::int32_t mode_width = 0;  ///< Width from `wl_output::mode` (pre-transform pixels).
    std::int32_t mode_height = 0;  ///< Height from `wl_output::mode` (pre-transform pixels).
    output_transform_t transform = output_transform_t::normal;  ///< From `wl_output::geometry`.
    std::int32_t wl_scale = 1;  ///< From `wl_output::scale`.
    std::int32_t refresh_mhz = 0;  ///< Refresh from `wl_output::mode`, in mHz.
    bool has_xdg_logical_position = false;  ///< True once `xdg_output::logical_position` arrived.
    std::int32_t xdg_logical_x = 0;  ///< X from `xdg_output::logical_position`.
    std::int32_t xdg_logical_y = 0;  ///< Y from `xdg_output::logical_position`.
    bool has_xdg_logical_size = false;  ///< True once `xdg_output::logical_size` arrived.
    std::int32_t xdg_logical_width = 0;  ///< Width from `xdg_output::logical_size`.
    std::int32_t xdg_logical_height = 0;  ///< Height from `xdg_output::logical_size`.
  };

  /**
   * @brief Position a **single-output** capture reports for an output.
   *
   * Always `wl_output::geometry`, never `xdg_output`, even when both are present. These
   * values become `display_t::offset_x`/`offset_y`, which `pipewire_display_t` matches for
   * *equality* against `wl::monitors()` — a different source disagreeing by even one pixel
   * makes that match fail, silently losing the display's logical size and mismapping
   * absolute pointer input on a scaled desktop. Binding `xdg_output` for the union must not
   * be observable here, so the two sources are never mixed.
   *
   * @param report Raw compositor report.
   * @return X offset for single-output capture.
   */
  [[nodiscard]] inline std::int32_t single_output_pos_x(const output_report_t &report) {
    return report.wl_x;
  }

  /**
   * @brief Y position a single-output capture reports for an output.
   * @param report Raw compositor report.
   * @return Y offset for single-output capture.
   * @see single_output_pos_x
   */
  [[nodiscard]] inline std::int32_t single_output_pos_y(const output_report_t &report) {
    return report.wl_y;
  }

  /**
   * @brief Fold a raw compositor report into the geometry the union math consumes.
   *
   * Here — and only here — `xdg_output` wins when it is present, because `stream_region`
   * speaks logical coordinates and `xdg_output` is the only source that stays correct under
   * fractional scaling. Absent it, `wl_output` is used and the scale falls back to the
   * integer `wl_output::scale`.
   *
   * @param report Raw compositor report.
   * @return Geometry for `compute_union()`.
   */
  [[nodiscard]] inline output_geometry_t describe_output(const output_report_t &report) {
    output_geometry_t geometry;
    geometry.name = report.name;
    geometry.logical_x = report.has_xdg_logical_position ? report.xdg_logical_x : report.wl_x;
    geometry.logical_y = report.has_xdg_logical_position ? report.xdg_logical_y : report.wl_y;
    geometry.logical_width = report.has_xdg_logical_size ? report.xdg_logical_width : 0;
    geometry.logical_height = report.has_xdg_logical_size ? report.xdg_logical_height : 0;
    geometry.pixel_width = report.mode_width;
    geometry.pixel_height = report.mode_height;
    geometry.transform = report.transform;
    geometry.wl_scale = report.wl_scale;
    geometry.refresh_mhz = report.refresh_mhz;
    // KWin only advertises a wl_output for outputs that are enabled, so anything reported
    // counts. The flag exists so the pure helpers stay exercisable from tests.
    geometry.enabled = true;
    return geometry;
  }

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
    bool covers_whole_region = false;  ///< False when the arrangement leaves uncovered dead space.
    bool has_negative_origin = false;  ///< True when the union starts left of / above the origin.
    bool exceeds_capture_limits = false;  ///< True when pixel size exceeds the PipeWire bound.
    bool valid = false;  ///< False when no usable output was supplied.
  };

  /**
   * @brief Lowest `zkde_screencast_unstable_v1` version that provides `stream_region`.
   */
  inline constexpr std::uint32_t min_stream_region_version = 3;

  /**
   * @brief Why unified capture was or was not used.
   */
  enum class union_status_t {
    ok,  ///< Region capture can be used.
    unsupported_protocol,  ///< Bound screencast version predates `stream_region`.
    no_usable_outputs,  ///< No output supplied a usable logical geometry.
    exceeds_capture_limits,  ///< The region is larger than the capture path can negotiate.
    negative_origin,  ///< The region starts left of / above (0,0), which mismaps pointer input.
  };

  /**
   * @brief The full decision: whether to use region capture, why, and with what geometry.
   */
  struct union_decision_t {
    union_status_t status = union_status_t::no_usable_outputs;  ///< Outcome of the decision.
    union_region_t region;  ///< The computed region; only meaningful when `status` is `ok`.

    /**
     * @brief Whether the caller should issue a `stream_region` request.
     * @return True when region capture is usable.
     */
    [[nodiscard]] bool use_region() const {
      return status == union_status_t::ok;
    }
  };

  /**
   * @brief Exact match against the reserved output name.
   *
   * Deliberately case-**sensitive**. Every other display name in Sunshine is matched
   * exactly — `video::refresh_displays()` selects the active display with
   * `display_names[x] == output_name` — so a case-insensitive reserved name would be a
   * promise only this function keeps: `output_name = All` would be recognised here, fail to
   * match the advertised `"all"` in `refresh_displays()`, and silently stream monitor #1
   * while encoder probing (which passes `output_name` through verbatim) took the union path.
   * Matching exactly removes that divergence rather than adding a second normalisation point.
   *
   * @param name Configured output name.
   * @return True when unified desktop capture was requested.
   */
  [[nodiscard]] inline bool is_union_output_name(const std::string_view name) {
    return name == union_output_name;
  }

  /**
   * @brief Mode width of an output, rotated into the logical coordinate space.
   *
   * `wl_output::mode` describes the panel's scanout mode, which is not rotated. A 1920x1080
   * panel mounted in portrait still reports 1920x1080 with a `rotate_90` transform, while
   * `xdg_output::logical_size` reports 1080x1920. Swapping here means every consumer can
   * compare pixels to logical units directly.
   *
   * @param output Output description.
   * @return Mode width expressed along the logical X axis.
   */
  [[nodiscard]] inline std::int32_t oriented_pixel_width(const output_geometry_t &output) {
    return swaps_axes(output.transform) ? output.pixel_height : output.pixel_width;
  }

  /**
   * @brief Mode height of an output, rotated into the logical coordinate space.
   * @param output Output description.
   * @return Mode height expressed along the logical Y axis.
   * @see oriented_pixel_width
   */
  [[nodiscard]] inline std::int32_t oriented_pixel_height(const output_geometry_t &output) {
    return swaps_axes(output.transform) ? output.pixel_width : output.pixel_height;
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
    const auto pixels = oriented_pixel_width(output);
    return pixels > 0 ? pixels / scale : 0;
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
    const auto pixels = oriented_pixel_height(output);
    return pixels > 0 ? pixels / scale : 0;
  }

  /**
   * @brief Scale factor of an output, derived from device pixels over logical size.
   *
   * Both dimensions are taken through `oriented_pixel_*` first. Without that, a rotated
   * output divides a pre-transform axis by a post-transform one — a rotated 1080p panel
   * yields `max(1920/1080, 1080/1920) = 1.78` instead of 1.0 — and because `compute_union()`
   * takes the *highest* scale across contributors, a single portrait monitor would inflate
   * the capture resolution of the entire desktop and could push it past the capture limit.
   *
   * @param output Output description.
   * @return Scale factor; 1.0 when it cannot be determined.
   */
  [[nodiscard]] inline double effective_scale(const output_geometry_t &output) {
    const auto logical_w = effective_logical_width(output);
    const auto logical_h = effective_logical_height(output);
    const auto pixel_w = oriented_pixel_width(output);
    const auto pixel_h = oriented_pixel_height(output);
    double scale = 0.0;
    if (logical_w > 0 && pixel_w > 0) {
      scale = static_cast<double>(pixel_w) / static_cast<double>(logical_w);
    }
    if (logical_h > 0 && pixel_h > 0) {
      scale = std::max(scale, static_cast<double>(pixel_h) / static_cast<double>(logical_h));
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
   * @brief Whether the contributing outputs cover every pixel of the given rectangle.
   *
   * Exact, not an area heuristic: summing output areas reports full coverage for a mirrored
   * pair beside a gap (two identical rectangles plus a detached third can sum to the bounding
   * area while leaving a hole). This compresses the distinct edge coordinates into a grid and
   * tests one representative point per cell, which is correct because every cell is, by
   * construction, either wholly inside or wholly outside each rectangle.
   *
   * @param outputs Every output the compositor advertised.
   * @param x Left edge of the rectangle to test.
   * @param y Top edge of the rectangle to test.
   * @param width Width of the rectangle to test.
   * @param height Height of the rectangle to test.
   * @return True when no part of the rectangle is left uncovered.
   */
  [[nodiscard]] inline bool region_fully_covered(const std::vector<output_geometry_t> &outputs, const std::int32_t x, const std::int32_t y, const std::int32_t width, const std::int32_t height) {
    struct rect_t {
      std::int32_t x0;
      std::int32_t y0;
      std::int32_t x1;
      std::int32_t y1;
    };

    std::vector<rect_t> rects;
    std::vector<std::int32_t> edges_x {x, x + width};
    std::vector<std::int32_t> edges_y {y, y + height};

    for (const auto &output : outputs) {
      if (!contributes(output)) {
        continue;
      }
      // Clip to the rectangle under test so outputs hanging outside it cannot mask a hole.
      const rect_t clipped {
        std::max(output.logical_x, x),
        std::max(output.logical_y, y),
        std::min(output.logical_x + effective_logical_width(output), x + width),
        std::min(output.logical_y + effective_logical_height(output), y + height)
      };
      if (clipped.x0 >= clipped.x1 || clipped.y0 >= clipped.y1) {
        continue;
      }
      edges_x.push_back(clipped.x0);
      edges_x.push_back(clipped.x1);
      edges_y.push_back(clipped.y0);
      edges_y.push_back(clipped.y1);
      rects.push_back(clipped);
    }

    if (rects.empty()) {
      return false;
    }

    std::ranges::sort(edges_x);
    edges_x.erase(std::ranges::unique(edges_x).begin(), edges_x.end());
    std::ranges::sort(edges_y);
    edges_y.erase(std::ranges::unique(edges_y).begin(), edges_y.end());

    for (std::size_t i = 0; i + 1 < edges_x.size(); ++i) {
      for (std::size_t j = 0; j + 1 < edges_y.size(); ++j) {
        const auto probe_x = edges_x[i];
        const auto probe_y = edges_y[j];
        const bool covered = std::ranges::any_of(rects, [probe_x, probe_y](const rect_t &r) {
          return r.x0 <= probe_x && probe_x < r.x1 && r.y0 <= probe_y && probe_y < r.y1;
        });
        if (!covered) {
          return false;
        }
      }
    }
    return true;
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
   * - **Negative origin**: KWin will happily place an output left of / above (0,0).
   *   `has_negative_origin` flags it because the shared PipeWire desktop-size calculation
   *   assumes origin-anchored outputs and mismaps absolute pointer input when it is not.
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

    region.covers_whole_region = region_fully_covered(outputs, region.x, region.y, region.width, region.height);
    region.has_negative_origin = region.x < 0 || region.y < 0;

    region.pixel_width = static_cast<std::int32_t>(std::lround(static_cast<double>(region.width) * region.scale));
    region.pixel_height = static_cast<std::int32_t>(std::lround(static_cast<double>(region.height) * region.scale));
    region.exceeds_capture_limits = region.pixel_width > max_pixel_width || region.pixel_height > max_pixel_height;

    return region;
  }

  /**
   * @brief Decide whether unified desktop capture can be used, and with what geometry.
   *
   * This is the whole policy — protocol-version gate, geometry, and the refusal to attempt a
   * region the capture path cannot negotiate — kept as a pure function so every branch is unit
   * testable. The caller is left with logging and the protocol request.
   *
   * Note that an oversized region is **refused, not clamped**. Silently shrinking it would
   * hand the user a stream that is neither the whole desktop nor the monitor they asked for.
   *
   * A region with a negative origin is refused for the same reason. `pipewire_display_t`
   * derives the desktop extent as `max(offset + size)` with no matching minimum, so an
   * arrangement extending left of / above (0,0) produces absolute pointer coordinates offset
   * by the negative origin: the picture looks perfect and the mouse lands somewhere else.
   * That is a worse outcome than falling back to a single output, because it is invisible in
   * the video and hard to attribute, whereas the fallback is a working stream. It is also
   * user-fixable — moving the arrangement so it starts at 0,0 makes the union available.
   *
   * @param outputs Every output the compositor advertised.
   * @param screencast_version Bound version of `zkde_screencast_unstable_v1`.
   * @return The decision; `use_region()` is true only when region capture should be issued.
   */
  [[nodiscard]] inline union_decision_t decide_union_capture(const std::vector<output_geometry_t> &outputs, const std::uint32_t screencast_version) {
    union_decision_t decision;

    if (screencast_version < min_stream_region_version) {
      decision.status = union_status_t::unsupported_protocol;
      return decision;
    }

    decision.region = compute_union(outputs);
    if (!decision.region.valid) {
      decision.status = union_status_t::no_usable_outputs;
      return decision;
    }
    if (decision.region.exceeds_capture_limits) {
      decision.status = union_status_t::exceeds_capture_limits;
      return decision;
    }
    if (decision.region.has_negative_origin) {
      decision.status = union_status_t::negative_origin;
      return decision;
    }

    decision.status = union_status_t::ok;
    return decision;
  }

  /**
   * @brief Warn when `output_name` requests the union on a backend that cannot provide it.
   *
   * Only the `kwin` capture backend implements unified desktop capture, and it is not
   * auto-selected on a default install: `verify_portal()` runs first and sets a source, after
   * which the `sources.none()` half of kwin's guard is false forever. A user who sets only
   * `output_name = all` therefore streams monitor #1, and nothing in the log says why.
   *
   * Returned as a string rather than logged here so this header stays free of Boost and
   * remains unit testable (CLAUDE.md §5.5); the caller does the logging.
   *
   * @param kwin_selected Whether the kwin capture source was selected.
   * @param output_name Configured `output_name`.
   * @return The warning to log, or an empty string when there is nothing to warn about.
   */
  [[nodiscard]] inline std::string union_backend_warning(const bool kwin_selected, const std::string_view output_name) {
    if (kwin_selected || !is_union_output_name(output_name)) {
      return {};
    }
    return std::string("output_name = '").append(union_output_name).append("' requests whole-desktop capture, which only the 'kwin' capture backend provides, but a different backend was selected. Set 'capture = kwin' in sunshine.conf; without it a single output will be streamed instead.");
  }

}  // namespace meow::display_union
