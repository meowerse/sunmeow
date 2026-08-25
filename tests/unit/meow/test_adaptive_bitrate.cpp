/**
 * @file tests/unit/meow/test_adaptive_bitrate.cpp
 * @brief Test src/meow/adaptive_bitrate.h.
 *
 * The controller is a pure state machine over a synthetic clock, so every case here drives
 * real behaviour: a loss sequence goes in, a bitrate trajectory comes out, and the assertions
 * are on the trajectory. Nothing is mocked (CLAUDE.md §5.4).
 */
// test includes
#include "../../tests_common.h"

// standard includes
#include <chrono>
#include <cstdint>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

// local includes
#include <src/meow/adaptive_bitrate.h>

namespace {

  using meow::adaptive_bitrate::bounds_t;
  using meow::adaptive_bitrate::controller_t;
  using meow::adaptive_bitrate::decision_t;
  using meow::adaptive_bitrate::floor_kbps;
  using meow::adaptive_bitrate::frame_fec_status_size;
  using meow::adaptive_bitrate::loss_sample_t;
  using meow::adaptive_bitrate::max_configurable_kbps;
  using meow::adaptive_bitrate::parse_frame_fec_status;
  using meow::adaptive_bitrate::rate_shape_t;
  using meow::adaptive_bitrate::rates_for;
  using meow::adaptive_bitrate::reason_t;
  using meow::adaptive_bitrate::resolve_bounds;
  using meow::adaptive_bitrate::tuning_t;
  using meow::adaptive_bitrate::validate_config;

  using namespace std::chrono_literals;

  /**
   * @brief Build a well-formed `SS_FRAME_FEC_STATUS` payload.
   *
   * Mirrors the client's `BE32`/`BE16` writes exactly, so a byte-order regression in the
   * parser fails these tests rather than silently reading garbage.
   *
   * @param total_data Data packets the host sent.
   * @param total_parity Parity packets the host sent.
   * @param received_data Data packets the client received.
   * @param received_parity Parity packets the client received.
   * @return The 21-byte payload.
   */
  std::string make_report(
    const std::uint16_t total_data,
    const std::uint16_t total_parity,
    const std::uint16_t received_data,
    const std::uint16_t received_parity
  ) {
    std::string p(frame_fec_status_size, '\0');
    auto put16 = [&p](const std::size_t off, const std::uint16_t v) {
      p[off] = static_cast<char>((v >> 8) & 0xFF);
      p[off + 1] = static_cast<char>(v & 0xFF);
    };
    // frameIndex (BE32) at 0 — the controller does not read it, but a realistic payload has it.
    p[0] = 0;
    p[1] = 0;
    p[2] = 1;
    p[3] = 44;
    put16(4, 100);  // highestReceivedSequenceNumber
    put16(6, 90);  // nextContiguousSequenceNumber
    put16(8, static_cast<std::uint16_t>(total_data - received_data));  // missingPacketsBeforeHighestReceived
    put16(10, total_data);
    put16(12, total_parity);
    put16(14, received_data);
    put16(16, received_parity);
    p[18] = 20;  // fecPercentage
    p[19] = 0;  // multiFecBlockIndex
    p[20] = 1;  // multiFecBlockCount
    return p;
  }

  /**
   * @brief A controller with compressed timings, so tests run without sleeping.
   *
   * Only the durations change; every threshold and streak length is the shipping value, so
   * the behaviour under test is the behaviour that ships.
   *
   * @return Tuning with a 100ms window and a 300ms minimum change interval.
   */
  tuning_t fast_tuning() {
    tuning_t t;
    t.window = 100ms;
    t.min_change_interval = 300ms;
    return t;
  }

  /**
   * @brief Frames the encoder produces per evaluation window in these tests.
   *
   * A real 60 fps stream encodes ~60 frames per 1 s window. The controller divides the number
   * of damaged frames by this, so tests must model it or they assert against a denominator
   * that never occurs in production.
   */
  constexpr int frames_per_window = 60;

  /**
   * @brief Drive the controller for a number of windows with a fixed damage rate.
   *
   * Each window encodes `frames_per_window` frames (one `tick()` each, which is exactly how
   * the governor calls it) of which `damaged_frames` are reported by the client as damaged.
   *
   * @param controller Controller under test.
   * @param now In/out clock, advanced across one window per iteration.
   * @param windows How many windows to run.
   * @param damaged_frames How many frames per window the client reports as damaged.
   * @param recovered Whether FEC rebuilt those frames.
   * @param tuning Tuning in use, for the window length.
   * @param packets_per_frame Packets in a reported frame.
   * @param lost_per_frame Packets lost in a reported frame.
   * @return Every decision that actually changed the bitrate.
   */
  std::vector<decision_t> run_windows(
    controller_t &controller,
    std::chrono::steady_clock::time_point &now,
    const int windows,
    const int damaged_frames,
    const bool recovered,
    const tuning_t &tuning,
    const std::uint32_t packets_per_frame = 19,
    const std::uint32_t lost_per_frame = 1
  ) {
    std::vector<decision_t> changes;
    const auto per_frame = tuning.window / frames_per_window;
    for (int w = 0; w < windows; ++w) {
      for (int f = 0; f < frames_per_window; ++f) {
        if (f < damaged_frames) {
          controller.observe(loss_sample_t {packets_per_frame, lost_per_frame, recovered});
        }
        // The last frame of the window crosses the boundary and triggers the decision.
        now += (f + 1 == frames_per_window) ? (tuning.window - per_frame * (frames_per_window - 1)) : per_frame;
        const auto d = controller.tick(now);
        if (d.changed) {
          changes.push_back(d);
        }
      }
    }
    return changes;
  }

