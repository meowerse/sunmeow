/**
 * @file src/meow/adaptive_bitrate.h
 * @brief Pure control logic for adapting the video bitrate to observed network loss.
 *
 * Everything in this header is a pure function or a pure state machine over
 * client-supplied loss reports and an explicitly-passed clock. There is no GPU, no
 * socket, no FFmpeg and no Sunshine dependency, so the whole controller is unit tested
 * by feeding it synthetic loss sequences and asserting the resulting bitrate trajectory
 * (CLAUDE.md §5.5). The two upstream hooks that feed it samples and apply its answer are
 * the only code that touches upstream files.
 *
 * ## Where the loss signal actually comes from
 *
 * `src/stream.cpp` maps `packetTypes[IDX_LOSS_STATS]` (`0x0201`) and logs it. That handler
 * is **dead code for every modern Moonlight client**, and it is not the signal this
 * controller consumes. In `third-party/moonlight-common-c/src/ControlStream.c`:
 *
 *   - `usePeriodicPing = APP_VERSION_AT_LEAST(7, 1, 415)`, and Sunshine advertises a
 *     `GfeVersion` of `3.23.0.74` with a modern `appversion`, so this is always true.
 *     `lossStatsThreadFunc()` therefore takes the `usePeriodicPing` branch and **never**
 *     sends `0x0201` at all. The legacy `else` branch even asserts `!IS_SUNSHINE()`.
 *   - In the legacy branch that Sunshine never reaches, the loss count is a hardcoded
 *     literal `BbPut32(&byteBuffer, 0)` — it carries no loss information even then.
 *
 * The signal a Sunshine host really receives is `SS_FRAME_FEC_PTYPE` (`0x5502`), a
 * Sunshine protocol extension the client emits from `reportFinalFrameFecStatus()` in
 * `RtpVideoQueue.c`. It is **event driven**: the client sends one only when a frame needed
 * FEC recovery or had to be dropped incomplete. Silence therefore means a clean link, which
 * is why this controller is driven by elapsed windows and not by sample arrival.
 *
 * `0x5502` is currently unmapped on the inbound path — `control_server_t::call()` logs
 * unknown types at debug level and drops them. (Sunshine also uses the number `0x5502` for
 * its outbound "Set RGB LED" extension; the two never collide because the host only ever
 * sends that one and only ever receives this one.)
 *
 * ## Why the payload is treated as hostile
 *
 * The report is attacker-controlled: any paired client can send arbitrary bytes at an
 * arbitrary rate. `parse_frame_fec_status()` therefore validates the exact length, decodes
 * the documented big-endian layout, and rejects impossible arithmetic (more packets
 * received than sent, zero packets sent) rather than clamping it into something plausible.
 * The controller additionally saturates its accumulators, so no sequence of reports can
 * overflow them, and clamps its output into the configured bounds on every path, so no
 * sequence of reports can drive the bitrate to zero or past the ceiling (CLAUDE.md §7).
 *
 * ## Why adjustments are rare by construction
 *
 * FFmpeg's `nvenc` applies a live bitrate change with `params.resetEncoder = 1` and
 * `params.forceIDR = 1` (`libavcodec/nvenc.c::reconfig_encoder`), so **every** change costs
 * one IDR frame. That is far cheaper than rebuilding the encoder, but it is not free, so
 * the controller is built to change its mind rarely: a dead band between the two loss
 * thresholds, a required number of *consecutive* windows agreeing before acting, a minimum
 * interval between changes, and a minimum step size below which a change is not worth an
 * IDR. Recovery is deliberately far slower than back-off (AIMD asymmetry).
 */
#pragma once

// standard includes
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <limits>
#include <optional>
#include <string>
#include <string_view>

namespace meow::adaptive_bitrate {

  /**
   * @brief Inbound control-stream message type carrying `SS_FRAME_FEC_STATUS`.
   *
   * Defined by `SS_FRAME_FEC_PTYPE` in `third-party/moonlight-common-c/src/Video.h`. Declared
   * here rather than reused from `packetTypes[]` in `src/stream.cpp` because that array
   * describes the *outbound* meaning of the same number ("Set RGB LED"); binding to it would
   * make this header's correctness depend on an unrelated table's ordering.
   */
  inline constexpr std::uint16_t frame_fec_status_packet_type = 0x5502;

