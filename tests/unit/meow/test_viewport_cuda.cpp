/**
 * @file tests/unit/meow/test_viewport_cuda.cpp
 * @brief Test src/meow/viewport_cuda.h -- the CUDA scaler's half of viewport following.
 *
 * Everything here is a pure function over integers and floats, so the whole CUDA crop
 * geometry is covered without a GPU, a driver or a captured frame (CLAUDE.md §5.5). What a
 * GPU is still needed for -- that the kernel really reads the rectangle these numbers
 * describe -- is covered by `tools/meow/viewport_cuda_probe.cpp`, which runs the real
 * `cuda::sws_t` against a synthetic image on real hardware.
 */
// test includes
#include "../../tests_common.h"

// standard includes
#include <cmath>
#include <optional>
#include <random>
#include <vector>

// local includes
#include <src/meow/viewport.h>
#include <src/meow/viewport_cuda.h>

#if defined(SUNSHINE_BUILD_CUDA)
  #include <src/platform/linux/cuda.h>
#endif

namespace {

  using meow::viewport::apply_cuda_scaler;
  using meow::viewport::cuda_baseline;
  using meow::viewport::cuda_dest_t;
  using meow::viewport::cuda_max_sample;
  using meow::viewport::cuda_scaler_config;
  using meow::viewport::cuda_scaler_t;
  using meow::viewport::cuda_source_t;
  using meow::viewport::plan;
  using meow::viewport::plan_t;
  using meow::viewport::rect_t;

  /// The developer's actual desktop: two monitors captured as one 3.7:1 frame.
  constexpr int capture_w = 5360;
  constexpr int capture_h = 1440;
  /// The encode surface a phone negotiates.
  constexpr int surface_w = 1280;
  constexpr int surface_h = 720;

  /**
   * @brief The baseline `cuda::sws_t`'s constructor would produce for a given geometry.
   *
   * Deliberately a *copy* of upstream's arithmetic rather than a call into
   * `full_frame_plan()`: the production code snapshots these numbers off the live scaler
   * precisely because the two formulas disagree by a pixel for some sizes, and a test that
   * used the other formula would be testing the wrong thing.
   *
   * @param in_width Captured width.
   * @param in_height Captured height.
   * @param out_width Encode surface width.
   * @param out_height Encode surface height.
   * @return The uncropped configuration.
   */
  cuda_scaler_t upstream_baseline(const int in_width, const int in_height, const int out_width, const int out_height) {
    const auto scalar = std::fminf(static_cast<float>(out_width) / static_cast<float>(in_width), static_cast<float>(out_height) / static_cast<float>(in_height));
    const auto out_width_f = static_cast<float>(in_width) * scalar;
    const auto out_height_f = static_cast<float>(in_height) * scalar;

    cuda_dest_t dest;
    dest.width = static_cast<int>(out_width_f);
    dest.height = static_cast<int>(out_height_f);
    dest.offsetX = static_cast<int>((static_cast<float>(out_width) - out_width_f) / 2);
    dest.offsetY = static_cast<int>((static_cast<float>(out_height) - out_height_f) / 2);

    return cuda_baseline(dest, 1.0f / scalar);
  }

  /// A destination rectangle standing in for `cuda::viewport_t` where CUDA is not built.
  struct fake_viewport_t {
    int width;
    int height;
    int offsetX;
    int offsetY;
  };

  /// A source map standing in for `cuda::source_t` where CUDA is not built.
  struct fake_source_t {
    float originX;
    float originY;
    float stepX;
    float stepY;
  };