  /**
   * @brief Run exactly one evaluation window and return the decision it produced.
   *
   * @param controller Controller under test.
   * @param now In/out clock, advanced by one window.
   * @param damaged_frames Frames in this window the client reports as damaged.
   * @param recovered Whether FEC rebuilt those frames.
   * @param tuning Tuning in use.
   * @param packets_per_frame Packets in a reported frame.
   * @param lost_per_frame Packets lost in a reported frame.
   * @return The decision from the window boundary.
   */
  decision_t run_one_window(
    controller_t &controller,
    std::chrono::steady_clock::time_point &now,
    const int damaged_frames,
    const bool recovered,
    const tuning_t &tuning,
    const std::uint32_t packets_per_frame = 19,
    const std::uint32_t lost_per_frame = 1
  ) {
    const auto changes = run_windows(controller, now, 1, damaged_frames, recovered, tuning, packets_per_frame, lost_per_frame);
    return changes.empty() ? decision_t {} : changes.front();
  }

  // ---------------------------------------------------------------------------
  // parse_frame_fec_status — the payload is hostile input (CLAUDE.md §7)
  // ---------------------------------------------------------------------------

  TEST(AdaptiveBitrateParseTest, DecodesAWellFormedReport) {
    // 80 data + 20 parity sent; 70 data + 20 parity received => 10 of 100 lost.
    const auto sample = parse_frame_fec_status(make_report(80, 20, 70, 20));
    ASSERT_TRUE(sample.has_value());
    EXPECT_EQ(sample->packets_sent, 100u);
    EXPECT_EQ(sample->packets_lost, 10u);
    EXPECT_FALSE(sample->frame_recovered);
  }

  TEST(AdaptiveBitrateParseTest, MarksAFullyReceivedFrameAsRecovered) {
    const auto sample = parse_frame_fec_status(make_report(80, 20, 80, 5));
    ASSERT_TRUE(sample.has_value());
    EXPECT_EQ(sample->packets_sent, 100u);
    EXPECT_EQ(sample->packets_lost, 15u);
    // All data shards arrived, so the frame itself was fine even though parity was lost.
    EXPECT_TRUE(sample->frame_recovered);
  }

  TEST(AdaptiveBitrateParseTest, ReadsBigEndianNotHostOrder) {
    // 0x0100 == 256. A little-endian misread would see 1.
    const auto sample = parse_frame_fec_status(make_report(256, 0, 0, 0));
    ASSERT_TRUE(sample.has_value());
    EXPECT_EQ(sample->packets_sent, 256u);
    EXPECT_EQ(sample->packets_lost, 256u);
  }

  TEST(AdaptiveBitrateParseTest, RejectsEveryPayloadLengthButTheExactOne) {
    // The upstream IDX_LOSS_STATS handler reads four int32s with no length check at all;
    // this asserts we never repeat that out-of-bounds read.
    for (std::size_t len = 0; len <= 64; ++len) {
      if (len == frame_fec_status_size) {
        continue;
      }
      const std::string payload(len, '\xAB');
      EXPECT_FALSE(parse_frame_fec_status(payload).has_value()) << "accepted length " << len;
    }
  }

  TEST(AdaptiveBitrateParseTest, RejectsAnEmptyPayload) {
    EXPECT_FALSE(parse_frame_fec_status(std::string_view {}).has_value());
  }

  TEST(AdaptiveBitrateParseTest, RejectsReceivingMoreThanWasSent) {
    // A client cannot receive packets that were never sent; guessing what it meant would let
    // it steer the controller.
    EXPECT_FALSE(parse_frame_fec_status(make_report(10, 5, 11, 5)).has_value());
    EXPECT_FALSE(parse_frame_fec_status(make_report(10, 5, 10, 6)).has_value());
  }

  TEST(AdaptiveBitrateParseTest, RejectsAFrameWithNoPackets) {
    // This is the only division-by-zero the controller could have had.
    EXPECT_FALSE(parse_frame_fec_status(make_report(0, 0, 0, 0)).has_value());
  }

  TEST(AdaptiveBitrateParseTest, AcceptsTheMaximumRepresentableCounts) {
    const auto sample = parse_frame_fec_status(make_report(65535, 65535, 0, 0));
    ASSERT_TRUE(sample.has_value());
    // Sum must not have wrapped a 16-bit accumulator.
    EXPECT_EQ(sample->packets_sent, 131070u);
    EXPECT_EQ(sample->packets_lost, 131070u);
  }

