/**
 * @file tools/meow/viewport_cuda_probe.cpp
 * @brief Prove on real hardware that the CUDA scaler reads the rectangle the plan describes.
 *
 * `tests/unit/meow/test_viewport_cuda.cpp` covers the geometry, but geometry is not the claim
 * being made. The claim is that `cuda::sws_t` -- the real one, running the real kernel on a
 * real GPU -- converts a *sub-rectangle* of the captured frame into the encode surface.
 * Nothing that runs without a GPU can check that, so this does: it builds a synthetic desktop
 * with two white markers on it, runs the production geometry (`meow::viewport::plan()` ->
 * `cuda_scaler_config()` -> `apply_cuda_scaler()`) into the production `sws_t`, reads the luma
 * plane back and asserts where the markers landed -- including that the one outside the crop
 * is not in the frame at all.
 *
 * It is deliberately **not** part of any CMake target. `tools/` is only added on Windows
 * (`cmake/compile_definitions/windows.cmake`), and adding a GPU-dependent executable to the
 * Linux build would fail the build on any machine without an NVIDIA card, which is most of
 * them. Build and run it by hand from the repository root:
 *
 * ```bash
 * FF=build/_deps/ffmpeg/include                       # any FFmpeg headers will do
 * /opt/cuda/bin/nvcc -std=c++17 -O2 -arch=native -DSUNSHINE_BUILD_CUDA \
 *     -I. -Ithird-party/nvfbc -I$FF -c src/platform/linux/cuda.cu -o /tmp/probe_cuda.o
 * g++ -std=c++20 -O2 -DSUNSHINE_BUILD_CUDA -I. -I/opt/cuda/include -I$FF \
 *     -c tools/meow/viewport_cuda_probe.cpp -o /tmp/probe_main.o
 * g++ /tmp/probe_main.o /tmp/probe_cuda.o -o /tmp/viewport_cuda_probe -L/opt/cuda/lib64 -lcudart
 * /tmp/viewport_cuda_probe
 * ```
 *
 * Two translation units rather than one because `src/platform/linux/cuda.cu` is built at
 * C++17 (`cmake/targets/common.cmake` pins nvcc to `-std=c++17`) while `src/meow/viewport.h`
 * uses defaulted `operator==`, which is C++20. That split is not an accident of this probe:
 * it is why nothing under `src/meow/` is included from the `.cu` in the first place, and why
 * the CUDA geometry is handed to the kernel as two plain PODs.
 *
 * Exit status is 0 when every check passes, 1 when one fails, 2 when the probe could not run
 * at all. Every check prints what it measured either way.
 */

// standard includes
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string_view>
#include <vector>

// lib includes
#include <cuda_runtime.h>

// local includes
#include "src/meow/viewport.h"
#include "src/meow/viewport_cuda.h"
#include "src/platform/linux/cuda.h"

namespace cuda {

  /**
   * @brief Definition of the error sink `cuda.cu` declares; the real one lives in `cuda.cpp`.
   *
   * @param sv Context string supplied by the CU_CHECK macros.
   * @param name CUDA error name.
   * @param description CUDA error description.
   */
  void pass_error(const std::string_view &sv, const char *name, const char *description) {
    std::fprintf(stderr, "cuda: %.*s%s: %s\n", static_cast<int>(sv.size()), sv.data(), name, description);
  }

}  // namespace cuda

namespace video {

