/**
 * @file tests/unit/meow/test_display_union.cpp
 * @brief Test src/meow/display_union.h.
 */
// test includes
#include "../../tests_common.h"

// standard includes
#include <cstdint>
#include <string>
#include <vector>

// local includes
#include <src/meow/display_union.h>

namespace {

  using meow::display_union::compute_union;
  using meow::display_union::decide_union_capture;
  using meow::display_union::describe_output;
  using meow::display_union::is_union_output_name;
  using meow::display_union::min_stream_region_version;
  using meow::display_union::output_geometry_t;
  using meow::display_union::output_report_t;
  using meow::display_union::output_transform_t;
  using meow::display_union::single_output_pos_x;
  using meow::display_union::single_output_pos_y;
  using meow::display_union::union_backend_warning;
  using meow::display_union::union_output_name;
  using meow::display_union::union_status_t;

  /**
   * @brief Build an output whose logical size equals its mode size (scale 1).
   * @param name Connector name.
   * @param x Logical left edge.
   * @param y Logical top edge.
   * @param w Logical and pixel width.
   * @param h Logical and pixel height.
   * @param refresh_mhz Current mode refresh in mHz.
   * @return The output description.
   */
  output_geometry_t unscaled(const std::string &name, const int x, const int y, const int w, const int h, const int refresh_mhz = 60000) {
    output_geometry_t output;
    output.name = name;
    output.logical_x = x;
    output.logical_y = y;
    output.logical_width = w;
    output.logical_height = h;
    output.pixel_width = w;
    output.pixel_height = h;
    output.wl_scale = 1;
    output.refresh_mhz = refresh_mhz;
    return output;
  }

  /**
   * @brief Build a rotated output the way a compositor actually reports one.
   *
   * `wl_output::mode` is pre-transform, so a quarter-turned panel keeps reporting its
   * landscape mode; `xdg_output::logical_size` is post-transform and reports the portrait
   * size. That mismatch is the whole point of the transform handling.
   *
   * @param name Connector name.
   * @param x Logical left edge.
   * @param y Logical top edge.
   * @param mode_w Mode width in device pixels, before the transform.
   * @param mode_h Mode height in device pixels, before the transform.
   * @param transform Output transform.
   * @param scale Scale factor to divide the rotated pixel size by to get the logical size.
   * @return The output description.
   */
  output_geometry_t rotated(const std::string &name, const int x, const int y, const int mode_w, const int mode_h, const output_transform_t transform, const int scale = 1) {
    output_geometry_t output;
    output.name = name;
    output.logical_x = x;
    output.logical_y = y;
    output.pixel_width = mode_w;
    output.pixel_height = mode_h;
    output.transform = transform;
    // Post-transform axes, then divided by the scale — exactly what xdg_output reports.
    const bool swapped = transform == output_transform_t::rotate_90 || transform == output_transform_t::rotate_270 ||
                         transform == output_transform_t::flipped_90 || transform == output_transform_t::flipped_270;
    output.logical_width = (swapped ? mode_h : mode_w) / scale;
    output.logical_height = (swapped ? mode_w : mode_h) / scale;
    output.wl_scale = scale;
    output.refresh_mhz = 60000;
    return output;
  }

}  // namespace

TEST(DisplayUnionNameTest, MatchesReservedNameExactlyAndCaseSensitively) {
  // Deliberately exact: video::refresh_displays() selects the active display with
  // `display_names[x] == output_name`, so anything this function accepts that the exact
  // comparison there rejects would be recognised at probe time and silently ignored at
  // stream time — encoder probing taking the union path while streaming took the single-
  // output path.
  EXPECT_TRUE(is_union_output_name("all"));
  EXPECT_FALSE(is_union_output_name("ALL"));
  EXPECT_FALSE(is_union_output_name("All"));
  EXPECT_FALSE(is_union_output_name("aLl"));
  EXPECT_FALSE(is_union_output_name(" all"));
  EXPECT_FALSE(is_union_output_name("all "));
}