  /**
   * @brief Wire size of `SS_FRAME_FEC_STATUS`.
   *
   * `#pragma pack(1)` struct of `uint32 + 7 * uint16 + 3 * uint8`. A payload of any other
   * length is not this message and is rejected outright — the upstream `IDX_LOSS_STATS`
   * handler reads `stats[0]`..`stats[3]` with no length check at all, which is exactly the
   * out-of-bounds read this parser exists to avoid repeating.
   */
  inline constexpr std::size_t frame_fec_status_size = 4 + 7 * 2 + 3 * 1;

  /**
   * @brief Mailbox channel id carrying samples from the control thread to the encoder thread.
   *
   * A plain string id is used with `safe::mail_raw_t::queue()` so that adding this feature
   * does not require a new `MAIL(x)` entry in the upstream `src/globals.h`.
   */
  inline constexpr std::string_view mail_id = "meow/adaptive_bitrate/loss_samples";

  /**
   * @brief One validated loss observation derived from a client FEC report.
   */
  struct loss_sample_t {
    std::uint32_t packets_sent = 0;  ///< Data + parity packets the host sent for this frame.
    std::uint32_t packets_lost = 0;  ///< Data + parity packets the client did not receive.
    bool frame_recovered = false;  ///< True when FEC reconstructed the frame; false when it was lost.
  };

  /**
   * @brief Effective bitrate bounds for one streaming session, in kbps.
   */
  struct bounds_t {
    int min_kbps = 0;  ///< Never adapt below this. Zero when adaptation is disabled.
    int max_kbps = 0;  ///< Never adapt above this. Zero when adaptation is disabled.

    /**
     * @brief Whether adaptation should run at all for this session.
     * @return True when a usable, non-degenerate range was resolved.
     */
    [[nodiscard]] constexpr bool enabled() const {
      return min_kbps > 0 && max_kbps > min_kbps;
    }
  };

  /**
   * @brief Absolute floor for the configured minimum, in kbps.
   *
   * Below roughly this rate a 720p60 stream stops being usable at all, and a user who typed
   * a nonsense value like `1` gets a warning and this instead of an unwatchable stream.
   */
  inline constexpr int floor_kbps = 500;

  /**
   * @brief Absolute ceiling accepted from configuration, in kbps.
   *
   * Bounds the configured values well below the point where `kbps * 1000` could overflow the
   * `int64_t` bit rate fields, so no configured value can produce a nonsensical encoder
   * setting.
   */
  inline constexpr int max_configurable_kbps = 500000;

  /**
   * @brief Tunable thresholds and timings for the controller.
   *
   * Held in a struct so tests can compress the timings without changing the logic under test.
   * The defaults are the shipping values.
   */
  struct tuning_t {
    std::chrono::milliseconds window {1000};  ///< Length of one evaluation window.
    double loss_high = 0.05;  ///< Window loss fraction at or above which the window is "bad".
    double loss_low = 0.005;  ///< Window loss fraction at or below which the window is "good".
    int bad_windows_to_back_off = 2;  ///< Consecutive bad windows required before backing off.
    int good_windows_to_recover = 10;  ///< Consecutive good windows required before recovering.
    double back_off_factor = 0.75;  ///< Multiplicative decrease applied on back-off.
    double recover_fraction = 0.10;  ///< Additive increase, as a fraction of the ceiling.
    std::chrono::milliseconds min_change_interval {3000};  ///< Floor on the time between two changes.
    int min_step_kbps = 200;  ///< Changes smaller than this are not worth the IDR they cost.
  };

  /**
   * @brief Why the controller changed the bitrate, for the info-level log line.
   */
  enum class reason_t {
    none,  ///< No change.
    sustained_loss,  ///< Loss stayed at or above `loss_high` for long enough.
    link_clean  ///< Loss stayed at or below `loss_low` for long enough.
  };

  /**
   * @brief Result of advancing the controller's clock.
   */
  struct decision_t {
    bool changed = false;  ///< True when `kbps` differs from the previous value.
    int kbps = 0;  ///< The bitrate the encoder should now use.
    int previous_kbps = 0;  ///< The bitrate in force before this decision.
    reason_t reason = reason_t::none;  ///< What triggered the change.
    double observed_loss = 0.0;  ///< Loss fraction of the window that triggered it.
  };