  /**
   * @brief Stand-in for the colorspace table, whose translation unit the probe does not link.
   *
   * Returns a trivial matrix in which luma *is* the red channel and chroma is zero, so a
   * white marker converts to a bright, unambiguous 245 and everything else to 0. That makes
   * "where did the marker land" answerable by looking at the luma plane, with no real colour
   * matrix to invert. `sws_t::apply_colorspace()` is the only caller and it is the real one.
   *
   * @param colorspace Ignored.
   * @param unorm_output Ignored.
   * @return The trivial matrix.
   */
  const color_t *color_vectors_from_colorspace(const sunshine_colorspace_t &colorspace, bool unorm_output) {
    static const color_t identity_luma {
      {1.0f, 0.0f, 0.0f, 0.0f},  // Y = R
      {0.0f, 0.0f, 0.0f, 0.0f},  // U = 0
      {0.0f, 0.0f, 0.0f, 0.0f},  // V = 0
      {1.0f, 0.0f},
      {1.0f, 0.0f}
    };
    (void) colorspace;
    (void) unorm_output;
    return &identity_luma;
  }

}  // namespace video

namespace {

  constexpr int capture_w = 5360;  ///< The developer's two-monitor desktop.
  constexpr int capture_h = 1440;  ///< Ditto.
  constexpr int surface_w = 1280;  ///< What a phone negotiates.
  constexpr int surface_h = 720;  ///< Ditto.

  constexpr int marker_a_x = 1200;  ///< Marker inside the requested crop.
  constexpr int marker_a_y = 400;  ///< Ditto.
  constexpr int marker_b_x = 4800;  ///< Marker far outside it, on the second monitor.
  constexpr int marker_b_y = 1100;  ///< Ditto.
  constexpr int marker_size = 200;  ///< Side of both markers, in desktop pixels.

  /// The rectangle the client asks for, in captured-desktop pixels.
  constexpr meow::viewport::rect_t requested {1000, 300, 1920, 1080};

  int failures = 0;  ///< Number of failed checks.

  /**
   * @brief Record the outcome of one check and print it.
   *
   * @param ok Whether the check passed.
   * @param what Human-readable description.
   */
  void check(const bool ok, const char *const what) {
    std::printf("%s  %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) {
      ++failures;
    }
  }

  /**
   * @brief Bounding box of the bright pixels found in a window of a luma plane.
   */
  struct bbox_t {
    int left = 1 << 30;  ///< Leftmost bright column.
    int top = 1 << 30;  ///< Topmost bright row.
    int right = -1;  ///< Rightmost bright column.
    int bottom = -1;  ///< Bottommost bright row.
    long count = 0;  ///< Number of bright pixels.

    /// @return Whether any bright pixel was found.
    bool empty() const {
      return count == 0;
    }

    /// @return Width of the bounding box in pixels.
    int width() const {
      return empty() ? 0 : right - left + 1;
    }

    /// @return Height of the bounding box in pixels.
    int height() const {
      return empty() ? 0 : bottom - top + 1;
    }
  };

  /**
   * @brief Bounding box of every pixel above `threshold` inside a window of the luma plane.
   *
   * @param luma Luma plane, `surface_w` bytes per row.
   * @param x0 Left edge of the window.
   * @param y0 Top edge of the window.
   * @param x1 Right edge of the window, exclusive.
   * @param y1 Bottom edge of the window, exclusive.
   * @param threshold Brightness a pixel must exceed to count.
   * @return The bounding box.
   */
  bbox_t bright_box(const std::vector<std::uint8_t> &luma, const int x0, const int y0, const int x1, const int y1, const int threshold = 128) {
    bbox_t box;
    for (int y = y0; y < y1; ++y) {
      for (int x = x0; x < x1; ++x) {
        if (luma[static_cast<std::size_t>(y) * surface_w + x] <= threshold) {
          continue;
        }
        box.left = std::min(box.left, x);
        box.top = std::min(box.top, y);
        box.right = std::max(box.right, x);
        box.bottom = std::max(box.bottom, y);
        ++box.count;
      }
    }
    return box;
  }

  /**
   * @brief Whether a measured edge is within `tolerance` pixels of where it was predicted.
   *
   * @param measured Measured value.
   * @param expected Predicted value.
   * @param tolerance Allowed difference in pixels.
   * @return True when the two agree.
   */
  bool near(const int measured, const double expected, const int tolerance) {
    return std::abs(measured - static_cast<int>(std::lround(expected))) <= tolerance;
  }