TEST(DisplayUnionNameTest, RejectsRealConnectorNames) {
  EXPECT_FALSE(is_union_output_name(""));
  EXPECT_FALSE(is_union_output_name("eDP-2"));
  EXPECT_FALSE(is_union_output_name("HDMI-A-1"));
  EXPECT_FALSE(is_union_output_name("alll"));
  EXPECT_FALSE(is_union_output_name("al"));
}

TEST(DisplayUnionTest, EmptyListIsInvalid) {
  const auto region = compute_union({});
  EXPECT_FALSE(region.valid);
  EXPECT_EQ(region.contributing_outputs, 0u);
  EXPECT_EQ(region.width, 0);
  EXPECT_EQ(region.height, 0);
}

TEST(DisplayUnionTest, SingleOutputIsItsOwnBoundingBox) {
  const auto region = compute_union({unscaled("eDP-2", 0, 0, 1920, 1200)});
  EXPECT_TRUE(region.valid);
  EXPECT_EQ(region.contributing_outputs, 1u);
  EXPECT_EQ(region.x, 0);
  EXPECT_EQ(region.y, 0);
  EXPECT_EQ(region.width, 1920);
  EXPECT_EQ(region.height, 1200);
  EXPECT_EQ(region.pixel_width, 1920);
  EXPECT_EQ(region.pixel_height, 1200);
  EXPECT_DOUBLE_EQ(region.scale, 1.0);
  EXPECT_TRUE(region.covers_whole_region);
  EXPECT_FALSE(region.exceeds_capture_limits);
}

TEST(DisplayUnionTest, SideBySideMatchesTheDeveloperHardware) {
  // eDP-2 at 0,0 1920x1200 @180Hz + HDMI-A-1 at 1920,0 3440x1440 @100Hz.
  const auto region = compute_union({
    unscaled("eDP-2", 0, 0, 1920, 1200, 180000),
    unscaled("HDMI-A-1", 1920, 0, 3440, 1440, 100000),
  });
  EXPECT_TRUE(region.valid);
  EXPECT_EQ(region.contributing_outputs, 2u);
  EXPECT_EQ(region.x, 0);
  EXPECT_EQ(region.y, 0);
  EXPECT_EQ(region.width, 5360);
  EXPECT_EQ(region.height, 1440);
  EXPECT_EQ(region.pixel_width, 5360);
  EXPECT_EQ(region.pixel_height, 1440);
  EXPECT_DOUBLE_EQ(region.scale, 1.0);
  // The 1920x1200 panel is shorter than the 1440-tall ultrawide, so 1920x240 is dead space.
  EXPECT_FALSE(region.covers_whole_region);
  EXPECT_EQ(region.refresh_mhz, 180000);
  EXPECT_FALSE(region.exceeds_capture_limits);
}

TEST(DisplayUnionTest, StackedOutputsUnionVertically) {
  const auto region = compute_union({
    unscaled("DP-1", 0, 0, 2560, 1440),
    unscaled("DP-2", 0, 1440, 2560, 1440),
  });
  EXPECT_TRUE(region.valid);
  EXPECT_EQ(region.x, 0);
  EXPECT_EQ(region.y, 0);
  EXPECT_EQ(region.width, 2560);
  EXPECT_EQ(region.height, 2880);
  EXPECT_TRUE(region.covers_whole_region);
}

TEST(DisplayUnionTest, NegativeOriginsAreNormalised) {
  // KWin happily places an output to the left of / above the origin.
  const auto region = compute_union({
    unscaled("DP-1", -1920, -200, 1920, 1080),
    unscaled("DP-2", 0, 0, 2560, 1440),
  });
  EXPECT_TRUE(region.valid);
  EXPECT_EQ(region.x, -1920);
  EXPECT_EQ(region.y, -200);
  EXPECT_EQ(region.width, 4480);
  EXPECT_EQ(region.height, 1640);
}