  /**
   * @brief Human-readable form of a change reason, for logging.
   * @param reason The reason to describe.
   * @return A short static string.
   */
  [[nodiscard]] inline constexpr std::string_view describe(const reason_t reason) {
    switch (reason) {
      case reason_t::sustained_loss:
        return "sustained packet loss";
      case reason_t::link_clean:
        return "link clean, recovering toward ceiling";
      case reason_t::none:
      default:
        return "no change";
    }
  }

  /**
   * @brief Decode a big-endian 16-bit field from a byte cursor.
   * @param p Pointer to the first of two bytes.
   * @return The decoded value.
   */
  [[nodiscard]] inline std::uint16_t read_be16(const unsigned char *p) {
    return static_cast<std::uint16_t>((static_cast<std::uint16_t>(p[0]) << 8) | static_cast<std::uint16_t>(p[1]));
  }

  /**
   * @brief Parse and validate a client `SS_FRAME_FEC_STATUS` report.
   *
   * The payload is hostile input from a paired client (CLAUDE.md §7). Anything that is not
   * exactly the documented message, or that describes arithmetic the protocol cannot
   * produce, is rejected rather than coerced — a report claiming more packets received than
   * sent is a buggy or malicious client, and guessing what it "meant" would let it steer the
   * controller.
   *
   * The fields are written by the client with `BE32`/`BE16`, so they are big-endian on the
   * wire regardless of host endianness.
   *
   * @param payload Raw control-stream payload, excluding the 2-byte type header.
   * @return The validated sample, or `std::nullopt` when the payload is not usable.
   */
  [[nodiscard]] inline std::optional<loss_sample_t> parse_frame_fec_status(const std::string_view payload) {
    if (payload.size() != frame_fec_status_size) {
      return std::nullopt;
    }

    const auto *p = reinterpret_cast<const unsigned char *>(payload.data());

    // Layout, from SS_FRAME_FEC_STATUS in moonlight-common-c/src/Video.h:
    //   0  uint32 frameIndex
    //   4  uint16 highestReceivedSequenceNumber
    //   6  uint16 nextContiguousSequenceNumber
    //   8  uint16 missingPacketsBeforeHighestReceived
    //  10  uint16 totalDataPackets
    //  12  uint16 totalParityPackets
    //  14  uint16 receivedDataPackets
    //  16  uint16 receivedParityPackets
    //  18  uint8  fecPercentage
    //  19  uint8  multiFecBlockIndex
    //  20  uint8  multiFecBlockCount
    const std::uint32_t total_data = read_be16(p + 10);
    const std::uint32_t total_parity = read_be16(p + 12);
    const std::uint32_t received_data = read_be16(p + 14);
    const std::uint32_t received_parity = read_be16(p + 16);

    const std::uint32_t sent = total_data + total_parity;
    const std::uint32_t received = received_data + received_parity;

    // A frame with no packets is not a frame. Rejecting this also removes the only
    // division-by-zero in the controller.
    if (sent == 0) {
      return std::nullopt;
    }

    // The client cannot receive packets the host never sent. Either field pair being
    // inconsistent invalidates the whole report.
    if (received_data > total_data || received_parity > total_parity) {
      return std::nullopt;
    }

    loss_sample_t sample;
    sample.packets_sent = sent;
    sample.packets_lost = sent - received;
    // The client only reports at all when it recovered a frame or dropped one; having every
    // data packet means the report describes a successful recovery.
    sample.frame_recovered = received_data >= total_data;
    return sample;
  }