  // ---------------------------------------------------------------------------
  // resolve_bounds — the client's request and max_bitrate stay hard limits
  // ---------------------------------------------------------------------------

  TEST(AdaptiveBitrateBoundsTest, DisabledWhenNoMinimumIsConfigured) {
    EXPECT_FALSE(resolve_bounds(0, 20000, 20000, 0).enabled());
  }

  TEST(AdaptiveBitrateBoundsTest, CeilingIsTheClientRequestWhenItIsLowest) {
    const auto b = resolve_bounds(2000, 30000, 8000, 0);
    EXPECT_TRUE(b.enabled());
    EXPECT_EQ(b.max_kbps, 8000);
    EXPECT_EQ(b.min_kbps, 2000);
  }

  TEST(AdaptiveBitrateBoundsTest, CeilingRespectsTheExistingMaxBitrate) {
    const auto b = resolve_bounds(2000, 30000, 20000, 6000);
    EXPECT_EQ(b.max_kbps, 6000) << "host max_bitrate must still cap the stream";
  }

  TEST(AdaptiveBitrateBoundsTest, CeilingRespectsTheConfiguredAdaptiveMaximum) {
    const auto b = resolve_bounds(2000, 5000, 20000, 0);
    EXPECT_EQ(b.max_kbps, 5000);
  }

  TEST(AdaptiveBitrateBoundsTest, CeilingIsTheMinimumOfAllThreeLimits) {
    EXPECT_EQ(resolve_bounds(1000, 9000, 7000, 8000).max_kbps, 7000);
    EXPECT_EQ(resolve_bounds(1000, 7000, 9000, 8000).max_kbps, 7000);
    EXPECT_EQ(resolve_bounds(1000, 8000, 9000, 7000).max_kbps, 7000);
  }

  TEST(AdaptiveBitrateBoundsTest, NeverRaisesTheStreamAboveWhatTheClientAskedFor) {
    // Client asked for less than our configured floor: the client wins and adaptation is off,
    // which is exactly today's behaviour rather than a quiet upgrade to 5000.
    const auto b = resolve_bounds(5000, 20000, 2000, 0);
    EXPECT_FALSE(b.enabled());
  }

  TEST(AdaptiveBitrateBoundsTest, DisabledWhenTheRangeCollapses) {
    EXPECT_FALSE(resolve_bounds(5000, 5000, 5000, 0).enabled());
    EXPECT_FALSE(resolve_bounds(5000, 20000, 5000, 0).enabled());
  }

  TEST(AdaptiveBitrateBoundsTest, DisabledForANonPositiveClientRequest) {
    EXPECT_FALSE(resolve_bounds(2000, 20000, 0, 0).enabled());
    EXPECT_FALSE(resolve_bounds(2000, 20000, -1, 0).enabled());
  }

  TEST(AdaptiveBitrateBoundsTest, TheResolvedCeilingNeverExceedsAnyOfTheThreeLimits) {
    // The governor applies this ceiling to the codec context at open time, because upstream
    // opens the encoder at min(client, max_bitrate) and knows nothing about
    // adaptive_bitrate_max. If this invariant ever broke, a session whose binding limit is
    // adaptive_bitrate_max would run above its configured ceiling for its whole lifetime,
    // since a clean link never produces a decision to bring it down.
    for (int client = 1000; client <= 20000; client += 3000) {
      for (const int host_max : {0, 4000, 9000, 25000}) {
        for (const int cfg_max : {0, 5000, 12000}) {
          const auto b = resolve_bounds(1000, cfg_max, client, host_max);
          if (!b.enabled()) {
            continue;
          }
          EXPECT_LE(b.max_kbps, client);
          if (host_max > 0) {
            EXPECT_LE(b.max_kbps, host_max);
          }
          if (cfg_max > 0) {
            EXPECT_LE(b.max_kbps, cfg_max);
          }
          EXPECT_GT(b.max_kbps, b.min_kbps);
        }
      }
    }
  }

  // ---------------------------------------------------------------------------
  // validate_config — hostile / mistyped configuration is corrected loudly
  // ---------------------------------------------------------------------------

  TEST(AdaptiveBitrateConfigTest, AcceptsASaneRangeSilently) {
    int lo = 2000;
    int hi = 8000;
    std::string warning;
    EXPECT_TRUE(validate_config(lo, hi, warning));
    EXPECT_EQ(lo, 2000);
    EXPECT_EQ(hi, 8000);
    EXPECT_TRUE(warning.empty());
  }

  TEST(AdaptiveBitrateConfigTest, AcceptsBothUnsetSilently) {
    int lo = 0;
    int hi = 0;
    std::string warning;
    EXPECT_TRUE(validate_config(lo, hi, warning));
    EXPECT_TRUE(warning.empty());
  }

  TEST(AdaptiveBitrateConfigTest, RejectsAnInvertedRangeAndSaysSo) {
    int lo = 9000;
    int hi = 3000;
    std::string warning;
    EXPECT_FALSE(validate_config(lo, hi, warning));
    EXPECT_EQ(lo, 0) << "an inverted range must disable, not silently swap";
    EXPECT_EQ(hi, 0);
    EXPECT_FALSE(warning.empty());
  }

