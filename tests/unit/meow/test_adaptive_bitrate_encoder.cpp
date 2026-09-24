/**
 * @file tests/unit/meow/test_adaptive_bitrate_encoder.cpp
 * @brief Test src/meow/adaptive_bitrate_encoder.h: the governor against a real (unopened)
 *        `AVCodecContext`, a real session mailbox and an injected clock.
 */
// test includes
#include "../../tests_common.h"

// standard includes
#include <chrono>
#include <memory>

// lib includes
extern "C" {
#include <libavcodec/avcodec.h>
}

// local includes
#include <src/meow/adaptive_bitrate_encoder.h>

namespace {

  using namespace std::chrono_literals;
  using meow::adaptive_bitrate::governor_config_t;
  using meow::adaptive_bitrate::governor_t;
  using meow::adaptive_bitrate::receiver_report_t;

  /**
   * @brief The injected clock.
   */
  std::chrono::steady_clock::time_point test_now {};

  /**
   * @brief Clock function handed to the governor.
   * @return `test_now`.
   */
  std::chrono::steady_clock::time_point test_clock() {
    return test_now;
  }

  /**
   * @brief Owning pointer for an `AVCodecContext`.
   */
  struct ctx_deleter_t {
    /**
     * @brief Free the context.
     * @param ctx Context.
     */
    void operator()(AVCodecContext *ctx) const {
      avcodec_free_context(&ctx);
    }
  };

  /**
   * @brief A context shaped the way `video.cpp` opens NVENC: CBR, one-frame VBV.
   * @param kbps Opening bitrate.
   * @return The context.
   */
  std::unique_ptr<AVCodecContext, ctx_deleter_t> nvenc_like(const int kbps) {
    std::unique_ptr<AVCodecContext, ctx_deleter_t> ctx {avcodec_alloc_context3(nullptr)};
    ctx->bit_rate = static_cast<std::int64_t>(kbps) * 1000;
    ctx->rc_max_rate = ctx->bit_rate;
    ctx->rc_min_rate = ctx->bit_rate;
    ctx->rc_buffer_size = static_cast<int>(ctx->bit_rate / 60);
    return ctx;
  }

  /**
   * @brief Run frames at 60 fps on the injected clock.
   * @param g Governor.
   * @param seconds How long.
   */
  void run(governor_t &g, const int seconds) {
    for (int i = 0; i < seconds * 60; ++i) {
      test_now += 16667us;
      g.tick();
    }
  }

  /**
   * @brief A clean receiver report.
   * @param max_kbps Client ceiling.
   * @return The report.
   */
  receiver_report_t clean_report(const std::uint32_t max_kbps = 0) {
    receiver_report_t r;
    r.auto_bitrate = true;
    r.interval_ms = 1000;
    r.received_kbps = 5000;
    r.rtt_ms = 20;
    r.max_kbps = max_kbps;
    return r;
  }

}  // namespace

TEST(MeowAdaptiveBitrateGovernor, AnswersTheFirstReportAndRaisesToTheClientCeiling) {
  test_now = std::chrono::steady_clock::time_point {} + 1h;
  auto mail = std::make_shared<safe::mail_raw_t>();
  auto ctx = nvenc_like(10000);
  governor_t g {mail, ctx.get(), "h264_nvenc", governor_config_t {true, 0, 0, 10000, 0, 20}, &test_clock};
  ASSERT_TRUE(g.enabled());
  EXPECT_EQ(g.current_kbps(), 10000);

  // The control thread's ends of the channels.
  auto applied = mail->event<int>(meow::adaptive_bitrate::applied_mail_id);
  auto reports = mail->queue<receiver_report_t>(meow::adaptive_bitrate::report_mail_id);

  reports->raise(clean_report(30000));
  g.tick();
  const auto first = applied->try_pop();
  ASSERT_TRUE(first);
  EXPECT_EQ(*first, 10000) << "once after the first report, even with no change";

  for (int s = 0; s < 60; ++s) {
    reports->raise(clean_report(30000));
    run(g, 1);
  }
  EXPECT_EQ(g.current_kbps(), 30000) << "probed up to the client's ceiling";
  EXPECT_EQ(ctx->rc_max_rate, 30000000);
  EXPECT_EQ(ctx->bit_rate, 30000000);
  EXPECT_EQ(ctx->rc_min_rate, 30000000) << "CBR shape preserved";
  EXPECT_NEAR(ctx->rc_buffer_size, 500000, 3) << "one-frame VBV preserved (to integer rounding)";
  const auto last = applied->try_pop();
  ASSERT_TRUE(last);
  EXPECT_EQ(*last, 30000) << "every change is announced";

  // A remembered rate survives an encoder reinit on the same session.
  auto ctx2 = nvenc_like(10000);
  governor_t again {mail, ctx2.get(), "h264_nvenc", governor_config_t {true, 0, 0, 10000, 0, 20}, &test_clock};
  EXPECT_EQ(again.current_kbps(), 10000) << "clamped: the new session has no client ceiling yet";
}

