/**
 * @file src/meow/adaptive_bitrate_encoder.h
 * @brief Glue between the pure controller and a live FFmpeg encoder.
 *
 * Kept separate from `adaptive_bitrate.h` so that the control logic stays free of FFmpeg,
 * Boost and Sunshine headers and can be unit tested on a machine with no GPU (CLAUDE.md
 * §5.5). Everything here is plumbing: drain the sample queues, advance the controller, write
 * the answer onto the codec context, tell the control thread, log it. There are no decisions
 * in this file.
 *
 * ## Which encoders can actually do this, and how we know
 *
 * Measured 2026-09-24 with `tools/meow/live_bitrate_probe.cpp` on this codebase's bundled
 * FFmpeg (RTX 5050; Radeon 760M via Mesa radeonsi for VA-API): it opens an encoder with the
 * rate-control fields `video.cpp` sets, encodes 1280x720@60 desktop-like content with a busy
 * window, lowers the live context from 8 to 3 Mbps at frame 180 and raises it back at 360.
 * Produced rate per 30-frame window (steady state of each phase):
 *
 * ```
 *               8 Mbps phase     3 Mbps phase     8 Mbps again     IDRs
 *   h264_nvenc  8.000            3.000            8.000            3 / 540 (one per change)
 *   libx264     2.9 - 3.2        1.6 - 1.8        3.0 - 3.5        1 / 540 (none per change)
 *   h264_vaapi  8.006            8.006            8.006            1 / 540 (change ignored)
 *   libx265     1.6 - 1.7        1.6 - 1.7        1.6 - 1.7        1 / 540 (change ignored)
 * ```
 *
 * NVENC tracks the request exactly. libx264 (quality-limited by `superfast`/`zerolatency`
 * with a one-frame VBV, so it never reaches its target) nonetheless moves with it in both
 * directions through `x264_encoder_reconfig()`, without a keyframe - an earlier revision of
 * this file recorded it as ignoring the change, which the measurement above contradicts.
 * VA-API (`vaapi_encode.c` has no reconfigure path) and libx265 ignore it entirely, and
 * report success. QSV and AMF were not measurable on this host and are treated as
 * unsupported. The conclusion is encoded in `encoder_supports_live_bitrate()`.
 *
 * The NVENC mechanism is `reconfig_encoder()` in `libavcodec/nvenc.c`, which FFmpeg calls
 * from `nvenc_send_frame()` on every frame. It compares `avctx->bit_rate`,
 * `avctx->rc_max_rate` and `avctx->rc_buffer_size` against the values the session was opened
 * with and, on any difference, calls `nvEncReconfigureEncoder()` with `resetEncoder = 1` and
 * `forceIDR = 1` - so **every change costs exactly one IDR frame**, with no encoder teardown,
 * no display reinit and no dropped frames. Because Sunshine opens NVENC as CBR with a
 * one-frame VBV, that IDR is not even a bandwidth spike.
 *
 * Encoders that ignore the mutation are not adapted at all, and say so once at info level,
 * because silently doing nothing is the failure mode most likely to waste a user's evening.
 */
#pragma once

// standard includes
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>

// lib includes
extern "C" {
#include <libavcodec/avcodec.h>
}

// local includes
#include "src/logging.h"
#include "src/meow/adaptive_bitrate.h"
#include "src/thread_safe.h"

namespace meow::adaptive_bitrate {

  // Declared locally rather than relying on an upstream header leaking it, so an upstream
  // sync that tidies its own includes cannot break this file.
  using namespace std::literals;

  /**
   * @brief Whether an FFmpeg encoder honours a live `bit_rate` mutation.
   *
   * Allow-listed rather than attempted-and-checked because FFmpeg gives no way to ask: an
   * encoder that ignores the change reports success and simply keeps its old rate. An encoder
   * is added here only after `tools/meow/live_bitrate_probe.cpp` has measured it.
   *
   * @param codec_name FFmpeg encoder name, e.g. `h264_nvenc`.
   * @return True when mutating the live context is known to take effect.
   */
  [[nodiscard]] inline bool encoder_supports_live_bitrate(const std::string_view codec_name) {
    return codec_name.find("nvenc") != std::string_view::npos || codec_name == "libx264";
  }

