/**
 * @file tests/unit/meow/test_adaptive_bitrate.cpp
 * @brief Test src/meow/adaptive_bitrate.h.
 *
 * The controller is a pure state machine over a synthetic clock, so every case here drives
 * real behaviour: a sequence of loss, delay and goodput conditions goes in, a bitrate
 * trajectory comes out, and the assertions are on the trajectory. Nothing is mocked
 * (CLAUDE.md §5.4). The wire vectors are the client's own test vectors, byte for byte.
 */
// test includes
#include "../../tests_common.h"

// standard includes
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <optional>
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
  using meow::adaptive_bitrate::parse_receiver_report;
  using meow::adaptive_bitrate::rate_shape_t;
  using meow::adaptive_bitrate::rates_for;
  using meow::adaptive_bitrate::reason_t;
  using meow::adaptive_bitrate::receiver_report_t;
  using meow::adaptive_bitrate::resolve_bounds;
  using meow::adaptive_bitrate::rtt_sample_t;
  using meow::adaptive_bitrate::tuning_t;
  using meow::adaptive_bitrate::validate_config;
  using meow::adaptive_bitrate::write_bitrate_applied;

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
   * @brief Frames the encoder produces per evaluation window in these tests.
   *
   * A real 60 fps stream encodes ~60 frames per 1 s window. The controller divides the number
   * of damaged frames by this, so tests must model it or they assert against a denominator
   * that never occurs in production.
   */
  constexpr int frames_per_window = 60;

  // ---------------------------------------------------------------------------
  // parse_frame_fec_status — the payload is hostile input (CLAUDE.md §7)
  // ---------------------------------------------------------------------------

  TEST(AdaptiveBitrateParseTest, DecodesAWellFormedReport) {
    // 80 data + 20 parity sent; 60 data + 10 parity received => 30 of 100 lost, and 70
    // shards are fewer than the 80 Reed-Solomon needs, so the frame is gone.
    const auto sample = parse_frame_fec_status(make_report(80, 20, 60, 10));
    ASSERT_TRUE(sample.has_value());
    EXPECT_EQ(sample->packets_sent, 100u);
    EXPECT_EQ(sample->packets_lost, 30u);
    EXPECT_FALSE(sample->frame_recovered);
  }

  TEST(AdaptiveBitrateParseTest, MarksAnFecRecoveredFrameAsRecovered) {
    // The regression this pins: the client only sends SS_FRAME_FEC_STATUS when data packets
    // were missing, so `received_data >= total_data` was false for every report and every
    // FEC recovery was counted as a lost frame. Reed-Solomon rebuilds the frame from ANY
    // `total_data` shards, so 70 data + 20 parity >= 80 is a recovery.
    const auto sample = parse_frame_fec_status(make_report(80, 20, 70, 20));
    ASSERT_TRUE(sample.has_value());
    EXPECT_EQ(sample->packets_lost, 10u);
    EXPECT_TRUE(sample->frame_recovered);

    // Exactly enough shards is still a recovery; one fewer is not.
    EXPECT_TRUE(parse_frame_fec_status(make_report(80, 20, 60, 20))->frame_recovered);
    EXPECT_FALSE(parse_frame_fec_status(make_report(80, 20, 59, 20))->frame_recovered);
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
  // parse_receiver_report / write_bitrate_applied — the 0x3005 wire contract
  // ---------------------------------------------------------------------------

  // clang-format off
  /**
   * @brief The REPORT vector from `tests/meow/test_meow_protocol.c` (meowerse/moonlight-common-c
   *        `meow` 1869ace), byte for byte, so the two ends cannot drift apart.
   */
  constexpr std::array<std::uint8_t, 24> client_report_vector {
    0x01, 0x01, 0xE8, 0x03,
    0x45, 0x23, 0x01, 0x00,
    0x25, 0x00, 0x02, 0x01,
    0x04, 0x03, 0x02, 0x00,
    0x06, 0x05, 0x00, 0x00,
    0xF0, 0x49, 0x02, 0x00,
  };
  // clang-format on

  /**
   * @brief View a byte array as a payload.
   * @param bytes The bytes.
   * @param length How many of them.
   * @return The payload.
   */
  template<std::size_t N>
  std::string payload_of(const std::array<std::uint8_t, N> &bytes, const std::size_t length = N) {
    return std::string(reinterpret_cast<const char *>(bytes.data()), length);
  }

  TEST(AdaptiveBitrateReportTest, DecodesTheClientTestVector) {
    const auto r = parse_receiver_report(payload_of(client_report_vector));
    ASSERT_TRUE(r.has_value());
    EXPECT_TRUE(r->auto_bitrate);
    EXPECT_EQ(r->interval_ms, 1000);
    EXPECT_EQ(r->received_kbps, 0x00012345u);
    EXPECT_EQ(r->loss_permille, 37);
    EXPECT_EQ(r->rtt_ms, 0x0102);
    EXPECT_EQ(r->rtt_var_ms, 0x0304);
    EXPECT_EQ(r->decode_queue_frames, 2);
    EXPECT_EQ(r->avg_decode_ms, 0x0506);
    EXPECT_EQ(r->max_kbps, 150000u);
  }

  TEST(AdaptiveBitrateReportTest, RejectsEveryWrongLengthAndVersion) {
    for (std::size_t len = 0; len < 24; ++len) {
      EXPECT_FALSE(parse_receiver_report(payload_of(client_report_vector, len)).has_value()) << len;
    }
    auto longer = payload_of(client_report_vector);
    longer.push_back('\0');
    EXPECT_FALSE(parse_receiver_report(longer).has_value()) << "oversize is dropped, never read past";

    auto bytes = client_report_vector;
    bytes[0] = 0;
    EXPECT_FALSE(parse_receiver_report(payload_of(bytes)).has_value());
    bytes[0] = 2;
    EXPECT_FALSE(parse_receiver_report(payload_of(bytes)).has_value());
  }

  TEST(AdaptiveBitrateReportTest, RejectsImpossibleValuesButIgnoresUnknownFlags) {
    auto bytes = client_report_vector;
    bytes[8] = 0xE9;  // loss 1001 per mille
    bytes[9] = 0x03;
    EXPECT_FALSE(parse_receiver_report(payload_of(bytes)).has_value());

    bytes = client_report_vector;
    bytes[2] = 0;  // zero-length interval
    bytes[3] = 0;
    EXPECT_FALSE(parse_receiver_report(payload_of(bytes)).has_value());

    bytes = client_report_vector;
    bytes[1] = 0xFE;  // unknown flags, auto_bitrate off
    bytes[18] = 0x55;  // reserved
    const auto r = parse_receiver_report(payload_of(bytes));
    ASSERT_TRUE(r.has_value());
    EXPECT_FALSE(r->auto_bitrate);
  }

  TEST(AdaptiveBitrateReportTest, AppliedMatchesTheClientTestVector) {
    // `APPLIED` from test_meow_protocol.c: version 1, flags 0, reserved 0, 25000 kbps.
    const std::array<std::uint8_t, 8> expected {0x01, 0x00, 0x00, 0x00, 0xA8, 0x61, 0x00, 0x00};
    std::array<std::uint8_t, 9> out {};
    out.fill(0xCC);
    write_bitrate_applied(25000, out.data());
    EXPECT_TRUE(std::equal(expected.begin(), expected.end(), out.begin()));
    EXPECT_EQ(out[8], 0xCC) << "exactly 8 bytes";
    write_bitrate_applied(0x01020304, out.data());
    EXPECT_EQ(out[4], 0x04);
    EXPECT_EQ(out[7], 0x01);
  }

  TEST(AdaptiveBitrateReportTest, PacketNumberIsTheClients) {
    EXPECT_EQ(meow::adaptive_bitrate::receiver_report_packet_type, 0x3005);
    EXPECT_EQ(meow::adaptive_bitrate::receiver_report_length, 24u);
    EXPECT_EQ(meow::adaptive_bitrate::bitrate_applied_length, 8u);
  }

  // ---------------------------------------------------------------------------
  // Client <-> encoder bitrate units (rtsp.cpp's FEC/audio/overhead deduction)
  // ---------------------------------------------------------------------------

  TEST(AdaptiveBitrateUnitsTest, ClientToEncoderIsRtspsFormula) {
    const meow::adaptive_bitrate::wire_budget_t stereo {20, 192};
    // 20000 * 0.8 = 16000; - min(192, 3200) = 15808; - min(500, 1580) = 15308.
    EXPECT_EQ(meow::adaptive_bitrate::client_to_encoder_kbps(20000, stereo), 15308);
    // Small budgets hit the percentage caps instead: 1000 * 0.8 = 800; - min(192, 160) = 640; - min(500, 64) = 576.
    EXPECT_EQ(meow::adaptive_bitrate::client_to_encoder_kbps(1000, stereo), 576);
    // FEC above 80% is not deducted, exactly like rtsp.cpp.
    EXPECT_EQ(meow::adaptive_bitrate::client_to_encoder_kbps(10000, {90, 0}), 9500);
    EXPECT_EQ(meow::adaptive_bitrate::client_to_encoder_kbps(0, stereo), 0);
  }

  TEST(AdaptiveBitrateUnitsTest, EncoderToClientRoundTripsWithoutRatcheting) {
    // A client that remembers APPLIED and negotiates it next session must get the same encoder
    // rate back, not one deducted twice.
    for (const auto &budget : {meow::adaptive_bitrate::wire_budget_t {20, 192}, meow::adaptive_bitrate::wire_budget_t {0, 0}, meow::adaptive_bitrate::wire_budget_t {50, 1536}, meow::adaptive_bitrate::wire_budget_t {90, 256}}) {
      for (int encoder = 400; encoder < 200000; encoder += 97) {
        const auto client = meow::adaptive_bitrate::encoder_to_client_kbps(encoder, budget);
        const auto back = meow::adaptive_bitrate::client_to_encoder_kbps(client, budget);
        ASSERT_GE(back, encoder) << encoder;
        ASSERT_LT(meow::adaptive_bitrate::client_to_encoder_kbps(client - 1, budget), encoder) << "smallest such client rate, " << encoder;
        ASSERT_EQ(meow::adaptive_bitrate::client_to_encoder_kbps(meow::adaptive_bitrate::encoder_to_client_kbps(back, budget), budget), back);
      }
    }
  }

  // ---------------------------------------------------------------------------
  // resolve_bounds — floor/ceiling rules (spec H1)
  // ---------------------------------------------------------------------------

  TEST(AdaptiveBitrateBoundsTest, OnByDefaultWithTheAutomaticFloor) {
    // max(1000, 25% of negotiated)
    EXPECT_EQ(resolve_bounds(true, 0, 0, 20000, 0), (bounds_t {5000, 20000}));
    EXPECT_EQ(resolve_bounds(true, 0, 0, 3000, 0), (bounds_t {1000, 3000}));
    EXPECT_EQ(meow::adaptive_bitrate::automatic_floor(150000), 37500);
  }

  TEST(AdaptiveBitrateBoundsTest, TheOffSwitchDisablesIt) {
    EXPECT_FALSE(resolve_bounds(false, 0, 0, 20000, 0).enabled());
    EXPECT_FALSE(resolve_bounds(false, 3000, 9000, 20000, 0, 50000).enabled());
  }

  TEST(AdaptiveBitrateBoundsTest, ClientCeilingRaisesAboveNegotiatedOnlyWhenHigher) {
    EXPECT_EQ(resolve_bounds(true, 0, 0, 10000, 0, 40000).max_kbps, 40000);
    EXPECT_EQ(resolve_bounds(true, 0, 0, 10000, 0, 8000).max_kbps, 10000) << "a lower client max never lowers the negotiated ceiling";
    EXPECT_EQ(resolve_bounds(true, 0, 0, 10000, 0, 0).max_kbps, 10000);
    // The floor stays relative to what was negotiated.
    EXPECT_EQ(resolve_bounds(true, 0, 0, 10000, 0, 40000).min_kbps, 2500);
  }

  TEST(AdaptiveBitrateBoundsTest, HostLimitsStillCapTheCeiling) {
    EXPECT_EQ(resolve_bounds(true, 0, 0, 10000, 30000, 40000).max_kbps, 30000) << "max_bitrate";
    EXPECT_EQ(resolve_bounds(true, 0, 25000, 10000, 30000, 40000).max_kbps, 25000) << "adaptive_bitrate_max";
    EXPECT_EQ(resolve_bounds(true, 0, 0, 10000, 0, 999999999).max_kbps, max_configurable_kbps) << "a hostile client max is clamped";
  }

  TEST(AdaptiveBitrateBoundsTest, AnExplicitMinimumReplacesTheAutomaticFloor) {
    EXPECT_EQ(resolve_bounds(true, 2000, 0, 20000, 0).min_kbps, 2000);
    EXPECT_EQ(resolve_bounds(true, 8000, 0, 20000, 0).min_kbps, 8000);
    // ...but never above the ceiling: then there is nothing to adapt between.
    EXPECT_FALSE(resolve_bounds(true, 30000, 0, 20000, 0).enabled());
  }

  TEST(AdaptiveBitrateBoundsTest, DisabledWhenTheRangeCollapsesOrTheRequestIsNonsense) {
    EXPECT_FALSE(resolve_bounds(true, 0, 0, 1000, 0).enabled()) << "floor 1000 == ceiling 1000";
    EXPECT_FALSE(resolve_bounds(true, 0, 0, 0, 0).enabled());
    EXPECT_FALSE(resolve_bounds(true, 0, 0, -5, 0).enabled());
  }

  TEST(AdaptiveBitrateBoundsTest, TheResolvedRangeIsAlwaysInsideEveryLimit) {
    for (const int negotiated : {500, 1000, 1500, 8000, 20000, 150000}) {
      for (const int host_max : {0, 800, 5000, 60000}) {
        for (const int cfg_max : {0, 700, 9000}) {
          for (const int client_max : {0, 3000, 200000}) {
            const auto b = resolve_bounds(true, 0, cfg_max, negotiated, host_max, client_max);
            if (!b.enabled()) {
              continue;
            }
            EXPECT_LT(b.min_kbps, b.max_kbps);
            EXPECT_GE(b.min_kbps, 1);
            if (host_max > 0) {
              EXPECT_LE(b.max_kbps, host_max);
            }
            if (cfg_max > 0) {
              EXPECT_LE(b.max_kbps, cfg_max);
            }
            EXPECT_LE(b.max_kbps, std::max(negotiated, client_max));
          }
        }
      }
    }
  }

  // ---------------------------------------------------------------------------
  // validate_config — hand-edited values are corrected loudly, never turn the feature off
  // ---------------------------------------------------------------------------

  TEST(AdaptiveBitrateConfigTest, AcceptsSaneAndAutomaticValuesSilently) {
    for (const auto &[lo, hi] : std::vector<std::pair<int, int>> {{0, 0}, {3000, 9000}, {2000, 0}, {0, 8000}}) {
      int min = lo;
      int max = hi;
      std::string warning;
      EXPECT_TRUE(validate_config(min, max, warning)) << lo << "," << hi;
      EXPECT_TRUE(warning.empty());
      EXPECT_EQ(min, lo);
      EXPECT_EQ(max, hi);
    }
  }

  TEST(AdaptiveBitrateConfigTest, NegativesFallBackToAutomaticWithAWarning) {
    int min = -5;
    int max = -1;
    std::string warning;
    EXPECT_FALSE(validate_config(min, max, warning));
    EXPECT_EQ(min, 0);
    EXPECT_EQ(max, 0);
    EXPECT_NE(warning.find("adaptive_bitrate_min = -5"), std::string::npos);
    EXPECT_NE(warning.find("adaptive_bitrate_max = -1"), std::string::npos);
  }

  TEST(AdaptiveBitrateConfigTest, ClampsTinyAndAbsurdValues) {
    int min = 1;
    int max = 999999999;
    std::string warning;
    EXPECT_FALSE(validate_config(min, max, warning));
    EXPECT_EQ(min, floor_kbps);
    EXPECT_EQ(max, max_configurable_kbps);

    min = 999999999;
    max = 0;
    EXPECT_FALSE(validate_config(min, max, warning));
    EXPECT_EQ(min, max_configurable_kbps);
  }

  TEST(AdaptiveBitrateConfigTest, AnInvertedRangeDropsTheMaximumAndKeepsTheFloor) {
    int min = 9000;
    int max = 3000;
    std::string warning;
    EXPECT_FALSE(validate_config(min, max, warning));
    EXPECT_EQ(min, 9000);
    EXPECT_EQ(max, 0);
    EXPECT_NE(warning.find("ignored"), std::string::npos);

    min = 5000;
    max = 5000;
    EXPECT_FALSE(validate_config(min, max, warning));
    EXPECT_EQ(max, 0);
  }

  // ---------------------------------------------------------------------------
  // controller_t — the control law, on a synthetic clock
  // ---------------------------------------------------------------------------

  /**
   * @brief A 1 s window of network conditions, as the controller sees it.
   */
  struct conditions_t {
    int damaged_frames = 0;  ///< FEC reports this window (of 60 frames).
    bool recovered = true;  ///< Whether FEC rebuilt them.
    std::optional<int> loss_permille;  ///< Receiver report loss; nullopt = no report.
    int received_kbps = 0;  ///< Receiver report goodput.
    int decode_queue = 0;  ///< Receiver report decode queue.
    int host_rtt_ms = 0;  ///< Host ENet RTT sample; 0 = none.
    int client_rtt_ms = 0;  ///< Receiver report RTT.
  };

  /**
   * @brief Drive whole 1 s windows through the controller exactly as the governor does.
   *
   * @param c Controller.
   * @param now In/out clock.
   * @param windows Number of windows.
   * @param cond Conditions in each window.
   * @param trajectory Receives the rate after every window.
   * @return Every decision that changed the rate.
   */
  std::vector<decision_t> drive(controller_t &c, std::chrono::steady_clock::time_point &now, const int windows, const conditions_t &cond, std::vector<int> *trajectory = nullptr) {
    std::vector<decision_t> changes;
    const auto frame = std::chrono::microseconds(1'000'000 / frames_per_window);
    for (int w = 0; w < windows; ++w) {
      for (int f = 0; f < frames_per_window; ++f) {
        if (f < cond.damaged_frames) {
          c.observe(loss_sample_t {19, 1, cond.recovered});
        }
        if (f == 10 && cond.host_rtt_ms > 0) {
          c.observe(rtt_sample_t {static_cast<std::uint32_t>(cond.host_rtt_ms), 2});
        }
        if (f == 30 && cond.loss_permille) {
          receiver_report_t r;
          r.auto_bitrate = true;
          r.interval_ms = 1000;
          r.loss_permille = static_cast<std::uint16_t>(*cond.loss_permille);
          r.received_kbps = static_cast<std::uint32_t>(cond.received_kbps);
          r.rtt_ms = static_cast<std::uint16_t>(cond.client_rtt_ms);
          r.decode_queue_frames = static_cast<std::uint16_t>(cond.decode_queue);
          c.observe(r);
        }
        now += (f + 1 == frames_per_window) ? std::chrono::microseconds(1'000'000) - frame * (frames_per_window - 1) : frame;
        const auto d = c.tick(now);
        if (d.changed) {
          changes.push_back(d);
        }
      }
      if (trajectory) {
        trajectory->push_back(c.current_kbps());
      }
    }
    return changes;
  }

  /// A clean link with a 20 ms path.
  const conditions_t clean_link {0, true, 0, 12000, 0, 20, 20};

  TEST(AdaptiveBitrateControllerTest, StartsAtTheNegotiatedRate) {
    auto now = std::chrono::steady_clock::time_point {};
    controller_t c {resolve_bounds(true, 0, 0, 10000, 0, 40000), now, {}, 10000};
    EXPECT_TRUE(c.enabled());
    EXPECT_EQ(c.current_kbps(), 10000) << "not the (raised) ceiling";
    controller_t resumed {resolve_bounds(true, 0, 0, 10000, 0), now, {}, 10000, 4000};
    EXPECT_EQ(resumed.current_kbps(), 4000) << "a remembered rate wins across a reinit";
    controller_t clamped {resolve_bounds(true, 0, 0, 10000, 0), now, {}, 10000, 999};
    EXPECT_EQ(clamped.current_kbps(), 2500) << "clamped into the new bounds";
  }

  TEST(AdaptiveBitrateControllerTest, DisabledControllerNeverChangesAnything) {
    auto now = std::chrono::steady_clock::time_point {};
    controller_t c {resolve_bounds(false, 0, 0, 10000, 0), now};
    conditions_t lossy = clean_link;
    lossy.loss_permille = 200;
    EXPECT_TRUE(drive(c, now, 20, lossy).empty());
    EXPECT_EQ(c.current_kbps(), 0);
  }

  TEST(AdaptiveBitrateControllerTest, LossBacksOffAfterTwoWindowsNotOne) {
    auto now = std::chrono::steady_clock::time_point {};
    controller_t c {resolve_bounds(true, 0, 0, 20000, 0), now, {}, 20000};
    drive(c, now, 5, clean_link);
    conditions_t lossy = clean_link;
    lossy.loss_permille = 150;  // beyond half of what 20% FEC carries
    EXPECT_TRUE(drive(c, now, 1, lossy).empty()) << "one bad window is noise";
    const auto changes = drive(c, now, 1, lossy);
    ASSERT_EQ(changes.size(), 1u);
    EXPECT_EQ(changes[0].reason, reason_t::sustained_loss);
    EXPECT_EQ(changes[0].kbps, 15000);
    EXPECT_EQ(changes[0].observed_loss_permille, 150);
  }

  TEST(AdaptiveBitrateControllerTest, UnrecoveredFramesAloneBackOff) {
    // A stock client sends no receiver reports; its FEC reports still drive the controller when
    // FEC could not rebuild the frames.
    auto now = std::chrono::steady_clock::time_point {};
    controller_t c {resolve_bounds(true, 0, 0, 20000, 0), now, {}, 20000};
    conditions_t unrecovered {3, false, std::nullopt, 0, 0, 20, 0};
    const auto changes = drive(c, now, 5, unrecovered);
    ASSERT_FALSE(changes.empty());
    EXPECT_EQ(changes[0].reason, reason_t::sustained_loss);
    EXPECT_EQ(changes[0].observed_loss_permille, -1);
  }

  TEST(AdaptiveBitrateControllerTest, RandomLossThatFecRepairsIsNotABackOff) {
    // The case the review caught: 0.5-2% random Wi-Fi/cellular loss damages ~16-50% of
    // 35-packet frames, every one of them rebuilt by FEC, with a flat RTT. That is FEC doing its
    // job; the stream must stay at its ceiling (spec N4, N5), stock client or not.
    for (const auto &[damaged, loss] : std::vector<std::pair<int, std::optional<int>>> {{10, std::nullopt}, {10, 5}, {20, 15}, {30, 20}, {45, 40}}) {
      auto now = std::chrono::steady_clock::time_point {};
      const auto bounds = resolve_bounds(true, 0, 0, 20000, 0);
      controller_t c {bounds, now, {}, 20000};
      conditions_t wifi {damaged, true, loss, 15000, 0, 25, 25};
      std::vector<int> trajectory;
      const auto changes = drive(c, now, 120, wifi, &trajectory);
      EXPECT_TRUE(changes.empty()) << damaged << " damaged frames/s";
      EXPECT_EQ(c.current_kbps(), bounds.max_kbps);
    }
  }

  TEST(AdaptiveBitrateControllerTest, RandomLossDoesNotStopTheClimbBack) {
    // After a real congestion back-off, a link with light random loss still probes back up.
    auto now = std::chrono::steady_clock::time_point {};
    const auto bounds = resolve_bounds(true, 0, 0, 20000, 0);
    controller_t c {bounds, now, {}, 20000, 8000};
    conditions_t wifi {15, true, 10, 9000, 0, 25, 25};
    drive(c, now, 90, wifi);
    EXPECT_EQ(c.current_kbps(), bounds.max_kbps);
  }

  TEST(AdaptiveBitrateControllerTest, DamageWithARisingRttIsCongestion) {
    // The same damaged-frame rate *with* a queue building is congestion, and backs off.
    auto now = std::chrono::steady_clock::time_point {};
    controller_t c {resolve_bounds(true, 0, 0, 20000, 0), now, {}, 20000};
    drive(c, now, 10, clean_link);
    std::vector<decision_t> changes;
    for (const int rtt : {120, 180, 240, 300}) {
      conditions_t congested {12, true, 40, 12000, 0, rtt, rtt};
      for (const auto &d : drive(c, now, 1, congested)) {
        changes.push_back(d);
      }
    }
    ASSERT_FALSE(changes.empty());
  }

  TEST(AdaptiveBitrateControllerTest, GoodputCapsTheBackOffOnASaturatedLink) {
    // 20 Mbps offered into a link that delivers 8 Mbps (incl. 20% FEC): the bottleneck queue
    // overflows, so it is losing packets AND the RTT is up. The new target is what actually got
    // through, not a blind 0.75x that is still far above it.
    auto now = std::chrono::steady_clock::time_point {};
    controller_t c {resolve_bounds(true, 0, 0, 20000, 0), now, {}, 20000};
    drive(c, now, 5, clean_link);
    conditions_t saturated {0, true, 120, 8000, 0, 250, 250};
    const auto changes = drive(c, now, 2, saturated);
    ASSERT_EQ(changes.size(), 1u);
    EXPECT_EQ(changes[0].reason, reason_t::sustained_loss);
    EXPECT_EQ(changes[0].kbps, static_cast<int>(8000 * 100.0 / 120.0 * 0.95));
    EXPECT_LT(changes[0].kbps, 15000);
  }

  TEST(AdaptiveBitrateControllerTest, GoodputIsIgnoredWithoutEvidenceOfSaturation) {
    // An idle desktop encodes far below its target, so a low goodput next to random loss with
    // no queue says nothing about the link: plain 0.75x, not a crash to the goodput.
    auto now = std::chrono::steady_clock::time_point {};
    controller_t c {resolve_bounds(true, 0, 0, 20000, 0), now, {}, 20000};
    drive(c, now, 5, clean_link);
    conditions_t random_loss {0, true, 150, 900, 0, 20, 20};
    const auto changes = drive(c, now, 2, random_loss);
    ASSERT_EQ(changes.size(), 1u);
    EXPECT_EQ(changes[0].kbps, 15000);
  }

  TEST(AdaptiveBitrateControllerTest, NeverLeavesTheBounds) {
    auto now = std::chrono::steady_clock::time_point {};
    const auto bounds = resolve_bounds(true, 0, 0, 20000, 0);
    controller_t c {bounds, now, {}, 20000};
    conditions_t terrible {60, false, 1000, 1, 0, 20, 20};
    std::vector<int> trajectory;
    drive(c, now, 120, terrible, &trajectory);
    for (const int k : trajectory) {
      EXPECT_GE(k, bounds.min_kbps);
      EXPECT_LE(k, bounds.max_kbps);
    }
    EXPECT_EQ(c.current_kbps(), bounds.min_kbps);
    trajectory.clear();
    drive(c, now, 300, clean_link, &trajectory);
    for (const int k : trajectory) {
      EXPECT_LE(k, bounds.max_kbps);
    }
    EXPECT_EQ(c.current_kbps(), bounds.max_kbps);
  }

  TEST(AdaptiveBitrateControllerTest, DelayBacksOffBeforeAnyLoss) {
    // A queue building: RTT climbs 20 -> 60 -> 110 -> 170 ms with no loss at all.
    auto now = std::chrono::steady_clock::time_point {};
    controller_t c {resolve_bounds(true, 0, 0, 20000, 0), now, {}, 20000};
    drive(c, now, 10, clean_link);
    std::vector<decision_t> changes;
    for (const int rtt : {60, 110, 170}) {
      conditions_t building = clean_link;
      building.host_rtt_ms = rtt;
      building.client_rtt_ms = rtt;
      for (const auto &d : drive(c, now, 1, building)) {
        changes.push_back(d);
      }
    }
    ASSERT_FALSE(changes.empty());
    EXPECT_EQ(changes[0].reason, reason_t::queueing_delay);
    EXPECT_EQ(changes[0].kbps, 17000) << "x0.85, and no goodput cap without loss";
    EXPECT_EQ(changes[0].observed_loss_permille, 0) << "backed off with zero loss reported";
    EXPECT_EQ(changes[0].baseline_ms, 20);
  }

  TEST(AdaptiveBitrateControllerTest, JitterBurstsDoNotMoveIt) {
    // Wi-Fi/cellular: RTT jumps around inside the threshold, and a single 150 ms spike.
    auto now = std::chrono::steady_clock::time_point {};
    controller_t c {resolve_bounds(true, 0, 0, 20000, 0), now, {}, 20000};
    drive(c, now, 5, clean_link);
    std::vector<decision_t> changes;
    for (const int rtt : {25, 45, 30, 150, 22, 50, 35, 28, 55, 20, 40, 150, 20, 30}) {
      conditions_t noisy = clean_link;
      noisy.host_rtt_ms = rtt;
      for (const auto &d : drive(c, now, 1, noisy)) {
        changes.push_back(d);
      }
    }
    for (const auto &d : changes) {
      EXPECT_NE(d.reason, reason_t::queueing_delay);
      EXPECT_NE(d.reason, reason_t::sustained_loss);
    }
  }

  TEST(AdaptiveBitrateControllerTest, ADerpPathSwitchRebaselinesInsteadOfPinningToTheFloor) {
    // Tailscale DIRECT -> DERP: RTT steps 20 -> 140 ms and stays there, no loss, goodput
    // unchanged. Model ENet's own smoothing too: the host sees the step as a ramp.
    auto now = std::chrono::steady_clock::time_point {};
    const auto bounds = resolve_bounds(true, 0, 0, 20000, 0);
    controller_t c {bounds, now, {}, 20000};
    drive(c, now, 30, clean_link);
    ASSERT_EQ(c.baseline_ms(), 20);

    std::vector<int> trajectory;
    bool rebaselined = false;
    double smoothed = 20.0;
    for (int w = 0; w < 90; ++w) {
      smoothed += (140.0 - smoothed) / 4.0;  // ENet's EWMA, two acks per window
      conditions_t derp = clean_link;
      derp.host_rtt_ms = static_cast<int>(smoothed);
      derp.client_rtt_ms = 140;
      for (const auto &d : drive(c, now, 1, derp, &trajectory)) {
        rebaselined = rebaselined || d.rebaselined;
      }
      rebaselined = rebaselined || c.baseline_ms() >= 130;
    }
    EXPECT_TRUE(rebaselined);
    EXPECT_GE(c.baseline_ms(), 130) << "the new path is the baseline now";
    const auto lowest = *std::min_element(trajectory.begin(), trajectory.end());
    EXPECT_GT(lowest, bounds.min_kbps) << "a path change must never pin the stream to the floor";
    EXPECT_GE(lowest, 14000) << "at most one or two false delay back-offs";
    EXPECT_EQ(c.current_kbps(), bounds.max_kbps) << "and it climbs straight back once re-baselined";
  }

  TEST(AdaptiveBitrateControllerTest, ProbesUpMultiplicativelyAndCapsBelowTheLastCongestion) {
    auto now = std::chrono::steady_clock::time_point {};
    const auto bounds = resolve_bounds(true, 0, 0, 20000, 0);
    controller_t c {bounds, now, {}, 20000};
    drive(c, now, 5, clean_link);
    // Congest at 20000.
    conditions_t lossy = clean_link;
    lossy.loss_permille = 150;
    ASSERT_EQ(drive(c, now, 2, lossy).size(), 1u);
    ASSERT_EQ(c.current_kbps(), 15000);

    // Within the hold: probing climbs, but never to 20000 - it stops at 95% of it.
    std::vector<int> trajectory;
    const auto probes = drive(c, now, 25, clean_link, &trajectory);
    ASSERT_FALSE(probes.empty());
    for (const auto &d : probes) {
      EXPECT_EQ(d.reason, reason_t::link_clean);
      EXPECT_LE(static_cast<double>(d.kbps) / d.previous_kbps, 1.25 + 1e-9) << "at most +25% per change";
    }
    EXPECT_EQ(*std::max_element(trajectory.begin(), trajectory.end()), 19000);

    // After the hold the cap lifts and it reaches the ceiling.
    drive(c, now, 30, clean_link);
    EXPECT_EQ(c.current_kbps(), bounds.max_kbps);
  }

  TEST(AdaptiveBitrateControllerTest, ClimbsBackFromTheFloorWithinSeconds) {
    auto now = std::chrono::steady_clock::time_point {};
    const auto bounds = resolve_bounds(true, 0, 0, 20000, 0);
    controller_t c {bounds, now, {}, 20000, 5000};
    std::vector<int> trajectory;
    drive(c, now, 40, clean_link, &trajectory);
    EXPECT_EQ(c.current_kbps(), bounds.max_kbps);
    // 5000 -> 20000 at <= +25% per >= 3 s: about seven changes.
    const auto reached = std::find(trajectory.begin(), trajectory.end(), bounds.max_kbps) - trajectory.begin();
    EXPECT_LE(reached, 30);
  }

  TEST(AdaptiveBitrateControllerTest, DoesNotOscillateOnAFlappingLink) {
    auto now = std::chrono::steady_clock::time_point {};
    controller_t c {resolve_bounds(true, 0, 0, 20000, 0), now, {}, 20000};
    conditions_t lossy = clean_link;
    lossy.loss_permille = 150;
    std::vector<decision_t> changes;
    for (int i = 0; i < 60; ++i) {
      for (const auto &d : drive(c, now, 1, (i % 2) ? lossy : clean_link)) {
        changes.push_back(d);
      }
    }
    EXPECT_TRUE(changes.empty()) << "alternating windows never agree twice in a row";
  }

  TEST(AdaptiveBitrateControllerTest, ChangesAreRareEvenUnderHostileChurn) {
    // Two bad, three good, repeated: the worst pattern for a naive controller.
    auto now = std::chrono::steady_clock::time_point {};
    controller_t c {resolve_bounds(true, 0, 0, 20000, 0), now, {}, 20000};
    conditions_t lossy = clean_link;
    lossy.loss_permille = 150;
    std::vector<decision_t> changes;
    for (int i = 0; i < 24; ++i) {
      for (const auto &d : drive(c, now, 2, lossy)) {
        changes.push_back(d);
      }
      for (const auto &d : drive(c, now, 3, clean_link)) {
        changes.push_back(d);
      }
    }
    // 120 s: at most one change per min_change_interval (3 s), and in practice far fewer.
    EXPECT_LE(changes.size(), 40u);
    for (std::size_t i = 1; i < changes.size(); ++i) {
      EXPECT_NE(changes[i].kbps, changes[i].previous_kbps);
    }
  }

  TEST(AdaptiveBitrateControllerTest, RespectsTheMinimumIntervalAndStep) {
    auto now = std::chrono::steady_clock::time_point {};
    tuning_t t;
    controller_t c {resolve_bounds(true, 0, 0, 20000, 0), now, t, 20000};
    conditions_t lossy = clean_link;
    lossy.loss_permille = 500;
    std::vector<std::chrono::steady_clock::time_point> at;
    for (int w = 0; w < 30; ++w) {
      if (!drive(c, now, 1, lossy).empty()) {
        at.push_back(now);
      }
    }
    for (std::size_t i = 1; i < at.size(); ++i) {
      EXPECT_GE(at[i] - at[i - 1], t.min_change_interval);
    }
  }

  TEST(AdaptiveBitrateControllerTest, AnOverloadedDecoderHoldsProbing) {
    auto now = std::chrono::steady_clock::time_point {};
    controller_t c {resolve_bounds(true, 0, 0, 20000, 0), now, {}, 20000, 10000};
    conditions_t slow = clean_link;
    slow.decode_queue = 6;
    EXPECT_TRUE(drive(c, now, 20, slow).empty());
    EXPECT_EQ(c.current_kbps(), 10000);
  }

  TEST(AdaptiveBitrateControllerTest, AnUnrecoveredFrameBlocksProbing) {
    auto now = std::chrono::steady_clock::time_point {};
    controller_t c {resolve_bounds(true, 0, 0, 20000, 0), now, {}, 20000, 10000};
    conditions_t lost {1, false, 0, 12000, 0, 20, 20};
    EXPECT_TRUE(drive(c, now, 20, lost).empty());
  }

  TEST(AdaptiveBitrateControllerTest, ARecoveredFrameDoesNotBlockProbing) {
    // The regression the recovery-flag fix closes: every FEC recovery used to count as a lost
    // frame, so a link that FEC was handling perfectly could never climb.
    auto now = std::chrono::steady_clock::time_point {};
    controller_t c {resolve_bounds(true, 0, 0, 20000, 0), now, {}, 20000, 10000};
    const auto sample = parse_frame_fec_status(make_report(80, 20, 70, 20));
    ASSERT_TRUE(sample.has_value());
    const auto frame = std::chrono::microseconds(1'000'000 / frames_per_window);
    for (int w = 0; w < 20; ++w) {
      for (int f = 0; f < frames_per_window; ++f) {
        if (f == 0) {
          c.observe(*sample);
        }
        now += frame;
        static_cast<void>(c.tick(now));
      }
    }
    EXPECT_GT(c.current_kbps(), 10000);
  }

  TEST(AdaptiveBitrateControllerTest, ARaisedClientCeilingIsProbedInto) {
    auto now = std::chrono::steady_clock::time_point {};
    controller_t c {resolve_bounds(true, 0, 0, 10000, 0), now, {}, 10000};
    drive(c, now, 10, clean_link);
    EXPECT_EQ(c.current_kbps(), 10000) << "already at the negotiated ceiling";

    const auto raised = c.set_bounds(resolve_bounds(true, 0, 0, 10000, 0, 40000), now);
    EXPECT_FALSE(raised.changed) << "raising the ceiling alone does not move the rate";
    drive(c, now, 60, clean_link);
    EXPECT_EQ(c.current_kbps(), 40000);

    const auto lowered = c.set_bounds(resolve_bounds(true, 0, 0, 10000, 0, 20000), now);
    EXPECT_TRUE(lowered.changed);
    EXPECT_EQ(lowered.reason, reason_t::ceiling_changed);
    EXPECT_EQ(c.current_kbps(), 20000);
  }

  TEST(AdaptiveBitrateControllerTest, AFloodOfReportsCannotPinTheBitrateToZero) {
    auto now = std::chrono::steady_clock::time_point {};
    const auto bounds = resolve_bounds(true, 0, 0, 20000, 0);
    controller_t c {bounds, now, {}, 20000};
    for (int i = 0; i < 200000; ++i) {
      c.observe(loss_sample_t {1, 1, false});
    }
    receiver_report_t r;
    r.interval_ms = 1;
    r.loss_permille = 1000;
    for (int i = 0; i < 10000; ++i) {
      c.observe(r);
    }
    drive(c, now, 60, conditions_t {60, false, 1000, 0, 60, 5000, 5000});
    EXPECT_EQ(c.current_kbps(), bounds.min_kbps);
    EXPECT_GT(c.current_kbps(), 0);
  }

  TEST(AdaptiveBitrateControllerTest, SurvivesAClockThatJumpsBackwards) {
    auto now = std::chrono::steady_clock::time_point {} + std::chrono::hours(1);
    controller_t c {resolve_bounds(true, 0, 0, 20000, 0), now, {}, 20000};
    drive(c, now, 3, clean_link);
    now -= std::chrono::minutes(30);
    static_cast<void>(c.tick(now));
    conditions_t lossy = clean_link;
    lossy.loss_permille = 150;
    EXPECT_FALSE(drive(c, now, 6, lossy).empty()) << "still decides after the jump";
  }

  TEST(AdaptiveBitrateControllerTest, EndToEndFromParsedPayloads) {
    // Bytes in, trajectory out: reports parsed from the wire exactly as the control thread does.
    auto now = std::chrono::steady_clock::time_point {};
    controller_t c {resolve_bounds(true, 0, 0, 20000, 0), now, {}, 20000};
    auto bytes = client_report_vector;
    bytes[4] = 0x70;  // received_kbps = 6000 (0x1770)
    bytes[5] = 0x17;
    bytes[6] = 0;
    bytes[7] = 0;
    bytes[8] = 100;  // loss 100 per mille
    bytes[9] = 0;
    bytes[10] = 20;  // rtt 20
    bytes[11] = 0;
    bytes[14] = 0;  // decode queue 0
    const auto report = parse_receiver_report(payload_of(bytes));
    ASSERT_TRUE(report.has_value());
    std::vector<decision_t> changes;
    const auto frame = std::chrono::microseconds(1'000'000 / frames_per_window);
    // Four windows of frames: the clock crosses the third window boundary (and the 3 s
    // minimum change interval since construction) on the first frame of the fourth.
    for (int w = 0; w < 4; ++w) {
      for (int f = 0; f < frames_per_window; ++f) {
        if (f == 1) {
          c.observe(*report);
        }
        now += frame;
        if (const auto d = c.tick(now); d.changed) {
          changes.push_back(d);
        }
      }
    }
    ASSERT_EQ(changes.size(), 1u);
    EXPECT_EQ(changes[0].reason, reason_t::sustained_loss);
    EXPECT_EQ(changes[0].kbps, 15000);
    EXPECT_EQ(changes[0].observed_loss_permille, 100);
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

}  // namespace
