/**
 * @file src/meow/adaptive_bitrate_encoder.h
 * @brief Glue between the pure controller and a live FFmpeg encoder.
 *
 * Kept separate from `adaptive_bitrate.h` so that the control logic stays free of FFmpeg,
 * Boost and Sunshine headers and can be unit tested on a machine with no GPU (CLAUDE.md
 * §5.5). Everything here is plumbing: drain the sample queue, advance the controller, write
 * the answer onto the codec context, log it. There are no decisions in this file.
 *
 * ## Which encoders can actually do this, and how we know
 *
 * Measured on this codebase's bundled FFmpeg (libavcodec 62.28.102) with a probe that opens
 * an encoder with Sunshine's own options, encodes 180 frames at 20 Mbps, mutates
 * `bit_rate`/`rc_max_rate`/`rc_min_rate` on the live context, encodes 180 more, then restores
 * them:
 *
 * | encoder      | asked 20 -> 2 Mbps | measured |
 * | ------------ | ------------------ | -------- |
 * | `h264_nvenc` | honoured           | 20.0 -> 10.2 -> 20.0 Mbps |
 * | `libx264`    | **ignored**        | 20.0 -> 20.0 -> 20.0 Mbps |
 *
 * The mechanism is `reconfig_encoder()` in `libavcodec/nvenc.c`, which FFmpeg calls from
 * `nvenc_send_frame()` on every frame. It compares `avctx->bit_rate`, `avctx->rc_max_rate`
 * and `avctx->rc_buffer_size` against the values the session was opened with and, on any
 * difference, calls `nvEncReconfigureEncoder()`. It is gated on
 * `NV_ENC_CAPS_SUPPORT_DYN_BITRATE_CHANGE` and on the rate control not being ConstQP;
 * Sunshine opens NVENC with `rc = cbr`, so the gate is satisfied.
 *
 * FFmpeg sets `params.resetEncoder = 1` and `params.forceIDR = 1` for that call, so **every
 * change costs exactly one IDR frame** — no encoder teardown, no display reinit, no dropped
 * frames. That is why this feature reuses the live context instead of the `reinit` seam in
 * `video.cpp`: the reinit path rebuilds the capture pipeline and the encoder session, which
 * is visible, whereas an IDR is something Sunshine already emits on demand whenever a client
 * asks for one (`IDX_REQUEST_IDR_FRAME`) — which is precisely what a lossy link makes it do
 * anyway.
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

  /**
   * @brief Whether an FFmpeg encoder honours a live `bit_rate` mutation.
   *
   * Allow-listed rather than attempted-and-checked because FFmpeg gives no way to ask: an
   * encoder that ignores the change reports success and simply keeps its old rate. The list
   * is short on purpose — an encoder is added to it only after the probe described in this
   * file's header has been run against it.
   *
   * @param codec_name FFmpeg encoder name, e.g. `h264_nvenc`.
   * @return True when mutating the live context is known to take effect.
   */
  [[nodiscard]] inline bool encoder_supports_live_bitrate(const std::string_view codec_name) {
    return codec_name.find("nvenc") != std::string_view::npos;
  }

  /**
   * @brief Applies the controller's decisions to a live `AVCodecContext`.
   *
   * Owned by the encode session, so its lifetime is exactly the lifetime of the encoder it
   * drives and there is no registry to clean up.
   */
  class governor_t {
  public:
    /**
     * @brief Construct a governor for one encode session.
     *
     * @param mail Session mailbox carrying loss samples from the control thread.
     * @param ctx Live codec context, or `nullptr` for a non-FFmpeg encode session.
     * @param codec_name FFmpeg encoder name used to open `ctx`.
     * @param client_kbps Bitrate the client requested over RTSP.
     * @param host_max_kbps Existing `max_bitrate` setting, in kbps; zero means unlimited.
     * @param cfg_min Configured `adaptive_bitrate_min`, in kbps.
     * @param cfg_max Configured `adaptive_bitrate_max`, in kbps.
     */
    governor_t(
      safe::mail_t mail,
      AVCodecContext *ctx,
      const std::string_view codec_name,
      const int client_kbps,
      const int host_max_kbps,
      const int cfg_min,
      const int cfg_max
    ):
        ctx_ {ctx},
        bounds_ {resolve_bounds(cfg_min, cfg_max, client_kbps, host_max_kbps)},
        controller_ {bounds_, std::chrono::steady_clock::now()},
        mail_ {std::move(mail)} {
      if (cfg_min <= 0) {
        // Not configured. Say nothing: this is the default and the overwhelmingly common case.
        return;
      }

      if (!controller_.enabled()) {
        BOOST_LOG(info)
          << "Adaptive bitrate: disabled for this session - the configured range ["sv << cfg_min << ", "sv << cfg_max
          << "] kbps leaves nothing to adapt between once the client's requested "sv << client_kbps
          << " kbps and max_bitrate are applied"sv;
        return;
      }

      if (!ctx_) {
        BOOST_LOG(info) << "Adaptive bitrate: disabled - this encode session is not an FFmpeg session"sv;
        return;
      }

      if (!encoder_supports_live_bitrate(codec_name)) {
        BOOST_LOG(info)
          << "Adaptive bitrate: disabled - encoder '"sv << codec_name
          << "' ignores a runtime bitrate change (only NVENC implements it in FFmpeg)"sv;
        ctx_ = nullptr;
        return;
      }

      // Preserve, rather than re-derive, the rate-control shape upstream established at
      // init: whether min == max (CBR) or bit_rate == max - 1 (VBR forced), and how large
      // the VBV buffer is relative to the bitrate. Re-deriving would silently discard the
      // nvenc `vbv_percentage_increase` setting.
      const auto opened_at = ctx_->rc_max_rate > 0 ? ctx_->rc_max_rate : ctx_->bit_rate;
      if (opened_at <= 0) {
        BOOST_LOG(warning) << "Adaptive bitrate: disabled - the encoder opened with no rate limit to scale"sv;
        ctx_ = nullptr;
        return;
      }
      cbr_ = ctx_->rc_min_rate > 0;
      vbr_offset_ = opened_at - ctx_->bit_rate;
      buffer_ratio_ = static_cast<double>(ctx_->rc_buffer_size) / static_cast<double>(opened_at);

      samples_ = mail_ ? mail_->queue<loss_sample_t>(mail_id) : nullptr;
      enabled_ = true;

      BOOST_LOG(info)
        << "Adaptive bitrate: enabled, adapting between "sv << bounds_.min_kbps << " and "sv << bounds_.max_kbps
        << " kbps, starting at "sv << controller_.current_kbps() << " kbps"sv;
    }

    /**
     * @brief Drain pending loss samples, advance the controller, apply any change.
     *
     * Cheap enough to call once per encoded frame: it does nothing until a full evaluation
     * window has elapsed.
     */
    void tick() {
      if (!enabled_) {
        return;
      }

      if (samples_) {
        while (samples_->peek()) {
          if (const auto sample = samples_->pop(std::chrono::milliseconds {0})) {
            controller_.observe(*sample);
          } else {
            break;
          }
        }
      }

      const auto decision = controller_.tick(std::chrono::steady_clock::now());
      if (!decision.changed) {
        return;
      }

      apply(decision.kbps);

      BOOST_LOG(info)
        << "Adaptive bitrate: "sv << decision.previous_kbps << " -> "sv << decision.kbps
        << " kbps ("sv << describe(decision.reason) << ", measured loss "sv
        << static_cast<int>(decision.observed_loss * 1000.0 + 0.5) / 10.0 << "%)"sv;
    }

  private:
    /**
     * @brief Write a new bitrate onto the live codec context.
     * @param kbps New bitrate in kbps.
     */
    void apply(const int kbps) {
      const std::int64_t bits = static_cast<std::int64_t>(kbps) * 1000;
      ctx_->rc_max_rate = bits;
      ctx_->bit_rate = bits - vbr_offset_;
      if (cbr_) {
        ctx_->rc_min_rate = bits;
      }
      if (buffer_ratio_ > 0.0) {
        ctx_->rc_buffer_size = static_cast<int>(static_cast<double>(bits) * buffer_ratio_);
      }
    }

    AVCodecContext *ctx_ = nullptr;  ///< Live codec context, or null when not adapting.
    bounds_t bounds_;  ///< Resolved bounds for this session.
    controller_t controller_;  ///< The pure control logic.
    safe::mail_t mail_;  ///< Session mailbox.
    safe::mail_raw_t::queue_t<loss_sample_t> samples_;  ///< Loss samples from the control thread.

    bool enabled_ = false;  ///< Whether this governor will ever change anything.
    bool cbr_ = false;  ///< Whether the session was opened with `rc_min_rate` pinned.
    std::int64_t vbr_offset_ = 0;  ///< `rc_max_rate - bit_rate` at init, preserved on every change.
    double buffer_ratio_ = 0.0;  ///< `rc_buffer_size / rc_max_rate` at init, preserved on every change.
  };

}  // namespace meow::adaptive_bitrate