  TEST(AdaptiveBitrateConfigTest, RejectsAnEqualRange) {
    int lo = 5000;
    int hi = 5000;
    std::string warning;
    EXPECT_FALSE(validate_config(lo, hi, warning));
    EXPECT_EQ(lo, 0);
    EXPECT_EQ(hi, 0);
    EXPECT_FALSE(warning.empty());
  }

  TEST(AdaptiveBitrateConfigTest, ClampsNegativesToDisabledAndWarns) {
    int lo = -5000;
    int hi = -1;
    std::string warning;
    EXPECT_FALSE(validate_config(lo, hi, warning));
    EXPECT_EQ(lo, 0);
    EXPECT_EQ(hi, 0);
    EXPECT_FALSE(warning.empty());
  }

  TEST(AdaptiveBitrateConfigTest, WarnsWhenAMaximumIsSetWithoutAMinimum) {
    int lo = 0;
    int hi = 8000;
    std::string warning;
    EXPECT_FALSE(validate_config(lo, hi, warning));
    EXPECT_EQ(hi, 0);
    EXPECT_NE(warning.find("no effect"), std::string::npos);
  }

  TEST(AdaptiveBitrateConfigTest, ClampsAMinimumBelowTheUsableFloor) {
    int lo = 1;
    int hi = 8000;
    std::string warning;
    EXPECT_FALSE(validate_config(lo, hi, warning));
    EXPECT_EQ(lo, floor_kbps);
    EXPECT_EQ(hi, 8000);
    EXPECT_FALSE(warning.empty());
  }

  TEST(AdaptiveBitrateConfigTest, ClampsAbsurdlyLargeValues) {
    int lo = 2000;
    int hi = 2000000000;
    std::string warning;
    EXPECT_FALSE(validate_config(lo, hi, warning));
    EXPECT_EQ(hi, max_configurable_kbps);
    EXPECT_FALSE(warning.empty());
  }

  TEST(AdaptiveBitrateConfigTest, ADisabledResultAlwaysResolvesToNoAdaptation) {
    // Whatever validate_config decides to zero, resolve_bounds must agree the feature is off.
    for (const auto [in_lo, in_hi] : std::vector<std::pair<int, int>> {{-1, -1}, {9000, 3000}, {0, 8000}, {5000, 5000}}) {
      int lo = in_lo;
      int hi = in_hi;
      std::string warning;
      validate_config(lo, hi, warning);
      if (lo == 0) {
        EXPECT_FALSE(resolve_bounds(lo, hi, 20000, 0).enabled());
      }
    }
  }

  // ---------------------------------------------------------------------------
  // controller_t — trajectory over synthetic loss sequences
  // ---------------------------------------------------------------------------

  TEST(AdaptiveBitrateControllerTest, StartsAtTheCeiling) {
    const auto t = fast_tuning();
    controller_t c {resolve_bounds(2000, 8000, 8000, 0), std::chrono::steady_clock::time_point {}, t};
    EXPECT_TRUE(c.enabled());
    EXPECT_EQ(c.current_kbps(), 8000) << "an upgrade must not lower an existing stream on its own";
  }

  TEST(AdaptiveBitrateControllerTest, DisabledControllerNeverChangesAnything) {
    const auto t = fast_tuning();
    auto now = std::chrono::steady_clock::time_point {};
    controller_t c {resolve_bounds(0, 0, 8000, 0), now, t};
    EXPECT_FALSE(c.enabled());
    const auto changes = run_windows(c, now, 200, 55, false, t);
    EXPECT_TRUE(changes.empty());
    EXPECT_EQ(c.current_kbps(), 0);
  }

  TEST(AdaptiveBitrateControllerTest, IgnoresASingleBadWindow) {
    const auto t = fast_tuning();
    auto now = std::chrono::steady_clock::time_point {};
    controller_t c {resolve_bounds(2000, 8000, 8000, 0), now, t};

    // One bad window, then a clean one. A single bad window must never move the bitrate.
    EXPECT_FALSE(run_one_window(c, now, 55, false, t).changed);
    EXPECT_FALSE(run_one_window(c, now, 0, true, t).changed);
    EXPECT_EQ(c.current_kbps(), 8000);
  }

  TEST(AdaptiveBitrateControllerTest, BacksOffOnSustainedLoss) {
    const auto t = fast_tuning();
    auto now = std::chrono::steady_clock::time_point {};
    controller_t c {resolve_bounds(2000, 8000, 8000, 0), now, t};

    const auto changes = run_windows(c, now, 8, 55, false, t);
    ASSERT_FALSE(changes.empty());
    EXPECT_EQ(changes.front().reason, reason_t::sustained_loss);
    EXPECT_LT(changes.front().kbps, changes.front().previous_kbps);
    EXPECT_LT(c.current_kbps(), 8000);
  }