TEST(DisplayUnionTest, GapBetweenOutputsIsIncludedAndReportedAsDeadSpace) {
  const auto region = compute_union({
    unscaled("DP-1", 0, 0, 1920, 1080),
    unscaled("DP-2", 3000, 0, 1920, 1080),
  });
  EXPECT_TRUE(region.valid);
  EXPECT_EQ(region.width, 4920);
  EXPECT_EQ(region.height, 1080);
  EXPECT_FALSE(region.covers_whole_region);
}

TEST(DisplayUnionTest, DisabledOutputsAreExcluded) {
  auto disabled = unscaled("HDMI-A-1", 1920, 0, 3440, 1440);
  disabled.enabled = false;
  const auto region = compute_union({unscaled("eDP-2", 0, 0, 1920, 1200), disabled});
  EXPECT_TRUE(region.valid);
  EXPECT_EQ(region.contributing_outputs, 1u);
  EXPECT_EQ(region.width, 1920);
  EXPECT_EQ(region.height, 1200);
}

TEST(DisplayUnionTest, AllOutputsDisabledIsInvalid) {
  auto disabled = unscaled("eDP-2", 0, 0, 1920, 1200);
  disabled.enabled = false;
  const auto region = compute_union({disabled});
  EXPECT_FALSE(region.valid);
  EXPECT_EQ(region.contributing_outputs, 0u);
}

TEST(DisplayUnionTest, DifferingScalesUseLogicalGeometryAndTheHighestScale) {
  // 1920x1200 at 100% next to a 4K panel at 150% (logical 2560x1440).
  output_geometry_t hidpi;
  hidpi.name = "DP-1";
  hidpi.logical_x = 1920;
  hidpi.logical_y = 0;
  hidpi.logical_width = 2560;
  hidpi.logical_height = 1440;
  hidpi.pixel_width = 3840;
  hidpi.pixel_height = 2160;
  hidpi.wl_scale = 2;  // KWin rounds fractional scale up here; it must not be trusted.
  hidpi.refresh_mhz = 60000;

  const auto region = compute_union({unscaled("eDP-2", 0, 0, 1920, 1200), hidpi});
  EXPECT_TRUE(region.valid);
  // Logical union, NOT the sum of device pixels.
  EXPECT_EQ(region.width, 4480);
  EXPECT_EQ(region.height, 1440);
  EXPECT_DOUBLE_EQ(region.scale, 1.5);
  EXPECT_EQ(region.pixel_width, 6720);
  EXPECT_EQ(region.pixel_height, 2160);
  EXPECT_FALSE(region.exceeds_capture_limits);
}

TEST(DisplayUnionTest, FallsBackToModeOverIntegerScaleWithoutXdgOutput) {
  // A compositor without xdg-output leaves logical_* at 0.
  output_geometry_t output;
  output.name = "DP-1";
  output.logical_x = 0;
  output.logical_y = 0;
  output.pixel_width = 3840;
  output.pixel_height = 2160;
  output.wl_scale = 2;

  const auto region = compute_union({output});
  EXPECT_TRUE(region.valid);
  EXPECT_EQ(region.width, 1920);
  EXPECT_EQ(region.height, 1080);
  EXPECT_DOUBLE_EQ(region.scale, 2.0);
  EXPECT_EQ(region.pixel_width, 3840);
  EXPECT_EQ(region.pixel_height, 2160);
}

TEST(DisplayUnionTest, MirroredOutputsAreNotDeadSpace) {
  const auto region = compute_union({
    unscaled("DP-1", 0, 0, 1920, 1080),
    unscaled("DP-2", 0, 0, 1920, 1080),
  });
  EXPECT_TRUE(region.valid);
  EXPECT_EQ(region.width, 1920);
  EXPECT_EQ(region.height, 1080);
  EXPECT_TRUE(region.covers_whole_region);
}

TEST(DisplayUnionTest, OversizedUnionIsFlaggedRatherThanSilentlyClamped) {
  const auto region = compute_union({
    unscaled("DP-1", 0, 0, 7680, 4320),
    unscaled("DP-2", 7680, 0, 3840, 2160),
  });
  EXPECT_TRUE(region.valid);
  EXPECT_EQ(region.width, 11520);
  EXPECT_EQ(region.height, 4320);
  // Untouched dimensions — the caller decides what to do, nothing is silently rescaled.
  EXPECT_EQ(region.pixel_width, 11520);
  EXPECT_EQ(region.pixel_height, 4320);
  EXPECT_TRUE(region.exceeds_capture_limits);
}

