/**
 * @file src/meow/viewport_cuda.h
 * @brief Viewport-following geometry for the CUDA scaler (`cuda::sws_t`).
 *
 * `src/meow/viewport.h` decides *what* rectangle to stream; `src/meow/viewport_runtime.h`
 * carries it from the control thread to the encode thread. This header is the third piece:
 * turning a `plan_t` into the two numbers a GPU scaling kernel actually needs.
 *
 * ## Why the CUDA path needs its own adapter
 *
 * The software path applies a crop by moving the scaler's *plane pointers*
 * (`offset_source_planes()`), so swscale never learns that a crop happened — it just sees a
 * smaller input frame. A CUDA kernel cannot do that. It reads through a
 * `cudaTextureObject_t` bound to a whole captured frame and computes its own source
 * coordinate per destination pixel:
 *
 * ```cuda
 * float x = idX * scale;   // upstream: source origin is implicitly (0, 0)
 * float y = idY * scale;
 * ```
 *
 * So a crop has to be expressed *inside* that mapping, as a source origin plus a step.
 *
 * ## Two coordinate spaces, kept apart on purpose
 *
 * `cuda::viewport_t {width, height, offsetX, offsetY}` already exists and is a
 * **destination** rectangle: where the scaled image lands inside the encode surface, i.e.
 * the aspect-ratio letterbox. It says nothing about what is read.
 *
 * This header adds `cuda_source_t {originX, originY, stepX, stepY}`, which is purely
 * **source** space: the texel the first destination pixel samples, and how far the sample
 * point advances per destination pixel. The two are never merged, because a reader who
 * cannot tell which space a field is in will eventually add an offset to the wrong one.
 *
 * ## Why the step is per axis
 *
 * Upstream carries a single `float scale`, and for the uncropped frame that is correct:
 * `sws_t::sws_t()` picks `scalar = min(out_w / in_w, out_h / in_h)` and applies it to both
 * axes, so one reciprocal serves both. A crop keeps that property *in intent* —
 * `meow::viewport::plan()` uses the same `min()` — but not in arithmetic. `plan()`
 * even-aligns each scaled extent on its own (`floor_even`), because NV12 chroma is
 * subsampled 2x2 and an odd extent fringes the seam, and that rounding removes the same
 * **absolute** two pixels from each axis — so the shorter axis loses far more of itself.
 *
 * That is not a rounding curiosity at the shapes this feature exists for. A 5360x142 strip
 * (a spreadsheet row spanning both monitors) scales by 0.2388 into 1280x33.9; the width
 * rounds to 1280 and loses nothing, the height rounds to 32 and loses 5.8%. The two ratios
 * are 4.1875 and 4.4375. Reuse the width's step on the height and the bottom rows of the
 * strip are never read; reuse the height's step on the width and the last sample lands at
 * texel 5675 of a 5360-wide frame — outside the capture entirely, which
 * `cudaAddressModeClamp` renders as a smear of the last column rather than a fault. Both are
 * asserted in `StepIsPerAxisBecauseTheScaledExtentsRoundIndependently`.
 *
 * Per-axis steps also make the CUDA path sample the same *rectangle* as the software one by
 * construction: swscale is configured with an input of `source.width x source.height` and an
 * output of `out_width x out_height`, which *is* a per-axis ratio. One scale factor here
 * would mean the two paths read measurably different rectangles from the same plan.
 *
 * Sampling the same rectangle is not the same as producing identical pixels, and this header
 * does not claim it is. Upstream's kernel fetches at `x` rather than `x - 0.5`, so CUDA's
 * linear filtering carries a half-texel bias that swscale does not, and the two resamplers
 * are different filters besides. The rectangle is the contract; the pixels inside it are each
 * backend's own business, exactly as they were before this change.
 *
 * ## Bounds safety
 *
 * The rectangle originates from a paired but untrusted client. `sanitize()` has already
 * clamped it into the captured frame, but a kernel indexing outside its texture (or a grid
 * launched with a zero dimension) is a memory-safety bug rather than a visual one, so
 * `cuda_scaler_config()` re-derives every bound from the capture and surface sizes it is
 * given and falls back to the caller's uncropped baseline on anything it cannot prove. The
 * guarantees it establishes, asserted in `tests/unit/meow/test_viewport_cuda.cpp`:
 *
 *  - `0 <= originX` and `originX + (dest.width - 1) * stepX < capture_width` (same for Y),
 *    so every `tex2D()` the kernel issues lands inside the captured frame;
 *  - `dest.offsetX + dest.width <= surface_width` (same for Y), so every byte the kernel
 *    writes lands inside the encode surface;
 *  - `dest.width >= 2`, `dest.height >= 2`, both even, and both offsets even — the NV12
 *    kernel writes 2x2 blocks and its grid is `dest.width / 2` by `dest.height / 2`, so an
 *    odd extent writes one column past the rectangle and a zero extent is an invalid launch.
 *
 * Everything here is a pure function over integers and floats, unit tested without a GPU
 * (CLAUDE.md §5.5). Nothing in this header includes a CUDA header: `apply_cuda_scaler()` is
 * a template over the two upstream POD types so that `src/platform/linux/cuda.h` stays a
 * dependency of the *caller*, not of this file.
 */