  TEST(AdaptiveBitrateControllerTest, NeverGoesBelowTheConfiguredMinimum) {
    const auto t = fast_tuning();
    auto now = std::chrono::steady_clock::time_point {};
    controller_t c {resolve_bounds(2000, 8000, 8000, 0), now, t};

    // Pathological: total loss forever. This is also the shape a malicious client would use
    // to try to pin the stream to zero.
    run_windows(c, now, 2000, 60, false, t);
    EXPECT_EQ(c.current_kbps(), 2000);
    EXPECT_GT(c.current_kbps(), 0);
  }

  TEST(AdaptiveBitrateControllerTest, NeverGoesAboveTheCeiling) {
    const auto t = fast_tuning();
    auto now = std::chrono::steady_clock::time_point {};
    controller_t c {resolve_bounds(2000, 8000, 8000, 0), now, t};

    // A perfectly clean link for a long time must sit at the ceiling, not climb past it.
    run_windows(c, now, 2000, 0, true, t);
    EXPECT_EQ(c.current_kbps(), 8000);
  }

  TEST(AdaptiveBitrateControllerTest, RecoversTowardTheCeilingWhenTheLinkClears) {
    const auto t = fast_tuning();
    auto now = std::chrono::steady_clock::time_point {};
    controller_t c {resolve_bounds(2000, 8000, 8000, 0), now, t};

    run_windows(c, now, 20, 55, false, t);
    const int after_loss = c.current_kbps();
    ASSERT_LT(after_loss, 8000);

    const auto recoveries = run_windows(c, now, 400, 0, true, t);
    ASSERT_FALSE(recoveries.empty());
    EXPECT_EQ(recoveries.front().reason, reason_t::link_clean);
    EXPECT_GT(c.current_kbps(), after_loss);
    EXPECT_EQ(c.current_kbps(), 8000) << "a long clean period should reach the ceiling";
  }

  TEST(AdaptiveBitrateControllerTest, RecoveryIsSlowerThanBackOff) {
    const auto t = fast_tuning();

    // Windows needed to make the first back-off happen.
    auto now_down = std::chrono::steady_clock::time_point {};
    controller_t down {resolve_bounds(2000, 8000, 8000, 0), now_down, t};
    int windows_to_back_off = 0;
    for (int i = 0; i < 500; ++i) {
      ++windows_to_back_off;
      if (run_one_window(down, now_down, 55, false, t).changed) {
        break;
      }
    }

    // Windows needed to make the first recovery happen, from the same starting state.
    auto now_up = std::chrono::steady_clock::time_point {};
    controller_t up {resolve_bounds(2000, 8000, 8000, 0), now_up, t};
    run_windows(up, now_up, 20, 55, false, t);
    int windows_to_recover = 0;
    for (int i = 0; i < 500; ++i) {
      ++windows_to_recover;
      if (run_one_window(up, now_up, 0, true, t).changed) {
        break;
      }
    }

    EXPECT_GT(windows_to_recover, windows_to_back_off)
      << "congestion control must be asymmetric: back off fast, recover slowly";
  }

  TEST(AdaptiveBitrateControllerTest, HoldsStillInsideTheDeadZone) {
    const auto t = fast_tuning();
    auto now = std::chrono::steady_clock::time_point {};
    controller_t c {resolve_bounds(2000, 8000, 8000, 0), now, t};

    // 2% loss: above loss_low (0.5%), below loss_high (5%). Neither direction is justified,
    // so the bitrate must not move at all — this is the band that kills oscillation.
    const auto changes = run_windows(c, now, 600, 5, true, t);
    EXPECT_TRUE(changes.empty());
    EXPECT_EQ(c.current_kbps(), 8000);
  }

  TEST(AdaptiveBitrateControllerTest, DoesNotOscillateOnAFlappingLink) {
    const auto t = fast_tuning();
    auto now = std::chrono::steady_clock::time_point {};
    controller_t c {resolve_bounds(2000, 8000, 8000, 0), now, t};

    // Alternate one bad window with one clean window for a long time. Neither streak ever
    // reaches its threshold, so a naive controller would flip on every window.
    int changes = 0;
    for (int i = 0; i < 1000; ++i) {
      if (run_one_window(c, now, i % 2 == 0 ? 55 : 0, i % 2 != 0, t).changed) {
        ++changes;
      }
    }
    EXPECT_EQ(changes, 0) << "alternating windows must never reach a streak threshold";
  }

  TEST(AdaptiveBitrateControllerTest, ChangesAreRareEvenUnderHostileChurn) {
    const auto t = fast_tuning();
    auto now = std::chrono::steady_clock::time_point {};
    controller_t c {resolve_bounds(2000, 8000, 8000, 0), now, t};

    // Worst realistic case: long bad stretches alternating with long clean stretches, i.e. a
    // client deliberately trying to make the encoder rebuild as often as possible. Every
    // change costs one IDR, so the count is the thing that matters.
    int changes = 0;
    for (int block = 0; block < 100; ++block) {
      const bool bad = (block % 2) == 0;
      for (int i = 0; i < 12; ++i) {
        if (run_one_window(c, now, bad ? 55 : 0, !bad, t).changed) {
          ++changes;
        }
      }
    }
    // 1200 windows. A per-window controller would change 1200 times.
    EXPECT_LT(changes, 200) << "observed " << changes << " changes in 1200 windows";
    EXPECT_GE(c.current_kbps(), 2000);
    EXPECT_LE(c.current_kbps(), 8000);
  }

