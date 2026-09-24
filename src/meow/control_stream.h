/**
 * @file src/meow/control_stream.h
 * @brief Control-stream side of every meow extension: handler registration, per-session
 *        state, the per-iteration send work, and length validation of upstream payloads.
 *
 * `src/stream.cpp` keeps `control_server_t`, `session_t`, the framing and the encryption
 * private, so this header is templated on the server and session types and only ever touches
 * the members it names (`session->mail`, `session->meow`, `session->control.peer`). The
 * upstream file is left with one registration call, one per-session call, one timeout wrapper
 * and one `send` function that frames a payload - every decision is in here.
 *
 * ## Network
 *
 * Every message rides the existing encrypted ENet control stream (N1): no new port, socket or
 * listener, so nothing changes for firewalls, Tailscale ACLs or NAT. The largest payload is
 * 24 bytes (N2), far below the 1280-byte tailnet MTU. A stock client never sends `0x3004` or
 * `0x3005` and therefore never receives POSITION or APPLIED (N5).
 */
#pragma once

// standard includes
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

// local includes
#include "src/audio.h"
#include "src/config.h"
#include "src/logging.h"
#include "src/meow/adaptive_bitrate.h"
#include "src/meow/cursor.h"
#include "src/meow/cursor_runtime.h"
#include "src/meow/viewport_runtime.h"
#include "src/thread_safe.h"

namespace meow::control {

  using namespace std::literals;

  /**
   * @brief Largest payload any meow message carries (the receiver report).
   */
  inline constexpr std::size_t max_payload_length = 24;

  static_assert(viewport::echo_v2_payload_length <= max_payload_length);
  static_assert(cursor::position_length <= max_payload_length);
  static_assert(adaptive_bitrate::bitrate_applied_length <= max_payload_length);

  /**
   * @brief How often the host samples the control peer's round-trip time.
   */
  inline constexpr std::chrono::milliseconds rtt_sample_interval {500};

  /**
   * @brief Control-thread poll interval while a client follows the cursor or an echo is owed.
   *
   * The upstream loop blocks in `enet_host_service()` for up to 150 ms when the link is quiet,
   * which would cap cursor updates at ~7 Hz and delay a viewport echo by up to that long.
   * Polling at 8 ms keeps a 60 Hz position stream within one frame of the pointer and an echo
   * within one frame of the frame it names. Only while one of the two is needed.
   */
  inline constexpr std::chrono::milliseconds cursor_poll_interval {8};

  /**
   * @brief Minimum length of an `IDX_LOSS_STATS` payload: upstream reads `int32 stats[0..3]`.
   */
  inline constexpr std::size_t loss_stats_min_length = 4 * sizeof(std::int32_t);

  /**
   * @brief Minimum length of an `IDX_INVALIDATE_REF_FRAMES` payload: upstream reads `int64 frames[0..1]`.
   */
  inline constexpr std::size_t invalidate_ref_frames_min_length = 2 * sizeof(std::int64_t);

  /**
   * @brief Whether an `IDX_LOSS_STATS` payload is long enough for upstream's handler to read.
   *
   * Upstream casts the payload to `int32_t *` and reads four fields with no length check - a
   * heap over-read an authenticated client can trigger with a short message (spec F5). The
   * guard lives here so the upstream edit is one early return.
   *
   * @param payload Payload, excluding the header.
   * @return True when it is safe to read.
   */
  [[nodiscard]] inline constexpr bool loss_stats_payload_ok(const std::string_view payload) noexcept {
    return payload.size() >= loss_stats_min_length;
  }

  /**
   * @brief Whether an `IDX_INVALIDATE_REF_FRAMES` payload is long enough for upstream's handler.
   *
   * Upstream reads two `int64_t`s from it with no length check (spec F5).
   *
   * @param payload Payload, excluding the header.
   * @return True when it is safe to read.
   */
  [[nodiscard]] inline constexpr bool invalidate_ref_frames_payload_ok(const std::string_view payload) noexcept {
    return payload.size() >= invalidate_ref_frames_min_length;
  }