  /**
   * @brief The one line logged when an encoder cannot change its bitrate at runtime.
   *
   * Returned rather than logged so the wording is testable.
   *
   * @param codec_name FFmpeg encoder name.
   * @return The line.
   */
  [[nodiscard]] inline std::string unsupported_encoder_note(const std::string_view codec_name) {
    return std::string("Adaptive bitrate: unavailable - encoder '").append(codec_name).append("' does not apply a bitrate change while running (measured: NVENC and libx264 do in this FFmpeg build; VA-API and libx265 do not); the stream keeps its negotiated bitrate");
  }

  /**
   * @brief Read the bitrate remembered from a previous encoder session, if any.
   *
   * @param resume Event channel holding the last applied bitrate, possibly null.
   * @return The remembered bitrate in kbps, or 0 when there is none.
   */
  [[nodiscard]] inline int remembered_kbps(const safe::mail_raw_t::event_t<int> &resume) {
    if (!resume || !resume->peek()) {
      return 0;
    }
    const auto value = resume->view(std::chrono::milliseconds {0});
    return value ? *value : 0;
  }

  /**
   * @brief Session-level inputs the governor needs, gathered by the caller.
   */
  struct governor_config_t {
    bool enabled = true;  ///< `meow_adaptive_bitrate`.
    int cfg_min = 0;  ///< `adaptive_bitrate_min`, 0 = automatic.
    int cfg_max = 0;  ///< `adaptive_bitrate_max`, 0 = automatic.
    int negotiated_kbps = 0;  ///< Bitrate the client asked for over RTSP.
    int host_max_kbps = 0;  ///< `max_bitrate`, 0 = unlimited.
    int fec_percentage = 20;  ///< FEC overhead included in the client's goodput.
  };

  /**
   * @brief Applies the controller's decisions to a live `AVCodecContext`.
   *
   * Owned by the encode session, so its lifetime is exactly the lifetime of the encoder it
   * drives and there is no registry to clean up.
   */
  class governor_t {
  public:
    /**
     * @brief Clock the governor reads; injectable so a test can drive it.
     */
    using clock_fn = std::chrono::steady_clock::time_point (*)();

    /**
     * @brief Construct a governor for one encode session.
     *
     * @param mail Session mailbox carrying samples from the control thread.
     * @param ctx Live codec context, or `nullptr` for a non-FFmpeg encode session.
     * @param codec_name FFmpeg encoder name used to open `ctx`.
     * @param config Session-level configuration.
     * @param now Clock; defaults to `std::chrono::steady_clock::now`.
     * @param tuning Controller tuning; defaults are the shipping values.
     */
    governor_t(safe::mail_t mail, AVCodecContext *ctx, const std::string_view codec_name, const governor_config_t &config, const clock_fn now = &std::chrono::steady_clock::now, tuning_t tuning = {}):
        ctx_ {ctx},
        config_ {config},
        now_ {now},
        mail_ {std::move(mail)},
        resume_ {mail_ ? mail_->event<int>(resume_mail_id) : nullptr},
        controller_ {resolve_bounds(config.enabled, config.cfg_min, config.cfg_max, config.negotiated_kbps, config.host_max_kbps), now_(), with_fec(tuning, config.fec_percentage), config.negotiated_kbps, remembered_kbps(resume_)} {
      if (!config_.enabled) {
        BOOST_LOG(info) << "Adaptive bitrate: disabled (meow_adaptive_bitrate = disabled)"sv;
        ctx_ = nullptr;
        return;
      }

      if (!controller_.enabled()) {
        BOOST_LOG(info)
          << "Adaptive bitrate: disabled for this session - no range left to adapt in once the negotiated "sv << config_.negotiated_kbps
          << " kbps, max_bitrate and adaptive_bitrate_min/max are applied"sv;
        ctx_ = nullptr;
        return;
      }

      if (!ctx_) {
        BOOST_LOG(info) << "Adaptive bitrate: unavailable - this encode session is not an FFmpeg session"sv;
        return;
      }

      if (!encoder_supports_live_bitrate(codec_name)) {
        BOOST_LOG(info) << unsupported_encoder_note(codec_name);
        ctx_ = nullptr;
        return;
      }

      // Preserve, rather than re-derive, the rate-control shape upstream established at init.
      const auto opened_at = ctx_->rc_max_rate > 0 ? ctx_->rc_max_rate : ctx_->bit_rate;
      if (opened_at <= 0) {
        BOOST_LOG(warning) << "Adaptive bitrate: disabled - the encoder opened with no rate limit to scale"sv;
        ctx_ = nullptr;
        return;
      }
      shape_ = rate_shape_t::capture(ctx_->bit_rate, ctx_->rc_max_rate, ctx_->rc_min_rate, ctx_->rc_buffer_size);

      if (mail_) {
        samples_ = mail_->queue<loss_sample_t>(mail_id);
        reports_ = mail_->queue<receiver_report_t>(report_mail_id);
        rtts_ = mail_->queue<rtt_sample_t>(rtt_mail_id);
        applied_ = mail_->event<int>(applied_mail_id);
      }
      enabled_ = true;

      const auto &bounds = controller_.bounds();
      BOOST_LOG(info)
        << "Adaptive bitrate: enabled, adapting between "sv << bounds.min_kbps << " and "sv << bounds.max_kbps
        << " kbps, starting at "sv << controller_.current_kbps() << " kbps"sv;

      // Upstream opens the encoder at min(client_requested, max_bitrate), which knows nothing
      // about `adaptive_bitrate_max` or a remembered rate. Reconcile once, here, so the
      // opening rate is the controller's from the first frame.
      if (opened_at != static_cast<std::int64_t>(controller_.current_kbps()) * 1000) {
        const auto opened_kbps = static_cast<int>(opened_at / 1000);
        publish(controller_.current_kbps());
        BOOST_LOG(info) << "Adaptive bitrate: opening bitrate "sv << opened_kbps << " -> "sv << controller_.current_kbps() << " kbps"sv;
      }
    }

