/**
 * @file tests/unit/meow/test_control_stream.cpp
 * @brief Test src/meow/control_stream.h: handler registration, per-session sends, and the
 *        length checks on upstream control payloads (spec F5).
 *
 * `stream::session_t` and `stream::control_server_t` are private to `src/stream.cpp` and need
 * an ENet host, so these tests drive the templated code with stand-ins that have exactly the
 * members it touches. The handlers are *invoked* and their real effects - on a real
 * `safe::mail_t`, the real cursor/viewport state and the bytes that would go on the wire - are
 * what is asserted.
 */
// test includes
#include "../../tests_common.h"

// standard includes
#include <array>
#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

// local includes
#include <src/meow/control_stream.h>

namespace {

  using namespace std::chrono_literals;

  /**
   * @brief The ENet peer fields the control code reads.
   */
  struct fake_peer_t {
    std::uint32_t roundTripTime = 0;  ///< ENet smoothed RTT.
    std::uint32_t roundTripTimeVariance = 0;  ///< ENet RTT variance.
  };

  /**
   * @brief Stand-in for `stream::session_t` with the members `src/meow/` touches.
   */
  struct fake_session_t {
    safe::mail_t mail = std::make_shared<safe::mail_raw_t>();  ///< Session mailbox.
    meow::control::session_state_t meow;  ///< Meow per-session state.

    struct {
      fake_peer_t *peer = nullptr;  ///< Control peer.
    } control;  ///< Control channel.
  };

  /**
   * @brief Records handler registrations, standing in for `stream::control_server_t`.
   */
  struct recording_server_t {
    std::map<std::uint16_t, std::function<void(fake_session_t *, const std::string_view &)>> handlers;  ///< Registered handlers.

    /**
     * @brief Record a registration.
     * @param type Packet type.
     * @param handler Handler.
     */
    void map(const std::uint16_t type, std::function<void(fake_session_t *, const std::string_view &)> handler) {
      handlers.emplace(type, std::move(handler));
    }
  };

  /**
   * @brief One message the code under test asked to send.
   */
  struct sent_t {
    std::uint16_t type;  ///< Packet type.
    std::vector<std::uint8_t> payload;  ///< Payload bytes.
  };

  /**
   * @brief Save and restore the config and global state these tests change.
   */
  struct MeowControlStreamTest: testing::Test {
    void SetUp() override {
      config::video.adaptive_bitrate = true;
      config::video.cursor_reporting = true;
      config::video.viewport_following = true;
      meow::viewport::reset();
      meow::cursor::deactivate();
    }

    void TearDown() override {
      config::video = saved;
      meow::viewport::reset();
      meow::cursor::deactivate();
    }

    /**
     * @brief A send callback that records instead of encrypting.
     * @return The callback.
     */
    auto recorder() {
      return [this](fake_session_t *, const std::uint16_t type, const std::uint8_t *payload, const std::size_t length) {
        sent.push_back({type, std::vector<std::uint8_t>(payload, payload + length)});
        return 0;
      };
    }

    config::video_t saved {config::video};  ///< Restored after each test.
    std::vector<sent_t> sent;  ///< Everything "sent".
  };

}  // namespace

TEST(MeowControlPayloads, LossStatsAndInvalidateRefFramesAreLengthChecked) {
  // F5: upstream reads int32 stats[0..3] and int64 frames[0..1] with no length check.
  const std::string bytes(64, '\0');
  for (std::size_t len = 0; len <= bytes.size(); ++len) {
    const std::string_view payload {bytes.data(), len};
    EXPECT_EQ(meow::control::loss_stats_payload_ok(payload), len >= 16) << len;
    EXPECT_EQ(meow::control::invalidate_ref_frames_payload_ok(payload), len >= 16) << len;
  }
  // What real clients send still passes: 32-byte loss stats, 24-byte invalidate (first, last, 0).
  EXPECT_TRUE(meow::control::loss_stats_payload_ok(std::string(32, '\0')));
  EXPECT_TRUE(meow::control::invalidate_ref_frames_payload_ok(std::string(24, '\0')));
}