TEST(DisplayUnionTest, OutputsWithoutAnyGeometryAreIgnored) {
  output_geometry_t unknown;
  unknown.name = "DP-9";
  const auto region = compute_union({unknown, unscaled("eDP-2", 0, 0, 1920, 1200)});
  EXPECT_TRUE(region.valid);
  EXPECT_EQ(region.contributing_outputs, 1u);
  EXPECT_EQ(region.width, 1920);
  EXPECT_EQ(region.height, 1200);
}

TEST(DisplayUnionTest, MirroredPairBesideAGapIsNotFullyCovered) {
  // Regression: an area-based heuristic reports this as covered because the mirrored pair
  // double-counts, exactly cancelling the 100x100 hole between the pair and the third output.
  const auto region = compute_union({
    unscaled("DP-1", 0, 0, 100, 100),
    unscaled("DP-2", 0, 0, 100, 100),
    unscaled("DP-3", 200, 0, 100, 100),
  });
  EXPECT_TRUE(region.valid);
  EXPECT_EQ(region.width, 300);
  EXPECT_EQ(region.height, 100);
  EXPECT_FALSE(region.covers_whole_region);
}

TEST(DisplayUnionTest, PartiallyOverlappingOutputsStillTileTheBox) {
  const auto region = compute_union({
    unscaled("DP-1", 0, 0, 1920, 1080),
    unscaled("DP-2", 960, 0, 1920, 1080),
  });
  EXPECT_TRUE(region.valid);
  EXPECT_EQ(region.width, 2880);
  EXPECT_TRUE(region.covers_whole_region);
}

TEST(DisplayUnionTest, NegativeOriginIsFlagged) {
  const auto with_negative = compute_union({
    unscaled("DP-1", -1920, 0, 1920, 1080),
    unscaled("DP-2", 0, 0, 1920, 1080),
  });
  EXPECT_TRUE(with_negative.valid);
  EXPECT_TRUE(with_negative.has_negative_origin);

  const auto anchored = compute_union({unscaled("DP-1", 0, 0, 1920, 1080)});
  EXPECT_FALSE(anchored.has_negative_origin);
}

TEST(DisplayUnionDecisionTest, UsesRegionOnASupportedCompositor) {
  const auto decision = decide_union_capture(
    {
      unscaled("eDP-2", 0, 0, 1920, 1200, 180000),
      unscaled("HDMI-A-1", 1920, 0, 3440, 1440, 100000),
    },
    6
  );
  EXPECT_EQ(decision.status, union_status_t::ok);
  EXPECT_TRUE(decision.use_region());
  EXPECT_EQ(decision.region.width, 5360);
  EXPECT_EQ(decision.region.height, 1440);
}

TEST(DisplayUnionDecisionTest, RefusesBelowTheMinimumProtocolVersion) {
  const std::vector<output_geometry_t> outputs {unscaled("eDP-2", 0, 0, 1920, 1200)};
  for (std::uint32_t version = 0; version < min_stream_region_version; ++version) {
    const auto decision = decide_union_capture(outputs, version);
    EXPECT_EQ(decision.status, union_status_t::unsupported_protocol) << "version " << version;
    EXPECT_FALSE(decision.use_region()) << "version " << version;
  }
  EXPECT_TRUE(decide_union_capture(outputs, min_stream_region_version).use_region());
}

TEST(DisplayUnionDecisionTest, RefusesWhenNoOutputIsUsable) {
  auto disabled = unscaled("eDP-2", 0, 0, 1920, 1200);
  disabled.enabled = false;
  const auto decision = decide_union_capture({disabled}, 6);
  EXPECT_EQ(decision.status, union_status_t::no_usable_outputs);
  EXPECT_FALSE(decision.use_region());
}

