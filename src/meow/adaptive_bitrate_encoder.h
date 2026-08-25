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
 * Measured on this codebase's bundled FFmpeg (libavcodec 62.28.102, RTX 5050) with a probe
 * that opens an encoder with Sunshine's own options (`preset p1`, `tune ull`, `rc cbr`,
 * `zerolatency`, `surfaces 1`, `delay 0`) at 1280x720@60, encodes 180 frames at 8 Mbps,
 * mutates `bit_rate`/`rc_max_rate`/`rc_min_rate`/`rc_buffer_size` on the live context,
 * encodes 180 more, then restores them. Measured per 30-frame window so the steady state is
 * unambiguous rather than averaged across the transition:
 *
 * ```
 * --- h264_nvenc ---            --- libx264 ---
 *   f  0- 29  8.000 Mbps IDR=1    f  0- 29  8.000 Mbps IDR=1
 *   f180-209  3.000 Mbps IDR=1    f180-209  8.000 Mbps IDR=0   <- lowered here
 *   f360-389  8.000 Mbps IDR=1    f360-389  8.000 Mbps IDR=0   <- raised here
 *   total IDRs: 3 / 540 frames    total IDRs: 1 / 540 frames
 * ```
 *
 * nvenc tracks the request exactly and converges inside the same 0.5 s window; libx264
 * ignores the mutation entirely and reports no error.
 *
 * An earlier revision of this comment recorded "20.0 -> 10.2 -> 20.0 Mbps" for a 20 -> 2 Mbps
 * request and called it honoured. That measurement was taken on synthetic incompressible
 * noise, where the encoder is quality-floored rather than rate-limited, so it said nothing
 * about rate tracking. The numbers above use compressible desktop-like content, which is what
 * this feature actually encodes.
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
 * frames. That IDR is not even a bandwidth spike: because Sunshine opens NVENC as CBR with a
 * one-frame VBV, the probe above measured the forced IDR at 6255 bytes against neighbours of
 * 6250. That is why this feature reuses the live context instead of the `reinit` seam in
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

  // Declared locally rather than relying on an upstream header leaking it, so an upstream
  // sync that tidies its own includes cannot break this file.
  using namespace std::literals;

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
   * @brief Read the bitrate remembered from a previous encoder session, if any.
   *
   * `view()` returns an empty optional when the event has never been raised or has been
   * stopped, so the result is checked rather than dereferenced blind.
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
        mail_ {std::move(mail)},
        resume_ {mail_ ? mail_->event<int>(resume_mail_id) : nullptr},
        controller_ {bounds_, std::chrono::steady_clock::now(), tuning_t {}, remembered_kbps(resume_)} {
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
      shape_ = rate_shape_t::capture(ctx_->bit_rate, ctx_->rc_max_rate, ctx_->rc_min_rate, ctx_->rc_buffer_size);

      samples_ = mail_ ? mail_->queue<loss_sample_t>(mail_id) : nullptr;
      enabled_ = true;

      BOOST_LOG(info)
        << "Adaptive bitrate: enabled, adapting between "sv << bounds_.min_kbps << " and "sv << bounds_.max_kbps
        << " kbps, starting at "sv << controller_.current_kbps() << " kbps"sv;

      // Upstream opens the encoder at min(client_requested, max_bitrate), which does not know
      // about `adaptive_bitrate_max`. When that is the binding limit the session would
      // otherwise run above its configured ceiling forever, because a clean link never
      // produces a decision to bring it down. Reconcile once, here, so the ceiling is real
      // from the first frame rather than only after the first back-off.
      const auto ceiling_bits = static_cast<std::int64_t>(bounds_.max_kbps) * 1000;
      if (opened_at != ceiling_bits) {
        apply(bounds_.max_kbps);
        if (resume_) {
          resume_->raise(bounds_.max_kbps);
        }
        BOOST_LOG(info)
          << "Adaptive bitrate: clamped opening bitrate "sv << (opened_at / 1000) << " -> "sv
          << bounds_.max_kbps << " kbps (adaptive_bitrate_max)"sv;
      }
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
      if (resume_) {
        resume_->raise(decision.kbps);
      }

      BOOST_LOG(info)
        << "Adaptive bitrate: "sv << decision.previous_kbps << " -> "sv << decision.kbps
        << " kbps ("sv << describe(decision.reason) << ", "sv
        << static_cast<int>(decision.observed_damaged * 1000.0 + 0.5) / 10.0 << "% of frames damaged)"sv;
    }

  private:
    /**
     * @brief Write a new bitrate onto the live codec context.
     * @param kbps New bitrate in kbps.
     */
    void apply(const int kbps) {
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
    bounds_t bounds_;  ///< Resolved bounds for this session.
    safe::mail_t mail_;  ///< Session mailbox.
    safe::mail_raw_t::event_t<int> resume_;  ///< Bitrate carried across encoder reinits.
    controller_t controller_;  ///< The pure control logic.
    safe::mail_raw_t::queue_t<loss_sample_t> samples_;  ///< Loss samples from the control thread.

    bool enabled_ = false;  ///< Whether this governor will ever change anything.
    rate_shape_t shape_;  ///< Rate-control shape captured at encoder init.
  };

}  // namespace meow::adaptive_bitrate