TEST(MeowControlPayloads, EveryMeowMessageIsTiny) {
  // N2: far below the 1280-byte tailnet MTU.
  EXPECT_LE(meow::control::max_payload_length, 64u);
}

TEST_F(MeowControlStreamTest, RegistersEveryHandlerAndRefusesCollisions) {
  recording_server_t server;
  static const short clean[] = {0x0305, 0x0307, 0x0201, 0x5503};
  meow::control::register_handlers(server, clean, std::size(clean));
  for (const std::uint16_t type : {0x3003, 0x3004, 0x3005, 0x5502}) {
    EXPECT_EQ(server.handlers.count(type), 1u) << std::hex << type;
  }

  recording_server_t colliding_server;
  static const short colliding[] = {0x0305, 0x3004, 0x3005};
  meow::control::register_handlers(colliding_server, colliding, std::size(colliding));
  EXPECT_EQ(colliding_server.handlers.count(0x3004), 0u);
  EXPECT_EQ(colliding_server.handlers.count(0x3005), 0u);
  EXPECT_EQ(colliding_server.handlers.count(0x3003), 1u);
}

TEST_F(MeowControlStreamTest, ANewSessionForgetsThePreviousClientsViewport) {
  int scaler = 0;
  meow::viewport::on_scaler_init(&scaler, 5360, 1440, 1280, 720);
  ASSERT_TRUE(meow::viewport::apply_request(std::string("\x01\x00\x80\x02\xBC\x00\x80\x02\x57\x01", 10), true));
  ASSERT_NE(meow::viewport::detail::last_request.load(), 0u);
  fake_session_t next;
  EXPECT_EQ(meow::viewport::detail::last_request.load(), 0u) << "constructing a session forgets it";
}

TEST_F(MeowControlStreamTest, SubscribeCountsSubscribersAndSurvivesSessionEnd) {
  recording_server_t server;
  static const short table[] = {0x0305};
  meow::control::register_handlers(server, table, std::size(table));
  const auto before = meow::cursor::detail::subscribers.load();
  {
    fake_session_t session;
    server.handlers.at(0x3004)(&session, std::string("\x01\x01", 2));
    EXPECT_TRUE(session.meow.cursor_subscribed);
    EXPECT_EQ(meow::cursor::detail::subscribers.load(), before + 1);
    server.handlers.at(0x3004)(&session, std::string("\x01\x01", 2));
    EXPECT_EQ(meow::cursor::detail::subscribers.load(), before + 1) << "subscribing twice is one subscriber";
    server.handlers.at(0x3004)(&session, std::string("\x09\x01", 2));
    EXPECT_TRUE(session.meow.cursor_subscribed) << "a malformed message changes nothing";
    server.handlers.at(0x3004)(&session, std::string("\x01\x00", 2));
    EXPECT_FALSE(session.meow.cursor_subscribed);
    EXPECT_EQ(meow::cursor::detail::subscribers.load(), before);
    server.handlers.at(0x3004)(&session, std::string("\x01\x01", 2));
  }
  EXPECT_EQ(meow::cursor::detail::subscribers.load(), before) << "a session that ends subscribed is unsubscribed";

  config::video.cursor_reporting = false;
  fake_session_t off;
  server.handlers.at(0x3004)(&off, std::string("\x01\x01", 2));
  EXPECT_FALSE(off.meow.cursor_subscribed) << "the off switch: a subscription is never honoured";
}