TEST(DisplayUnionDecisionTest, RefusesAnOversizedRegionInsteadOfClampingIt) {
  const auto decision = decide_union_capture(
    {
      unscaled("DP-1", 0, 0, 7680, 4320),
      unscaled("DP-2", 7680, 0, 3840, 2160),
    },
    6
  );
  EXPECT_EQ(decision.status, union_status_t::exceeds_capture_limits);
  EXPECT_FALSE(decision.use_region());
  // The geometry is reported untouched so the caller can log what was refused.
  EXPECT_EQ(decision.region.pixel_width, 11520);
  EXPECT_EQ(decision.region.pixel_height, 4320);
}

TEST(DisplayUnionRotationTest, QuarterTurnedOutputAloneHasScaleOne) {
  // A 1920x1080 panel mounted in portrait: mode stays 1920x1080, logical becomes 1080x1920.
  // Dividing the pre-transform width by the post-transform width gives 1.78, not 1.0.
  const auto region = compute_union({rotated("DP-1", 0, 0, 1920, 1080, output_transform_t::rotate_90)});
  EXPECT_TRUE(region.valid);
  EXPECT_EQ(region.width, 1080);
  EXPECT_EQ(region.height, 1920);
  EXPECT_DOUBLE_EQ(region.scale, 1.0);
  EXPECT_EQ(region.pixel_width, 1080);
  EXPECT_EQ(region.pixel_height, 1920);
  EXPECT_FALSE(region.exceeds_capture_limits);
}

TEST(DisplayUnionRotationTest, ThreeQuarterTurnBehavesLikeTheQuarterTurn) {
  const auto region = compute_union({rotated("DP-1", 0, 0, 2560, 1440, output_transform_t::rotate_270)});
  EXPECT_TRUE(region.valid);
  EXPECT_EQ(region.width, 1440);
  EXPECT_EQ(region.height, 2560);
  EXPECT_DOUBLE_EQ(region.scale, 1.0);
}

TEST(DisplayUnionRotationTest, FlippedQuarterTurnsAlsoSwapAxes) {
  for (const auto transform : {output_transform_t::flipped_90, output_transform_t::flipped_270}) {
    const auto region = compute_union({rotated("DP-1", 0, 0, 1920, 1080, transform)});
    EXPECT_TRUE(region.valid);
    EXPECT_EQ(region.width, 1080) << "transform " << static_cast<int>(transform);
    EXPECT_EQ(region.height, 1920) << "transform " << static_cast<int>(transform);
    EXPECT_DOUBLE_EQ(region.scale, 1.0) << "transform " << static_cast<int>(transform);
  }
}

TEST(DisplayUnionRotationTest, HalfTurnsAndPlainFlipsDoNotSwapAxes) {
  for (const auto transform : {output_transform_t::normal, output_transform_t::rotate_180, output_transform_t::flipped, output_transform_t::flipped_180}) {
    const auto region = compute_union({rotated("DP-1", 0, 0, 1920, 1080, transform)});
    EXPECT_TRUE(region.valid);
    EXPECT_EQ(region.width, 1920) << "transform " << static_cast<int>(transform);
    EXPECT_EQ(region.height, 1080) << "transform " << static_cast<int>(transform);
    EXPECT_DOUBLE_EQ(region.scale, 1.0) << "transform " << static_cast<int>(transform);
  }
}