  /**
   * @brief Assert that a configuration cannot make the kernel touch memory it does not own.
   *
   * The rectangle comes from the network, so this is the property that matters most: every
   * `tex2D()` inside the captured frame, every byte written inside the encode surface, and a
   * grid the launch configuration will accept.
   *
   * @param config Configuration under test.
   * @param cap_w Captured frame width.
   * @param cap_h Captured frame height.
   * @param surf_w Encode surface width.
   * @param surf_h Encode surface height.
   */
  void expect_in_bounds(const cuda_scaler_t &config, const int cap_w, const int cap_h, const int surf_w, const int surf_h) {
    // Reads.
    EXPECT_GE(config.source.originX, 0.0f);
    EXPECT_GE(config.source.originY, 0.0f);
    EXPECT_GT(config.source.stepX, 0.0f);
    EXPECT_GT(config.source.stepY, 0.0f);
    // The NV12 kernel samples one step past its last pixel for the right/bottom of the 2x2
    // block, so bound that rather than the last pixel's own coordinate.
    EXPECT_LT(cuda_max_sample(config.source.originX, config.source.stepX, config.dest.width), static_cast<float>(cap_w));
    EXPECT_LT(cuda_max_sample(config.source.originY, config.source.stepY, config.dest.height), static_cast<float>(cap_h));

    // Writes.
    EXPECT_GE(config.dest.offsetX, 0);
    EXPECT_GE(config.dest.offsetY, 0);
    EXPECT_LE(config.dest.offsetX + config.dest.width, surf_w);
    EXPECT_LE(config.dest.offsetY + config.dest.height, surf_h);

    // Launch configuration: `grid = (div_align(width / 2, tpb), height / 2)`.
    EXPECT_GE(config.dest.width, 2);
    EXPECT_GE(config.dest.height, 2);
  }

  /**
   * @brief A wide, short crop -- the case where one shared step falls apart.
   *
   * A 5360x142 strip scales by 0.2388 into 1280x33.9. `floor_even()` takes the width to 1280
   * (nothing lost) and the height to 32 (5.8% lost), because it removes the same *absolute*
   * two pixels from each axis and the short one has far fewer to give. The two ratios come
   * out 4.1875 and 4.4375.
   *
   * This is not a contrived shape: it is a row of a spreadsheet spanning both monitors, which
   * is exactly what a user zooms into on a 3.7:1 desktop.
   */
  constexpr rect_t skewed_crop {0, 600, 5360, 142};

  TEST(MeowViewportCuda, BaselineIsUpstreamsMappingExactly) {
    const auto base = upstream_baseline(capture_w, capture_h, surface_w, surface_h);

    // Upstream computed `float x = idX * scale` from the origin of the captured frame.
    EXPECT_FLOAT_EQ(base.source.originX, 0.0f);
    EXPECT_FLOAT_EQ(base.source.originY, 0.0f);
    EXPECT_FLOAT_EQ(base.source.stepX, base.source.stepY);
    EXPECT_FALSE(base.cropped);

    // 5360x1440 into 1280x720 letterboxes to the documented 1280x343 strip.
    EXPECT_EQ(base.dest.width, 1280);
    EXPECT_EQ(base.dest.height, 343);
    EXPECT_EQ(base.dest.offsetY, 188);
  }

  TEST(MeowViewportCuda, NoPlanLeavesTheBaselineExactly) {
    const auto base = upstream_baseline(capture_w, capture_h, surface_w, surface_h);
    EXPECT_EQ(cuda_scaler_config(std::nullopt, base, capture_w, capture_h, surface_w, surface_h), base);
  }

  TEST(MeowViewportCuda, AnUncroppedPlanLeavesTheBaselineExactly) {
    const auto base = upstream_baseline(capture_w, capture_h, surface_w, surface_h);
    const auto full = plan(capture_w, capture_h, surface_w, surface_h, std::nullopt);
    ASSERT_FALSE(full.cropped);

    // Not merely equivalent -- identical. The uncropped CUDA frame must not change at all,
    // and `plan()`'s integer halving of the letterbox is not upstream's float halving.
    EXPECT_EQ(cuda_scaler_config(full, base, capture_w, capture_h, surface_w, surface_h), base);
  }