  /**
   * @brief Upload a BGRA host buffer into a CUDA array.
   *
   * Done here rather than through `sws_t::load_ram()` on purpose: `load_ram` takes a
   * `platf::img_t`, and `cuda.cu` compiles against a hand-copied minimal declaration of that
   * type rather than the real one in `src/platform/common.h`. Passing the real type across
   * that boundary is an ODR hazard the probe has no reason to take on -- `load_ram` is a 2D
   * memcpy and this is the same 2D memcpy.
   *
   * @param array Destination CUDA array.
   * @param pixels Source pixels, BGRA.
   * @param width Width in pixels.
   * @param height Height in pixels.
   * @return True on success.
   */
  bool upload(cudaArray_t array, const std::uint8_t *const pixels, const int width, const int height) {
    const auto status = cudaMemcpy2DToArray(array, 0, 0, pixels, static_cast<std::size_t>(width) * 4, static_cast<std::size_t>(width) * 4, height, cudaMemcpyHostToDevice);
    if (status != cudaSuccess) {
      std::fprintf(stderr, "upload failed: %s\n", cudaGetErrorString(status));
      return false;
    }
    return true;
  }

}  // namespace

/**
 * @brief Run the probe.
 *
 * @return 0 when every check passed, 1 when one failed, 2 when the probe could not run.
 */