TEST(DisplayUnionRotationTest, LandscapeBesidePortraitDoesNotInflateTheWholeDesktop) {
  // Regression for the reported bug: 2560x1440 at 0,0 plus a 90-rotated 1920x1080 at 2560,0.
  // Before the transform was taken into account the portrait panel derived a scale of 1.78,
  // compute_union() took the maximum scale across contributors, and the capture became
  // 6471x3413 instead of 3640x1920 — on a slightly larger desktop that crosses the 8192x4096
  // limit and decide_union_capture() refuses with a bogus "exceeds capture limit" error.
  const auto region = compute_union({
    unscaled("DP-1", 0, 0, 2560, 1440),
    rotated("DP-2", 2560, 0, 1920, 1080, output_transform_t::rotate_90),
  });
  EXPECT_TRUE(region.valid);
  EXPECT_EQ(region.contributing_outputs, 2u);
  EXPECT_EQ(region.width, 3640);
  EXPECT_EQ(region.height, 1920);
  EXPECT_DOUBLE_EQ(region.scale, 1.0);
  EXPECT_EQ(region.pixel_width, 3640);
  EXPECT_EQ(region.pixel_height, 1920);
  EXPECT_FALSE(region.exceeds_capture_limits);
  EXPECT_EQ(decide_union_capture({unscaled("DP-1", 0, 0, 2560, 1440), rotated("DP-2", 2560, 0, 1920, 1080, output_transform_t::rotate_90)}, 6).status, union_status_t::ok);
}

TEST(DisplayUnionRotationTest, RotatedHiDpiOutputDerivesTheScaleFromRotatedAxes) {
  // A 4K panel in portrait at 200%: mode 3840x2160, logical 1080x1920, scale 2.0.
  const auto rotated_hidpi = rotated("DP-1", 0, 0, 3840, 2160, output_transform_t::rotate_90, 2);
  ASSERT_EQ(rotated_hidpi.logical_width, 1080);
  ASSERT_EQ(rotated_hidpi.logical_height, 1920);

  const auto region = compute_union({rotated_hidpi});
  EXPECT_TRUE(region.valid);
  EXPECT_EQ(region.width, 1080);
  EXPECT_EQ(region.height, 1920);
  EXPECT_DOUBLE_EQ(region.scale, 2.0);
  EXPECT_EQ(region.pixel_width, 2160);
  EXPECT_EQ(region.pixel_height, 3840);
}

TEST(DisplayUnionRotationTest, RotatedHiDpiBesideUnscaledTakesTheHigherScaleOnly) {
  const auto region = compute_union({
    unscaled("eDP-2", 0, 0, 1920, 1200),
    rotated("DP-1", 1920, 0, 3840, 2160, output_transform_t::rotate_90, 2),
  });
  EXPECT_TRUE(region.valid);
  EXPECT_EQ(region.width, 3000);  // 1920 + 1080
  EXPECT_EQ(region.height, 1920);
  EXPECT_DOUBLE_EQ(region.scale, 2.0);
  EXPECT_EQ(region.pixel_width, 6000);
  EXPECT_EQ(region.pixel_height, 3840);
  EXPECT_FALSE(region.exceeds_capture_limits);
}

TEST(DisplayUnionRotationTest, RotationIsHandledWithoutXdgOutputToo) {
  // No xdg-output: the logical size has to come from the mode divided by wl_output::scale,
  // and the mode still needs rotating first.
  output_geometry_t output;
  output.name = "DP-1";
  output.pixel_width = 3840;
  output.pixel_height = 2160;
  output.transform = output_transform_t::rotate_270;
  output.wl_scale = 2;

  const auto region = compute_union({output});
  EXPECT_TRUE(region.valid);
  EXPECT_EQ(region.width, 1080);
  EXPECT_EQ(region.height, 1920);
  EXPECT_DOUBLE_EQ(region.scale, 2.0);
}

TEST(DisplayUnionRotationTest, AnUnknownTransformIsTreatedAsUnrotated) {
  // Forward compatibility: a transform this build does not know must not guess a rotation.
  output_geometry_t output = unscaled("DP-1", 0, 0, 1920, 1080);
  output.transform = static_cast<output_transform_t>(99);
  const auto region = compute_union({output});
  EXPECT_TRUE(region.valid);
  EXPECT_EQ(region.width, 1920);
  EXPECT_EQ(region.height, 1080);
  EXPECT_DOUBLE_EQ(region.scale, 1.0);
}