  TEST(AdaptiveBitrateControllerTest, RespectsTheMinimumIntervalBetweenChanges) {
    tuning_t t = fast_tuning();
    t.min_change_interval = 5000ms;  // far longer than the streak requirement
    auto now = std::chrono::steady_clock::time_point {};
    const auto start = now;
    controller_t c {resolve_bounds(2000, 8000, 8000, 0), now, t};

    std::vector<std::chrono::steady_clock::time_point> change_times;
    for (int i = 0; i < 400; ++i) {
      if (run_one_window(c, now, 60, false, t).changed) {
        change_times.push_back(now);
      }
    }
    ASSERT_GE(change_times.size(), 2u);
    EXPECT_GE(change_times.front() - start, t.min_change_interval);
    for (std::size_t i = 1; i < change_times.size(); ++i) {
      EXPECT_GE(change_times[i] - change_times[i - 1], t.min_change_interval)
        << "two changes were closer together than min_change_interval";
    }
  }

  TEST(AdaptiveBitrateControllerTest, AFloodOfReportsCannotPinTheBitrateToZero) {
    const auto t = fast_tuning();
    auto now = std::chrono::steady_clock::time_point {};
    controller_t c {resolve_bounds(2000, 8000, 8000, 0), now, t};

    // Hostile client: the largest loss report the protocol can express, as fast as it can
    // send it. The accumulators must saturate rather than wrap into a state that inverts a
    // comparison, and the output must stay inside the configured bounds.
    for (int i = 0; i < 400; ++i) {
      // 500 maximum-magnitude reports per frame: far more than any honest client sends, which
      // is the point - the accumulators must saturate rather than wrap into a state that
      // inverts a comparison.
      run_one_window(c, now, frames_per_window, false, t, 131070, 131070);
      for (int j = 0; j < 499; ++j) {
        c.observe(loss_sample_t {131070, 131070, false});
      }
      EXPECT_GE(c.current_kbps(), 2000);
      EXPECT_LE(c.current_kbps(), 8000);
    }
    EXPECT_EQ(c.current_kbps(), 2000);
  }

  TEST(AdaptiveBitrateControllerTest, SurvivesAClockThatJumpsBackwards) {
    const auto t = fast_tuning();
    auto now = std::chrono::steady_clock::time_point {} + 1h;
    controller_t c {resolve_bounds(2000, 8000, 8000, 0), now, t};

    // A backwards jump must not wedge the controller: after it, normal windows still work.
    now -= 30min;
    EXPECT_FALSE(c.tick(now).changed);

    const auto changes = run_windows(c, now, 20, 55, false, t);
    EXPECT_FALSE(changes.empty()) << "controller stopped adapting after a backwards clock jump";
  }

  TEST(AdaptiveBitrateControllerTest, AWindowWithNoReportsCountsAsClean) {
    const auto t = fast_tuning();
    auto now = std::chrono::steady_clock::time_point {};
    controller_t c {resolve_bounds(2000, 8000, 8000, 0), now, t};

    run_windows(c, now, 20, 55, false, t);
    const int after_loss = c.current_kbps();
    ASSERT_LT(after_loss, 8000);

    // The client only reports when something went wrong, so silence is the clean signal.
    run_windows(c, now, 400, 0, true, t);
    EXPECT_GT(c.current_kbps(), after_loss);
  }

  TEST(AdaptiveBitrateControllerTest, AnUnrecoveredFrameBlocksRecovery) {
    const auto t = fast_tuning();
    auto now = std::chrono::steady_clock::time_point {};
    controller_t c {resolve_bounds(2000, 8000, 8000, 0), now, t};

    run_windows(c, now, 20, 55, false, t);
    const int after_loss = c.current_kbps();

    // Loss fraction is tiny (below loss_low) but FEC failed outright every window: the user
    // is seeing dropped frames, so climbing back up would be wrong.
    const auto changes = run_windows(c, now, 400, 1, false, t);
    EXPECT_TRUE(changes.empty());
    EXPECT_EQ(c.current_kbps(), after_loss);
  }

  TEST(AdaptiveBitrateControllerTest, EveryChangeReportsBothEndpointsForLogging) {
    const auto t = fast_tuning();
    auto now = std::chrono::steady_clock::time_point {};
    controller_t c {resolve_bounds(2000, 8000, 8000, 0), now, t};

    const auto changes = run_windows(c, now, 40, 55, false, t);
    ASSERT_FALSE(changes.empty());
    for (const auto &d : changes) {
      EXPECT_NE(d.kbps, d.previous_kbps);
      EXPECT_NE(d.reason, reason_t::none);
      EXPECT_FALSE(meow::adaptive_bitrate::describe(d.reason).empty());
    }
  }

