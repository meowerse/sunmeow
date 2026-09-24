/**
 * @file src/meow/adaptive_bitrate.h
 * @brief Pure control logic for adapting the video bitrate to the network path.
 *
 * Everything in this header is a pure function or a pure state machine over client reports,
 * host round-trip samples and an explicitly-passed clock. There is no GPU, no socket, no
 * FFmpeg and no Sunshine dependency, so the whole controller is unit tested by feeding it
 * synthetic sequences and asserting the resulting bitrate trajectory (CLAUDE.md §5.5). The
 * upstream hooks that feed it samples and apply its answer are the only code that touches
 * upstream files.
 *
 * ## The signals, and where each one actually comes from
 *
 * 1. **FEC status reports** (`SS_FRAME_FEC_PTYPE`, `0x5502`) - a Sunshine protocol extension
 *    every modern Moonlight client emits from `reportFinalFrameFecStatus()` in
 *    `RtpVideoQueue.c`. It is **event driven**: the client sends one only when a frame needed
 *    FEC recovery or had to be dropped incomplete, so silence means a clean link. That is why
 *    the controller is driven by elapsed windows and not by sample arrival. (`src/stream.cpp`
 *    also maps `IDX_LOSS_STATS`, `0x0201`, but no client talking to Sunshine ever sends it:
 *    `lossStatsThreadFunc()` takes the `usePeriodicPing` branch for every modern client.)
 * 2. **Receiver reports** (`0x3005`, meow only) - once a second a moonmeow client reports its
 *    video goodput (incl. FEC), pre-FEC packet loss, its RTT and jitter, its decode queue and
 *    its decode time, plus the highest bitrate its user allows. See `parse_receiver_report()`.
 * 3. **Host round-trip time** - ENet's own `roundTripTime`/`roundTripTimeVariance` for the
 *    control peer, sampled on the control thread. It is available for *every* client, stock
 *    Moonlight included, and it rides the same downlink bottleneck queue as the video, so
 *    queueing delay caused by the video shows up in it before packets start to drop.
 *
 * ## The control law
 *
 * Decisions are made once per window (1 s):
 *
 *  - **Loss back-off** (multiplicative, x0.75) after two consecutive lossy windows. Loss that
 *    FEC repairs is not a reason to back off - that is what FEC is for, and random wireless
 *    loss damages a large share of frames while every one of them still decodes. A window is
 *    lossy when frames could not be rebuilt, when pre-FEC loss exceeds half of what FEC can
 *    carry, or when loss coincides with an elevated round-trip time (a queue overflowing).
 *  - **Delay back-off** (x0.85) *before* loss: when the round-trip time has risen well above
 *    its baseline and is still rising for two consecutive windows, a queue is building.
 *  - **Goodput ceiling.** When the link is *saturated* - losing packets while the round-trip
 *    time is elevated, i.e. a bottleneck queue is overflowing - the loss back-off is also
 *    capped at the goodput the client reported (minus the FEC share), so the link is not asked
 *    to carry far more than it demonstrably delivers. Goodput is *not* used without that
 *    evidence: an idle desktop encodes far below its target, so "goodput below target" on its
 *    own says nothing about the link, and random Wi-Fi loss without a queue is not saturation.
 *  - **Probe up** multiplicatively (~+8% per clean window, at most +25% per change), capped
 *    just below the rate at which the link last congested for a hold period, and at the
 *    ceiling: the client's `max_kbps` when that is above the negotiated rate, otherwise the
 *    negotiated rate.
 *
 * ## Why a path change can never pin the stream to the floor
 *
 * On Tailscale the path switches between a hole-punched DIRECT route and a DERP relay
 * mid-stream, which moves the round-trip time by a large constant step without any
 * congestion. The RTT baseline is therefore a **windowed** minimum (the last 20 windows), not
 * an all-time minimum, and it is **re-baselined** once the RTT has sat above it for five
 * windows with no loss and no goodput drop: a queue caused by overload keeps growing and
 * eventually drops packets, a path change does neither. A re-baseline also forgets the probe
 * cap that a false delay back-off may have set, so the stream climbs straight back.
 *
 * ## Why adjustments are rare by construction
 *
 * FFmpeg's `nvenc` applies a live bitrate change with `params.resetEncoder = 1` and
 * `params.forceIDR = 1` (`libavcodec/nvenc.c::reconfig_encoder`), so **every** change costs
 * one IDR frame. The controller is built to change its mind rarely: dead bands between the
 * high and low thresholds, a required number of *consecutive* windows agreeing before acting,
 * a minimum interval between changes, and a minimum step size below which a change is not
 * worth an IDR. Back-off is fast and probing is deliberately slower (AIMD-like asymmetry).
 *
 * ## Why every payload is treated as hostile
 *
 * Both reports are attacker-controlled: any paired client can send arbitrary bytes at an
 * arbitrary rate. The parsers validate the exact length, decode the documented layout and
 * reject impossible values rather than clamping them into something plausible. The
 * controller saturates its accumulators and clamps its output into the resolved bounds on
 * every path, so no sequence of reports can drive the bitrate to zero or past the ceiling
 * (CLAUDE.md §7).
 */
#pragma once