    /**
     * @brief Whether this governor will ever change anything.
     * @return True when adapting.
     */
    [[nodiscard]] bool enabled() const {
      return enabled_;
    }

    /**
     * @brief The bitrate currently in force, in kbps.
     * @return The controller's current rate, or 0 when not adapting.
     */
    [[nodiscard]] int current_kbps() const {
      return enabled_ ? controller_.current_kbps() : 0;
    }

    /**
     * @brief Drain pending samples, advance the controller, apply any change.
     *
     * Cheap enough to call once per encoded frame: each queue check is one `peek()`, and the
     * controller does nothing until a full evaluation window has elapsed.
     */
    void tick() {
      if (!enabled_) {
        return;
      }
      const auto now = now_();

      if (samples_) {
        while (samples_->peek()) {
          if (const auto sample = samples_->pop(std::chrono::milliseconds {0})) {
            controller_.observe(*sample);
          } else {
            break;
          }
        }
      }
      if (rtts_) {
        while (rtts_->peek()) {
          if (const auto sample = rtts_->pop(std::chrono::milliseconds {0})) {
            controller_.observe(*sample);
          } else {
            break;
          }
        }
      }
      bool acknowledge = false;
      if (reports_) {
        while (reports_->peek()) {
          const auto report = reports_->pop(std::chrono::milliseconds {0});
          if (!report) {
            break;
          }
          controller_.observe(*report);
          if (!first_report_seen_) {
            first_report_seen_ = true;
            acknowledge = true;
          }
          // Only a client that runs automatic bitrate may raise the ceiling above what it
          // negotiated; a user who fixed their bitrate keeps it as the ceiling.
          const auto client_max = report->auto_bitrate ? static_cast<int>(std::min<std::uint32_t>(report->max_kbps, max_configurable_kbps)) : 0;
          if (client_max != client_max_kbps_) {
            client_max_kbps_ = client_max;
            const auto bounds = resolve_bounds(config_.enabled, config_.cfg_min, config_.cfg_max, config_.negotiated_kbps, config_.host_max_kbps, client_max_kbps_);
            if (bounds.enabled() && bounds != controller_.bounds()) {
              BOOST_LOG(info) << "Adaptive bitrate: client ceiling "sv << client_max_kbps_ << " kbps, now adapting between "sv << bounds.min_kbps << " and "sv << bounds.max_kbps << " kbps"sv;
              const auto clamp = controller_.set_bounds(bounds, now);
              if (clamp.changed) {
                publish(clamp.kbps);
                log(clamp);
                acknowledge = false;
              }
            }
          }
        }
      }

      const auto decision = controller_.tick(now);
      if (decision.rebaselined) {
        BOOST_LOG(info) << "Adaptive bitrate: round-trip time settled at "sv << decision.baseline_ms << " ms with no loss - treating it as a new network path"sv;
      }
      if (decision.changed) {
        publish(decision.kbps);
        log(decision);
      } else if (acknowledge && applied_) {
        // The client gives up on receiver reports after a few with no APPLIED, so the first
        // one is answered even when nothing changed.
        applied_->raise(controller_.current_kbps());
      }
    }