  /**
   * @brief Whether an `IDX_INPUT_DATA` payload holds the ciphertext its length prefix claims.
   *
   * Upstream reads a big-endian `int32` length from the first four bytes and then builds a view
   * of that many bytes after them with no bounds check, so a short payload or an inflated
   * length is decrypted straight off the end of the buffer - the same class of bug as F5, in
   * the handler next to them.
   *
   * @param payload Payload, excluding the header.
   * @return True when it is safe to read.
   */
  [[nodiscard]] inline constexpr bool input_data_payload_ok(const std::string_view payload) noexcept {
    if (payload.size() < sizeof(std::int32_t)) {
      return false;
    }
    const auto length = (static_cast<std::uint32_t>(static_cast<std::uint8_t>(payload[0])) << 24) | (static_cast<std::uint32_t>(static_cast<std::uint8_t>(payload[1])) << 16) | (static_cast<std::uint32_t>(static_cast<std::uint8_t>(payload[2])) << 8) | static_cast<std::uint32_t>(static_cast<std::uint8_t>(payload[3]));
    return length <= payload.size() - sizeof(std::int32_t);
  }

  /**
   * @brief Whether `type` already appears in the host's packet type table.
   *
   * @param type Packet type to check.
   * @param table The host's `packetTypes`.
   * @param count Entries in `table`.
   * @return True on a collision.
   */
  [[nodiscard]] inline constexpr bool collides(const std::uint16_t type, const short *const table, const std::size_t count) noexcept {
    for (std::size_t i = 0; i < count; ++i) {
      if (static_cast<std::uint16_t>(table[i]) == type) {
        return true;
      }
    }
    return false;
  }

  /**
   * @brief The FEC and audio budget `rtsp.cpp` deducted from this session's requested bitrate.
   *
   * @tparam Session `stream::session_t`.
   * @param session The session.
   * @return The budget, for converting between client and encoder bitrates.
   */
  template<class Session>
  [[nodiscard]] adaptive_bitrate::wire_budget_t wire_budget(const Session *session) {
    const auto &audio = session->config.audio;
    return {config::stream.fec_percentage, (audio.flags[audio::config_t::HIGH_QUALITY] ? 256 : 96) * audio.channels};
  }

  /**
   * @brief Whether adaptive bitrate is switched on in the host configuration.
   * @return `config::video.adaptive_bitrate`.
   */
  [[nodiscard]] inline bool adaptive_bitrate_enabled() {
    return config::video.adaptive_bitrate;
  }

  /**
   * @brief Per-session meow state, owned by `stream::session_t`.
   *
   * Touched only on the control thread, except for construction and destruction.
   */
  struct session_state_t {
    /**
     * @brief A new session: forget the previous client's viewport request.
     */
    session_state_t() {
      viewport::forget_request();
    }

    session_state_t(const session_state_t &) = delete;
    session_state_t &operator=(const session_state_t &) = delete;

    ~session_state_t() {
      unsubscribe();
    }

    /**
     * @brief Record a cursor subscription change, keeping the global subscriber count right.
     * @param subscribe The new state.
     */
    void set_subscribed(const bool subscribe) {
      if (subscribe && !cursor_subscribed) {
        cursor::detail::subscribers.fetch_add(1, std::memory_order_relaxed);
        cursor_subscribed = true;
        cursor_coalescer = {};
        cursor_coalescer.force();
      } else if (!subscribe) {
        unsubscribe();
      }
    }

    /**
     * @brief Drop a subscription, if any.
     */
    void unsubscribe() {
      if (cursor_subscribed) {
        cursor::detail::subscribers.fetch_sub(1, std::memory_order_relaxed);
        cursor_subscribed = false;
      }
    }

    std::uint32_t echo_seq = viewport::current_echo_seq();  ///< Last viewport echo sent to this session.
    bool cursor_subscribed = false;  ///< Whether POSITION messages are wanted.
    cursor::coalescer_t cursor_coalescer;  ///< When the next POSITION is due.
    std::uint16_t cursor_seq = 0;  ///< Sequence number of the next POSITION.
    safe::mail_raw_t::event_t<int> applied;  ///< Bitrate applied by the encoder; held once the client has reported.
    std::chrono::steady_clock::time_point next_rtt_sample {};  ///< When to sample ENet RTT next.
  };