// standard includes
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
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
   * length is not this message and is rejected outright.
   */
  inline constexpr std::size_t frame_fec_status_size = 4 + 7 * 2 + 3 * 1;

  /**
   * @brief Control-stream message type of the meow receiver report / bitrate applied pair.
   *
   * Must equal `packetTypesGen7Enc[IDX_RECEIVER_REPORT]` on branch `meow` of
   * `meowerse/moonlight-common-c` (`src/ControlStream.c`). The host never indexes `packetTypes`
   * for it; `src/meow/control_stream.h` refuses to register it on a collision.
   */
  inline constexpr std::uint16_t receiver_report_packet_type = 0x3005;

  /**
   * @brief Version byte of both `0x3005` payloads.
   */
  inline constexpr std::uint8_t receiver_report_version = 1;

  /**
   * @brief Exact length of a client -> host receiver report.
   */
  inline constexpr std::size_t receiver_report_length = 24;

  /**
   * @brief Receiver report flag: the client runs automatic bitrate and may be raised.
   */
  inline constexpr std::uint8_t receiver_report_flag_auto_bitrate = 0x01;

  /**
   * @brief Exact length of a host -> client bitrate APPLIED message.
   */
  inline constexpr std::size_t bitrate_applied_length = 8;

  /**
   * @brief Mailbox channel id carrying FEC loss samples from the control thread to the encoder.
   *
   * A plain string id is used with `safe::mail_raw_t::queue()` so that adding this feature
   * does not require a new `MAIL(x)` entry in the upstream `src/globals.h`.
   */
  inline constexpr std::string_view mail_id = "meow/adaptive_bitrate/loss_samples";

  /**
   * @brief Mailbox channel id carrying validated receiver reports to the encoder thread.
   */
  inline constexpr std::string_view report_mail_id = "meow/adaptive_bitrate/receiver_reports";

  /**
   * @brief Mailbox channel id carrying host-measured round-trip samples to the encoder thread.
   */
  inline constexpr std::string_view rtt_mail_id = "meow/adaptive_bitrate/host_rtt";

  /**
   * @brief Mailbox channel id carrying the applied bitrate from the encoder to the control thread.
   *
   * The control thread holds this event for the session's lifetime and sends a `0x3005`
   * APPLIED message whenever the encoder thread raises a value on it.
   */
  inline constexpr std::string_view applied_mail_id = "meow/adaptive_bitrate/applied_kbps";

  /**
   * @brief Mailbox channel id remembering the bitrate in force across an encoder reinit.
   *
   * `encode_run()` returns and is re-entered on every `capture_e::reinit` (display switch, HDR
   * toggle, resolution change), destroying the governor with it. Without this, a session that
   * had backed off because the link is bad would snap back to its opening rate on the next
   * reinit and have to re-converge from scratch - the wrong direction at the worst moment.
   */
  inline constexpr std::string_view resume_mail_id = "meow/adaptive_bitrate/current_kbps";

  /**
   * @brief One validated loss observation derived from a client FEC report.
   */
  struct loss_sample_t {
    std::uint32_t packets_sent = 0;  ///< Data + parity packets the host sent for this frame.
    std::uint32_t packets_lost = 0;  ///< Data + parity packets the client did not receive.
    bool frame_recovered = false;  ///< True when FEC reconstructed the frame; false when it was lost.
  };

  /**
   * @brief One validated `0x3005` receiver report.
   */
  struct receiver_report_t {
    bool auto_bitrate = false;  ///< The client runs automatic bitrate (its `max_kbps` may raise the ceiling).
    std::uint16_t interval_ms = 0;  ///< Length of the interval the report covers.
    std::uint32_t received_kbps = 0;  ///< Video goodput including FEC parity.
    std::uint16_t loss_permille = 0;  ///< Network-lost video packets per thousand expected, pre-FEC.
    std::uint16_t rtt_ms = 0;  ///< Client-measured round-trip time.
    std::uint16_t rtt_var_ms = 0;  ///< Client-measured round-trip variance.
    std::uint16_t decode_queue_frames = 0;  ///< Frames waiting for the client's decoder.
    std::uint16_t avg_decode_ms = 0;  ///< Average decode time over the interval.
    std::uint32_t max_kbps = 0;  ///< Client ceiling; 0 = the negotiated bitrate is the ceiling.
  };

  /**
   * @brief One host-side round-trip measurement of the control peer.
   */
  struct rtt_sample_t {
    std::uint32_t rtt_ms = 0;  ///< ENet `roundTripTime`.
    std::uint32_t rtt_var_ms = 0;  ///< ENet `roundTripTimeVariance`.
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

    bool operator==(const bounds_t &) const = default;
  };

  /**
   * @brief Lowest floor a configured minimum is clamped up to, in kbps.
   *
   * Below roughly this rate a 720p60 stream stops being usable at all, and a user who typed
   * a nonsense value like `1` gets a warning and this instead of an unwatchable stream.
   */
  inline constexpr int floor_kbps = 500;

  /**
   * @brief Absolute floor of the automatic minimum, in kbps.
   *
   * The automatic floor is `max(auto_floor_kbps, auto_floor_percent of the negotiated rate)`.
   */
  inline constexpr int auto_floor_kbps = 1000;

  /**
   * @brief Automatic floor as a percentage of the negotiated bitrate.
   */
  inline constexpr int auto_floor_percent = 25;

  /**
   * @brief Absolute ceiling accepted from configuration or a client, in kbps.
   *
   * Bounds every value well below the point where `kbps * 1000` could overflow the `int64_t`
   * bit rate fields, so nothing can produce a nonsensical encoder setting.
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

    double unrecovered_high = 0.02;  ///< Fraction of frames FEC could NOT rebuild at or above which the window is lossy.
    double damaged_high = 0.15;  ///< Damaged-frame fraction that is lossy *when the RTT is also elevated*.
    int loss_high_permille = 30;  ///< Reported pre-FEC loss that is lossy *when the RTT is also elevated*.
    double loss_beyond_fec = 0.5;  ///< Pre-FEC loss above this share of the FEC percentage is lossy on its own.
    double loss_clean_fec = 0.25;  ///< Pre-FEC loss at or below this share of the FEC percentage still counts as clean.
    int bad_windows_to_back_off = 2;  ///< Consecutive lossy windows required before backing off.
    double loss_back_off_factor = 0.75;  ///< Multiplicative decrease on loss.

    int delay_threshold_ms = 40;  ///< Minimum queueing delay (RTT above baseline) treated as a queue.
    double delay_threshold_fraction = 0.5;  ///< ...or this fraction of the baseline, whichever is larger.
    int delay_gradient_ms = 4;  ///< Rise per window that counts as "still growing".
    int delay_windows_to_back_off = 2;  ///< Consecutive growing-queue windows required before backing off.
    double delay_back_off_factor = 0.85;  ///< Multiplicative decrease on queueing delay.
    int baseline_windows = 20;  ///< Windows the RTT baseline minimum is taken over (at most `max_baseline_windows`).
    int rebaseline_windows = 5;  ///< Windows of elevated-but-harmless RTT after which the baseline moves.
    double goodput_drop_fraction = 0.7;  ///< Goodput below this fraction of its average is a "drop".

    int fec_percentage = 20;  ///< FEC overhead included in the client's goodput.
    double goodput_headroom = 0.95;  ///< Fraction of the measured goodput a saturated link is asked for.

    int good_windows_to_probe = 3;  ///< Consecutive clean windows required before probing up.
    double probe_factor = 1.08;  ///< Increase per clean window, compounded.
    double max_probe_factor = 1.25;  ///< Largest single increase.
    double probe_cap_fraction = 0.95;  ///< Probing stays this far below the last congestion point...
    std::chrono::milliseconds probe_cap_hold {30000};  ///< ...for this long.

    int decode_queue_high = 4;  ///< A decode queue this deep holds probing (the client cannot keep up).

    std::chrono::milliseconds min_change_interval {3000};  ///< Floor on the time between two changes.
    int min_step_kbps = 200;  ///< Changes smaller than this are not worth the IDR they cost.
  };

  /**
   * @brief Largest RTT baseline window the controller can hold without allocating.
   */
  inline constexpr int max_baseline_windows = 64;

  /**
   * @brief Why the controller changed the bitrate, for the info-level log line.
   */
  enum class reason_t {
    none,  ///< No change.
    sustained_loss,  ///< Loss stayed at or above its high threshold for long enough.
    queueing_delay,  ///< The round-trip time kept rising above its baseline.
    link_clean,  ///< The link stayed clean for long enough; probing up.
    ceiling_changed  ///< The client lowered its ceiling below the current rate.
  };

  /**
   * @brief Result of advancing the controller's clock.
   */
  struct decision_t {
    bool changed = false;  ///< True when `kbps` differs from the previous value.
    int kbps = 0;  ///< The bitrate the encoder should now use.
    int previous_kbps = 0;  ///< The bitrate in force before this decision.
    reason_t reason = reason_t::none;  ///< What triggered the change.
    double observed_damaged = 0.0;  ///< Damaged-frame fraction of the window that triggered it.
    int observed_loss_permille = -1;  ///< Reported pre-FEC loss of that window; -1 when none was reported.
    int queueing_ms = -1;  ///< RTT above baseline in that window; -1 when unknown.
    int baseline_ms = -1;  ///< RTT baseline after this window; -1 when unknown.
    bool rebaselined = false;  ///< The RTT baseline moved to a new path this window.
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
      case reason_t::queueing_delay:
        return "round-trip time rising, queue building";
      case reason_t::link_clean:
        return "link clean, probing up";
      case reason_t::ceiling_changed:
        return "client lowered its ceiling";
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
   * @brief Decode a little-endian 16-bit field from a byte cursor.
   * @param p Pointer to the first of two bytes.
   * @return The decoded value.
   */
  [[nodiscard]] inline std::uint16_t read_le16(const unsigned char *p) {
    return static_cast<std::uint16_t>(static_cast<std::uint16_t>(p[0]) | (static_cast<std::uint16_t>(p[1]) << 8));
  }

  /**
   * @brief Decode a little-endian 32-bit field from a byte cursor.
   * @param p Pointer to the first of four bytes.
   * @return The decoded value.
   */
  [[nodiscard]] inline std::uint32_t read_le32(const unsigned char *p) {
    return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) | (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
  }

  /**
   * @brief Parse and validate a client `SS_FRAME_FEC_STATUS` report.
   *
   * The payload is hostile input from a paired client (CLAUDE.md §7). Anything that is not
   * exactly the documented message, or that describes arithmetic the protocol cannot
   * produce, is rejected rather than coerced.
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
    // The client only reports at all when data packets were missing, so `received_data >=
    // total_data` is false for every report it ever sends. Reed-Solomon rebuilds a block from
    // ANY `total_data` of its shards, data or parity, so that is the recovery condition.
    sample.frame_recovered = received >= total_data;
    return sample;
  }

  /**
   * @brief Parse and validate a client `0x3005` receiver report.
   *
   * Layout (little endian, 24 bytes): `u8 version, u8 flags, u16 interval_ms,
   * u32 received_kbps, u16 loss_permille, u16 rtt_ms, u16 rtt_var_ms,
   * u16 decode_queue_frames, u16 avg_decode_ms, u16 reserved, u32 max_kbps`.
   *
   * Short, oversize, unknown-version and impossible payloads (a loss rate above 1000 per
   * mille, a zero-length interval) are rejected outright; unknown flag bits and the reserved
   * field are ignored, which is how the format grows without a version bump.
   *
   * @param payload Raw control-stream payload, excluding the header.
   * @return The validated report, or `std::nullopt` when it must be dropped.
   */
  [[nodiscard]] inline std::optional<receiver_report_t> parse_receiver_report(const std::string_view payload) {
    if (payload.size() != receiver_report_length) {
      return std::nullopt;
    }
    const auto *p = reinterpret_cast<const unsigned char *>(payload.data());
    if (p[0] != receiver_report_version) {
      return std::nullopt;
    }

    receiver_report_t r;
    r.auto_bitrate = (p[1] & receiver_report_flag_auto_bitrate) != 0;
    r.interval_ms = read_le16(p + 2);
    r.received_kbps = read_le32(p + 4);
    r.loss_permille = read_le16(p + 8);
    r.rtt_ms = read_le16(p + 10);
    r.rtt_var_ms = read_le16(p + 12);
    r.decode_queue_frames = read_le16(p + 14);
    r.avg_decode_ms = read_le16(p + 16);
    // p[18..19] reserved
    r.max_kbps = read_le32(p + 20);

    if (r.interval_ms == 0 || r.loss_permille > 1000) {
      return std::nullopt;
    }
    return r;
  }

  /**
   * @brief Serialize a host -> client `0x3005` APPLIED message.
   *
   * Layout (little endian, 8 bytes): `u8 version=1, u8 flags=0, u16 reserved=0,
   * u32 applied_kbps`.
   *
   * @param applied_kbps Bitrate now in force.
   * @param out Destination, at least `bitrate_applied_length` bytes.
   */
  inline void write_bitrate_applied(const std::uint32_t applied_kbps, std::uint8_t *const out) noexcept {
    out[0] = receiver_report_version;
    out[1] = 0;
    out[2] = 0;
    out[3] = 0;
    out[4] = static_cast<std::uint8_t>(applied_kbps & 0xFF);
    out[5] = static_cast<std::uint8_t>((applied_kbps >> 8) & 0xFF);
    out[6] = static_cast<std::uint8_t>((applied_kbps >> 16) & 0xFF);
    out[7] = static_cast<std::uint8_t>((applied_kbps >> 24) & 0xFF);
  }

  /**
   * @brief The automatic floor for a negotiated bitrate.
   *
   * @param negotiated_kbps Bitrate the stream opened at.
   * @return `max(auto_floor_kbps, auto_floor_percent% of negotiated_kbps)`.
   */
  [[nodiscard]] inline constexpr int automatic_floor(const int negotiated_kbps) {
    const auto share = static_cast<int>(static_cast<std::int64_t>(std::max(negotiated_kbps, 0)) * auto_floor_percent / 100);
    return std::max(auto_floor_kbps, share);
  }

  /**
   * @brief The share of the client's bitrate budget Sunshine spends on things other than video.
   *
   * A client asks for a *total* bitrate. `rtsp.cpp` turns that into the encoder bitrate by
   * making room for FEC parity, audio and packet overhead, so the client's numbers (`max_kbps`,
   * the bitrate it negotiates) and the encoder's are different units. Both conversions below
   * use exactly that formula, so a client that remembers an APPLIED rate and negotiates it next
   * session gets the same encoder rate back instead of a lower one each time.
   */
  struct wire_budget_t {
    int fec_percentage = 20;  ///< `fec_percentage` (parity shards per 100 data shards).
    int audio_kbps = 192;  ///< Audio budget: 256 (high quality) or 96 kbps per channel.
  };

  /**
   * @brief A client bitrate in the encoder's units: `rtsp.cpp`'s adjustment, verbatim.
   *
   * @param client_kbps Total bitrate as the client states it.
   * @param budget FEC and audio budget of the session.
   * @return The encoder bitrate `rtsp.cpp` would configure for it.
   */
  [[nodiscard]] inline int client_to_encoder_kbps(const int client_kbps, const wire_budget_t &budget) {
    if (client_kbps <= 0) {
      return 0;
    }
    std::int64_t kbps = client_kbps;
    if (budget.fec_percentage <= 80) {
      kbps /= 100.f / static_cast<float>(100 - budget.fec_percentage);
    }
    kbps -= std::min(static_cast<std::int64_t>(std::max(budget.audio_kbps, 0)), kbps / 5);
    kbps -= std::min(static_cast<std::int64_t>(500), kbps / 10);
    return static_cast<int>(std::clamp<std::int64_t>(kbps, 0, max_configurable_kbps));
  }

  /**
   * @brief An encoder bitrate in the client's units: the smallest total bitrate `rtsp.cpp`
   *        would turn into at least that encoder bitrate.
   *
   * @param encoder_kbps Encoder bitrate.
   * @param budget FEC and audio budget of the session.
   * @return The client-side total bitrate.
   */
  [[nodiscard]] inline int encoder_to_client_kbps(const int encoder_kbps, const wire_budget_t &budget) {
    if (encoder_kbps <= 0) {
      return 0;
    }
    // Invert the three steps analytically, then settle the rounding by search.
    double y = encoder_kbps >= 4500 ? encoder_kbps + 500.0 : encoder_kbps * 10.0 / 9.0;
    const double audio = std::max(budget.audio_kbps, 0);
    double x = y >= 4 * audio ? y + audio : y * 5.0 / 4.0;
    if (budget.fec_percentage <= 80) {
      x = x * 100.0 / (100 - budget.fec_percentage);
    }
    auto client = static_cast<int>(std::min<double>(x, max_configurable_kbps * 2.0));
    for (int i = 0; i < 64 && client_to_encoder_kbps(client, budget) < encoder_kbps; ++i) {
      ++client;
    }
    for (int i = 0; i < 64 && client > 1 && client_to_encoder_kbps(client - 1, budget) >= encoder_kbps; ++i) {
      --client;
    }
    return client;
  }

  /**
   * @brief Resolve the effective adaptation bounds for one session.
   *
   * The **ceiling** is the negotiated bitrate, raised to the client's `max_kbps` when the
   * client runs automatic bitrate and advertises more than it negotiated (it negotiates a
   * remembered *starting* rate and advertises its user's configured maximum). It never
   * exceeds the host's `max_bitrate` or the configured `adaptive_bitrate_max`.
   *
   * The **floor** is the configured `adaptive_bitrate_min` when set, otherwise
   * `max(1000 kbps, 25% of negotiated)`, and never above the ceiling.
   *
   * A degenerate result (feature off, or a range that collapsed to a single value) disables
   * adaptation, which reproduces a fixed-bitrate stream exactly.
   *
   * @param enabled Whether `meow_adaptive_bitrate` is on.
   * @param cfg_min Configured `adaptive_bitrate_min` in kbps; zero means automatic.
   * @param cfg_max Configured `adaptive_bitrate_max` in kbps; zero means automatic.
   * @param negotiated_kbps Bitrate the stream opened at (the client's RTSP request).
   * @param host_max_kbps Existing `max_bitrate` setting in kbps; zero means unlimited.
   * @param client_max_kbps Client ceiling from a receiver report; zero when there is none.
   * @return The resolved bounds; `enabled()` is false when adaptation should not run.
   */
  [[nodiscard]] inline bounds_t resolve_bounds(const bool enabled, const int cfg_min, const int cfg_max, const int negotiated_kbps, const int host_max_kbps, const int client_max_kbps = 0) {
    bounds_t bounds;
    if (!enabled || negotiated_kbps <= 0) {
      return bounds;
    }

    int ceiling = std::min(negotiated_kbps, max_configurable_kbps);
    if (client_max_kbps > ceiling) {
      ceiling = std::min(client_max_kbps, max_configurable_kbps);
    }
    if (host_max_kbps > 0) {
      ceiling = std::min(ceiling, host_max_kbps);
    }
    if (cfg_max > 0) {
      ceiling = std::min(ceiling, cfg_max);
    }
    if (ceiling <= 0) {
      return bounds;
    }

    // The floor never raises the ceiling.
    const int floor_value = std::min(cfg_min > 0 ? cfg_min : automatic_floor(negotiated_kbps), ceiling);
    if (floor_value >= ceiling) {
      return bounds;
    }

    bounds.min_kbps = floor_value;
    bounds.max_kbps = ceiling;
    return bounds;
  }

  /**
   * @brief Clamp and sanity-check the two configured bounds.
   *
   * Both are optional: zero means "automatic". The values arrive from a file the user edits
   * by hand, so every impossible combination is corrected loudly rather than silently
   * honoured (CLAUDE.md §7). `warning` is filled in with a message the caller logs; it is
   * empty when the values were already sane.
   *
   * The feature itself is switched with `meow_adaptive_bitrate`, not with these, so no
   * correction here ever turns adaptation off.
   *
   * @param cfg_min In/out: configured minimum in kbps, corrected in place.
   * @param cfg_max In/out: configured maximum in kbps, corrected in place.
   * @param warning Out: human-readable description of any correction applied.
   * @return True when the values were usable as given, false when something was corrected.
   */
  inline bool validate_config(int &cfg_min, int &cfg_max, std::string &warning) {
    warning.clear();
    const auto append = [&warning](const std::string &text) {
      if (!warning.empty()) {
        warning += "; ";
      }
      warning += text;
    };

    const int orig_min = cfg_min;
    const int orig_max = cfg_max;

    // Negative values are always a typo. Treat them as "automatic" rather than guessing a sign.
    if (cfg_min < 0) {
      append("adaptive_bitrate_min = " + std::to_string(orig_min) + " is negative; using the automatic floor");
      cfg_min = 0;
    } else if (cfg_min > max_configurable_kbps) {
      append("adaptive_bitrate_min = " + std::to_string(orig_min) + " exceeds the maximum supported " + std::to_string(max_configurable_kbps) + " kbps; clamped");
      cfg_min = max_configurable_kbps;
    } else if (cfg_min > 0 && cfg_min < floor_kbps) {
      append("adaptive_bitrate_min = " + std::to_string(orig_min) + " is below the usable floor of " + std::to_string(floor_kbps) + " kbps; clamped");
      cfg_min = floor_kbps;
    }

    if (cfg_max < 0) {
      append("adaptive_bitrate_max = " + std::to_string(orig_max) + " is negative; using the automatic ceiling");
      cfg_max = 0;
    } else if (cfg_max > max_configurable_kbps) {
      append("adaptive_bitrate_max = " + std::to_string(orig_max) + " exceeds the maximum supported " + std::to_string(max_configurable_kbps) + " kbps; clamped");
      cfg_max = max_configurable_kbps;
    }

    // An inverted range cannot be repaired by guessing which end the user meant. The floor is
    // the safety property (never starve the stream), so it is kept and the ceiling is dropped
    // back to automatic.
    if (cfg_min > 0 && cfg_max > 0 && cfg_max <= cfg_min) {
      append("adaptive_bitrate_max = " + std::to_string(orig_max) + " is not above adaptive_bitrate_min = " + std::to_string(orig_min) + "; the maximum is ignored");
      cfg_max = 0;
    }

    return warning.empty();
  }

  /**
   * @brief The rate-control shape an encoder session was opened with.
   *
   * `video.cpp` establishes a specific relationship between `bit_rate`, `rc_max_rate`,
   * `rc_min_rate` and `rc_buffer_size` at init: CBR pins `rc_min_rate` to the bitrate, the
   * `CBR_WITH_VBR` encoders instead set `bit_rate = rc_max_rate - 1` to force VBR mode, and
   * the VBV buffer is a fraction of the bitrate that the nvenc `vbv_percentage_increase`
   * setting can enlarge. Re-deriving those from scratch on every change would silently
   * discard that tuning, so the shape is captured once and re-applied proportionally.
   */
  struct rate_shape_t {
    bool cbr = false;  ///< Whether `rc_min_rate` was pinned to the bitrate.
    std::int64_t vbr_offset = 0;  ///< `rc_max_rate - bit_rate` at init (1 for `CBR_WITH_VBR`, else 0).
    double buffer_ratio = 0.0;  ///< `rc_buffer_size / rc_max_rate` at init; 0 when unlimited.

    /**
     * @brief Capture the shape from the values an encoder session was opened with.
     *
     * @param bit_rate `AVCodecContext::bit_rate` at init.
     * @param rc_max_rate `AVCodecContext::rc_max_rate` at init.
     * @param rc_min_rate `AVCodecContext::rc_min_rate` at init.
     * @param rc_buffer_size `AVCodecContext::rc_buffer_size` at init.
     * @return The captured shape.
     */
    [[nodiscard]] static rate_shape_t capture(
      const std::int64_t bit_rate,
      const std::int64_t rc_max_rate,
      const std::int64_t rc_min_rate,
      const int rc_buffer_size
    ) {
      const std::int64_t opened = rc_max_rate > 0 ? rc_max_rate : bit_rate;
      rate_shape_t shape;
      if (opened <= 0) {
        return shape;
      }
      shape.cbr = rc_min_rate > 0;
      shape.vbr_offset = opened - bit_rate;
      shape.buffer_ratio = static_cast<double>(rc_buffer_size) / static_cast<double>(opened);
      return shape;
    }
  };

  /**
   * @brief The four rate-control values to write for a new bitrate.
   */
  struct rates_t {
    std::int64_t bit_rate = 0;  ///< New `AVCodecContext::bit_rate`.
    std::int64_t rc_max_rate = 0;  ///< New `AVCodecContext::rc_max_rate`.
    std::int64_t rc_min_rate = 0;  ///< New `AVCodecContext::rc_min_rate`; 0 leaves it alone.
    int rc_buffer_size = 0;  ///< New `AVCodecContext::rc_buffer_size`; 0 leaves it alone.
  };

  /**
   * @brief Scale a captured rate-control shape to a new bitrate.
   *
   * @param shape Shape captured at encoder init.
   * @param kbps New bitrate in kbps.
   * @return The values to write onto the codec context.
   */
  [[nodiscard]] inline rates_t rates_for(const rate_shape_t &shape, const int kbps) {
    const std::int64_t bits = static_cast<std::int64_t>(kbps) * 1000;
    rates_t rates;
    rates.rc_max_rate = bits;
    rates.bit_rate = bits - shape.vbr_offset;
    rates.rc_min_rate = shape.cbr ? bits : 0;
    if (shape.buffer_ratio > 0.0) {
      rates.rc_buffer_size = static_cast<int>(static_cast<double>(bits) * shape.buffer_ratio);
    }
    return rates;
  }

  /**
   * @brief Windowed bitrate controller over loss, delay and goodput.
   *
   * See the file comment for the control law. All state transitions take the current time as
   * a parameter, so tests drive it with a synthetic clock and no sleeping; nothing in here
   * allocates, so it is safe to tick once per encoded frame.
   */
  class controller_t {
  public:
    /**
     * @brief Construct a controller for a session.
     *
     * The stream starts at the rate it was negotiated at, clamped into the bounds, or at the
     * rate remembered from a previous encoder session.
     *
     * @param bounds Resolved bounds for this session.
     * @param start Current time, used as the origin of the first window.
     * @param tuning Thresholds and timings; defaults are the shipping values.
     * @param start_kbps Bitrate the encoder opened at, clamped into the bounds. Zero starts at the ceiling.
     * @param resume_kbps Bitrate carried over from a previous encoder session; wins over
     *        `start_kbps` when non-zero.
     */
    controller_t(const bounds_t bounds, const std::chrono::steady_clock::time_point start, const tuning_t tuning = {}, const int start_kbps = 0, const int resume_kbps = 0):
        bounds_ {bounds},
        tuning_ {tuning},
        window_start_ {start},
        last_change_ {start},
        cap_until_ {start} {
      tuning_.baseline_windows = std::clamp(tuning_.baseline_windows, 1, max_baseline_windows);
      if (bounds_.enabled()) {
        const int initial = resume_kbps > 0 ? resume_kbps : (start_kbps > 0 ? start_kbps : bounds_.max_kbps);
        current_kbps_ = std::clamp(initial, bounds_.min_kbps, bounds_.max_kbps);
      }
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
     * @brief The bounds currently in force.
     * @return The bounds.
     */
    [[nodiscard]] const bounds_t &bounds() const {
      return bounds_;
    }

    /**
     * @brief The current RTT baseline, in ms.
     * @return The windowed-minimum RTT, or -1 before any sample.
     */
    [[nodiscard]] int baseline_ms() const {
      return baseline();
    }

    /**
     * @brief Replace the bounds, e.g. when a receiver report raises the client ceiling.
     *
     * The current rate is clamped into the new range on the spot; a clamp that moves it is
     * returned as a decision so the caller applies and announces it like any other change.
     *
     * @param bounds New bounds; ignored unless `enabled()`.
     * @param now Current time.
     * @return The decision; `changed` is true when the current rate had to move.
     */
    decision_t set_bounds(const bounds_t bounds, const std::chrono::steady_clock::time_point now) {
      decision_t decision;
      decision.kbps = current_kbps_;
      decision.previous_kbps = current_kbps_;
      if (!bounds.enabled() || !enabled() || bounds == bounds_) {
        return decision;
      }
      bounds_ = bounds;
      const int clamped = std::clamp(current_kbps_, bounds_.min_kbps, bounds_.max_kbps);
      if (clamped != current_kbps_) {
        current_kbps_ = clamped;
        last_change_ = now;
        decision.changed = true;
        decision.kbps = clamped;
        decision.reason = reason_t::ceiling_changed;
      }
      return decision;
    }

    /**
     * @brief Fold one validated FEC loss report into the current window.
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
      saturating_increment(window_damaged_);
      if (!sample.frame_recovered) {
        saturating_increment(window_unrecovered_);
      }
    }

    /**
     * @brief Fold one validated receiver report into the current window.
     *
     * Several reports inside one window keep the worst loss and the latest everything else.
     *
     * @param report A report returned by `parse_receiver_report()`.
     */
    void observe(const receiver_report_t &report) {
      if (!enabled()) {
        return;
      }
      if (!window_report_) {
        window_report_ = report;
      } else {
        const auto worst = std::max(window_report_->loss_permille, report.loss_permille);
        window_report_ = report;
        window_report_->loss_permille = worst;
      }
      if (report.rtt_ms > 0) {
        window_client_rtt_min_ = std::min<std::uint32_t>(window_client_rtt_min_, report.rtt_ms);
      }
    }

    /**
     * @brief Fold one host-measured round-trip sample into the current window.
     *
     * The window keeps the *minimum* sample, which is what makes a Wi-Fi jitter burst inside
     * one window invisible to the delay detector.
     *
     * @param sample ENet round-trip measurement of the control peer.
     */
    void observe(const rtt_sample_t &sample) {
      if (!enabled() || sample.rtt_ms == 0) {
        return;
      }
      window_host_rtt_min_ = std::min(window_host_rtt_min_, sample.rtt_ms);
      window_rtt_var_ = sample.rtt_var_ms;
    }

    /**
     * @brief Advance the clock and, at a window boundary, decide whether to change bitrate.
     *
     * Safe and cheap to call once per encoded frame; it does nothing until a full window has
     * elapsed. Every call counts one encoded frame, which is the denominator of the
     * damaged-frame fraction.
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
        reset_window(now);
        last_change_ = now;
        cap_until_ = now;
        return decision;
      }

      saturating_increment(window_frames_);

      if (now - window_start_ < tuning_.window) {
        return decision;
      }

      evaluate_window(now, decision);
      reset_window(now);
      return decision;
    }

  private:
    /**
     * @brief Increment a counter without letting it wrap.
     * @param counter Counter to increment.
     */
    static void saturating_increment(std::uint64_t &counter) {
      if (counter < std::numeric_limits<std::uint64_t>::max() / 2) {
        ++counter;
      }
    }

    /**
     * @brief The windowed-minimum RTT baseline.
     * @return The baseline in ms, or -1 when no window has had a sample yet.
     */
    [[nodiscard]] int baseline() const {
      if (baseline_count_ == 0) {
        return -1;
      }
      std::uint32_t lowest = std::numeric_limits<std::uint32_t>::max();
      for (int i = 0; i < baseline_count_; ++i) {
        lowest = std::min(lowest, baseline_ring_[static_cast<std::size_t>(i)]);
      }
      return static_cast<int>(lowest);
    }

    /**
     * @brief Push one window's RTT into the baseline ring.
     * @param rtt_ms The window's minimum RTT.
     */
    void push_baseline(const std::uint32_t rtt_ms) {
      baseline_ring_[static_cast<std::size_t>(baseline_next_)] = rtt_ms;
      baseline_next_ = (baseline_next_ + 1) % tuning_.baseline_windows;
      baseline_count_ = std::min(baseline_count_ + 1, tuning_.baseline_windows);
    }

    /**
     * @brief Forget the baseline and start it again from one sample (a new network path).
     * @param rtt_ms The RTT of the new path.
     */
    void rebaseline(const std::uint32_t rtt_ms) {
      baseline_count_ = 0;
      baseline_next_ = 0;
      push_baseline(rtt_ms);
    }

    /**
     * @brief Classify the window that just ended and make at most one decision.
     * @param now Current time.
     * @param decision In/out: the decision being built.
     */
    void evaluate_window(const std::chrono::steady_clock::time_point now, decision_t &decision) {
      // ---- loss ------------------------------------------------------------------------
      // The denominator is the frames WE ENCODED this window, not the packets the client
      // happened to mention: the client reports only damaged frames, so "lost / reported"
      // measures how bad the bad frames were and is inflated by roughly the frame rate.
      double damaged = 0.0;
      double unrecovered = 0.0;
      if (window_frames_ > 0) {
        damaged = std::min(1.0, static_cast<double>(window_damaged_) / static_cast<double>(window_frames_));
        unrecovered = std::min(1.0, static_cast<double>(window_unrecovered_) / static_cast<double>(window_frames_));
      }
      const int loss = window_report_ ? static_cast<int>(window_report_->loss_permille) : -1;
      decision.observed_damaged = damaged;
      decision.observed_loss_permille = loss;

      // ---- goodput ---------------------------------------------------------------------
      const bool have_goodput = window_report_ && window_report_->received_kbps > 0;
      const auto goodput = have_goodput ? static_cast<double>(window_report_->received_kbps) : 0.0;
      const bool goodput_dropped = have_goodput && goodput_avg_kbps_ > 0.0 && goodput < goodput_avg_kbps_ * tuning_.goodput_drop_fraction;

      // ---- delay -----------------------------------------------------------------------
      // Host ENet RTT first (it is measured on the path the video shares, for every client),
      // the client's own figure when the host has none this window.
      std::uint32_t window_rtt = window_host_rtt_min_;
      if (window_rtt == std::numeric_limits<std::uint32_t>::max()) {
        window_rtt = window_client_rtt_min_;
      }
      const bool have_rtt = window_rtt != std::numeric_limits<std::uint32_t>::max();
      bool elevated = false;
      bool growing = false;
      if (have_rtt) {
        const int base = baseline();
        if (base >= 0) {
          const int queueing = static_cast<int>(window_rtt) - base;
          const int jitter_allowance = window_report_ ? 2 * static_cast<int>(window_report_->rtt_var_ms) : 0;
          const int threshold = std::max({tuning_.delay_threshold_ms, static_cast<int>(base * tuning_.delay_threshold_fraction), 2 * static_cast<int>(window_rtt_var_), jitter_allowance});
          elevated = queueing > threshold;
          growing = previous_window_rtt_ >= 0 && static_cast<int>(window_rtt) >= previous_window_rtt_ + tuning_.delay_gradient_ms;
          decision.queueing_ms = std::max(queueing, 0);
        }
        previous_window_rtt_ = static_cast<int>(window_rtt);
      }

      if (elevated && growing) {
        ++delay_windows_;
      } else {
        delay_windows_ = 0;
      }

      // ---- classify ----------------------------------------------------------------------
      // Loss that FEC repairs is what FEC is for. Random Wi-Fi or cellular loss of 0.5% damages
      // ~16% of 35-packet frames and every one of them is still decoded, so backing off on it
      // would ratchet an ordinary wireless link to the floor for nothing. Loss is only a reason
      // to back off when it hurts: frames FEC could not rebuild, loss beyond what FEC can carry,
      // or loss together with a rising round-trip time - the signature of a queue overflowing.
      const double fec_permille = 10.0 * tuning_.fec_percentage;
      const bool lossy = unrecovered >= tuning_.unrecovered_high || loss >= fec_permille * tuning_.loss_beyond_fec || (elevated && (damaged >= tuning_.damaged_high || loss >= tuning_.loss_high_permille));
      const bool clean = window_unrecovered_ == 0 && !elevated && loss <= fec_permille * tuning_.loss_clean_fec;

      // A path change moves the RTT by a step and then sits still, without loss and without
      // the goodput falling. A queue keeps growing and ends in loss. After enough windows of
      // the former, the elevated RTT *is* the path, so it becomes the baseline.
      if (have_rtt && elevated && !growing && !lossy && !goodput_dropped) {
        ++rebaseline_windows_;
      } else {
        rebaseline_windows_ = 0;
      }
      if (have_rtt) {
        if (rebaseline_windows_ >= tuning_.rebaseline_windows) {
          rebaseline(window_rtt);
          rebaseline_windows_ = 0;
          delay_windows_ = 0;
          // A delay back-off that was really a path change must not keep probing capped.
          if (last_back_off_ == reason_t::queueing_delay) {
            cap_until_ = now;
          }
          decision.rebaselined = true;
        } else {
          push_baseline(window_rtt);
        }
      }
      decision.baseline_ms = baseline();

      if (have_goodput && clean && !elevated) {
        goodput_avg_kbps_ = goodput_avg_kbps_ > 0.0 ? goodput_avg_kbps_ * 0.8 + goodput * 0.2 : goodput;
      }

      // ---- streaks ---------------------------------------------------------------------
      const bool decoder_overloaded = window_report_ && window_report_->decode_queue_frames >= tuning_.decode_queue_high;
      if (lossy) {
        ++bad_windows_;
        good_windows_ = 0;
      } else if (clean && !elevated && !decoder_overloaded) {
        ++good_windows_;
        bad_windows_ = 0;
      } else {
        // Dead zone: neither direction is justified, so forget any partial streak.
        bad_windows_ = 0;
        good_windows_ = 0;
      }

      // ---- decide ----------------------------------------------------------------------
      const bool interval_elapsed = (now - last_change_) >= tuning_.min_change_interval;
      // Saturated: losing packets *and* queueing. Only then is goodput a capacity measurement.
      const bool saturated = lossy && elevated;
      const double goodput_ceiling = (have_goodput && saturated) ? goodput * 100.0 / (100.0 + tuning_.fec_percentage) * tuning_.goodput_headroom : 0.0;

      if (bad_windows_ >= tuning_.bad_windows_to_back_off && interval_elapsed) {
        back_off(tuning_.loss_back_off_factor, goodput_ceiling, reason_t::sustained_loss, now, decision);
        bad_windows_ = 0;
        return;
      }
      if (delay_windows_ >= tuning_.delay_windows_to_back_off && interval_elapsed) {
        back_off(tuning_.delay_back_off_factor, 0.0, reason_t::queueing_delay, now, decision);
        delay_windows_ = 0;
        return;
      }
      if (good_windows_ >= tuning_.good_windows_to_probe && interval_elapsed && current_kbps_ < bounds_.max_kbps) {
        const double factor = std::min(std::pow(tuning_.probe_factor, good_windows_), tuning_.max_probe_factor);
        int target = static_cast<int>(static_cast<double>(current_kbps_) * factor);
        // Never propose a step commit() will reject as too small, or a narrow range could go
        // down but never come back up.
        target = std::max(target, current_kbps_ + tuning_.min_step_kbps);
        if (now < cap_until_) {
          target = std::min(target, cap_kbps_);
        }
        target = std::min(target, bounds_.max_kbps);
        if (target > current_kbps_ && commit(target, now, decision, reason_t::link_clean)) {
          good_windows_ = 0;
        }
      }
    }

    /**
     * @brief Multiplicative decrease, capped at the goodput a saturated link delivered.
     * @param factor Multiplicative factor.
     * @param goodput_ceiling Goodput-derived ceiling in kbps; 0 when unknown or not saturated.
     * @param reason Why.
     * @param now Current time.
     * @param decision In/out: the decision being built.
     */
    void back_off(const double factor, const double goodput_ceiling, const reason_t reason, const std::chrono::steady_clock::time_point now, decision_t &decision) {
      const int from = current_kbps_;
      int target = static_cast<int>(static_cast<double>(from) * factor);
      if (goodput_ceiling > 0.0) {
        target = std::min(target, static_cast<int>(goodput_ceiling));
      }
      if (commit(target, now, decision, reason)) {
        // Probe back up to just below the rate that congested, and no further, for a while.
        cap_kbps_ = static_cast<int>(static_cast<double>(from) * tuning_.probe_cap_fraction);
        cap_until_ = now + tuning_.probe_cap_hold;
        last_back_off_ = reason;
        good_windows_ = 0;
      }
    }

    /**
     * @brief Apply a proposed bitrate if it is in range and worth the IDR it will cost.
     * @param target Proposed bitrate in kbps.
     * @param now Current time, recorded as the change time on success.
     * @param decision In/out: filled in when the rate moves.
     * @param reason Why.
     * @return True when the bitrate actually moved.
     */
    bool commit(const int target, const std::chrono::steady_clock::time_point now, decision_t &decision, const reason_t reason) {
      const int clamped = std::clamp(target, bounds_.min_kbps, bounds_.max_kbps);
      const int delta = clamped > current_kbps_ ? clamped - current_kbps_ : current_kbps_ - clamped;

      // A change that is not worth an IDR is not worth making. The exception is a move that
      // lands exactly on a bound, so the stream can actually reach its floor and ceiling.
      if (delta == 0) {
        return false;
      }
      if (delta < tuning_.min_step_kbps && clamped != bounds_.min_kbps && clamped != bounds_.max_kbps) {
        return false;
      }

      current_kbps_ = clamped;
      last_change_ = now;
      decision.changed = true;
      decision.kbps = clamped;
      decision.reason = reason;
      return true;
    }

    /**
     * @brief Start a fresh accumulation window at `now`.
     * @param now Current time.
     */
    void reset_window(const std::chrono::steady_clock::time_point now) {
      window_start_ = now;
      window_damaged_ = 0;
      window_unrecovered_ = 0;
      window_frames_ = 0;
      window_report_.reset();
      window_host_rtt_min_ = std::numeric_limits<std::uint32_t>::max();
      window_client_rtt_min_ = std::numeric_limits<std::uint32_t>::max();
    }

    bounds_t bounds_;  ///< Resolved bounds for this session.
    tuning_t tuning_;  ///< Thresholds and timings.
    int current_kbps_ = 0;  ///< Bitrate currently in force.

    std::chrono::steady_clock::time_point window_start_;  ///< Start of the accumulation window.
    std::chrono::steady_clock::time_point last_change_;  ///< When the bitrate last moved.
    std::chrono::steady_clock::time_point cap_until_;  ///< Probing is capped at `cap_kbps_` until then.
    int cap_kbps_ = 0;  ///< Just below the rate the link last congested at.
    reason_t last_back_off_ = reason_t::none;  ///< What caused the most recent back-off.

    std::uint64_t window_damaged_ = 0;  ///< FEC reports received, this window (the numerator).
    std::uint64_t window_unrecovered_ = 0;  ///< Frames FEC could not rebuild, this window.
    std::uint64_t window_frames_ = 0;  ///< Frames we encoded, this window (the denominator).
    std::optional<receiver_report_t> window_report_;  ///< Receiver report(s) of this window, merged.
    std::uint32_t window_host_rtt_min_ = std::numeric_limits<std::uint32_t>::max();  ///< Lowest host RTT this window.
    std::uint32_t window_client_rtt_min_ = std::numeric_limits<std::uint32_t>::max();  ///< Lowest client RTT this window.
    std::uint32_t window_rtt_var_ = 0;  ///< Latest host RTT variance.

    std::array<std::uint32_t, max_baseline_windows> baseline_ring_ {};  ///< Per-window RTT minima.
    int baseline_count_ = 0;  ///< Valid entries in `baseline_ring_`.
    int baseline_next_ = 0;  ///< Next slot to write in `baseline_ring_`.
    int previous_window_rtt_ = -1;  ///< RTT of the previous window with a sample.
    double goodput_avg_kbps_ = 0.0;  ///< Smoothed goodput over clean windows.

    int bad_windows_ = 0;  ///< Consecutive lossy windows.
    int good_windows_ = 0;  ///< Consecutive clean windows.
    int delay_windows_ = 0;  ///< Consecutive windows with a growing queue.
    int rebaseline_windows_ = 0;  ///< Consecutive windows of elevated, harmless RTT.
  };

}  // namespace meow::adaptive_bitrate