  TEST(AdaptiveBitrateControllerTest, AnOrdinaryMobileLinkIsNotRatchetedToTheFloor) {
    // THE regression this controller exists to avoid, and the reason the denominator is
    // frames-encoded rather than packets-reported.
    //
    // One FEC-recovered frame per second on a 60 fps stream is an entirely ordinary mobile
    // link: the frame was fully repaired, the user saw nothing. At 8 Mbps a frame is ~19
    // packets, so measuring "lost packets / reported packets" reads 1/19 = 5.3% and trips a
    // 5% threshold, ratcheting 8000 -> 3000 kbps in about eleven seconds. Measuring
    // "damaged frames / frames encoded" reads 1/60 = 1.7% and correctly leaves it alone.
    const auto t = fast_tuning();
    auto now = std::chrono::steady_clock::time_point {};
    controller_t c {resolve_bounds(3000, 8000, 8000, 0), now, t};

    const auto changes = run_windows(c, now, 300, 1, true, t, 19, 1);

    for (const auto &d : changes) {
      EXPECT_GE(d.kbps, d.previous_kbps)
        << "backed off on a link losing ~0.1% of packets: " << d.previous_kbps << " -> " << d.kbps;
    }
    EXPECT_EQ(c.current_kbps(), 8000) << "an ordinary mobile link must stay at the ceiling";
  }

  TEST(AdaptiveBitrateControllerTest, StillBacksOffWhenMostFramesAreDamaged) {
    // The other side of the same coin: the frame-based denominator must not make the
    // controller blind. Most frames damaged is a genuinely overloaded link.
    const auto t = fast_tuning();
    auto now = std::chrono::steady_clock::time_point {};
    controller_t c {resolve_bounds(3000, 8000, 8000, 0), now, t};

    run_windows(c, now, 40, 40, false, t);
    EXPECT_LT(c.current_kbps(), 8000);
  }

  TEST(AdaptiveBitrateControllerTest, RecoversAcrossANarrowRangeToo) {
    // step_up() proposes a fraction of the ceiling; commit() rejects steps below
    // min_step_kbps unless they land on a bound. Back-off gets that exemption by landing on
    // min_kbps, so without a floor on the proposed step a narrow range could go down but
    // never come back up.
    const auto t = fast_tuning();
    for (const auto [lo, hi] : std::vector<std::pair<int, int>> {{900, 1900}, {500, 1500}, {1000, 1900}, {500, 1900}}) {
      auto now = std::chrono::steady_clock::time_point {};
      controller_t c {resolve_bounds(lo, hi, hi, 0), now, t};
      ASSERT_TRUE(c.enabled()) << lo << "/" << hi;

      run_windows(c, now, 40, 55, false, t);
      ASSERT_LT(c.current_kbps(), hi) << lo << "/" << hi << " never backed off";

      run_windows(c, now, 600, 0, true, t);
      EXPECT_EQ(c.current_kbps(), hi)
        << "range " << lo << "/" << hi << " could go down but not come back up";
    }
  }

  TEST(AdaptiveBitrateControllerTest, ResumesFromARememberedBitrateAcrossAReinit) {
    // encode_run() is re-entered on every capture reinit, destroying the governor. A session
    // that had backed off must not snap back to the ceiling and re-converge from scratch.
    const auto t = fast_tuning();
    auto now = std::chrono::steady_clock::time_point {};
    controller_t first {resolve_bounds(2000, 8000, 8000, 0), now, t};
    run_windows(first, now, 40, 55, false, t);
    const int carried = first.current_kbps();
    ASSERT_LT(carried, 8000);

    controller_t second {resolve_bounds(2000, 8000, 8000, 0), now, t, carried};
    EXPECT_EQ(second.current_kbps(), carried);
  }

  TEST(AdaptiveBitrateControllerTest, AResumedBitrateIsClampedIntoTheNewBounds) {
    // The remembered value comes from a previous session whose ceiling may have been
    // different (the client can renegotiate on reinit), so it is not trusted blind.
    const auto t = fast_tuning();
    const auto now = std::chrono::steady_clock::time_point {};
    const auto bounds = resolve_bounds(2000, 5000, 5000, 0);

    EXPECT_EQ(controller_t(bounds, now, t, 99999).current_kbps(), bounds.max_kbps);
    EXPECT_EQ(controller_t(bounds, now, t, 1).current_kbps(), bounds.min_kbps);
    EXPECT_EQ(controller_t(bounds, now, t, 0).current_kbps(), bounds.max_kbps);
    EXPECT_EQ(controller_t(bounds, now, t, -50).current_kbps(), bounds.max_kbps);
  }

  // ---------------------------------------------------------------------------
  // rate_shape_t / rates_for — the math that actually writes to the encoder
  // ---------------------------------------------------------------------------

  TEST(AdaptiveBitrateRateShapeTest, PreservesCbrShape) {
    // video.cpp CBR path: bit_rate == rc_max_rate == rc_min_rate, VBV one frame.
    const auto shape = rate_shape_t::capture(8000000, 8000000, 8000000, 8000000 / 60);
    EXPECT_TRUE(shape.cbr);
    EXPECT_EQ(shape.vbr_offset, 0);

    const auto r = rates_for(shape, 3000);
    EXPECT_EQ(r.bit_rate, 3000000);
    EXPECT_EQ(r.rc_max_rate, 3000000);
    EXPECT_EQ(r.rc_min_rate, 3000000) << "CBR must stay CBR after a change";
    EXPECT_NEAR(r.rc_buffer_size, 3000000 / 60, 2) << "VBV must scale with the bitrate";
  }