  /**
   * @brief Resolve the effective adaptation bounds for one session.
   *
   * The ceiling can never exceed what the client asked for, nor the existing host-wide
   * `max_bitrate` ceiling, nor the configured adaptive maximum — matching today's rule that
   * the client's request and `max_bitrate` are both hard limits. The configured minimum is
   * clamped down to that ceiling rather than raising it, so a host that wants a 5000 kbps
   * floor never overrides a client that only asked for 2000 kbps.
   *
   * A degenerate result (no configured minimum, or a range that collapsed to a single value)
   * disables adaptation, which reproduces today's fixed-bitrate behaviour exactly.
   *
   * @param cfg_min Configured `adaptive_bitrate_min` in kbps; zero disables adaptation.
   * @param cfg_max Configured `adaptive_bitrate_max` in kbps; zero means "use the ceiling".
   * @param client_kbps Bitrate the client requested over RTSP.
   * @param host_max_kbps Existing `max_bitrate` setting in kbps; zero means unlimited.
   * @return The resolved bounds; `enabled()` is false when adaptation should not run.
   */
  [[nodiscard]] inline bounds_t resolve_bounds(const int cfg_min, const int cfg_max, const int client_kbps, const int host_max_kbps) {
    bounds_t bounds;

    // Adaptation is opt-in: with no configured minimum there is nothing to adapt between.
    if (cfg_min <= 0 || client_kbps <= 0) {
      return bounds;
    }

    int ceiling = client_kbps;
    if (host_max_kbps > 0) {
      ceiling = std::min(ceiling, host_max_kbps);
    }
    if (cfg_max > 0) {
      ceiling = std::min(ceiling, cfg_max);
    }
    if (ceiling <= 0) {
      return bounds;
    }

    // The floor never raises the ceiling: the client's request wins.
    const int floor_value = std::min(cfg_min, ceiling);

    if (floor_value >= ceiling) {
      // Nothing to move between - behave exactly as an unadapted stream would.
      return bounds;
    }

    bounds.min_kbps = floor_value;
    bounds.max_kbps = ceiling;
    return bounds;
  }

  /**
   * @brief Clamp and sanity-check the two configured bounds.
   *
   * Config parsing is a documented untested area of this codebase and the values arrive from
   * a file the user edits by hand, so every impossible combination is corrected loudly rather
   * than silently honoured (CLAUDE.md §7). `warning` is filled in with a message the caller
   * logs; it is empty when the values were already sane.
   *
   * @param cfg_min In/out: configured minimum in kbps, corrected in place.
   * @param cfg_max In/out: configured maximum in kbps, corrected in place.
   * @param warning Out: human-readable description of any correction applied.
   * @return True when the values were usable as given, false when something was corrected.
   */
  inline bool validate_config(int &cfg_min, int &cfg_max, std::string &warning) {
    warning.clear();

    const int orig_min = cfg_min;
    const int orig_max = cfg_max;

    // Negative values are always a typo. Treat them as "unset" rather than guessing a sign.
    if (cfg_min < 0) {
      cfg_min = 0;
    }
    if (cfg_max < 0) {
      cfg_max = 0;
    }

    if (cfg_min == 0) {
      // Adaptation disabled. A maximum on its own does nothing, and saying so is kinder than
      // letting the user believe it is in force.
      if (cfg_max != 0) {
        warning = "adaptive_bitrate_max = " + std::to_string(orig_max) +
                  " has no effect because adaptive_bitrate_min is not set; adaptive bitrate is disabled";
        cfg_max = 0;
        return false;
      }
      if (orig_min < 0) {
        warning = "adaptive_bitrate_min = " + std::to_string(orig_min) +
                  " is negative; adaptive bitrate is disabled";
        return false;
      }
      return true;
    }

    if (cfg_min > max_configurable_kbps) {
      warning = "adaptive_bitrate_min = " + std::to_string(orig_min) + " exceeds the maximum supported " +
                std::to_string(max_configurable_kbps) + " kbps; clamped";
      cfg_min = max_configurable_kbps;
    } else if (cfg_min < floor_kbps) {
      warning = "adaptive_bitrate_min = " + std::to_string(orig_min) + " is below the usable floor of " +
                std::to_string(floor_kbps) + " kbps; clamped";
      cfg_min = floor_kbps;
    }

    if (cfg_max > max_configurable_kbps) {
      if (!warning.empty()) {
        warning += "; ";
      }
      warning += "adaptive_bitrate_max = " + std::to_string(orig_max) + " exceeds the maximum supported " +
                 std::to_string(max_configurable_kbps) + " kbps; clamped";
      cfg_max = max_configurable_kbps;
    }

    // An inverted range cannot be repaired by guessing which end the user meant, so the
    // feature turns itself off and says why. Turning off reproduces today's behaviour, which
    // is the only safe direction to fail.
    if (cfg_max != 0 && cfg_max <= cfg_min) {
      if (!warning.empty()) {
        warning += "; ";
      }
      warning += "adaptive_bitrate_min = " + std::to_string(orig_min) +
                 " is not below adaptive_bitrate_max = " + std::to_string(orig_max) +
                 "; adaptive bitrate is disabled";
      cfg_min = 0;
      cfg_max = 0;
      return false;
    }

    if (orig_min < 0 || orig_max < 0) {
      if (!warning.empty()) {
        warning += "; ";
      }
      warning += "negative adaptive bitrate bounds were ignored";
      return false;
    }

    return warning.empty();
  }