  private:
    /**
     * @brief Copy the FEC share into the tuning the controller uses.
     * @param tuning Tuning to adjust.
     * @param fec_percentage Configured FEC percentage.
     * @return The adjusted tuning.
     */
    [[nodiscard]] static tuning_t with_fec(tuning_t tuning, const int fec_percentage) {
      tuning.fec_percentage = std::clamp(fec_percentage, 0, 255);
      return tuning;
    }

    /**
     * @brief Apply a bitrate to the codec, remember it across reinits and announce it.
     * @param kbps New bitrate in kbps.
     */
    void publish(const int kbps) {
      apply(kbps);
      if (resume_) {
        resume_->raise(kbps);
      }
      if (applied_) {
        applied_->raise(kbps);
      }
    }

    /**
     * @brief Log one change.
     * @param decision The change.
     */
    static void log(const decision_t &decision) {
      BOOST_LOG(info)
        << "Adaptive bitrate: "sv << decision.previous_kbps << " -> "sv << decision.kbps
        << " kbps ("sv << describe(decision.reason) << "; "sv
        << static_cast<int>(decision.observed_damaged * 1000.0 + 0.5) / 10.0 << "% of frames damaged, loss "sv
        << decision.observed_loss_permille << "‰, queueing "sv << decision.queueing_ms << " ms over a "sv
        << decision.baseline_ms << " ms baseline)"sv;
    }

    /**
     * @brief Write a new bitrate onto the live codec context.
     * @param kbps New bitrate in kbps.
     */
    void apply(const int kbps) {
      if (!ctx_) {
        return;
      }
      const auto rates = rates_for(shape_, kbps);
      ctx_->rc_max_rate = rates.rc_max_rate;
      ctx_->bit_rate = rates.bit_rate;
      if (rates.rc_min_rate > 0) {
        ctx_->rc_min_rate = rates.rc_min_rate;
      }
      if (rates.rc_buffer_size > 0) {
        ctx_->rc_buffer_size = rates.rc_buffer_size;
      }
    }

    AVCodecContext *ctx_ = nullptr;  ///< Live codec context, or null when not adapting.
    governor_config_t config_;  ///< Session-level configuration.
    clock_fn now_;  ///< Clock.
    safe::mail_t mail_;  ///< Session mailbox.
    safe::mail_raw_t::event_t<int> resume_;  ///< Bitrate carried across encoder reinits.
    controller_t controller_;  ///< The pure control logic.
    safe::mail_raw_t::queue_t<loss_sample_t> samples_;  ///< FEC loss samples from the control thread.
    safe::mail_raw_t::queue_t<receiver_report_t> reports_;  ///< Receiver reports from the control thread.
    safe::mail_raw_t::queue_t<rtt_sample_t> rtts_;  ///< Host RTT samples from the control thread.
    safe::mail_raw_t::event_t<int> applied_;  ///< Applied bitrate, sent to the client by the control thread.

    bool enabled_ = false;  ///< Whether this governor will ever change anything.
    bool first_report_seen_ = false;  ///< Whether the first receiver report has been answered.
    int client_max_kbps_ = 0;  ///< Last client ceiling seen.
    rate_shape_t shape_;  ///< Rate-control shape captured at encoder init.
  };

}  // namespace meow::adaptive_bitrate