  /**
   * @brief Handle a `0x3004` SUBSCRIBE.
   * @tparam Session `stream::session_t`.
   * @param session The session.
   * @param payload Payload, excluding the header.
   */
  template<class Session>
  void on_cursor_subscribe(Session *session, const std::string_view payload) {
    const auto subscribe = cursor::parse_subscribe(payload);
    if (!subscribe) {
      return;
    }
    session->meow.set_subscribed(*subscribe && cursor::reporting_enabled());
  }

  /**
   * @brief Handle a `0x3005` receiver report: forward it to the encoder thread.
   * @tparam Session `stream::session_t`.
   * @param session The session.
   * @param payload Payload, excluding the header.
   */
  template<class Session>
  void on_receiver_report(Session *session, const std::string_view payload) {
    if (!adaptive_bitrate_enabled()) {
      return;  // The client stops reporting after a few unanswered reports.
    }
    const auto report = adaptive_bitrate::parse_receiver_report(payload);
    if (!report) {
      return;
    }
    if (!session->meow.applied) {
      // Held from the first report on, so APPLIED reaches only clients that report.
      session->meow.applied = session->mail->template event<int>(adaptive_bitrate::applied_mail_id);
    }
    // The client's ceiling is its *total* bitrate, like the one it negotiated over RTSP; the
    // controller works in encoder bitrate, which rtsp.cpp derived by deducting FEC, audio and
    // overhead. Convert the same way, or the stream could overshoot the user's cap by ~25%.
    auto converted = *report;
    converted.max_kbps = static_cast<std::uint32_t>(adaptive_bitrate::client_to_encoder_kbps(static_cast<int>(std::min<std::uint32_t>(report->max_kbps, adaptive_bitrate::max_configurable_kbps)), wire_budget(session)));
    // queue() returns null while an expired entry lingers; the client controls the timing.
    if (auto reports = session->mail->template queue<adaptive_bitrate::receiver_report_t>(adaptive_bitrate::report_mail_id)) {
      reports->raise(converted);
    }
  }

  /**
   * @brief Handle a `0x5502` FEC status report: forward a validated sample to the encoder.
   * @tparam Session `stream::session_t`.
   * @param session The session.
   * @param payload Payload, excluding the header.
   */
  template<class Session>
  void on_frame_fec_status(Session *session, const std::string_view payload) {
    if (!adaptive_bitrate_enabled()) {
      return;  // Feature off: do no per-packet work at all.
    }
    if (const auto sample = adaptive_bitrate::parse_frame_fec_status(payload)) {
      if (auto samples = session->mail->template queue<adaptive_bitrate::loss_sample_t>(adaptive_bitrate::mail_id)) {
        samples->raise(*sample);
      }
    }
  }

  /**
   * @brief Register every meow control-stream handler, refusing any that would collide.
   *
   * @tparam Server `stream::control_server_t`.
   * @param server Control server.
   * @param table The host's `packetTypes`.
   * @param count Entries in `table`.
   */
  template<class Server>
  void register_handlers(Server &server, const short *const table, const std::size_t count) {
    auto viewport_registration = viewport::map_request_handler(server, table, count, viewport::following_enabled());
    BOOST_LOG(info) << viewport_registration.note;
    if (!viewport_registration.warning.empty()) {
      BOOST_LOG(error) << viewport_registration.warning;
    }

    if (collides(cursor::control_packet_type, table, count)) {
      BOOST_LOG(error) << "meow cursor: control packet type 0x3004 is used by an upstream message; cursor reporting is disabled for this build"sv;
    } else {
      server.map(cursor::control_packet_type, [](auto *session, const std::string_view &payload) {
        on_cursor_subscribe(session, payload);
      });
    }

    if (collides(adaptive_bitrate::receiver_report_packet_type, table, count)) {
      BOOST_LOG(error) << "meow adaptive bitrate: control packet type 0x3005 is used by an upstream message; receiver reports are ignored for this build"sv;
    } else {
      server.map(adaptive_bitrate::receiver_report_packet_type, [](auto *session, const std::string_view &payload) {
        on_receiver_report(session, payload);
      });
    }

    // 0x5502 is the Sunshine extension SS_FRAME_FEC_STATUS; inbound it is not in `packetTypes`
    // (the table lists the outbound "Set RGB LED" meaning of the same number).
    server.map(adaptive_bitrate::frame_fec_status_packet_type, [](auto *session, const std::string_view &payload) {
      on_frame_fec_status(session, payload);
    });

    BOOST_LOG(info) << "meow adaptive bitrate: "sv << (adaptive_bitrate_enabled() ? "enabled"sv : "disabled (meow_adaptive_bitrate = disabled)"sv)
                    << "; meow cursor reporting: "sv << (cursor::reporting_enabled() ? "enabled"sv : "disabled (meow_cursor_reporting = disabled)"sv);
  }