  /**
   * @brief Windowed AIMD bitrate controller.
   *
   * Loss reports arrive irregularly and only when something went wrong, so the controller
   * accumulates them into fixed-length windows and makes at most one decision per window.
   * Within a window it decides nothing; at each boundary it classifies the window as bad
   * (loss at or above `loss_high`), good (loss at or below `loss_low`), or neither.
   *
   * The band between the two thresholds is a deliberate dead zone: a window that lands in it
   * resets *both* counters, so a link hovering around 2% loss neither backs off nor recovers
   * and the bitrate simply holds still. That dead zone, plus the requirement that several
   * *consecutive* windows agree, plus `min_change_interval`, plus `min_step_kbps`, are what
   * make a fast oscillation impossible rather than merely unlikely.
   *
   * All state transitions take the current time as a parameter, so tests drive it with a
   * synthetic clock and no sleeping.
   */
  class controller_t {
  public:
    /**
     * @brief Construct a controller for a session.
     *
     * The stream starts at the ceiling, matching today's behaviour: an upgrade only ever
     * changes what happens *after* loss is observed.
     *
     * @param bounds Resolved bounds for this session.
     * @param start Current time, used as the origin of the first window.
     * @param tuning Thresholds and timings; defaults are the shipping values.
     */
    controller_t(const bounds_t bounds, const std::chrono::steady_clock::time_point start, const tuning_t tuning = {}):
        bounds_ {bounds},
        tuning_ {tuning},
        current_kbps_ {bounds.enabled() ? bounds.max_kbps : 0},
        window_start_ {start},
        last_change_ {start} {
    }

    /**
     * @brief Whether this controller will ever change anything.
     * @return True when the resolved bounds allow adaptation.
     */
    [[nodiscard]] bool enabled() const {
      return bounds_.enabled();
    }

    /**
     * @brief The bitrate currently in force, in kbps.
     * @return The current bitrate, or zero when adaptation is disabled.
     */
    [[nodiscard]] int current_kbps() const {
      return current_kbps_;
    }

    /**
     * @brief Fold one validated loss report into the current window.
     *
     * Accumulators saturate rather than wrap, so no volume of client reports can overflow
     * them into a state that would invert a comparison (CLAUDE.md §7).
     *
     * @param sample A sample returned by `parse_frame_fec_status()`.
     */
    void observe(const loss_sample_t &sample) {
      if (!enabled()) {
        return;
      }
      constexpr std::uint64_t cap = std::numeric_limits<std::uint64_t>::max() / 2;
      if (window_sent_ < cap) {
        window_sent_ += sample.packets_sent;
      }
      if (window_lost_ < cap) {
        window_lost_ += sample.packets_lost;
      }
      if (!sample.frame_recovered && window_unrecovered_ < cap) {
        ++window_unrecovered_;
      }
    }

    /**
     * @brief Advance the clock and, at a window boundary, decide whether to change bitrate.
     *
     * Safe and cheap to call once per encoded frame; it does nothing until a full window has
     * elapsed.
     *
     * @param now Current time.
     * @return The decision; `changed` is false unless the bitrate actually moved.
     */
    decision_t tick(const std::chrono::steady_clock::time_point now) {
      decision_t decision;
      decision.kbps = current_kbps_;
      decision.previous_kbps = current_kbps_;

      if (!enabled()) {
        return decision;
      }

      // A clock that jumped backwards would otherwise stall the controller forever.
      if (now < window_start_) {
        window_start_ = now;
        last_change_ = now;
        return decision;
      }

      if (now - window_start_ < tuning_.window) {
        return decision;
      }

      // A window with no reports at all is a clean window: the client only reports on loss.
      const double loss = window_sent_ > 0 ? static_cast<double>(window_lost_) / static_cast<double>(window_sent_) : 0.0;

      if (loss >= tuning_.loss_high) {
        ++bad_windows_;
        good_windows_ = 0;
      } else if (loss <= tuning_.loss_low && window_unrecovered_ == 0) {
        ++good_windows_;
        bad_windows_ = 0;
      } else {
        // Dead zone: neither direction is justified, so forget any partial streak.
        bad_windows_ = 0;
        good_windows_ = 0;
      }

      const double observed_loss = loss;
      reset_window(now);

      const bool interval_elapsed = (now - last_change_) >= tuning_.min_change_interval;

      if (bad_windows_ >= tuning_.bad_windows_to_back_off && interval_elapsed) {
        const int target = scale_down(current_kbps_);
        if (commit(target, now)) {
          bad_windows_ = 0;
          good_windows_ = 0;
          decision.changed = true;
          decision.kbps = current_kbps_;
          decision.reason = reason_t::sustained_loss;
          decision.observed_loss = observed_loss;
          return decision;
        }
        // Already at the floor, or the step was too small to be worth an IDR. Drop the
        // streak so we do not retry every window against the same wall.
        bad_windows_ = 0;
      } else if (good_windows_ >= tuning_.good_windows_to_recover && interval_elapsed) {
        const int target = step_up(current_kbps_);
        if (commit(target, now)) {
          bad_windows_ = 0;
          good_windows_ = 0;
          decision.changed = true;
          decision.kbps = current_kbps_;
          decision.reason = reason_t::link_clean;
          decision.observed_loss = observed_loss;
          return decision;
        }
        good_windows_ = 0;
      }

      decision.kbps = current_kbps_;
      return decision;
    }