int main() {
  int device = 0;
  if (cudaGetDevice(&device) != cudaSuccess) {
    std::fprintf(stderr, "no CUDA device available; this probe needs real hardware\n");
    return 2;
  }
  cudaDeviceProp props {};
  cudaGetDeviceProperties(&props, device);
  std::printf("device: %s\n", props.name);

  // ---- the synthetic desktop -------------------------------------------------------------
  std::vector<std::uint8_t> desktop(static_cast<std::size_t>(capture_w) * capture_h * 4, 0);
  const auto paint = [&desktop](const int x0, const int y0) {
    for (int y = y0; y < y0 + marker_size; ++y) {
      std::memset(desktop.data() + (static_cast<std::size_t>(y) * capture_w + x0) * 4, 0xFF, static_cast<std::size_t>(marker_size) * 4);
    }
  };
  paint(marker_a_x, marker_a_y);
  paint(marker_b_x, marker_b_y);

  // ---- the real scaler -------------------------------------------------------------------
  auto sws_opt = cuda::sws_t::make(capture_w, capture_h, surface_w, surface_h, capture_w * 4);
  if (!sws_opt) {
    std::fprintf(stderr, "couldn't create cuda::sws_t\n");
    return 2;
  }
  auto sws = std::move(*sws_opt);
  sws.apply_colorspace({video::colorspace_e::rec709, true, 8});

  auto tex_opt = cuda::tex_t::make(capture_h, capture_w * 4);
  if (!tex_opt) {
    std::fprintf(stderr, "couldn't allocate the source texture\n");
    return 2;
  }
  auto tex = std::move(*tex_opt);
  if (!upload(tex.array, desktop.data(), capture_w, capture_h)) {
    return 2;
  }

  // A 2x2 all-black texture, exactly as `cuda_t::meow_viewport_init()` builds it.
  auto blank_opt = cuda::tex_t::make(2, 2);
  if (!blank_opt) {
    return 2;
  }
  auto blank = std::move(*blank_opt);
  const std::uint8_t black_pixels[2 * 2 * 4] {};
  if (!upload(blank.array, black_pixels, 2, 2)) {
    return 2;
  }

  auto stream = cuda::make_stream();
  std::uint8_t *Y = nullptr;
  std::uint8_t *UV = nullptr;
  cudaMalloc(reinterpret_cast<void **>(&Y), static_cast<std::size_t>(surface_w) * surface_h);
  cudaMalloc(reinterpret_cast<void **>(&UV), static_cast<std::size_t>(surface_w) * surface_h / 2);
  std::vector<std::uint8_t> luma(static_cast<std::size_t>(surface_w) * surface_h);

  const auto run = [&](const char *const label) {
    if (sws.convert_nv12(Y, UV, surface_w, surface_w, tex.texture.linear, stream.get())) {
      std::fprintf(stderr, "%s: conversion failed\n", label);
      ++failures;
      return;
    }
    cudaStreamSynchronize(stream.get());
    cudaMemcpy(luma.data(), Y, luma.size(), cudaMemcpyDeviceToHost);
  };

  // ---- 1. the uncropped frame, i.e. what the host streams today --------------------------
  const auto baseline = meow::viewport::cuda_baseline({sws.viewport.width, sws.viewport.height, sws.viewport.offsetX, sws.viewport.offsetY}, sws.scale);
  std::printf("\nbaseline: dest %dx%d at (%d,%d), step %.4f\n", baseline.dest.width, baseline.dest.height, baseline.dest.offsetX, baseline.dest.offsetY, baseline.source.stepX);

  cudaMemset(Y, 0x00, luma.size());
  run("baseline");

  const auto base_a = bright_box(luma, 0, 0, surface_w / 2, surface_h);
  const auto base_b = bright_box(luma, surface_w / 2, 0, surface_w, surface_h);
  std::printf("  marker A -> x[%d,%d] y[%d,%d] (%ld px)\n", base_a.left, base_a.right, base_a.top, base_a.bottom, base_a.count);
  std::printf("  marker B -> x[%d,%d] y[%d,%d] (%ld px)\n", base_b.left, base_b.right, base_b.top, base_b.bottom, base_b.count);

  const double base_step = baseline.source.stepX;
  check(!base_a.empty() && !base_b.empty(), "uncropped: both markers are visible");
  check(near(base_a.left, baseline.dest.offsetX + marker_a_x / base_step, 2), "uncropped: marker A left edge");
  check(near(base_a.top, baseline.dest.offsetY + marker_a_y / base_step, 2), "uncropped: marker A top edge");
  check(near(base_b.left, baseline.dest.offsetX + marker_b_x / base_step, 2), "uncropped: marker B left edge");

  const int base_marker_px = base_a.width();

  // ---- 2. the same desktop, cropped to what the client is displaying ----------------------
  const auto planned = meow::viewport::plan(capture_w, capture_h, surface_w, surface_h, requested);
  if (!planned.cropped) {
    std::fprintf(stderr, "the test rectangle was refused by plan(); the probe is misconfigured\n");
    return 2;
  }
  const auto config = meow::viewport::cuda_scaler_config(planned, baseline, capture_w, capture_h, surface_w, surface_h);
  const bool must_blank = meow::viewport::apply_cuda_scaler(config, sws.viewport, sws.source);
  std::printf("\ncropped: source (%d,%d) %dx%d -> dest %dx%d at (%d,%d), step %.4f/%.4f, blank=%d\n", planned.source.x, planned.source.y, planned.source.width, planned.source.height, config.dest.width, config.dest.height, config.dest.offsetX, config.dest.offsetY, config.source.stepX, config.source.stepY, static_cast<int>(must_blank));

  check(config.cropped, "cropped: the configuration is a crop");
  check(must_blank, "cropped: the destination rectangle moved, so the surface must be blanked");

  if (must_blank) {
    // The same call `cuda_t::meow_viewport_apply()` makes: the ordinary kernel over the whole
    // surface, reading a texture that is black at every coordinate.
    sws.convert_nv12(Y, UV, surface_w, surface_w, blank.texture.point, stream.get(), {surface_w, surface_h, 0, 0});
  }
  run("cropped");

  const auto crop_all = bright_box(luma, 0, 0, surface_w, surface_h);
  std::printf("  bright pixels -> x[%d,%d] y[%d,%d] (%ld px)\n", crop_all.left, crop_all.right, crop_all.top, crop_all.bottom, crop_all.count);

  const double want_left = config.dest.offsetX + (marker_a_x - planned.source.x) / static_cast<double>(config.source.stepX);
  const double want_right = config.dest.offsetX + (marker_a_x + marker_size - planned.source.x) / static_cast<double>(config.source.stepX) - 1;
  const double want_top = config.dest.offsetY + (marker_a_y - planned.source.y) / static_cast<double>(config.source.stepY);
  const double want_bottom = config.dest.offsetY + (marker_a_y + marker_size - planned.source.y) / static_cast<double>(config.source.stepY) - 1;
  std::printf("  expected      -> x[%.1f,%.1f] y[%.1f,%.1f]\n", want_left, want_right, want_top, want_bottom);

  check(!crop_all.empty(), "cropped: the in-crop marker is visible");
  check(near(crop_all.left, want_left, 2), "cropped: marker A left edge is where the plan says");
  check(near(crop_all.right, want_right, 2), "cropped: marker A right edge is where the plan says");
  check(near(crop_all.top, want_top, 2), "cropped: marker A top edge is where the plan says");
  check(near(crop_all.bottom, want_bottom, 2), "cropped: marker A bottom edge is where the plan says");

  // The load-bearing negative: the second monitor is outside the requested rectangle, so it
  // must not reach the encoder at all. A bounding box covering both markers would be ~900 px
  // wide; this one is the size of the single marker that is inside the crop.
  check(crop_all.width() < marker_size && crop_all.height() < marker_size, "cropped: the out-of-crop marker is NOT in the encoded frame");

  // And the point of the whole feature: the same desktop pixels now occupy far more of the
  // encode surface.
  const double magnification = crop_all.width() / static_cast<double>(base_marker_px);
  std::printf("  marker occupies %d px cropped vs %d px uncropped (%.2fx)\n", crop_all.width(), base_marker_px, magnification);
  check(magnification > 2.0, "cropped: the marker occupies at least 2x more encoded pixels");

  // ---- 3. reverting, and the blanking that makes reverting safe --------------------------
  // Reverting shrinks the destination from the whole surface back to the 1280x343 letterbox.
  // The uncropped kernel does not write the 188 padding rows above it, so they keep whatever
  // the crop put there. Prove that first -- a blanking pass nobody can see fail is a blanking
  // pass that can be deleted by accident.
  const bool revert_blank = meow::viewport::apply_cuda_scaler(baseline, sws.viewport, sws.source);
  check(revert_blank, "revert: the destination rectangle moved back, so the surface must be blanked");

  run("revert without blanking");
  const auto stale = bright_box(luma, 0, 0, surface_w, baseline.dest.offsetY, 8);
  std::printf("\nrevert without blanking: %ld non-black pixels in the %d padding rows above the letterbox\n", stale.count, baseline.dest.offsetY);
  check(!stale.empty(), "revert: WITHOUT the blanking pass the padding really does keep the crop (so the pass is load-bearing)");

  if (revert_blank) {
    sws.convert_nv12(Y, UV, surface_w, surface_w, blank.texture.point, stream.get(), {surface_w, surface_h, 0, 0});
  }
  run("revert");

  const auto padding = bright_box(luma, 0, 0, surface_w, baseline.dest.offsetY, 8);
  std::printf("revert with blanking:    %ld non-black pixels in the same rows\n", padding.count);
  check(padding.empty(), "revert: with the blanking pass the padding is black, not a leftover of the crop");

  const auto revert_a = bright_box(luma, 0, 0, surface_w / 2, surface_h);
  check(near(revert_a.left, baseline.dest.offsetX + marker_a_x / base_step, 2), "revert: marker A is back where the uncropped frame puts it");

  cudaFree(Y);
  cudaFree(UV);

  std::printf("\n%s (%d failed)\n", failures ? "PROBE FAILED" : "PROBE PASSED", failures);
  return failures ? 1 : 0;
}