  TEST(MeowViewportCuda, ACropMovesTheSourceOriginAndFillsTheSurface) {
    const auto base = upstream_baseline(capture_w, capture_h, surface_w, surface_h);
    const rect_t want {1000, 200, 1920, 1080};
    const auto cropped = plan(capture_w, capture_h, surface_w, surface_h, want);
    ASSERT_TRUE(cropped.cropped);

    const auto config = cuda_scaler_config(cropped, base, capture_w, capture_h, surface_w, surface_h);
    ASSERT_TRUE(config.cropped);

    EXPECT_FLOAT_EQ(config.source.originX, static_cast<float>(cropped.source.x));
    EXPECT_FLOAT_EQ(config.source.originY, static_cast<float>(cropped.source.y));
    EXPECT_EQ(config.dest.width, cropped.out_width);
    EXPECT_EQ(config.dest.height, cropped.out_height);
    EXPECT_EQ(config.dest.offsetX, cropped.offset_w);
    EXPECT_EQ(config.dest.offsetY, cropped.offset_h);

    // The whole point of the feature: a 16:9 crop of a 3.7:1 desktop uses the full height of
    // the surface instead of the 343-row strip.
    EXPECT_EQ(config.dest.height, surface_h);
    EXPECT_GT(config.dest.height, base.dest.height * 2);

    expect_in_bounds(config, capture_w, capture_h, surface_w, surface_h);
  }

  TEST(MeowViewportCuda, StepIsPerAxisBecauseTheScaledExtentsRoundIndependently) {
    const auto base = upstream_baseline(capture_w, capture_h, surface_w, surface_h);
    const auto cropped = plan(capture_w, capture_h, surface_w, surface_h, skewed_crop);
    ASSERT_TRUE(cropped.cropped);

    const auto config = cuda_scaler_config(cropped, base, capture_w, capture_h, surface_w, surface_h);
    ASSERT_TRUE(config.cropped);

    // `plan()` picks one `min()` scalar for both axes and then even-aligns each scaled extent
    // on its own, so the two axis ratios are equal in intent and different in arithmetic.
    // A single `float scale` would have to pick one of them for both.
    EXPECT_NE(config.source.stepX, config.source.stepY);
    EXPECT_FLOAT_EQ(config.source.stepX, static_cast<float>(cropped.source.width) / static_cast<float>(cropped.out_width));
    EXPECT_FLOAT_EQ(config.source.stepY, static_cast<float>(cropped.source.height) / static_cast<float>(cropped.out_height));

    // Per axis, the last sample sits exactly one step short of the far edge -- on both axes.
    // That is what "the kernel reads the rectangle that was planned" means, and it is also
    // exactly what swscale does on the software path, where the input frame is
    // `source.width x source.height` and the output is `out_width x out_height`.
    const auto right = static_cast<float>(cropped.source.x + cropped.source.width);
    const auto bottom = static_cast<float>(cropped.source.y + cropped.source.height);
    EXPECT_FLOAT_EQ(cuda_max_sample(config.source.originX, config.source.stepX, config.dest.width), right - config.source.stepX);
    EXPECT_FLOAT_EQ(cuda_max_sample(config.source.originY, config.source.stepY, config.dest.height), bottom - config.source.stepY);

    // One shared step cannot do that for both, and on this shape it is not a rounding
    // curiosity. Reusing the X step on Y leaves the bottom rows of the strip unread...
    const auto y_with_step_x = cuda_max_sample(config.source.originY, config.source.stepX, config.dest.height);
    EXPECT_GT((bottom - config.source.stepY) - y_with_step_x, 4.0f);

    // ...and reusing the Y step on X walks clean off the right-hand edge of the *captured
    // frame*, not merely off the crop. `cudaAddressModeClamp` would turn that into a smear of
    // the last column rather than a fault, which is exactly the kind of wrong that survives a
    // casual look at the picture.
    const auto x_with_step_y = cuda_max_sample(config.source.originX, config.source.stepY, config.dest.width);
    EXPECT_GT(x_with_step_y, static_cast<float>(capture_w));

    expect_in_bounds(config, capture_w, capture_h, surface_w, surface_h);
  }

  TEST(MeowViewportCuda, EveryClientRectangleStaysInBounds) {
    const auto base = upstream_baseline(capture_w, capture_h, surface_w, surface_h);

    // The wire carries uint16s, so this is the entire space a client can express -- including
    // rectangles that start off the right edge or run far past the bottom.
    std::mt19937 rng {20260825};
    std::uniform_int_distribution<int> coord {0, 0xFFFF};

    for (int i = 0; i < 20000; ++i) {
      const rect_t want {coord(rng), coord(rng), coord(rng), coord(rng)};
      const auto planned = plan(capture_w, capture_h, surface_w, surface_h, want);
      const auto config = cuda_scaler_config(planned, base, capture_w, capture_h, surface_w, surface_h);
      if (!config.cropped) {
        ASSERT_EQ(config, base) << "a refused rectangle must land on the baseline, not on something else";
        continue;
      }
      expect_in_bounds(config, capture_w, capture_h, surface_w, surface_h);
    }
  }