  TEST(AdaptiveBitrateRateShapeTest, PreservesTheForcedVbrOffset) {
    // video.cpp CBR_WITH_VBR path: bit_rate = rc_max_rate - 1 to force VBR mode, and
    // rc_min_rate is left at 0. Losing that -1 would silently switch the encoder's mode.
    const auto shape = rate_shape_t::capture(8000000 - 1, 8000000, 0, 8000000 / 60);
    EXPECT_FALSE(shape.cbr);
    EXPECT_EQ(shape.vbr_offset, 1);

    const auto r = rates_for(shape, 3000);
    EXPECT_EQ(r.rc_max_rate, 3000000);
    EXPECT_EQ(r.bit_rate, 2999999) << "rc_max_rate != bit_rate is what forces VBR";
    EXPECT_EQ(r.rc_min_rate, 0) << "must not pin rc_min_rate on a VBR session";
  }

  TEST(AdaptiveBitrateRateShapeTest, PreservesAnEnlargedVbvBuffer) {
    // nvenc vbv_percentage_increase = 40 enlarges the buffer by 40%. Re-deriving the buffer
    // as bitrate/framerate instead of scaling it would silently discard that setting.
    const auto base = 8000000 / 60;
    const auto enlarged = base + base * 40 / 100;
    const auto shape = rate_shape_t::capture(8000000, 8000000, 8000000, enlarged);

    const auto r = rates_for(shape, 4000);
    EXPECT_NEAR(r.rc_buffer_size, enlarged / 2, 2) << "the 40% enlargement must survive";
    EXPECT_GT(r.rc_buffer_size, 4000000 / 60) << "buffer collapsed back to one frame";
  }

  TEST(AdaptiveBitrateRateShapeTest, LeavesAnUnlimitedBufferAlone) {
    // NO_RC_BUF_LIMIT encoders open with rc_buffer_size == 0.
    const auto shape = rate_shape_t::capture(8000000, 8000000, 8000000, 0);
    EXPECT_DOUBLE_EQ(shape.buffer_ratio, 0.0);
    EXPECT_EQ(rates_for(shape, 3000).rc_buffer_size, 0);
  }

  TEST(AdaptiveBitrateRateShapeTest, ARoundTripAtTheOpeningRateIsIdentity) {
    // Applying the opening bitrate must reproduce the opening values exactly, or the very
    // first adjustment would perturb the encoder for no reason.
    for (const auto [bit_rate, max_rate, min_rate, buf] :
         std::vector<std::tuple<std::int64_t, std::int64_t, std::int64_t, int>> {
           {8000000, 8000000, 8000000, 8000000 / 60},
           {7999999, 8000000, 0, 8000000 / 60},
           {8000000, 8000000, 8000000, 0},
         }) {
      const auto shape = rate_shape_t::capture(bit_rate, max_rate, min_rate, buf);
      const auto r = rates_for(shape, 8000);
      EXPECT_EQ(r.bit_rate, bit_rate);
      EXPECT_EQ(r.rc_max_rate, max_rate);
      EXPECT_EQ(r.rc_min_rate, min_rate);
      EXPECT_NEAR(r.rc_buffer_size, buf, 1);
    }
  }

  TEST(AdaptiveBitrateRateShapeTest, ASessionWithNoRateLimitCapturesNothing) {
    const auto shape = rate_shape_t::capture(0, 0, 0, 0);
    EXPECT_FALSE(shape.cbr);
    EXPECT_EQ(shape.vbr_offset, 0);
    EXPECT_DOUBLE_EQ(shape.buffer_ratio, 0.0);
  }

  TEST(AdaptiveBitrateControllerTest, EndToEndFromParsedReportsMatchesTheTrajectory) {
    // Same sequence, but fed through the real wire parser rather than synthetic samples, so
    // a parser/controller mismatch cannot hide behind hand-built structs.
    const auto t = fast_tuning();
    auto now = std::chrono::steady_clock::time_point {};
    controller_t c {resolve_bounds(2000, 8000, 8000, 0), now, t};

    const auto lossy = make_report(800, 200, 400, 100);  // 500 of 1000 lost
    const auto per_frame = t.window / frames_per_window;
    for (int w = 0; w < 20; ++w) {
      for (int f = 0; f < frames_per_window; ++f) {
        if (f < 55) {
          const auto sample = parse_frame_fec_status(lossy);
          ASSERT_TRUE(sample.has_value());
          c.observe(*sample);
        }
        now += (f + 1 == frames_per_window) ? (t.window - per_frame * (frames_per_window - 1)) : per_frame;
        c.tick(now);
      }
    }
    EXPECT_LT(c.current_kbps(), 8000);
    EXPECT_GE(c.current_kbps(), 2000);
  }

}  // namespace