TEST(DisplayUnionDecisionTest, RefusesANegativeOriginInsteadOfStreamingABrokenPointerMapping) {
  const auto decision = decide_union_capture(
    {
      unscaled("DP-1", -1920, 0, 1920, 1080),
      unscaled("DP-2", 0, 0, 1920, 1080),
    },
    6
  );
  EXPECT_EQ(decision.status, union_status_t::negative_origin);
  EXPECT_FALSE(decision.use_region());
  // Reported untouched so the caller can name the offending origin in the log.
  EXPECT_EQ(decision.region.x, -1920);
  EXPECT_TRUE(decision.region.has_negative_origin);
}

TEST(DisplayUnionDecisionTest, ANegativeVerticalOriginIsRefusedToo) {
  const auto decision = decide_union_capture(
    {
      unscaled("DP-1", 0, -200, 1920, 1080),
      unscaled("DP-2", 0, 880, 1920, 1080),
    },
    6
  );
  EXPECT_EQ(decision.status, union_status_t::negative_origin);
  EXPECT_FALSE(decision.use_region());
}

TEST(DisplayUnionDecisionTest, AnOriginAnchoredArrangementIsStillAccepted) {
  const auto decision = decide_union_capture(
    {
      unscaled("DP-1", 0, 0, 1920, 1080),
      unscaled("DP-2", 1920, 0, 1920, 1080),
    },
    6
  );
  EXPECT_EQ(decision.status, union_status_t::ok);
  EXPECT_TRUE(decision.use_region());
}

//
// Characterization tests for the pre-existing single-output path (CLAUDE.md §5 rule 3).
//
// `kwingrab.cpp` is untested upstream, and this branch binds `zxdg_output_manager_v1` in it
// for the first time. Requirement 2 of the feature is that any `output_name` other than the
// reserved value keeps single-output behaviour byte-for-byte unchanged. The risk is
// `display_t::offset_x`/`offset_y`: `pipewire.cpp` matches those for *equality* against
// `wl::monitors()`, so if the offsets started coming from `xdg_output` instead of
// `wl_output::geometry` and the two ever disagreed, single-output capture would silently lose
// its logical size and mismap absolute pointer input on a scaled desktop.
//

TEST(DisplayUnionCharacterizationTest, SingleOutputPositionIgnoresXdgOutputEntirely) {
  output_report_t report;
  report.name = "eDP-2";
  report.wl_x = 100;
  report.wl_y = 200;
  report.mode_width = 1920;
  report.mode_height = 1200;

  const auto without_xdg_x = single_output_pos_x(report);
  const auto without_xdg_y = single_output_pos_y(report);
  EXPECT_EQ(without_xdg_x, 100);
  EXPECT_EQ(without_xdg_y, 200);

  // Now let the xdg-output listener contribute, and disagree on purpose.
  report.has_xdg_logical_position = true;
  report.xdg_logical_x = 7;
  report.xdg_logical_y = 9;
  report.has_xdg_logical_size = true;
  report.xdg_logical_width = 1280;
  report.xdg_logical_height = 800;

  EXPECT_EQ(single_output_pos_x(report), without_xdg_x);
  EXPECT_EQ(single_output_pos_y(report), without_xdg_y);
}

TEST(DisplayUnionCharacterizationTest, SingleOutputPositionIsUnchangedAcrossEveryOutputShape) {
  output_report_t plain;
  plain.name = "DP-1";
  plain.mode_width = 1920;
  plain.mode_height = 1080;

  output_report_t scaled;
  scaled.name = "DP-2";
  scaled.wl_x = 1920;
  scaled.wl_y = -540;
  scaled.mode_width = 3840;
  scaled.mode_height = 2160;
  scaled.wl_scale = 2;

  output_report_t portrait;
  portrait.name = "DP-3";
  portrait.wl_x = -1080;
  portrait.mode_width = 1920;
  portrait.mode_height = 1080;
  portrait.transform = output_transform_t::rotate_90;

  for (auto report : {plain, scaled, portrait}) {
    const auto baseline_x = single_output_pos_x(report);
    const auto baseline_y = single_output_pos_y(report);
    report.has_xdg_logical_position = true;
    report.xdg_logical_x = baseline_x + 13;  // Any disagreement at all must not leak through.
    report.xdg_logical_y = baseline_y - 13;
    report.has_xdg_logical_size = true;
    report.xdg_logical_width = 999;
    report.xdg_logical_height = 999;
    EXPECT_EQ(single_output_pos_x(report), baseline_x) << report.name;
    EXPECT_EQ(single_output_pos_y(report), baseline_y) << report.name;
  }
}