#pragma once

// standard includes
#include <algorithm>
#include <optional>

// local includes
#include "src/meow/viewport.h"

namespace meow::viewport {

  /**
   * @brief Where a GPU scaling kernel reads from, in captured-texel space.
   *
   * Mirrors `cuda::source_t`. Deliberately *not* merged with the destination rectangle —
   * see the coordinate-space note at the top of this file.
   */
  struct cuda_source_t {
    float originX {};  ///< Texel column sampled by the first destination column.
    float originY {};  ///< Texel row sampled by the first destination row.
    float stepX {};  ///< Texel columns advanced per destination column.
    float stepY {};  ///< Texel rows advanced per destination row.

    bool operator==(const cuda_source_t &) const = default;
  };

  /**
   * @brief Where the scaled image lands inside the encode surface.
   *
   * Mirrors `cuda::viewport_t` field for field, so `apply_cuda_scaler()` can write straight
   * into one without this header knowing the type.
   */
  struct cuda_dest_t {
    int width {};  ///< Width of the scaled image in the encode surface.
    int height {};  ///< Height of the scaled image in the encode surface.
    int offsetX {};  ///< Horizontal placement of the scaled image in the encode surface.
    int offsetY {};  ///< Vertical placement of the scaled image in the encode surface.

    bool operator==(const cuda_dest_t &) const = default;
  };

  /**
   * @brief A complete CUDA scaler configuration: one destination rectangle, one source map.
   */
  struct cuda_scaler_t {
    cuda_dest_t dest {};  ///< Destination placement in the encode surface.
    cuda_source_t source {};  ///< Source sampling map in the captured frame.
    bool cropped {};  ///< Whether this configuration is a crop (false == upstream's framing).

    bool operator==(const cuda_scaler_t &) const = default;
  };

  /**
   * @brief The uncropped configuration, snapshotted from a freshly constructed `cuda::sws_t`.
   *
   * Taken from the live object rather than recomputed here on purpose. Upstream's
   * `sws_t::sws_t()` derives the letterbox with float arithmetic
   * (`(out_width - out_width_f) / 2`, truncated) while `full_frame_plan()` derives it with
   * integers (`(surface_width - out_width) / 2`), and the two disagree by a pixel for some
   * sizes. Reproducing the formula here would make an uncropped CUDA frame differ from what
   * upstream renders today; reading the numbers back cannot.
   *
   * @param dest Destination rectangle the scaler was constructed with.
   * @param scale Upstream's `sws_t::scale`, i.e. `1 / scalar`.
   * @return The baseline configuration.
   */
  [[nodiscard]] inline cuda_scaler_t cuda_baseline(const cuda_dest_t &dest, const float scale) noexcept {
    return {dest, {0.0f, 0.0f, scale, scale}, false};
  }

  /**
   * @brief Largest source coordinate a kernel will sample along one axis.
   *
   * The kernel's last destination pixel on an axis is `dest_extent - 1`, so this is the
   * furthest `tex2D()` coordinate it can ask for. Exposed so the bound can be asserted in a
   * test rather than argued about in a comment.
   *
   * @param origin Source origin on this axis.
   * @param step Source step on this axis.
   * @param dest_extent Destination extent on this axis.
   * @return The largest coordinate sampled, or `origin` for a degenerate extent.
   */
  [[nodiscard]] inline float cuda_max_sample(const float origin, const float step, const int dest_extent) noexcept {
    if (dest_extent <= 1) {
      return origin;
    }
    return origin + static_cast<float>(dest_extent - 1) * step;
  }