  TEST(MeowViewportCuda, RejectsAPlanThatWouldWritePastTheSurface) {
    const auto base = upstream_baseline(capture_w, capture_h, surface_w, surface_h);

    // Not reachable through `plan()`, which is exactly why it is checked here: this is the
    // last gate before a kernel indexes device memory, and it must not trust its caller.
    plan_t forged;
    forged.source = {0, 0, 1000, 800};
    forged.out_width = 1280;
    forged.out_height = 720;
    forged.offset_w = 2;  // 2 + 1280 > 1280
    forged.offset_h = 0;
    forged.cropped = true;
    EXPECT_EQ(cuda_scaler_config(forged, base, capture_w, capture_h, surface_w, surface_h), base);

    forged.offset_w = 0;
    forged.offset_h = 2;  // 2 + 720 > 720
    EXPECT_EQ(cuda_scaler_config(forged, base, capture_w, capture_h, surface_w, surface_h), base);

    forged.offset_h = 0;
    EXPECT_TRUE(cuda_scaler_config(forged, base, capture_w, capture_h, surface_w, surface_h).cropped);
  }

  TEST(MeowViewportCuda, RejectsASourceRectangleOutsideTheCapturedFrame) {
    const auto base = upstream_baseline(capture_w, capture_h, surface_w, surface_h);

    plan_t forged;
    forged.out_width = 640;
    forged.out_height = 480;
    forged.offset_w = 320;
    forged.offset_h = 120;
    forged.cropped = true;

    forged.source = {capture_w, 0, 640, 480};
    EXPECT_EQ(cuda_scaler_config(forged, base, capture_w, capture_h, surface_w, surface_h), base);

    forged.source = {0, capture_h, 640, 480};
    EXPECT_EQ(cuda_scaler_config(forged, base, capture_w, capture_h, surface_w, surface_h), base);

    forged.source = {-4, 0, 640, 480};
    EXPECT_EQ(cuda_scaler_config(forged, base, capture_w, capture_h, surface_w, surface_h), base);

    forged.source = {0, 0, 0, 480};
    EXPECT_EQ(cuda_scaler_config(forged, base, capture_w, capture_h, surface_w, surface_h), base);
  }

  TEST(MeowViewportCuda, ClampsASourceRectangleThatOverhangsTheCapturedFrame) {
    const auto base = upstream_baseline(capture_w, capture_h, surface_w, surface_h);

    plan_t forged;
    forged.source = {capture_w - 100, capture_h - 100, 4000, 4000};  // runs 3900 px past both edges
    forged.out_width = 640;
    forged.out_height = 480;
    forged.offset_w = 320;
    forged.offset_h = 120;
    forged.cropped = true;

    const auto config = cuda_scaler_config(forged, base, capture_w, capture_h, surface_w, surface_h);
    ASSERT_TRUE(config.cropped);
    expect_in_bounds(config, capture_w, capture_h, surface_w, surface_h);
    // Trimmed to the 100x100 that actually exists, not to the 4000x4000 that was claimed.
    EXPECT_FLOAT_EQ(config.source.stepX, 100.0f / 640.0f);
    EXPECT_FLOAT_EQ(config.source.stepY, 100.0f / 480.0f);
  }