TEST(DisplayUnionCharacterizationTest, UnionGeometryPrefersXdgOutputWhenItIsPresent) {
  output_report_t report;
  report.name = "DP-1";
  report.wl_x = 100;
  report.wl_y = 200;
  report.mode_width = 3840;
  report.mode_height = 2160;
  report.wl_scale = 2;
  report.refresh_mhz = 144000;
  report.has_xdg_logical_position = true;
  report.xdg_logical_x = 96;
  report.xdg_logical_y = 192;
  report.has_xdg_logical_size = true;
  report.xdg_logical_width = 2560;
  report.xdg_logical_height = 1440;

  const auto geometry = describe_output(report);
  EXPECT_EQ(geometry.name, "DP-1");
  EXPECT_EQ(geometry.logical_x, 96);
  EXPECT_EQ(geometry.logical_y, 192);
  EXPECT_EQ(geometry.logical_width, 2560);
  EXPECT_EQ(geometry.logical_height, 1440);
  EXPECT_EQ(geometry.pixel_width, 3840);
  EXPECT_EQ(geometry.pixel_height, 2160);
  EXPECT_EQ(geometry.refresh_mhz, 144000);
  EXPECT_TRUE(geometry.enabled);
  EXPECT_DOUBLE_EQ(meow::display_union::effective_scale(geometry), 1.5);
}

TEST(DisplayUnionCharacterizationTest, UnionGeometryFallsBackToWlOutputWithoutXdgOutput) {
  output_report_t report;
  report.name = "DP-1";
  report.wl_x = 100;
  report.wl_y = 200;
  report.mode_width = 3840;
  report.mode_height = 2160;
  report.wl_scale = 2;

  const auto geometry = describe_output(report);
  EXPECT_EQ(geometry.logical_x, 100);
  EXPECT_EQ(geometry.logical_y, 200);
  // Left at 0 so the mode/scale fallback in effective_logical_* takes over.
  EXPECT_EQ(geometry.logical_width, 0);
  EXPECT_EQ(geometry.logical_height, 0);
  EXPECT_EQ(meow::display_union::effective_logical_width(geometry), 1920);
  EXPECT_EQ(meow::display_union::effective_logical_height(geometry), 1080);
}

TEST(DisplayUnionCharacterizationTest, TransformSurvivesTheReportConversion) {
  output_report_t report;
  report.name = "DP-1";
  report.mode_width = 1920;
  report.mode_height = 1080;
  report.transform = output_transform_t::rotate_270;
  report.has_xdg_logical_size = true;
  report.xdg_logical_width = 1080;
  report.xdg_logical_height = 1920;

  const auto geometry = describe_output(report);
  EXPECT_EQ(geometry.transform, output_transform_t::rotate_270);
  EXPECT_DOUBLE_EQ(meow::display_union::effective_scale(geometry), 1.0);
}

TEST(DisplayUnionBackendWarningTest, WarnsWhenTheReservedNameIsUsedOffTheKwinBackend) {
  const auto warning = union_backend_warning(false, union_output_name);
  EXPECT_FALSE(warning.empty());
  EXPECT_NE(warning.find("capture = kwin"), std::string::npos);
}

TEST(DisplayUnionBackendWarningTest, StaysQuietOnKwinOrForRealOutputNames) {
  EXPECT_TRUE(union_backend_warning(true, union_output_name).empty());
  EXPECT_TRUE(union_backend_warning(false, "").empty());
  EXPECT_TRUE(union_backend_warning(false, "HDMI-A-1").empty());
  EXPECT_TRUE(union_backend_warning(true, "HDMI-A-1").empty());
  // Not the reserved name, so nothing to warn about — the exact match is the contract.
  EXPECT_TRUE(union_backend_warning(false, "All").empty());
}