  /**
   * @brief Turn a plan into a CUDA scaler configuration, or fall back to the baseline.
   *
   * The hostile-input boundary for the CUDA path. Every way of saying "no" — no plan, a plan
   * that is not a crop, a degenerate capture or surface, a destination that does not fit the
   * surface, an extent the NV12 kernel cannot express — returns `baseline` unchanged, which
   * is exactly what upstream renders today.
   *
   * The source rectangle is re-clamped against `capture_width`/`capture_height` even though
   * `sanitize()` already did it, because this is the last place before a GPU kernel indexes a
   * texture and the cost is four `std::clamp` calls on a path that runs at most once per
   * frame.
   *
   * @param planned Plan from `plan_for_frame()`; `std::nullopt` means "no crop".
   * @param baseline Uncropped configuration to fall back to, from `cuda_baseline()`.
   * @param capture_width Width of the captured frame backing the source texture.
   * @param capture_height Height of the captured frame backing the source texture.
   * @param surface_width Width of the encode surface being written.
   * @param surface_height Height of the encode surface being written.
   * @return The configuration to apply.
   */
  [[nodiscard]] inline cuda_scaler_t cuda_scaler_config(const std::optional<plan_t> &planned, const cuda_scaler_t &baseline, const int capture_width, const int capture_height, const int surface_width, const int surface_height) noexcept {
    if (!planned || !planned->cropped) {
      return baseline;
    }
    if (capture_width <= 0 || capture_height <= 0 || surface_width <= 0 || surface_height <= 0) {
      return baseline;
    }

    const auto &p = *planned;
    if (p.out_width < 2 || p.out_height < 2) {
      // The NV12 kernel's grid is `out_width / 2` by `out_height / 2`; a zero dimension is an
      // invalid launch configuration, not a small picture.
      return baseline;
    }
    if ((p.out_width & 1) || (p.out_height & 1) || (p.offset_w & 1) || (p.offset_h & 1)) {
      // The NV12 kernel writes 2x2 luma blocks and one interleaved chroma pair per block. An
      // odd extent makes the last block write a column past the rectangle, and an odd offset
      // lands the chroma pair half a sample early.
      return baseline;
    }
    if (p.offset_w < 0 || p.offset_h < 0 || p.offset_w + p.out_width > surface_width || p.offset_h + p.out_height > surface_height) {
      return baseline;
    }
    if (p.source.width <= 0 || p.source.height <= 0 || p.source.x < 0 || p.source.y < 0) {
      return baseline;
    }
    if (p.source.x >= capture_width || p.source.y >= capture_height) {
      return baseline;
    }

    const auto x = std::clamp(p.source.x, 0, capture_width - 1);
    const auto y = std::clamp(p.source.y, 0, capture_height - 1);
    const auto width = std::clamp(p.source.width, 1, capture_width - x);
    const auto height = std::clamp(p.source.height, 1, capture_height - y);

    cuda_scaler_t config;
    config.dest = {p.out_width, p.out_height, p.offset_w, p.offset_h};
    config.source.originX = static_cast<float>(x);
    config.source.originY = static_cast<float>(y);
    // `dest.width` destination columns span `width` source columns, so the last one samples
    // `x + (dest.width - 1) * width / dest.width`, which is strictly inside `x + width`.
    config.source.stepX = static_cast<float>(width) / static_cast<float>(p.out_width);
    config.source.stepY = static_cast<float>(height) / static_cast<float>(p.out_height);
    config.cropped = true;
    return config;
  }

  /**
   * @brief Write a configuration onto a CUDA scaler, reporting whether the surface is stale.
   *
   * Templated on the two upstream POD types so this header needs no CUDA include; `Viewport`
   * must have `width`/`height`/`offsetX`/`offsetY` and `Source` must have
   * `originX`/`originY`/`stepX`/`stepY`.
   *
   * The return value is the whole reason this is a function rather than four assignments.
   * When the destination rectangle shrinks — which it does on the very first crop, because a
   * 3.7:1 desktop letterboxes to a thin strip and a phone-shaped crop does not — the kernel
   * stops writing the region the previous rectangle covered, and that region keeps the last
   * frame's desktop pixels. The caller has to blank the surface before the next launch, and
   * only when this returns `true`, so a steady-state frame costs four comparisons.
   *
   * @tparam Viewport Destination rectangle type (`cuda::viewport_t`).
   * @tparam Source Source map type (`cuda::source_t`).
   * @param config Configuration to apply.
   * @param viewport Destination rectangle; overwritten.
   * @param source Source map; overwritten.
   * @return `true` when the destination rectangle changed and the encode surface must be
   *         re-blanked before the next conversion.
   */
  template<class Viewport, class Source>
  inline bool apply_cuda_scaler(const cuda_scaler_t &config, Viewport &viewport, Source &source) noexcept {
    const bool dest_changed = viewport.width != config.dest.width || viewport.height != config.dest.height || viewport.offsetX != config.dest.offsetX || viewport.offsetY != config.dest.offsetY;

    viewport.width = config.dest.width;
    viewport.height = config.dest.height;
    viewport.offsetX = config.dest.offsetX;
    viewport.offsetY = config.dest.offsetY;

    source.originX = config.source.originX;
    source.originY = config.source.originY;
    source.stepX = config.source.stepX;
    source.stepY = config.source.stepY;

    return dest_changed;
  }

}  // namespace meow::viewport