  private:
    /**
     * @brief Start a fresh accumulation window at `now`.
     * @param now Current time.
     */
    void reset_window(const std::chrono::steady_clock::time_point now) {
      window_start_ = now;
      window_sent_ = 0;
      window_lost_ = 0;
      window_unrecovered_ = 0;
    }

    /**
     * @brief Multiplicative decrease, clamped to the configured floor.
     * @param from Current bitrate in kbps.
     * @return The proposed lower bitrate in kbps.
     */
    [[nodiscard]] int scale_down(const int from) const {
      const auto scaled = static_cast<double>(from) * tuning_.back_off_factor;
      const int target = static_cast<int>(scaled);
      return std::max(target, bounds_.min_kbps);
    }

    /**
     * @brief Additive increase, clamped to the ceiling.
     *
     * The step is a fraction of the *ceiling* rather than of the current value, so recovery
     * from deep back-off takes a predictable number of steps instead of crawling.
     *
     * @param from Current bitrate in kbps.
     * @return The proposed higher bitrate in kbps.
     */
    [[nodiscard]] int step_up(const int from) const {
      const auto step = static_cast<double>(bounds_.max_kbps) * tuning_.recover_fraction;
      const int target = from + std::max(1, static_cast<int>(step));
      return std::min(target, bounds_.max_kbps);
    }

    /**
     * @brief Apply a proposed bitrate if it is in range and worth the IDR it will cost.
     * @param target Proposed bitrate in kbps.
     * @param now Current time, recorded as the change time on success.
     * @return True when the bitrate actually moved.
     */
    bool commit(const int target, const std::chrono::steady_clock::time_point now) {
      const int clamped = std::clamp(target, bounds_.min_kbps, bounds_.max_kbps);
      const int delta = clamped > current_kbps_ ? clamped - current_kbps_ : current_kbps_ - clamped;

      // A change that is not worth an IDR is not worth making. The exception is a move that
      // lands exactly on a bound, which is worth taking so the stream can actually reach its
      // floor and ceiling instead of stalling just short of them.
      if (delta == 0) {
        return false;
      }
      if (delta < tuning_.min_step_kbps && clamped != bounds_.min_kbps && clamped != bounds_.max_kbps) {
        return false;
      }

      current_kbps_ = clamped;
      last_change_ = now;
      return true;
    }

    bounds_t bounds_;  ///< Resolved bounds for this session.
    tuning_t tuning_;  ///< Thresholds and timings.
    int current_kbps_ = 0;  ///< Bitrate currently in force.

    std::chrono::steady_clock::time_point window_start_;  ///< Start of the accumulation window.
    std::chrono::steady_clock::time_point last_change_;  ///< When the bitrate last moved.

    std::uint64_t window_sent_ = 0;  ///< Packets sent, this window.
    std::uint64_t window_lost_ = 0;  ///< Packets lost, this window.
    std::uint64_t window_unrecovered_ = 0;  ///< Frames FEC could not rebuild, this window.

    int bad_windows_ = 0;  ///< Consecutive windows at or above `loss_high`.
    int good_windows_ = 0;  ///< Consecutive windows at or below `loss_low`.
  };

}  // namespace meow::adaptive_bitrate