TEST_F(MeowControlStreamTest, ReportsReachTheEncoderAndArmTheAppliedChannel) {
  recording_server_t server;
  static const short table[] = {0x0305};
  meow::control::register_handlers(server, table, std::size(table));

  fake_session_t session;
  // The governor holds these for the encode session's lifetime.
  auto reports = session.mail->queue<meow::adaptive_bitrate::receiver_report_t>(meow::adaptive_bitrate::report_mail_id);
  auto losses = session.mail->queue<meow::adaptive_bitrate::loss_sample_t>(meow::adaptive_bitrate::mail_id);

  EXPECT_FALSE(session.meow.applied) << "no APPLIED channel for a client that never reported";
  std::string report(24, '\0');
  report[0] = 1;
  report[2] = static_cast<char>(0xE8);
  report[3] = 0x03;
  server.handlers.at(0x3005)(&session, report);
  ASSERT_TRUE(reports->peek());
  EXPECT_EQ(reports->pop()->interval_ms, 1000);
  EXPECT_TRUE(session.meow.applied);

  server.handlers.at(0x3005)(&session, report.substr(0, 23));
  EXPECT_FALSE(reports->peek()) << "a short report is dropped";

  // 0x5502 FEC status still flows, now through the same module.
  std::string fec(21, '\0');
  fec[11] = 10;  // total data
  fec[15] = 9;  // received data
  server.handlers.at(0x5502)(&session, fec);
  EXPECT_TRUE(losses->peek());

  config::video.adaptive_bitrate = false;
  server.handlers.at(0x3005)(&session, report);
  EXPECT_FALSE(reports->peek()) << "the off switch drops reports, so the client gives up on them";
}

TEST_F(MeowControlStreamTest, TickSendsTheEchoTheEncodePathPublished) {
  fake_session_t session;
  fake_peer_t peer {20, 3};
  session.control.peer = &peer;
  int scaler = 0;
  meow::viewport::on_scaler_init(&scaler, 5360, 1440, 1280, 720);
  const auto now = std::chrono::steady_clock::now();

  meow::control::on_session_tick(&session, recorder(), now);
  for (const auto &m : sent) {
    EXPECT_NE(m.type, 0x3003) << "nothing to echo yet";
  }
  sent.clear();

  ASSERT_TRUE(meow::viewport::apply_request(std::string("\x01\x00\x80\x02\xBC\x00\x80\x02\x57\x01", 10), true));
  static_cast<void>(meow::viewport::plan_for_frame(&scaler, 5360, 1440, 1280, 720));
  meow::viewport::on_frame_encoded(0x1234);
  meow::control::on_session_tick(&session, recorder(), now);

  const sent_t *echo = nullptr;
  for (const auto &m : sent) {
    if (m.type == 0x3003) {
      echo = &m;
    }
  }
  ASSERT_NE(echo, nullptr);
  ASSERT_EQ(echo->payload.size(), 18u);
  EXPECT_EQ(echo->payload[0], 1);
  EXPECT_EQ(echo->payload[1], 0x03) << "desktop extent + frame index";
  EXPECT_EQ(echo->payload[10] | (echo->payload[11] << 8), 5360);
  EXPECT_EQ(echo->payload[14] | (echo->payload[15] << 8), 0x1234);

  sent.clear();
  meow::control::on_session_tick(&session, recorder(), now);
  for (const auto &m : sent) {
    EXPECT_NE(m.type, 0x3003) << "each echo is sent once per session";
  }
}

TEST_F(MeowControlStreamTest, TickSendsAppliedBitrate) {
  fake_session_t session;
  fake_peer_t peer;
  session.control.peer = &peer;
  session.meow.applied = session.mail->event<int>(meow::adaptive_bitrate::applied_mail_id);
  // The encoder thread's side of the channel.
  session.mail->event<int>(meow::adaptive_bitrate::applied_mail_id)->raise(25000);

  meow::control::on_session_tick(&session, recorder(), std::chrono::steady_clock::now());
  ASSERT_FALSE(sent.empty());
  const auto &applied = sent.front();
  EXPECT_EQ(applied.type, 0x3005);
  EXPECT_EQ(applied.payload, (std::vector<std::uint8_t> {0x01, 0x00, 0x00, 0x00, 0xA8, 0x61, 0x00, 0x00}));

  sent.clear();
  meow::control::on_session_tick(&session, recorder(), std::chrono::steady_clock::now());
  for (const auto &m : sent) {
    EXPECT_NE(m.type, 0x3005) << "sent once per change";
  }
}