  /**
   * @brief Per-iteration work for one connected session on the control thread.
   *
   * Sends any viewport echo the encode path published, any bitrate the encoder applied, and a
   * cursor POSITION when one is due; samples the ENet round-trip time for the controller.
   *
   * @tparam Session `stream::session_t`.
   * @tparam Send Callable `int(Session *, std::uint16_t type, const std::uint8_t *payload, std::size_t length)`.
   * @param session The session; must have a connected control peer.
   * @param send Frames, encrypts and queues one control message.
   * @param now Current time.
   */
  template<class Session, class Send>
  void on_session_tick(Session *session, Send &&send, const std::chrono::steady_clock::time_point now) {
    auto &state = session->meow;
    std::array<std::uint8_t, max_payload_length> buffer {};

    if (const auto echo = viewport::take_echo(state.echo_seq)) {
      viewport::write_echo_v2_payload(echo->applied, echo->capture_width, echo->capture_height, echo->frame_index, buffer.data());
      if (send(session, viewport::control_packet_type, buffer.data(), viewport::echo_v2_payload_length) == 0) {
        BOOST_LOG(debug) << "meow viewport: echoed "sv << echo->applied.width << 'x' << echo->applied.height << '+' << echo->applied.x << '+' << echo->applied.y
                         << " (stream coordinates) from frame "sv << echo->frame_index;
      }
    }

    if (state.applied) {
      if (const auto kbps = state.applied->try_pop(); kbps && *kbps > 0) {
        // Reported in the client's units - the total bitrate it would negotiate to get this
        // encoder rate - so a client that remembers it and asks for it next session gets the
        // same stream back rather than one deducted twice.
        adaptive_bitrate::write_bitrate_applied(static_cast<std::uint32_t>(adaptive_bitrate::encoder_to_client_kbps(*kbps, wire_budget(session))), buffer.data());
        send(session, adaptive_bitrate::receiver_report_packet_type, buffer.data(), adaptive_bitrate::bitrate_applied_length);
      }
    }

    if (state.cursor_subscribed) {
      const auto cursor_state = cursor::current();
      const auto geometry = viewport::current_geometry();
      if (cursor_state && geometry && state.cursor_coalescer.due(*cursor_state, now)) {
        if (const auto point = cursor::to_reference(cursor_state->x, cursor_state->y, geometry->capture_width, geometry->capture_height, geometry->surface_width, geometry->surface_height)) {
          cursor::write_position(static_cast<std::uint16_t>(point->first), static_cast<std::uint16_t>(point->second), cursor_state->visible, state.cursor_seq, buffer.data());
          if (send(session, cursor::control_packet_type, buffer.data(), cursor::position_length) == 0) {
            ++state.cursor_seq;
            state.cursor_coalescer.sent(*cursor_state, now);
          }
        }
      }
    }

    if (adaptive_bitrate_enabled() && session->control.peer && now >= state.next_rtt_sample) {
      state.next_rtt_sample = now + rtt_sample_interval;
      if (auto rtts = session->mail->template queue<adaptive_bitrate::rtt_sample_t>(adaptive_bitrate::rtt_mail_id)) {
        rtts->raise(adaptive_bitrate::rtt_sample_t {static_cast<std::uint32_t>(session->control.peer->roundTripTime), static_cast<std::uint32_t>(session->control.peer->roundTripTimeVariance)});
      }
    }
  }

  /**
   * @brief The control loop's `enet_host_service()` timeout.
   *
   * @param upstream The timeout upstream uses.
   * @return `upstream`, or `cursor_poll_interval` while a client follows the cursor.
   */
  [[nodiscard]] inline std::chrono::milliseconds iterate_timeout(const std::chrono::milliseconds upstream) noexcept {
    const bool fast = cursor::polling_wanted() || viewport::echo_owed(std::chrono::steady_clock::now());
    return fast ? std::min(upstream, cursor_poll_interval) : upstream;
  }

}  // namespace meow::control