  TEST(MeowViewportCuda, RejectsExtentsTheNv12KernelCannotExpress) {
    const auto base = upstream_baseline(capture_w, capture_h, surface_w, surface_h);

    plan_t forged;
    forged.source = {0, 0, 1000, 800};
    forged.out_width = 640;
    forged.out_height = 480;
    forged.offset_w = 320;
    forged.offset_h = 120;
    forged.cropped = true;
    ASSERT_TRUE(cuda_scaler_config(forged, base, capture_w, capture_h, surface_w, surface_h).cropped);

    // An odd extent makes the last 2x2 block write one column/row past the rectangle.
    auto odd = forged;
    odd.out_width = 641;
    EXPECT_EQ(cuda_scaler_config(odd, base, capture_w, capture_h, surface_w, surface_h), base);
    odd = forged;
    odd.out_height = 481;
    EXPECT_EQ(cuda_scaler_config(odd, base, capture_w, capture_h, surface_w, surface_h), base);

    // An odd offset lands the interleaved chroma pair half a sample early.
    odd = forged;
    odd.offset_w = 321;
    EXPECT_EQ(cuda_scaler_config(odd, base, capture_w, capture_h, surface_w, surface_h), base);
    odd = forged;
    odd.offset_h = 121;
    EXPECT_EQ(cuda_scaler_config(odd, base, capture_w, capture_h, surface_w, surface_h), base);

    // A zero extent is an invalid launch configuration, not a small picture: the grid would
    // be `(0, 0)` and `cudaGetLastError()` would fail the frame.
    auto degenerate = forged;
    degenerate.out_width = 0;
    EXPECT_EQ(cuda_scaler_config(degenerate, base, capture_w, capture_h, surface_w, surface_h), base);
    degenerate = forged;
    degenerate.out_height = 0;
    EXPECT_EQ(cuda_scaler_config(degenerate, base, capture_w, capture_h, surface_w, surface_h), base);
  }

  TEST(MeowViewportCuda, RejectsDegenerateCaptureAndSurfaceSizes) {
    const auto base = upstream_baseline(capture_w, capture_h, surface_w, surface_h);
    const auto cropped = plan(capture_w, capture_h, surface_w, surface_h, rect_t {1000, 200, 1920, 1080});
    ASSERT_TRUE(cropped.cropped);

    EXPECT_EQ(cuda_scaler_config(cropped, base, 0, capture_h, surface_w, surface_h), base);
    EXPECT_EQ(cuda_scaler_config(cropped, base, capture_w, 0, surface_w, surface_h), base);
    EXPECT_EQ(cuda_scaler_config(cropped, base, capture_w, capture_h, 0, surface_h), base);
    EXPECT_EQ(cuda_scaler_config(cropped, base, capture_w, capture_h, surface_w, 0), base);
    EXPECT_EQ(cuda_scaler_config(cropped, base, -1, -1, -1, -1), base);
  }

  TEST(MeowViewportCuda, ShrinkingFrameTightensTheCropRatherThanReadingPastIt) {
    // `cuda_t::meow_viewport_apply()` passes `min(allocated, in hand)` for exactly this case:
    // the encoder was set up for a 5360x1440 capture but the frame that arrived is smaller.
    // Written without an `if` on the outcome on purpose -- a test that asserts one thing when
    // the crop survives and another when it does not cannot fail, and this repository has
    // already deleted one guard for that reason (`7e4e43f9`).
    const auto base = upstream_baseline(capture_w, capture_h, surface_w, surface_h);
    const auto cropped = plan(capture_w, capture_h, surface_w, surface_h, rect_t {4000, 1000, 1000, 400});
    ASSERT_TRUE(cropped.cropped);
    ASSERT_EQ(cropped.source, (rect_t {4000, 1000, 1000, 400}));

    // Against the frame it was planned for, the crop reads all 1000x400 of what it asked for.
    const auto full = cuda_scaler_config(cropped, base, capture_w, capture_h, surface_w, surface_h);
    ASSERT_TRUE(full.cropped);
    EXPECT_FLOAT_EQ(full.source.stepX, 1000.0f / static_cast<float>(cropped.out_width));
    EXPECT_FLOAT_EQ(full.source.stepY, 400.0f / static_cast<float>(cropped.out_height));

    // Against a 4200x1100 frame only 200x100 of it exists, and the steps shrink to match
    // rather than the sampling running off the end of what was uploaded.
    const auto config = cuda_scaler_config(cropped, base, 4200, 1100, surface_w, surface_h);
    ASSERT_TRUE(config.cropped);
    EXPECT_FLOAT_EQ(config.source.originX, 4000.0f);
    EXPECT_FLOAT_EQ(config.source.originY, 1000.0f);
    EXPECT_FLOAT_EQ(config.source.stepX, 200.0f / static_cast<float>(cropped.out_width));
    EXPECT_FLOAT_EQ(config.source.stepY, 100.0f / static_cast<float>(cropped.out_height));
    EXPECT_LT(config.source.stepX, full.source.stepX);
    EXPECT_LT(config.source.stepY, full.source.stepY);
    expect_in_bounds(config, 4200, 1100, surface_w, surface_h);

    // The destination is untouched -- a short frame changes what is read, never what is
    // written, so the encode surface stays exactly as full it was.
    EXPECT_EQ(config.dest, full.dest);
  }