TEST_F(MeowControlStreamTest, TickSendsCursorPositionsOnlyToSubscribersMappedAndCoalesced) {
  int scaler = 0;
  meow::viewport::on_scaler_init(&scaler, 5360, 1440, 1280, 720);
  fake_session_t subscribed;
  fake_session_t stock;
  fake_peer_t peer;
  subscribed.control.peer = &peer;
  stock.control.peer = &peer;
  subscribed.meow.set_subscribed(true);
  auto now = std::chrono::steady_clock::now();

  const auto positions = [this] {
    std::vector<std::vector<std::uint8_t>> out;
    for (const auto &m : sent) {
      if (m.type == 0x3004) {
        out.push_back(m.payload);
      }
    }
    sent.clear();
    return out;
  };

  meow::control::on_session_tick(&subscribed, recorder(), now);
  EXPECT_TRUE(positions().empty()) << "no metadata stream, no positions (the client dead-reckons)";

  meow::cursor::publish(5359, 1439, true);
  meow::control::on_session_tick(&stock, recorder(), now);
  EXPECT_TRUE(positions().empty()) << "never to a client that did not subscribe";

  meow::control::on_session_tick(&subscribed, recorder(), now);
  auto p = positions();
  ASSERT_EQ(p.size(), 1u) << "immediately on subscribe";
  EXPECT_EQ(p[0], (std::vector<std::uint8_t> {0x01, 0x01, 0x00, 0x00, 0xFF, 0x04, 0x12, 0x02})) << "(1279, 530) in the reference frame, seq 0";

  meow::cursor::publish(100, 100, true);
  meow::control::on_session_tick(&subscribed, recorder(), now + 1ms);
  EXPECT_TRUE(positions().empty()) << "coalesced to 60 Hz";
  meow::control::on_session_tick(&subscribed, recorder(), now + 17ms);
  p = positions();
  ASSERT_EQ(p.size(), 1u);
  EXPECT_EQ(p[0][2], 1) << "seq increments per send";

  meow::cursor::publish(100, 100, false);
  meow::control::on_session_tick(&subscribed, recorder(), now + 18ms);
  p = positions();
  ASSERT_EQ(p.size(), 1u) << "visibility changes are not held back";
  EXPECT_EQ(p[0][1], 0x00);

  subscribed.meow.set_subscribed(false);
}

TEST_F(MeowControlStreamTest, TickSamplesHostRttForTheController) {
  fake_session_t session;
  fake_peer_t peer {37, 6};
  session.control.peer = &peer;
  auto rtts = session.mail->queue<meow::adaptive_bitrate::rtt_sample_t>(meow::adaptive_bitrate::rtt_mail_id);
  const auto now = std::chrono::steady_clock::now();

  meow::control::on_session_tick(&session, recorder(), now);
  ASSERT_TRUE(rtts->peek());
  const auto sample = rtts->pop();
  EXPECT_EQ(sample->rtt_ms, 37u);
  EXPECT_EQ(sample->rtt_var_ms, 6u);

  meow::control::on_session_tick(&session, recorder(), now + 100ms);
  EXPECT_FALSE(rtts->peek()) << "sampled every 500 ms, not every loop";
  meow::control::on_session_tick(&session, recorder(), now + 500ms);
  EXPECT_TRUE(rtts->peek());

  config::video.adaptive_bitrate = false;
  rtts->pop();
  meow::control::on_session_tick(&session, recorder(), now + 2s);
  EXPECT_FALSE(rtts->peek()) << "no sampling with the feature off";
}

TEST_F(MeowControlStreamTest, PollsAt60HzOnlyWhileSomeoneFollowsTheCursor) {
  EXPECT_EQ(meow::control::iterate_timeout(150ms), 150ms);
  fake_session_t session;
  session.meow.set_subscribed(true);
  EXPECT_EQ(meow::control::iterate_timeout(150ms), 150ms) << "subscribed, but no position source";
  meow::cursor::publish(1, 1, true);
  EXPECT_EQ(meow::control::iterate_timeout(150ms), meow::control::cursor_poll_interval);
  session.meow.set_subscribed(false);
  EXPECT_EQ(meow::control::iterate_timeout(150ms), 150ms);
}