TEST(MeowAdaptiveBitrateGovernor, AFixedBitrateClientIsNotRaised) {
  test_now = std::chrono::steady_clock::time_point {} + 1h;
  auto mail = std::make_shared<safe::mail_raw_t>();
  auto ctx = nvenc_like(10000);
  governor_t g {mail, ctx.get(), "hevc_nvenc", governor_config_t {true, 0, 0, 10000, 0, 20}, &test_clock};
  auto reports = mail->queue<receiver_report_t>(meow::adaptive_bitrate::report_mail_id);
  for (int s = 0; s < 40; ++s) {
    auto r = clean_report(30000);
    r.auto_bitrate = false;
    reports->raise(r);
    run(g, 1);
  }
  EXPECT_EQ(g.current_kbps(), 10000);
}

TEST(MeowAdaptiveBitrateGovernor, EncodersThatIgnoreRuntimeChangesAreLeftAloneAndSaySo) {
  test_now = std::chrono::steady_clock::time_point {} + 1h;
  auto mail = std::make_shared<safe::mail_raw_t>();
  for (const auto *name : {"h264_vaapi", "hevc_vaapi", "libx265", "libsvtav1", "hevc_qsv", "h264_amf"}) {
    auto ctx = nvenc_like(10000);
    governor_t g {mail, ctx.get(), name, governor_config_t {true, 0, 0, 10000, 0, 20}, &test_clock};
    EXPECT_FALSE(g.enabled()) << name;
    EXPECT_EQ(ctx->bit_rate, 10000000) << name;
  }
  const auto note = meow::adaptive_bitrate::unsupported_encoder_note("h264_vaapi");
  EXPECT_NE(note.find("h264_vaapi"), std::string::npos);
  EXPECT_NE(note.find("negotiated bitrate"), std::string::npos);
}

TEST(MeowAdaptiveBitrateGovernor, OnlyMeasuredEncodersAreAllowed) {
  // tools/meow/live_bitrate_probe.cpp: NVENC tracks exactly, libx264 moves with the request.
  for (const auto *name : {"h264_nvenc", "hevc_nvenc", "av1_nvenc", "libx264"}) {
    EXPECT_TRUE(meow::adaptive_bitrate::encoder_supports_live_bitrate(name)) << name;
  }
  for (const auto *name : {"h264_vaapi", "libx265", "h264_qsv", "h264_amf", "libsvtav1", "libx264rgb", ""}) {
    EXPECT_FALSE(meow::adaptive_bitrate::encoder_supports_live_bitrate(name)) << name;
  }
}

TEST(MeowAdaptiveBitrateGovernor, TheOffSwitchLeavesTheEncoderUntouched) {
  test_now = std::chrono::steady_clock::time_point {} + 1h;
  auto mail = std::make_shared<safe::mail_raw_t>();
  auto ctx = nvenc_like(10000);
  governor_t g {mail, ctx.get(), "h264_nvenc", governor_config_t {false, 0, 0, 10000, 0, 20}, &test_clock};
  EXPECT_FALSE(g.enabled());
  run(g, 5);
  EXPECT_EQ(ctx->bit_rate, 10000000);
  EXPECT_FALSE(mail->event<int>(meow::adaptive_bitrate::applied_mail_id)->peek());
}

TEST(MeowAdaptiveBitrateGovernor, AConfiguredMaximumClampsTheOpeningRate) {
  test_now = std::chrono::steady_clock::time_point {} + 1h;
  auto mail = std::make_shared<safe::mail_raw_t>();
  auto applied = mail->event<int>(meow::adaptive_bitrate::applied_mail_id);
  auto ctx = nvenc_like(20000);
  governor_t g {mail, ctx.get(), "h264_nvenc", governor_config_t {true, 0, 8000, 20000, 0, 20}, &test_clock};
  EXPECT_EQ(g.current_kbps(), 8000);
  EXPECT_EQ(ctx->rc_max_rate, 8000000);
  EXPECT_EQ(applied->try_pop().value_or(0), 8000);
}
