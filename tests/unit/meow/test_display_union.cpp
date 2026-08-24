/**
 * @file tests/unit/meow/test_display_union.cpp
 * @brief Test src/meow/display_union.h.
 */
// test includes
#include "../../tests_common.h"

// standard includes
#include <string>
#include <vector>

// local includes
#include <src/meow/display_union.h>

namespace {

  using meow::display_union::compute_union;
  using meow::display_union::is_union_output_name;
  using meow::display_union::output_geometry_t;

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

}  // namespace

TEST(DisplayUnionNameTest, MatchesReservedNameCaseInsensitively) {
  EXPECT_TRUE(is_union_output_name("all"));
  EXPECT_TRUE(is_union_output_name("ALL"));
  EXPECT_TRUE(is_union_output_name("All"));
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