  TEST(MeowViewportCuda, ApplyReportsOnlyDestinationChanges) {
    fake_viewport_t viewport {1280, 343, 0, 188};
    fake_source_t source {0.0f, 0.0f, 4.1875f, 4.1875f};

    const auto base = cuda_baseline({viewport.width, viewport.height, viewport.offsetX, viewport.offsetY}, source.stepX);
    // Steady state: nothing moved, nothing to re-blacken.
    EXPECT_FALSE(apply_cuda_scaler(base, viewport, source));

    const cuda_scaler_t zoom {{960, 720, 160, 0}, {1000.0f, 200.0f, 1.5f, 1.5f}, true};
    EXPECT_TRUE(apply_cuda_scaler(zoom, viewport, source));
    EXPECT_EQ(viewport.width, 960);
    EXPECT_EQ(viewport.offsetX, 160);
    EXPECT_FLOAT_EQ(source.originX, 1000.0f);
    EXPECT_FLOAT_EQ(source.stepY, 1.5f);

    // A pan is the same rectangle somewhere else on the desktop. The destination did not
    // move, so the surface is still fully covered and must not be blanked -- that is what
    // keeps a pan free of an extra full-surface kernel launch.
    cuda_scaler_t pan = zoom;
    pan.source.originX = 2000.0f;
    pan.source.originY = 400.0f;
    EXPECT_FALSE(apply_cuda_scaler(pan, viewport, source));
    EXPECT_FLOAT_EQ(source.originX, 2000.0f);

    // Reverting to the full desktop shrinks the destination back and must blacken.
    EXPECT_TRUE(apply_cuda_scaler(base, viewport, source));
    EXPECT_EQ(viewport.height, 343);
    EXPECT_FLOAT_EQ(source.originY, 0.0f);
  }

  TEST(MeowViewportCuda, MaxSampleHandlesDegenerateExtents) {
    EXPECT_FLOAT_EQ(cuda_max_sample(7.0f, 2.0f, 0), 7.0f);
    EXPECT_FLOAT_EQ(cuda_max_sample(7.0f, 2.0f, 1), 7.0f);
    EXPECT_FLOAT_EQ(cuda_max_sample(7.0f, 2.0f, 3), 11.0f);
  }

#if defined(SUNSHINE_BUILD_CUDA)
  TEST(MeowViewportCuda, WritesStraightIntoTheRealCudaTypes) {
    // `apply_cuda_scaler()` is a template so that `src/meow/` needs no CUDA include. That
    // makes the field-name agreement with `cuda::viewport_t` and `cuda::source_t` a
    // convention rather than a compile error at the definition -- so pin it here, where the
    // real types are in scope.
    cuda::viewport_t viewport {1280, 343, 0, 188};
    cuda::source_t source {0.0f, 0.0f, 4.1875f, 4.1875f};

    const cuda_scaler_t zoom {{960, 720, 160, 0}, {1000.0f, 200.0f, 1.5f, 1.5f}, true};
    EXPECT_TRUE(apply_cuda_scaler(zoom, viewport, source));
    EXPECT_EQ(viewport.width, 960);
    EXPECT_EQ(viewport.height, 720);
    EXPECT_EQ(viewport.offsetX, 160);
    EXPECT_EQ(viewport.offsetY, 0);
    EXPECT_FLOAT_EQ(source.originX, 1000.0f);
    EXPECT_FLOAT_EQ(source.originY, 200.0f);
    EXPECT_FLOAT_EQ(source.stepX, 1.5f);
    EXPECT_FLOAT_EQ(source.stepY, 1.5f);
  }
#endif

}  // namespace
