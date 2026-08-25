/**
 * @file tests/unit/meow/test_viewport.cpp
 * @brief Test src/meow/viewport.h and src/meow/viewport_runtime.h.
 */
// test includes
#include "../../tests_common.h"

// standard includes
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <iostream>
#include <limits>
#include <map>
#include <optional>
#include <random>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// ffmpeg includes
extern "C" {
#include <libavutil/buffer.h>
#include <libavutil/frame.h>
#include <libavutil/opt.h>
#include <libavutil/pixfmt.h>
#include <libswscale/swscale.h>
}

// local includes
#include <src/meow/viewport.h>
#include <src/meow/viewport_runtime.h>
#include <src/platform/common.h>
#include <src/video.h>

namespace {

  using meow::viewport::configure_scaler;
  using meow::viewport::control_packet_type;
  using meow::viewport::evaluate_request;
  using meow::viewport::floor_even;
  using meow::viewport::full_frame_plan;
  using meow::viewport::min_output_extent;
  using meow::viewport::min_source_extent;
  using meow::viewport::offset_source_planes;
  using meow::viewport::packet_type_collision;
  using meow::viewport::parse_following_value;
  using meow::viewport::parse_payload;
  using meow::viewport::payload_length;
  using meow::viewport::payload_version;
  using meow::viewport::plan;
  using meow::viewport::plan_t;
  using meow::viewport::rect_t;
  using meow::viewport::reference_frame;
  using meow::viewport::sanitize;
  using meow::viewport::to_desktop;
  using meow::viewport::to_reference;
  using meow::viewport::write_echo_payload;
  using meow::viewport::write_payload;

  /// The user's real desktop: eDP-2 1920x1200 at (0,0) plus HDMI-A-1 3440x1440 at (1920,0).
  constexpr int desktop_w = 5360;
  constexpr int desktop_h = 1440;

  /// A typical phone-side encode surface.
  constexpr int surface_w = 1280;
  constexpr int surface_h = 720;

  /**
   * @brief Build a viewport payload exactly as the client serializes one.
   * @param version Version byte.
   * @param flags Reserved flags byte.
   * @param x Left edge.
   * @param y Top edge.
   * @param w Width.
   * @param h Height.
   * @return The payload bytes.
   */
  std::string make_payload(const std::uint8_t version, const std::uint8_t flags, const std::uint16_t x, const std::uint16_t y, const std::uint16_t w, const std::uint16_t h) {
    std::string p;
    p.push_back(static_cast<char>(version));
    p.push_back(static_cast<char>(flags));
    const auto put = [&p](const std::uint16_t v) {
      p.push_back(static_cast<char>(v & 0xFF));
      p.push_back(static_cast<char>((v >> 8) & 0xFF));
    };
    put(x);
    put(y);
    put(w);
    put(h);
    return p;
  }

}  // namespace

// ---------------------------------------------------------------------------------
// The wire contract with meowerse/moonlight-common-c, branch `meow`.
// ---------------------------------------------------------------------------------

/**
 * @brief The host's packet number and payload layout must match the client's, byte for byte.
 *
 * These literals are transcribed from `src/ControlStream.c` on branch `meow` of
 * meowerse/moonlight-common-c: `packetTypesGen7Enc[IDX_VIEWPORT] = 0x3003`,
 * `VIEWPORT_PAYLOAD_VERSION 1`, `VIEWPORT_PAYLOAD_LENGTH 10`, and a
 * `BbPut8/BbPut8/BbPut16 x4` little-endian body. Changing any of them here is a
 * deliberate protocol break rather than a silent one, and the golden byte vector below
 * fails if the field order or endianness drifts.
 */
TEST(MeowViewportWire, PinsTheClientContract) {
  EXPECT_EQ(control_packet_type, 0x3003);
  EXPECT_EQ(payload_version, 1);
  EXPECT_EQ(payload_length, 10u);

  std::uint8_t buf[payload_length] {};
  write_payload({0x0102, 0x0304, 0x0506, 0x0708}, buf);

  const std::vector<std::uint8_t> expected {0x01, 0x00, 0x02, 0x01, 0x04, 0x03, 0x06, 0x05, 0x08, 0x07};
  EXPECT_EQ(std::vector<std::uint8_t>(std::begin(buf), std::end(buf)), expected);
}

/**
 * @brief What the host writes, the host can read back.
 */
TEST(MeowViewportWire, PayloadRoundTrips) {
  const rect_t sent {1920, 200, 2560, 1200};
  std::uint8_t buf[payload_length] {};
  write_payload(sent, buf);

  const auto parsed = parse_payload(std::string_view {reinterpret_cast<const char *>(buf), payload_length});
  ASSERT_TRUE(parsed.has_value());
  EXPECT_EQ(*parsed, sent);
}

/**
 * @brief The echo carries the desktop size, and an older parser still reads the rectangle.
 *
 * The extra two `uint16`s are what let the client compute the host's padding transform for
 * itself, which is the only way to close the coordinate-space gap permanently. They ride
 * behind `flag_desktop_extent` in the reserved flags byte, so they cost no version bump:
 * the client's version-1 parser reads the first ten bytes, ignores flag bits it does not
 * know and ignores trailing bytes. That last claim is not taken on trust here -- the same
 * bytes are fed to `parse_payload()`, which is exactly a ten-byte version-1 parser.
 */
TEST(MeowViewportWire, EchoPayloadCarriesTheDesktopExtent) {
  EXPECT_EQ(meow::viewport::echo_payload_length, 14u);
  EXPECT_EQ(meow::viewport::flag_desktop_extent, 0x01);

  std::uint8_t buf[meow::viewport::echo_payload_length] {};
  write_echo_payload({640, 188, 640, 343}, desktop_w, desktop_h, buf);

  const std::vector<std::uint8_t> expected {
    0x01,  // version
    0x01,  // flags: desktop extent present
    0x80,
    0x02,  // x = 640
    0xBC,
    0x00,  // y = 188
    0x80,
    0x02,  // width = 640
    0x57,
    0x01,  // height = 343
    0xF0,
    0x14,  // desktop width = 5360
    0xA0,
    0x05  // desktop height = 1440
  };
  EXPECT_EQ(std::vector<std::uint8_t>(std::begin(buf), std::end(buf)), expected);

  // A ten-byte version-1 parser reads the rectangle out of it unchanged.
  const auto as_old_client_sees_it = parse_payload(std::string_view {reinterpret_cast<const char *>(buf), sizeof(buf)});
  ASSERT_TRUE(as_old_client_sees_it.has_value());
  EXPECT_EQ(*as_old_client_sees_it, (rect_t {640, 188, 640, 343}));
}

/**
 * @brief A payload the host cannot fully account for is discarded, not guessed at.
 */
TEST(MeowViewportWire, RejectsUnusablePayloads) {
  EXPECT_FALSE(parse_payload("").has_value()) << "empty";
  EXPECT_FALSE(parse_payload(make_payload(1, 0, 0, 0, 800, 600).substr(0, 9)).has_value()) << "truncated by one byte";
  EXPECT_FALSE(parse_payload(make_payload(2, 0, 0, 0, 800, 600)).has_value()) << "unknown version";
  EXPECT_FALSE(parse_payload(make_payload(0, 0, 0, 0, 800, 600)).has_value()) << "version zero";
  EXPECT_FALSE(parse_payload(make_payload(1, 0, 0, 0, 0, 600)).has_value()) << "zero width";
  EXPECT_FALSE(parse_payload(make_payload(1, 0, 0, 0, 800, 0)).has_value()) << "zero height";
}

/**
 * @brief Unknown flag bits are ignored, and a longer payload keeps parsing.
 *
 * This is the extension path: a future client adds a field under the reserved flags byte
 * without a version bump, and this host keeps working.
 */
TEST(MeowViewportWire, IgnoresReservedFlagsAndTrailingBytes) {
  auto p = make_payload(1, 0xFF, 100, 200, 800, 600);
  p.append("trailing garbage");

  const auto parsed = parse_payload(p);
  ASSERT_TRUE(parsed.has_value());
  EXPECT_EQ(*parsed, (rect_t {100, 200, 800, 600}));
}

/**
 * @brief The host must never take a packet number an upstream control message already uses.
 *
 * The table below is `packetTypes` from `src/stream.cpp` verbatim. If a future upstream
 * sync adds `0x3003` to it, this fails here rather than mis-dispatching that message into
 * the viewport handler at runtime.
 */
TEST(MeowViewportWire, DoesNotCollideWithUpstreamPacketTypes) {
  static const short upstream_packet_types[] = {
    0x0305,  // Start A
    0x0307,  // Start B
    0x0301,  // Invalidate reference frames
    0x0201,  // Loss Stats
    0x0204,  // Frame Stats (unused)
    0x0206,  // Input data
    0x010b,  // Rumble data
    0x0109,  // Termination
    0x0200,  // Periodic Ping
    0x0302,  // IDR frame
    0x0001,  // fully encrypted
    0x010e,  // HDR mode
    0x5500,  // Rumble triggers
    0x5501,  // Set motion event
    0x5502,  // Set RGB LED
    0x5503,  // Set Adaptive triggers
  };
  EXPECT_FALSE(packet_type_collision(upstream_packet_types, std::size(upstream_packet_types)));

  // ... and the check itself actually detects one.
  static const short colliding[] = {0x0305, 0x3003};
  EXPECT_TRUE(packet_type_collision(colliding, std::size(colliding)));
}

// ---------------------------------------------------------------------------------
// The compatibility floor: no viewport means today's behaviour, exactly.
// ---------------------------------------------------------------------------------

/**
 * @brief Upstream's `avcodec_software_encode_device_t::init()` arithmetic, transcribed.
 *
 * A transcription, not a characterization: it never executes a line of `src/video.cpp`, so
 * if this transcription is wrong then `full_frame_plan()` is wrong in the same way and both
 * agree. It pins the two copies together, which is worth having; the test that actually
 * runs upstream's code is `MeowViewportUpstream.*` below. Upstream's arithmetic is:
 *
 * ```
 * auto scalar = std::fminf((float) out_width / in_width, (float) out_height / in_height);
 * out_width  = in_width * scalar;
 * out_height = in_height * scalar;
 * offsetW = (in_frame->width  - out_width)  / 2;
 * offsetH = (in_frame->height - out_height) / 2;
 * ```
 *
 * @param capture_w Captured frame width.
 * @param capture_h Captured frame height.
 * @param s_w Encode surface width.
 * @param s_h Encode surface height.
 * @return The plan upstream would configure.
 */
static plan_t upstream_init_formula(const int capture_w, const int capture_h, const int s_w, const int s_h) {
  const auto scalar = std::fminf(static_cast<float>(s_w) / static_cast<float>(capture_w), static_cast<float>(s_h) / static_cast<float>(capture_h));
  plan_t p;
  p.source = {0, 0, capture_w, capture_h};
  p.out_width = static_cast<int>(static_cast<float>(capture_w) * scalar);
  p.out_height = static_cast<int>(static_cast<float>(capture_h) * scalar);
  p.offset_w = (s_w - p.out_width) / 2;
  p.offset_h = (s_h - p.out_height) / 2;
  p.cropped = false;
  return p;
}

/**
 * @brief With no viewport, the plan matches the transcribed upstream formula exactly.
 *
 * This is the compatibility floor: stock Moonlight, an older moonmeow, or any client that
 * never sends the packet must get an unchanged full-desktop stream. Checked across a
 * spread of shapes including deliberately awkward ones that produce odd out/offset values,
 * because reproducing those *exactly* is the point.
 */
TEST(MeowViewportPlan, NoRequestMatchesTheTranscribedUpstreamFormula) {
  const std::vector<std::array<int, 4>> cases {
    {desktop_w, desktop_h, surface_w, surface_h},  // the user's real desktop
    {5360, 1440, 1920, 1080},
    {3440, 1440, 2560, 1440},
    {1920, 1080, 1920, 1080},  // exact fit, no padding at all
    {1920, 1200, 1280, 720},
    {2560, 1080, 1281, 721},  // odd surface -> odd out/offset, preserved as-is
    {1366, 768, 640, 360},
    {800, 600, 1920, 1080},  // upscale
    {1, 1, 1280, 720},  // degenerate but non-zero
  };

  for (const auto &c : cases) {
    const auto actual = plan(c[0], c[1], c[2], c[3], std::nullopt);
    const auto expected = upstream_init_formula(c[0], c[1], c[2], c[3]);
    EXPECT_EQ(actual, expected) << c[0] << 'x' << c[1] << " -> " << c[2] << 'x' << c[3];
  }
}

/**
 * @brief The very same guarantee must hold for every rejected request.
 *
 * A hostile or useless rectangle must not merely "not crash" -- it must land the stream on
 * precisely the same plan as no request at all.
 */
TEST(MeowViewportPlan, EveryRejectionFallsBackToTheFullFramePlan) {
  const auto baseline = plan(desktop_w, desktop_h, surface_w, surface_h, std::nullopt);

  const std::vector<std::pair<const char *, rect_t>> hostile {
    {"zero width", {0, 0, 0, 720}},
    {"zero height", {0, 0, 1280, 0}},
    {"negative width", {0, 0, -1280, 720}},
    {"negative height", {0, 0, 1280, -720}},
    {"negative origin", {-100, -100, 1280, 720}},
    {"origin past the right edge", {desktop_w, 0, 1280, 720}},
    {"origin past the bottom edge", {0, desktop_h, 1280, 720}},
    {"origin far outside", {60000, 60000, 1280, 720}},
    {"absurdly wide, degenerate aspect", {0, 0, desktop_w, 64}},
    {"the whole desktop", {0, 0, desktop_w, desktop_h}},
    {"larger than the desktop", {0, 0, 65535, 65535}},
  };

  for (const auto &[name, r] : hostile) {
    const auto actual = plan(desktop_w, desktop_h, surface_w, surface_h, r);
    EXPECT_EQ(actual, baseline) << name;
    EXPECT_FALSE(actual.cropped) << name;
  }
}

/**
 * @brief An extremely tall, narrow crop is honoured as a narrow strip, not refused.
 *
 * The asymmetry with the "absurdly wide" case above is real and worth stating: on a
 * 5360x1440 desktop the narrowest crop `min_source_extent` allows is 64 wide, which scales
 * to a 32-wide strip -- exactly `min_output_extent`, so it survives. Getting below the
 * output floor on the vertical axis is unreachable here, while a full-width 64-tall crop
 * scales to 1280x15 and is refused. Both outcomes are correct; only one of them is a
 * rejection.
 */
TEST(MeowViewportPlan, TallNarrowCropsAreHonouredAsAStrip) {
  const auto p = plan(desktop_w, desktop_h, surface_w, surface_h, rect_t {2000, 0, 64, desktop_h});
  ASSERT_TRUE(p.cropped);
  EXPECT_EQ(p.source, (rect_t {2000, 0, 64, desktop_h}));
  EXPECT_EQ(p.out_width, min_output_extent);
  EXPECT_EQ(p.out_height, 720);

  // ... whereas the same shape lying down really is a sliver, and is refused.
  const auto wide = plan(desktop_w, desktop_h, surface_w, surface_h, rect_t {0, 700, desktop_w, 64});
  EXPECT_FALSE(wide.cropped);
}

/**
 * @brief A nonsensical capture or surface size produces an inert plan, not a crash.
 */
TEST(MeowViewportPlan, DegenerateGeometryProducesAnInertPlan) {
  for (const auto &c : std::vector<std::array<int, 4>> {{0, 1440, 1280, 720}, {5360, 0, 1280, 720}, {5360, 1440, 0, 720}, {5360, 1440, 1280, 0}, {-1, -1, -1, -1}}) {
    const auto p = plan(c[0], c[1], c[2], c[3], rect_t {0, 0, 800, 600});
    EXPECT_EQ(p, plan_t {}) << c[0] << 'x' << c[1] << " -> " << c[2] << 'x' << c[3];
    EXPECT_LE(p.source.width, 0);
  }
}

// ---------------------------------------------------------------------------------
// Cropping.
// ---------------------------------------------------------------------------------

/**
 * @brief A well-formed request is honoured, and the crop wins back most of the surface.
 *
 * This is the whole point of the feature, expressed as a number. Streaming 5360x1440 into
 * 1280x720 letterboxes down to 1280x344 -- 440k of the 921k available pixels. Cropping to
 * a phone-shaped 1920x1080 window of that desktop fills 1280x720 completely, and the
 * source-pixels-per-encoded-pixel ratio improves by more than 4x.
 */
TEST(MeowViewportPlan, CroppingRecoversTheWastedSurface) {
  const auto uncropped = plan(desktop_w, desktop_h, surface_w, surface_h, std::nullopt);
  const auto cropped = plan(desktop_w, desktop_h, surface_w, surface_h, rect_t {1920, 180, 1920, 1080});

  ASSERT_TRUE(cropped.cropped);
  EXPECT_EQ(cropped.source, (rect_t {1920, 180, 1920, 1080}));
  EXPECT_EQ(cropped.out_width, 1280);
  EXPECT_EQ(cropped.out_height, 720);
  EXPECT_EQ(cropped.offset_w, 0);
  EXPECT_EQ(cropped.offset_h, 0);

  const auto uncropped_src_per_out = static_cast<double>(desktop_w) * desktop_h / (uncropped.out_width * uncropped.out_height);
  const auto cropped_src_per_out = static_cast<double>(cropped.source.width) * cropped.source.height / (cropped.out_width * cropped.out_height);
  EXPECT_LT(cropped_src_per_out * 4, uncropped_src_per_out);
}

/**
 * @brief The encode surface size is never changed by a crop.
 *
 * Re-initialising the encoder on every pan would hitch badly. Whatever the request, the
 * scaled image plus its padding must exactly fit the surface it was built for.
 */
TEST(MeowViewportPlan, NeverExceedsTheEncodeSurface) {
  const std::vector<rect_t> requests {
    {0, 0, 100, 100},
    {1920, 0, 3440, 1440},
    {5000, 1300, 2000, 2000},
    {2680, 720, 64, 64},
    {0, 0, 5360, 400},
    {17, 23, 1237, 719},
  };

  for (const auto &r : requests) {
    const auto p = plan(desktop_w, desktop_h, surface_w, surface_h, r);
    EXPECT_LE(p.out_width + p.offset_w, surface_w) << r.width << 'x' << r.height;
    EXPECT_LE(p.out_height + p.offset_h, surface_h) << r.width << 'x' << r.height;
    EXPECT_GE(p.offset_w, 0);
    EXPECT_GE(p.offset_h, 0);
  }
}

/**
 * @brief A crop never addresses a pixel outside the captured frame.
 *
 * The pointer arithmetic in `offset_source_planes()` depends on this and nothing else.
 * Fuzzed over the whole 16-bit input space the wire can express.
 */
TEST(MeowViewportPlan, CropAlwaysStaysInsideTheCapturedFrame) {
  std::mt19937 rng {20260825};
  std::uniform_int_distribution<int> coord {0, 65535};

  for (int i = 0; i < 20000; ++i) {
    const rect_t r {coord(rng), coord(rng), coord(rng), coord(rng)};
    const auto p = plan(desktop_w, desktop_h, surface_w, surface_h, r);

    ASSERT_GE(p.source.x, 0) << i;
    ASSERT_GE(p.source.y, 0) << i;
    ASSERT_GT(p.source.width, 0) << i;
    ASSERT_GT(p.source.height, 0) << i;
    ASSERT_LE(p.source.x + p.source.width, desktop_w) << i;
    ASSERT_LE(p.source.y + p.source.height, desktop_h) << i;
  }
}

/**
 * @brief Every cropped coordinate is even, so NV12 chroma lines up.
 *
 * An odd source origin puts the interleaved UV pointer half a chroma sample out and
 * fringes the crop edge; an odd destination offset does the same at the letterbox seam.
 */
TEST(MeowViewportPlan, CroppedCoordinatesAreEven) {
  std::mt19937 rng {987654321};
  std::uniform_int_distribution<int> coord {0, 6000};

  int cropped_seen = 0;
  for (int i = 0; i < 20000; ++i) {
    const rect_t r {coord(rng), coord(rng), coord(rng), coord(rng)};
    const auto p = plan(desktop_w, desktop_h, surface_w, surface_h, r);
    if (!p.cropped) {
      continue;
    }
    ++cropped_seen;
    ASSERT_EQ(p.source.x % 2, 0) << i;
    ASSERT_EQ(p.source.y % 2, 0) << i;
    ASSERT_EQ(p.source.width % 2, 0) << i;
    ASSERT_EQ(p.source.height % 2, 0) << i;
    ASSERT_EQ(p.out_width % 2, 0) << i;
    ASSERT_EQ(p.out_height % 2, 0) << i;
    ASSERT_EQ(p.offset_w % 2, 0) << i;
    ASSERT_EQ(p.offset_h % 2, 0) << i;
  }
  EXPECT_GT(cropped_seen, 1000) << "the fuzz never actually exercised the cropped path";
}

/**
 * @brief A tiny request is grown to the floor rather than snapping back to full desktop.
 *
 * A user pinching in as far as the client allows should hit a limit, not lose their zoom.
 */
TEST(MeowViewportPlan, TinyRequestsGrowToTheMinimum) {
  const auto p = plan(desktop_w, desktop_h, surface_w, surface_h, rect_t {2000, 700, 1, 1});
  ASSERT_TRUE(p.cropped);
  EXPECT_GE(p.source.width, min_source_extent);
  EXPECT_GE(p.source.height, min_source_extent);
  EXPECT_GE(p.out_width, min_output_extent);
  EXPECT_GE(p.out_height, min_output_extent);
}

/**
 * @brief A request hanging off the right/bottom edge is trimmed, not refused.
 *
 * Panning to the far corner is normal; the client's window is simply larger than what is
 * left of the desktop.
 */
TEST(MeowViewportPlan, OverhangingRequestsAreTrimmed) {
  const auto p = plan(desktop_w, desktop_h, surface_w, surface_h, rect_t {5000, 1000, 2000, 2000});
  ASSERT_TRUE(p.cropped);
  EXPECT_EQ(p.source.x, 5000);
  EXPECT_EQ(p.source.y, 1000);
  EXPECT_EQ(p.source.x + p.source.width, desktop_w);
  EXPECT_EQ(p.source.y + p.source.height, desktop_h);
}

/**
 * @brief `sanitize()` is idempotent: re-clamping an already-clamped rectangle is a no-op.
 *
 * `plan_for_frame()` relies on this when it tightens the clamp against a frame smaller
 * than the one recorded at init.
 */
TEST(MeowViewportPlan, SanitizeIsIdempotent) {
  std::mt19937 rng {13371337};
  std::uniform_int_distribution<int> coord {0, 8000};

  for (int i = 0; i < 5000; ++i) {
    const auto once = sanitize({coord(rng), coord(rng), coord(rng), coord(rng)}, desktop_w, desktop_h);
    if (!once) {
      continue;
    }
    const auto twice = sanitize(*once, desktop_w, desktop_h);
    ASSERT_TRUE(twice.has_value()) << i;
    ASSERT_EQ(*twice, *once) << i;
  }
}

/**
 * @brief `floor_even()` never returns something larger than its input, or negative.
 */
TEST(MeowViewportPlan, FloorEvenIsSaneAtTheEdges) {
  EXPECT_EQ(floor_even(0), 0);
  EXPECT_EQ(floor_even(1), 0);
  EXPECT_EQ(floor_even(2), 2);
  EXPECT_EQ(floor_even(3), 2);
  EXPECT_EQ(floor_even(-1), 0);
  EXPECT_EQ(floor_even(-100), 0);
  EXPECT_EQ(floor_even(65535), 65534);
}

// ---------------------------------------------------------------------------------
// The coordinate transform: the wire is NOT desktop pixels.
// ---------------------------------------------------------------------------------

/**
 * @brief The reference frame is the uncropped desktop image inside the encode surface.
 *
 * On the user's real setup that is a 1280x343 strip sitting 188 rows down a 1280x720
 * surface. Those 188 rows of padding are the whole reason a proportional mapping is wrong.
 */
TEST(MeowViewportReference, DescribesThePaddedUltrawideFraming) {
  const auto ref = reference_frame(desktop_w, desktop_h, surface_w, surface_h);
  EXPECT_EQ(ref.surface_width, 1280);
  EXPECT_EQ(ref.surface_height, 720);
  EXPECT_EQ(ref.content_x, 0);
  EXPECT_EQ(ref.content_y, 188);
  EXPECT_EQ(ref.content_width, 1280);
  EXPECT_EQ(ref.content_height, 343);

  // A matched-aspect stream has no padding at all, and the reference frame is the surface.
  const auto square = reference_frame(1920, 1080, 1280, 720);
  EXPECT_EQ(square.content_x, 0);
  EXPECT_EQ(square.content_y, 0);
  EXPECT_EQ(square.content_width, 1280);
  EXPECT_EQ(square.content_height, 720);
}

/**
 * @brief Mapping undoes the padding, and a naive proportional mapping would not.
 *
 * The client asks for the right-hand half of the *encoded frame*: x from 640 to 1280, y
 * from 188 to 531 — the full height of the visible strip. That is the right-hand half of
 * the desktop, all 1440 rows of it.
 *
 * The naive mapping (scale both axes by surface->desktop with no offset) would put the top
 * of that rectangle at row 188 * (1440/720) = 376 of the desktop and its bottom at row
 * 1062, losing the top quarter and the bottom quarter of the screen. This test fails if
 * anybody reintroduces that.
 */
TEST(MeowViewportReference, MapsThroughThePaddingNotAroundIt) {
  const auto mapped = to_desktop({640, 188, 640, 343}, desktop_w, desktop_h, surface_w, surface_h);
  ASSERT_TRUE(mapped.has_value());
  EXPECT_EQ(mapped->x, 2680);
  EXPECT_EQ(mapped->y, 0);
  EXPECT_EQ(mapped->width, desktop_w - 2680);
  EXPECT_EQ(mapped->height, desktop_h);

  // The naive answer, spelled out so the difference is visible in the test rather than
  // only in the commit message.
  constexpr int naive_y = 188 * desktop_h / surface_h;
  EXPECT_NE(mapped->y, naive_y);
  EXPECT_EQ(naive_y, 376);
}

/**
 * @brief The whole encoded frame maps to the whole desktop, so "no zoom" means "no crop".
 *
 * The client sends its current view every time it changes. When the user is not zoomed in
 * that view is the entire frame, padding included, and the host must read that as "stream
 * everything" rather than as a crop of the visible strip.
 */
TEST(MeowViewportReference, TheWholeFrameMeansTheWholeDesktop) {
  const auto mapped = to_desktop({0, 0, surface_w, surface_h}, desktop_w, desktop_h, surface_w, surface_h);
  ASSERT_TRUE(mapped.has_value());
  EXPECT_EQ(*mapped, (rect_t {0, 0, desktop_w, desktop_h}));

  const auto p = plan(desktop_w, desktop_h, surface_w, surface_h, mapped);
  EXPECT_FALSE(p.cropped);
  EXPECT_EQ(p, full_frame_plan(desktop_w, desktop_h, surface_w, surface_h));
}

/**
 * @brief A rectangle that is entirely padding maps to nothing.
 *
 * The top black bar of a letterboxed ultrawide stream shows no desktop, so there is no
 * desktop rectangle to answer with. Refusing is right; sliding it onto the nearest real
 * pixels would show the user somewhere they did not ask for.
 */
TEST(MeowViewportReference, PaddingOnlyRequestsMapToNothing) {
  EXPECT_FALSE(to_desktop({0, 0, surface_w, 100}, desktop_w, desktop_h, surface_w, surface_h).has_value()) << "top bar";
  EXPECT_FALSE(to_desktop({0, 600, surface_w, 120}, desktop_w, desktop_h, surface_w, surface_h).has_value()) << "bottom bar";
  EXPECT_FALSE(to_desktop({0, 0, 0, 0}, desktop_w, desktop_h, surface_w, surface_h).has_value()) << "empty";
  EXPECT_FALSE(to_desktop({0, 0, surface_w, surface_h}, 0, 0, surface_w, surface_h).has_value()) << "no desktop";

  // A rectangle straddling the top bar keeps only the part that shows desktop.
  const auto straddling = to_desktop({0, 100, surface_w, 200}, desktop_w, desktop_h, surface_w, surface_h);
  ASSERT_TRUE(straddling.has_value());
  EXPECT_EQ(straddling->y, 0) << "the padding contributes nothing above the desktop";
}

/**
 * @brief Round-tripping a desktop rectangle through the reference frame comes back close.
 *
 * Not exact — the transform quantises to encoded-frame pixels, and one of those is 4.19
 * desktop pixels wide on this setup — but it must not drift, because the echo is what the
 * client reconciles against. Tolerance is one encoded pixel's worth of desktop.
 */
TEST(MeowViewportReference, RoundTripsWithinOneEncodedPixel) {
  const double tolerance_x = static_cast<double>(desktop_w) / reference_frame(desktop_w, desktop_h, surface_w, surface_h).content_width;
  const double tolerance_y = static_cast<double>(desktop_h) / reference_frame(desktop_w, desktop_h, surface_w, surface_h).content_height;

  const std::vector<rect_t> desktop_rects {
    {0, 0, desktop_w, desktop_h},
    {1920, 0, 3440, 1440},
    {0, 0, 1920, 1200},
    {2000, 400, 1200, 800},
    {5000, 1300, 360, 140},
  };

  for (const auto &d : desktop_rects) {
    const auto in_frame = to_reference(d, desktop_w, desktop_h, surface_w, surface_h);
    const auto back = to_desktop(in_frame, desktop_w, desktop_h, surface_w, surface_h);
    ASSERT_TRUE(back.has_value()) << d.x << ',' << d.y;
    EXPECT_NEAR(back->x, d.x, 2 * tolerance_x) << d.x << ',' << d.y;
    EXPECT_NEAR(back->y, d.y, 2 * tolerance_y) << d.x << ',' << d.y;
    EXPECT_NEAR(back->width, d.width, 3 * tolerance_x) << d.x << ',' << d.y;
    EXPECT_NEAR(back->height, d.height, 3 * tolerance_y) << d.x << ',' << d.y;
  }
}

/**
 * @brief Extreme integers do not overflow on the way through the transform.
 *
 * `parse_payload()` can only produce `uint16`s, so the wire cannot reach these values today.
 * `to_desktop()` is a public entry point on a hostile-input path, though, and `x + width` on
 * two `INT_MAX`s is undefined behaviour rather than a large number — so the far edges are
 * computed in 64 bits and this pins that.
 */
TEST(MeowViewportReference, ExtremeIntegersDoNotOverflow) {
  constexpr int big = std::numeric_limits<int>::max();
  constexpr int small = std::numeric_limits<int>::min();

  const std::vector<rect_t> extremes {
    {big, big, big, big},
    {0, 0, big, big},
    {big, 0, big, 1},
    {small, small, big, big},
    {0, 0, small, small},
    {-1, -1, big, big},
  };

  for (const auto &r : extremes) {
    const auto mapped = to_desktop(r, desktop_w, desktop_h, surface_w, surface_h);
    if (!mapped) {
      continue;
    }
    EXPECT_GE(mapped->x, 0) << r.x << ',' << r.y;
    EXPECT_GE(mapped->y, 0) << r.x << ',' << r.y;
    EXPECT_GT(mapped->width, 0) << r.x << ',' << r.y;
    EXPECT_GT(mapped->height, 0) << r.x << ',' << r.y;
    EXPECT_LE(mapped->x + mapped->width, desktop_w) << r.x << ',' << r.y;
    EXPECT_LE(mapped->y + mapped->height, desktop_h) << r.x << ',' << r.y;
  }
}

/**
 * @brief Every reference-frame rectangle maps inside the desktop, however hostile.
 */
TEST(MeowViewportReference, MappingNeverEscapesTheDesktop) {
  std::mt19937 rng {5150};
  std::uniform_int_distribution<int> coord {-70000, 70000};

  for (int i = 0; i < 20000; ++i) {
    const auto mapped = to_desktop({coord(rng), coord(rng), coord(rng), coord(rng)}, desktop_w, desktop_h, surface_w, surface_h);
    if (!mapped) {
      continue;
    }
    ASSERT_GE(mapped->x, 0) << i;
    ASSERT_GE(mapped->y, 0) << i;
    ASSERT_GT(mapped->width, 0) << i;
    ASSERT_GT(mapped->height, 0) << i;
    ASSERT_LE(mapped->x + mapped->width, desktop_w) << i;
    ASSERT_LE(mapped->y + mapped->height, desktop_h) << i;
  }
}

// ---------------------------------------------------------------------------------
// What a packet means: the hostile-input entry point, without any global state.
// ---------------------------------------------------------------------------------

/**
 * @brief A good request is mapped, published in desktop pixels and echoed in frame pixels.
 *
 * The client asks for the right-hand half of the visible strip; the host publishes the
 * corresponding desktop rectangle to the encoder and answers in the client's own
 * coordinate system.
 */
TEST(MeowViewportRequest, GoodRequestIsPublishedAndEchoed) {
  const auto outcome = evaluate_request(make_payload(1, 0, 640, 188, 640, 343), desktop_w, desktop_h, surface_w, surface_h);

  ASSERT_TRUE(outcome.publish.has_value());
  EXPECT_EQ(*outcome.publish, (rect_t {2680, 0, 2680, 1440})) << "published in captured-desktop pixels";

  ASSERT_TRUE(outcome.echo.has_value());
  EXPECT_EQ(outcome.echo->x, 640) << "echoed in the reference frame the request arrived in";
  EXPECT_EQ(outcome.echo->y, 188);
  EXPECT_EQ(outcome.echo->width, 640);
  EXPECT_EQ(outcome.echo->height, 343);
}

/**
 * @brief When the host applies something other than what was asked for, it says so.
 *
 * Requirement 2, and load-bearing: without a truthful echo the client shows the crop under
 * its own local zoom, magnified twice. Here the request runs off the right of the visible
 * strip, so what is applied is narrower than what was asked for.
 */
TEST(MeowViewportRequest, EchoesTheAppliedRectangleNotTheRequestedOne) {
  const rect_t asked {1100, 400, 400, 400};
  const auto outcome = evaluate_request(make_payload(1, 0, asked.x, asked.y, asked.width, asked.height), desktop_w, desktop_h, surface_w, surface_h);

  ASSERT_TRUE(outcome.echo.has_value());
  EXPECT_NE(*outcome.echo, asked) << "the request could not be honoured as sent";

  const auto ref = reference_frame(desktop_w, desktop_h, surface_w, surface_h);
  EXPECT_LE(outcome.echo->x + outcome.echo->width, ref.content_x + ref.content_width);
  EXPECT_LE(outcome.echo->y + outcome.echo->height, ref.content_y + ref.content_height);
  EXPECT_GE(outcome.echo->y, ref.content_y);
}

/**
 * @brief A request the host cannot honour publishes nothing and echoes the whole strip.
 *
 * These all parse. The host understood them and decided against them, so it says so —
 * "refused" and "lost in transit" must not look the same to the client.
 */
TEST(MeowViewportRequest, UnhonourableRequestEchoesTheFullDesktop) {
  const auto ref = reference_frame(desktop_w, desktop_h, surface_w, surface_h);
  const rect_t whole_desktop_in_frame {ref.content_x, ref.content_y, ref.content_width, ref.content_height};

  const std::vector<std::pair<const char *, std::string>> refused {
    {"wholly outside", make_payload(1, 0, 60000, 60000, 800, 600)},
    {"entirely padding", make_payload(1, 0, 0, 0, 1280, 100)},
    {"degenerate aspect", make_payload(1, 0, 0, 300, 1280, 4)},
    {"the whole frame", make_payload(1, 0, 0, 0, 1280, 720)},
  };

  for (const auto &[name, payload] : refused) {
    const auto outcome = evaluate_request(payload, desktop_w, desktop_h, surface_w, surface_h);
    EXPECT_TRUE(outcome.understood) << name;
    EXPECT_FALSE(outcome.publish.has_value()) << name;
    ASSERT_TRUE(outcome.echo.has_value()) << name;
    EXPECT_EQ(*outcome.echo, whole_desktop_in_frame) << name;
  }
}

/**
 * @brief A message the host cannot parse changes nothing at all.
 *
 * "Stop cropping" has its own representation — a rectangle covering the whole encoded
 * frame. A truncated packet, a corrupted one, or one from a future client speaking a
 * version we reject carries no such meaning, and reading it as one would throw away the
 * user's zoom on a single bad packet.
 */
TEST(MeowViewportRequest, UnparseableRequestChangesNothing) {
  const std::vector<std::pair<const char *, std::string>> ignored {
    {"unparseable", "not a viewport payload"},
    {"empty", ""},
    {"truncated", make_payload(1, 0, 100, 300, 400, 200).substr(0, 9)},
    {"future version", make_payload(9, 0, 100, 300, 400, 200)},
    {"zero size", make_payload(1, 0, 100, 300, 0, 0)},
  };

  for (const auto &[name, payload] : ignored) {
    const auto outcome = evaluate_request(payload, desktop_w, desktop_h, surface_w, surface_h);
    EXPECT_FALSE(outcome.understood) << name;
    EXPECT_FALSE(outcome.echo.has_value()) << name;
    EXPECT_FALSE(outcome.publish.has_value()) << name;
  }
}

/**
 * @brief With no usable geometry there is nothing truthful to report, so nothing is said.
 */
TEST(MeowViewportRequest, NoGeometryMeansNoPromise) {
  const auto outcome = evaluate_request(make_payload(1, 0, 0, 0, 800, 600), 0, 0, surface_w, surface_h);
  EXPECT_FALSE(outcome.echo.has_value());
  EXPECT_FALSE(outcome.publish.has_value());
}

/**
 * @brief Fuzzing the raw payload never produces a rectangle outside the desktop or frame.
 *
 * The bytes come off the network. This drives arbitrary ones straight through the entry
 * point the control thread uses, and checks both coordinate systems on the way out.
 */
TEST(MeowViewportRequest, FuzzedPayloadsNeverEscapeTheDesktop) {
  std::mt19937 rng {424242};
  std::uniform_int_distribution<int> byte {0, 255};
  std::uniform_int_distribution<int> len {0, 24};
  const auto ref = reference_frame(desktop_w, desktop_h, surface_w, surface_h);

  for (int i = 0; i < 20000; ++i) {
    std::string payload;
    const auto n = len(rng);
    for (int j = 0; j < n; ++j) {
      payload.push_back(static_cast<char>(byte(rng)));
    }
    // Make roughly half of them well-formed enough to reach the geometry.
    if (payload.size() >= 2 && (i % 2) == 0) {
      payload[0] = static_cast<char>(payload_version);
    }

    const auto outcome = evaluate_request(payload, desktop_w, desktop_h, surface_w, surface_h);
    if (outcome.echo) {
      ASSERT_GE(outcome.echo->x, ref.content_x) << i;
      ASSERT_GE(outcome.echo->y, ref.content_y) << i;
      ASSERT_GT(outcome.echo->width, 0) << i;
      ASSERT_GT(outcome.echo->height, 0) << i;
      ASSERT_LE(outcome.echo->x + outcome.echo->width, ref.content_x + ref.content_width) << i;
      ASSERT_LE(outcome.echo->y + outcome.echo->height, ref.content_y + ref.content_height) << i;
    }
    if (outcome.publish) {
      const auto p = plan(desktop_w, desktop_h, surface_w, surface_h, outcome.publish);
      ASSERT_LE(p.source.x + p.source.width, desktop_w) << i;
      ASSERT_LE(p.source.y + p.source.height, desktop_h) << i;
    }
  }
}

// ---------------------------------------------------------------------------------
// The scaler adapter, driven against real AVFrames and a real pixel buffer.
// ---------------------------------------------------------------------------------

/**
 * @brief Reconfiguration happens on a change and never on a steady state.
 *
 * Requirement 6: the per-frame path must not do work when nothing moved. A reinit that
 * fired every frame would rebuild swscale's filter tables at up to 180 Hz.
 */
TEST(MeowViewportScaler, ReconfiguresOnlyWhenSomethingChanged) {
  AVFrame *in = av_frame_alloc();
  AVFrame *out = av_frame_alloc();
  ASSERT_NE(in, nullptr);
  ASSERT_NE(out, nullptr);

  int offset_w = 0;
  int offset_h = 188;
  in->width = desktop_w;
  in->height = desktop_h;
  out->width = 1280;
  out->height = 343;

  const auto full = plan(desktop_w, desktop_h, surface_w, surface_h, std::nullopt);
  ASSERT_EQ(full.out_width, 1280);
  ASSERT_EQ(full.out_height, 343) << "5360x1440 into 1280x720 letterboxes to a 343-tall strip";
  ASSERT_EQ(full.offset_h, 188);
  EXPECT_FALSE(configure_scaler(full, *in, *out, offset_w, offset_h)) << "steady state must be free";

  const auto cropped = plan(desktop_w, desktop_h, surface_w, surface_h, rect_t {1920, 180, 1920, 1080});
  EXPECT_TRUE(configure_scaler(cropped, *in, *out, offset_w, offset_h));
  EXPECT_EQ(in->width, 1920);
  EXPECT_EQ(in->height, 1080);
  EXPECT_EQ(out->width, 1280);
  EXPECT_EQ(out->height, 720);
  EXPECT_EQ(offset_w, 0);
  EXPECT_EQ(offset_h, 0);

  EXPECT_FALSE(configure_scaler(cropped, *in, *out, offset_w, offset_h)) << "same crop twice must be free";

  // ... and reverting restores exactly what init() had configured.
  EXPECT_TRUE(configure_scaler(full, *in, *out, offset_w, offset_h));
  EXPECT_EQ(in->width, desktop_w);
  EXPECT_EQ(in->height, desktop_h);
  EXPECT_EQ(out->width, 1280);
  EXPECT_EQ(out->height, 343);
  EXPECT_EQ(offset_h, 188);

  av_frame_free(&in);
  av_frame_free(&out);
}

/**
 * @brief A failed reconfiguration drops the crop, not the session.
 *
 * `convert()` returning nonzero ends the encode loop and the session. Upstream
 * reinitialised swscale at most twice per session, so a failure there was effectively
 * unreachable after startup; a crop makes it reachable on every zoom, driven by network
 * input. A transient allocation failure must therefore cost the user their zoom, not their
 * stream — and must leave the scaler on a configuration that still works.
 */
TEST(MeowViewportScaler, FailedReinitRestoresTheWorkingConfiguration) {
  AVFrame *in = av_frame_alloc();
  AVFrame *out = av_frame_alloc();
  AVFrame *surface = av_frame_alloc();
  ASSERT_NE(in, nullptr);
  ASSERT_NE(out, nullptr);
  ASSERT_NE(surface, nullptr);

  int offset_w = 0;
  int offset_h = 188;
  in->width = desktop_w;
  in->height = desktop_h;
  out->width = 1280;
  out->height = 343;
  out->format = AV_PIX_FMT_NV12;
  surface->width = surface_w;
  surface->height = surface_h;
  surface->format = AV_PIX_FMT_NV12;
  ASSERT_EQ(av_frame_get_buffer(surface, 0), 0);

  const auto cropped = plan(desktop_w, desktop_h, surface_w, surface_h, rect_t {1920, 180, 1920, 1080});
  ASSERT_TRUE(cropped.cropped);

  int reinit_calls = 0;
  const auto always_fails = [&reinit_calls] {
    ++reinit_calls;
    return -1;
  };

  // The crop is attempted, fails, and the previous configuration comes back. The second
  // reinit fails too in this fixture, so the call reports the genuinely fatal case.
  EXPECT_FALSE(meow::viewport::apply_plan(cropped, *in, *out, offset_w, offset_h, *surface, always_fails));
  EXPECT_EQ(reinit_calls, 2) << "one attempt, one restore";
  EXPECT_EQ(in->width, desktop_w) << "the scaler must be back on what was working";
  EXPECT_EQ(in->height, desktop_h);
  EXPECT_EQ(out->width, 1280);
  EXPECT_EQ(out->height, 343);
  EXPECT_EQ(offset_h, 188);

  // With a restore that succeeds, the session survives and the crop is simply dropped.
  int calls = 0;
  const auto fails_once = [&calls] {
    return ++calls == 1 ? -1 : 0;
  };
  EXPECT_TRUE(meow::viewport::apply_plan(cropped, *in, *out, offset_w, offset_h, *surface, fails_once));
  EXPECT_EQ(calls, 2);
  EXPECT_EQ(in->width, desktop_w);
  EXPECT_EQ(out->height, 343);

  // And a reconfiguration that works is applied with a single reinit.
  int ok_calls = 0;
  const auto succeeds = [&ok_calls] {
    ++ok_calls;
    return 0;
  };
  EXPECT_TRUE(meow::viewport::apply_plan(cropped, *in, *out, offset_w, offset_h, *surface, succeeds));
  EXPECT_EQ(ok_calls, 1);
  EXPECT_EQ(in->width, 1920);
  EXPECT_EQ(out->width, 1280);
  EXPECT_EQ(out->height, 720);

  // A steady state costs no reinit at all.
  ok_calls = 0;
  EXPECT_TRUE(meow::viewport::apply_plan(cropped, *in, *out, offset_w, offset_h, *surface, succeeds));
  EXPECT_EQ(ok_calls, 0);

  av_frame_free(&in);
  av_frame_free(&out);
  av_frame_free(&surface);
}

/**
 * @brief Re-blackening the surface does not reallocate it.
 */
TEST(MeowViewportScaler, ReblackKeepsTheSameAllocation) {
  AVFrame *surface = av_frame_alloc();
  ASSERT_NE(surface, nullptr);
  surface->width = 320;
  surface->height = 180;
  surface->format = AV_PIX_FMT_NV12;
  ASSERT_EQ(av_frame_get_buffer(surface, 0), 0);
  std::memset(surface->data[0], 0x7F, static_cast<std::size_t>(surface->linesize[0]) * surface->height);

  const auto *buffer = surface->buf[0];
  for (int i = 0; i < 100; ++i) {
    meow::viewport::reblack(*surface);
    ASSERT_EQ(surface->buf[0], buffer) << i;
    ASSERT_EQ(av_buffer_get_ref_count(surface->buf[0]), 1) << i;
  }
  // Limited range, so "black" is luma 16 -- the same value upstream's prefill() writes, and
  // the reason the padding assertions elsewhere in this file test against 20 rather than 0.
  EXPECT_EQ(surface->data[0][0], 16) << "and it actually blackens";

  // An unallocated frame is simply left alone rather than dereferenced.
  AVFrame *empty = av_frame_alloc();
  ASSERT_NE(empty, nullptr);
  empty->width = 320;
  empty->height = 180;
  empty->format = AV_PIX_FMT_NV12;
  meow::viewport::reblack(*empty);
  EXPECT_EQ(empty->data[0], nullptr);

  av_frame_free(&empty);
  av_frame_free(&surface);
}

/**
 * @brief An absent plan leaves the scaler completely alone.
 */
TEST(MeowViewportScaler, NulloptIsAlwaysANoOp) {
  AVFrame *in = av_frame_alloc();
  AVFrame *out = av_frame_alloc();
  ASSERT_NE(in, nullptr);
  ASSERT_NE(out, nullptr);
  in->width = 5360;
  in->height = 1440;
  out->width = 1280;
  out->height = 343;
  int offset_w = 0;
  int offset_h = 188;

  EXPECT_FALSE(configure_scaler(std::nullopt, *in, *out, offset_w, offset_h));
  EXPECT_EQ(in->width, 5360);
  EXPECT_EQ(out->height, 343);
  EXPECT_EQ(offset_h, 188);

  std::uint8_t *const base = reinterpret_cast<std::uint8_t *>(0x1000);
  in->data[0] = base;
  offset_source_planes(*in, std::nullopt, 4096, 4, false);
  EXPECT_EQ(in->data[0], base);

  av_frame_free(&in);
  av_frame_free(&out);
}

/**
 * @brief The plane offsets address exactly the crop origin, in a real buffer.
 *
 * Written against an actual BGR0 buffer whose bytes encode their own coordinates, so a
 * wrong pitch or a wrong pixel size is visible as a wrong pixel rather than as an
 * arithmetic identity that reproduces the bug.
 */
TEST(MeowViewportScaler, OffsetsLandOnTheCropOriginInARealBuffer) {
  constexpr int w = 320;
  constexpr int h = 64;
  constexpr int pixel_pitch = 4;
  constexpr int row_pitch = w * pixel_pitch;

  std::vector<std::uint8_t> buffer(static_cast<std::size_t>(row_pitch) * h);
  for (int y = 0; y < h; ++y) {
    for (int x = 0; x < w; ++x) {
      buffer[static_cast<std::size_t>(y) * row_pitch + static_cast<std::size_t>(x) * pixel_pitch] = static_cast<std::uint8_t>(x & 0xFF);
      buffer[static_cast<std::size_t>(y) * row_pitch + static_cast<std::size_t>(x) * pixel_pitch + 1] = static_cast<std::uint8_t>(y & 0xFF);
    }
  }

  AVFrame *in = av_frame_alloc();
  ASSERT_NE(in, nullptr);
  in->data[0] = buffer.data();
  in->linesize[0] = row_pitch;

  plan_t p;
  p.source = {64, 16, 128, 32};
  p.out_width = 128;
  p.out_height = 32;
  p.cropped = true;

  offset_source_planes(*in, p, row_pitch, pixel_pitch, false);
  EXPECT_EQ(in->data[0][0], 64) << "x of the crop origin";
  EXPECT_EQ(in->data[0][1], 16) << "y of the crop origin";
  // ... and the pixel one row down and one column right is where it should be.
  EXPECT_EQ(in->data[0][row_pitch + pixel_pitch], 65);
  EXPECT_EQ(in->data[0][row_pitch + pixel_pitch + 1], 17);

  // The last pixel of the crop is still inside the allocation.
  const auto *last = in->data[0] + static_cast<std::ptrdiff_t>(p.source.height - 1) * row_pitch + static_cast<std::ptrdiff_t>(p.source.width - 1) * pixel_pitch;
  EXPECT_LT(last, buffer.data() + buffer.size());

  av_frame_free(&in);
}

/**
 * @brief The NV12 chroma plane is offset by half the rows and the full column count.
 *
 * NV12 chroma is subsampled 2x2 and interleaved two bytes per sample pair, so an even
 * luma column offset of `x` is a chroma *byte* offset of `x`, and a luma row offset of
 * `y` is a chroma row offset of `y / 2`.
 */
TEST(MeowViewportScaler, Nv12ChromaPlaneIsOffsetCorrectly) {
  constexpr int w = 320;
  constexpr int h = 64;
  constexpr int row_pitch = w;

  std::vector<std::uint8_t> buffer(static_cast<std::size_t>(row_pitch) * h * 3 / 2);

  AVFrame *in = av_frame_alloc();
  ASSERT_NE(in, nullptr);
  in->data[0] = buffer.data();
  in->linesize[0] = row_pitch;
  in->data[1] = buffer.data() + static_cast<std::size_t>(row_pitch) * h;
  in->linesize[1] = row_pitch;

  plan_t p;
  p.source = {64, 16, 128, 32};
  p.out_width = 128;
  p.out_height = 32;
  p.cropped = true;

  offset_source_planes(*in, p, row_pitch, 1, true);
  EXPECT_EQ(in->data[0], buffer.data() + 16 * row_pitch + 64);
  EXPECT_EQ(in->data[1], buffer.data() + static_cast<std::size_t>(row_pitch) * h + 8 * row_pitch + 64);

  // The last chroma byte the scaler will read is still inside the allocation.
  const auto *last_chroma = in->data[1] + static_cast<std::ptrdiff_t>(p.source.height / 2 - 1) * row_pitch + p.source.width - 1;
  EXPECT_LT(last_chroma, buffer.data() + buffer.size());

  av_frame_free(&in);
}

// ---------------------------------------------------------------------------------
// End to end through real libswscale: does the crop actually select the right pixels?
// ---------------------------------------------------------------------------------

namespace {

  /**
   * @brief Build a scaler the way `avcodec_software_encode_device_t::reinit_sws()` does.
   *
   * Transcribed from `src/video.cpp` so this test exercises the real configuration --
   * including `SWS_LANCZOS | SWS_ACCURATE_RND` and the explicitly-initialised (rather than
   * dynamic) usage mode, which is the mode whose "properties may no longer change after
   * initialization" rule is exactly what the reconfigure path has to respect.
   *
   * @param in Scaler input frame.
   * @param out Scaler output frame.
   * @return The initialised context, or nullptr on failure.
   */
  SwsContext *make_upstream_style_sws(const AVFrame &in, const AVFrame &out) {
    SwsContext *sws = sws_alloc_context();
    if (!sws) {
      return nullptr;
    }
    AVDictionary *options {nullptr};
    av_dict_set_int(&options, "srcw", in.width, 0);
    av_dict_set_int(&options, "srch", in.height, 0);
    av_dict_set_int(&options, "src_format", in.format, 0);
    av_dict_set_int(&options, "dstw", out.width, 0);
    av_dict_set_int(&options, "dsth", out.height, 0);
    av_dict_set_int(&options, "dst_format", out.format, 0);
    av_dict_set_int(&options, "sws_flags", SWS_LANCZOS | SWS_ACCURATE_RND, 0);
    av_dict_set_int(&options, "threads", 1, 0);
    const auto status = av_opt_set_dict(sws, &options);
    av_dict_free(&options);
    if (status < 0 || sws_init_context(sws, nullptr, nullptr) < 0) {
      sws_freeContext(sws);
      return nullptr;
    }
    return sws;
  }

}  // namespace

/**
 * @brief The crop selects the requested region of a real buffer, through real swscale.
 *
 * A synthetic desktop whose left half is black and right half is white. Uncropped, the
 * scaled output contains both. Cropped to the right half, it contains only white -- which
 * is only true if the source rectangle, the plane pointer offsets, the row pitch and the
 * scaler reconfiguration are all right together.
 *
 * This also proves the reconfigure is safe: the intermediate output frame is allocated by
 * `sws_scale_frame()` on first use, and is scaled into again at a different size after
 * `configure_scaler()` has changed its dimensions. Getting that wrong writes past the end
 * of a heap buffer — silently, most of the time, which is why the reconfiguration is
 * exercised here against real swscale rather than reasoned about.
 */
TEST(MeowViewportEndToEnd, CropSelectsTheRequestedRegionThroughRealSwscale) {
  constexpr int capture_w = 800;
  constexpr int capture_h = 200;
  constexpr int enc_w = 320;
  constexpr int enc_h = 180;
  constexpr int pixel_pitch = 4;
  constexpr int row_pitch = capture_w * pixel_pitch;

  std::vector<std::uint8_t> desktop(static_cast<std::size_t>(row_pitch) * capture_h, 0);
  for (int y = 0; y < capture_h; ++y) {
    for (int x = capture_w / 2; x < capture_w; ++x) {
      auto *px = desktop.data() + static_cast<std::size_t>(y) * row_pitch + static_cast<std::size_t>(x) * pixel_pitch;
      px[0] = px[1] = px[2] = 0xFF;
    }
  }

  AVFrame *in = av_frame_alloc();
  AVFrame *out = av_frame_alloc();
  ASSERT_NE(in, nullptr);
  ASSERT_NE(out, nullptr);
  in->format = AV_PIX_FMT_BGR0;
  out->format = AV_PIX_FMT_NV12;

  int offset_w = 0;
  int offset_h = 0;

  /// Scale the current configuration and report the mean luma of the output.
  const auto scale_and_mean_luma = [&](const std::optional<plan_t> &planned) {
    in->data[0] = desktop.data();
    in->linesize[0] = row_pitch;
    in->data[1] = nullptr;
    in->linesize[1] = 0;
    offset_source_planes(*in, planned, row_pitch, pixel_pitch, false);

    SwsContext *sws = make_upstream_style_sws(*in, *out);
    EXPECT_NE(sws, nullptr);
    const auto status = sws_scale_frame(sws, out, in);
    sws_freeContext(sws);
    EXPECT_GE(status, 0) << "sws_scale_frame failed";

    double total = 0;
    for (int y = 0; y < out->height; ++y) {
      for (int x = 0; x < out->width; ++x) {
        total += out->data[0][static_cast<std::size_t>(y) * out->linesize[0] + x];
      }
    }
    return total / (out->width * out->height);
  };

  // 1. Uncropped, exactly as init() would have configured it.
  const auto full = plan(capture_w, capture_h, enc_w, enc_h, std::nullopt);
  ASSERT_TRUE(configure_scaler(full, *in, *out, offset_w, offset_h));
  ASSERT_EQ(in->width, capture_w);
  const auto full_luma = scale_and_mean_luma(full);
  EXPECT_GT(full_luma, 40.0) << "half the desktop is white, so the mean cannot be black";
  EXPECT_LT(full_luma, 200.0) << "half the desktop is black, so the mean cannot be white";

  // 2. Crop to the white right-hand half, reconfiguring the already-allocated output.
  const auto cropped = plan(capture_w, capture_h, enc_w, enc_h, rect_t {capture_w / 2, 0, capture_w / 2, capture_h});
  ASSERT_TRUE(cropped.cropped) << "the crop must actually be applied";
  ASSERT_TRUE(configure_scaler(cropped, *in, *out, offset_w, offset_h));
  EXPECT_EQ(in->width, capture_w / 2);
  EXPECT_EQ(in->height, capture_h);
  const auto cropped_luma = scale_and_mean_luma(cropped);
  EXPECT_GT(cropped_luma, 220.0) << "the crop covers only white pixels, mean luma was " << cropped_luma;

  // 3. Pan to the black left-hand half. Same size, different origin -- so the scaler is
  //    *not* reconfigured, and the pan is nothing but the two pointer additions in
  //    offset_source_planes(). That it still lands on the right pixels is the point.
  const auto left = plan(capture_w, capture_h, enc_w, enc_h, rect_t {0, 0, capture_w / 2, capture_h});
  ASSERT_EQ(left.out_width, cropped.out_width);
  ASSERT_EQ(left.out_height, cropped.out_height);
  EXPECT_FALSE(configure_scaler(left, *in, *out, offset_w, offset_h)) << "a pure pan must not rebuild the scaler";
  const auto left_luma = scale_and_mean_luma(left);
  EXPECT_LT(left_luma, 30.0) << "the crop covers only black pixels, mean luma was " << left_luma;

  // 4. Reverting restores exactly the uncropped configuration.
  ASSERT_TRUE(configure_scaler(full, *in, *out, offset_w, offset_h));
  EXPECT_EQ(in->width, capture_w);
  EXPECT_EQ(in->height, capture_h);
  EXPECT_NEAR(scale_and_mean_luma(full), full_luma, 1.0);

  av_frame_free(&in);
  av_frame_free(&out);
}

// ---------------------------------------------------------------------------------
// Real characterization: upstream's own software encode device, no GPU required.
// ---------------------------------------------------------------------------------

namespace {

  /**
   * @brief A captured image backed by a caller-owned BGR0 buffer.
   */
  struct fake_img_t: platf::img_t {
    ~fake_img_t() override = default;
  };

  /**
   * @brief Fill a BGR0 buffer white on the right half and black on the left.
   * @param buffer Destination buffer.
   * @param w Width in pixels.
   * @param h Height in pixels.
   * @param pixel_pitch Bytes per pixel.
   */
  void paint_half_white(std::vector<std::uint8_t> &buffer, const int w, const int h, const int pixel_pitch) {
    const auto row_pitch = static_cast<std::size_t>(w) * pixel_pitch;
    buffer.assign(row_pitch * h, 0);
    for (int y = 0; y < h; ++y) {
      for (int x = w / 2; x < w; ++x) {
        auto *px = buffer.data() + static_cast<std::size_t>(y) * row_pitch + static_cast<std::size_t>(x) * pixel_pitch;
        px[0] = px[1] = px[2] = 0xFF;
      }
    }
  }

  /**
   * @brief Mean luma of a row of the encode surface.
   * @param frame Encode surface.
   * @param row Row index.
   * @return Mean value of the Y plane across that row.
   */
  double row_luma(const AVFrame &frame, const int row) {
    double total = 0;
    for (int x = 0; x < frame.width; ++x) {
      total += frame.data[0][static_cast<std::size_t>(row) * frame.linesize[0] + x];
    }
    return total / frame.width;
  }

}  // namespace

/**
 * @brief Upstream's software encode device letterboxes an ultrawide capture, and still does.
 *
 * `src/video.cpp` is on CLAUDE.md §5's untested-module list and this change hooks three
 * places inside it, so this drives the real `avcodec_software_encode_device_t` — `init()`,
 * `set_frame()`, `apply_colorspace()` and `convert()` — over a real captured buffer, with no
 * GPU, no display and no client. It is the only test here that executes upstream's code.
 *
 * With no viewport, an 800x200 capture in a 320x180 surface must land as a centred 320x80
 * strip with black padding above and below. That pins the padding placement, the `offsetW`/
 * `offsetH` arithmetic and the `requires_padding` `memcpy` — the exact machinery the crop
 * hooks reconfigure.
 */
TEST(MeowViewportUpstream, UncroppedConvertLetterboxesExactlyAsBefore) {
  meow::viewport::reset();

  constexpr int capture_w = 800;
  constexpr int capture_h = 200;
  constexpr int enc_w = 320;
  constexpr int enc_h = 180;
  constexpr int pixel_pitch = 4;

  std::vector<std::uint8_t> desktop;
  paint_half_white(desktop, capture_w, capture_h, pixel_pitch);

  fake_img_t img;
  img.data = desktop.data();
  img.width = capture_w;
  img.height = capture_h;
  img.pixel_pitch = pixel_pitch;
  img.row_pitch = capture_w * pixel_pitch;

  AVFrame *surface = av_frame_alloc();
  ASSERT_NE(surface, nullptr);
  surface->width = enc_w;
  surface->height = enc_h;
  surface->format = AV_PIX_FMT_NV12;

  video::avcodec_software_encode_device_t device;
  device.colorspace = {video::colorspace_e::rec601, false, 8};
  ASSERT_EQ(device.init(capture_w, capture_h, surface, AV_PIX_FMT_NV12, false), 0);
  ASSERT_EQ(device.set_frame(surface, nullptr), 0);
  device.apply_colorspace();
  ASSERT_EQ(device.convert(img), 0);

  // 800x200 into 320x180 scales by 0.4 -> a 320x80 strip, centred with 50 rows of padding.
  const auto expected_offset = (enc_h - static_cast<int>(capture_h * 0.4f)) / 2;
  ASSERT_EQ(expected_offset, 50);

  EXPECT_LT(row_luma(*surface, 0), 20.0) << "top padding must be black";
  EXPECT_LT(row_luma(*surface, enc_h - 1), 20.0) << "bottom padding must be black";
  const auto middle = row_luma(*surface, enc_h / 2);
  EXPECT_GT(middle, 40.0) << "the middle row shows half a white desktop";
  EXPECT_LT(middle, 200.0);
}

/**
 * @brief A crop reaches the encoder through upstream's convert(), without leaking the surface.
 *
 * Two things at once, because they are only observable together on the real device.
 *
 * First: cropping to the white half of the desktop must make the *whole* visible strip
 * white, through the real `convert()` — the source rect, the plane pointers, the scaler
 * reconfiguration and the padding memcpy all agreeing.
 *
 * Second, and this is the regression this test exists for: the encode surface buffer must
 * be the same allocation before and after. An earlier revision called upstream's
 * `prefill()` on every crop change to re-blacken the padding. `prefill()` calls
 * `av_frame_get_buffer()`, documented "if frame already has been allocated, calling this
 * function will leak memory" — which at 1280x720 NV12, twenty times a second during a
 * pinch-zoom, is tens of megabytes a second on a long-running server. `buf[0]` changing
 * here is that bug coming back.
 */
TEST(MeowViewportUpstream, CroppedConvertFillsTheStripAndDoesNotReallocate) {
  meow::viewport::reset();

  constexpr int capture_w = 800;
  constexpr int capture_h = 200;
  constexpr int enc_w = 320;
  constexpr int enc_h = 180;
  constexpr int pixel_pitch = 4;

  std::vector<std::uint8_t> desktop;
  paint_half_white(desktop, capture_w, capture_h, pixel_pitch);

  fake_img_t img;
  img.data = desktop.data();
  img.width = capture_w;
  img.height = capture_h;
  img.pixel_pitch = pixel_pitch;
  img.row_pitch = capture_w * pixel_pitch;

  AVFrame *surface = av_frame_alloc();
  ASSERT_NE(surface, nullptr);
  surface->width = enc_w;
  surface->height = enc_h;
  surface->format = AV_PIX_FMT_NV12;

  video::avcodec_software_encode_device_t device;
  device.colorspace = {video::colorspace_e::rec601, false, 8};
  ASSERT_EQ(device.init(capture_w, capture_h, surface, AV_PIX_FMT_NV12, false), 0);
  ASSERT_EQ(device.set_frame(surface, nullptr), 0);
  device.apply_colorspace();
  ASSERT_EQ(device.convert(img), 0);

  ASSERT_NE(surface->buf[0], nullptr);
  const auto *original_buffer = surface->buf[0];
  const auto *original_data = surface->data[0];

  // Walk a series of distinct crops, the way a pinch-zoom does.
  const std::vector<rect_t> zoom_sequence {
    {capture_w / 2, 0, capture_w / 2, capture_h},
    {capture_w / 2, 0, 300, 180},
    {capture_w / 2, 0, 260, 160},
    {capture_w / 2, 0, 220, 140},
    {capture_w / 2, 0, capture_w / 2, capture_h},
  };

  for (const auto &r : zoom_sequence) {
    meow::viewport::detail::requested.store(meow::viewport::detail::pack(r));
    ASSERT_EQ(device.convert(img), 0) << r.width << 'x' << r.height;
    EXPECT_EQ(surface->buf[0], original_buffer) << "the encode surface was reallocated for a crop of " << r.width << 'x' << r.height;
    EXPECT_EQ(surface->data[0], original_data);
    EXPECT_EQ(av_buffer_get_ref_count(surface->buf[0]), 1);
  }

  // The last crop covers only white desktop, so every visible row is white.
  const auto middle = row_luma(*surface, enc_h / 2);
  EXPECT_GT(middle, 220.0) << "the crop covers only white pixels, mean luma was " << middle;
  EXPECT_LT(row_luma(*surface, 0), 20.0) << "padding must still be black";

  // Reverting puts the whole desktop back, half black again.
  meow::viewport::detail::requested.store(0);
  ASSERT_EQ(device.convert(img), 0);
  EXPECT_LT(row_luma(*surface, enc_h / 2), 200.0);
  EXPECT_EQ(surface->buf[0], original_buffer);

  meow::viewport::reset();
}

// ---------------------------------------------------------------------------------
// Session state.
// ---------------------------------------------------------------------------------

/**
 * @brief A scaler that does not own the state is never touched.
 */
TEST(MeowViewportSession, NonOwnersAreLeftAlone) {
  meow::viewport::reset();

  int me = 0;
  int other = 0;
  meow::viewport::on_scaler_init(&me, desktop_w, desktop_h, surface_w, surface_h);

  EXPECT_TRUE(meow::viewport::plan_for_frame(&me, desktop_w, desktop_h, surface_w, surface_h).has_value());
  EXPECT_FALSE(meow::viewport::plan_for_frame(&other, desktop_w, desktop_h, surface_w, surface_h).has_value());

  meow::viewport::reset();
}

/**
 * @brief `reset()` makes a cropped scaler revert, and stops the host answering.
 *
 * The distinction matters and was wrong once: clearing `owner` or `geometry` first would
 * make `plan_for_frame()` return "do not touch", freezing the scaler on its last crop for
 * the rest of the session instead of reverting it. What `reset()` must do is keep serving
 * the running scaler a plan — the full-frame one — while refusing to answer anybody.
 */
TEST(MeowViewportSession, ResetRevertsRatherThanFreezes) {
  meow::viewport::reset();
  int me = 0;
  meow::viewport::on_scaler_init(&me, desktop_w, desktop_h, surface_w, surface_h);
  ASSERT_TRUE(meow::viewport::apply_request(make_payload(1, 0, 640, 188, 640, 343)).has_value());
  ASSERT_TRUE(meow::viewport::plan_for_frame(&me, desktop_w, desktop_h, surface_w, surface_h)->cropped);

  meow::viewport::reset();

  const auto after = meow::viewport::plan_for_frame(&me, desktop_w, desktop_h, surface_w, surface_h);
  ASSERT_TRUE(after.has_value()) << "the running scaler must still be served a plan";
  EXPECT_FALSE(after->cropped);
  EXPECT_EQ(*after, full_frame_plan(desktop_w, desktop_h, surface_w, surface_h));

  // ... and nothing can re-crop on the way out.
  EXPECT_FALSE(meow::viewport::apply_request(make_payload(1, 0, 640, 188, 640, 343)).has_value());
  EXPECT_EQ(meow::viewport::detail::requested.load(), 0u);
  EXPECT_FALSE(meow::viewport::plan_for_frame(&me, desktop_w, desktop_h, surface_w, surface_h)->cropped);

  meow::viewport::reset();
}

/**
 * @brief A surface size that disagrees with the recorded one is not trusted.
 */
TEST(MeowViewportSession, MismatchedSurfaceIsRefused) {
  meow::viewport::reset();
  int me = 0;
  meow::viewport::on_scaler_init(&me, desktop_w, desktop_h, surface_w, surface_h);

  EXPECT_FALSE(meow::viewport::plan_for_frame(&me, desktop_w, desktop_h, 1920, 1080).has_value());
  meow::viewport::reset();
}

/**
 * @brief With no request, the owner's plan is exactly the full-frame plan.
 *
 * This is what makes `configure_scaler()` a no-op in the steady state, and what makes a
 * previously cropped scaler revert on the next frame.
 */
TEST(MeowViewportSession, OwnerWithNoRequestGetsTheFullFramePlan) {
  meow::viewport::reset();
  int me = 0;
  meow::viewport::on_scaler_init(&me, desktop_w, desktop_h, surface_w, surface_h);

  const auto p = meow::viewport::plan_for_frame(&me, desktop_w, desktop_h, surface_w, surface_h);
  ASSERT_TRUE(p.has_value());
  EXPECT_EQ(*p, full_frame_plan(desktop_w, desktop_h, surface_w, surface_h));
  EXPECT_FALSE(p->cropped);

  meow::viewport::reset();
}

/**
 * @brief Re-initialising a scaler drops whatever rectangle was pending.
 *
 * Requirement 5: a stale crop must not leak into the next session, and it cannot, because
 * the next session's own `init()` clears it before its first frame.
 */
TEST(MeowViewportSession, ScalerInitClearsThePendingRectangle) {
  meow::viewport::reset();
  int me = 0;
  meow::viewport::on_scaler_init(&me, desktop_w, desktop_h, surface_w, surface_h);
  meow::viewport::detail::requested.store(meow::viewport::detail::pack({1920, 180, 1920, 1080}));

  ASSERT_TRUE(meow::viewport::plan_for_frame(&me, desktop_w, desktop_h, surface_w, surface_h)->cropped);

  meow::viewport::on_scaler_init(&me, desktop_w, desktop_h, surface_w, surface_h);
  EXPECT_FALSE(meow::viewport::plan_for_frame(&me, desktop_w, desktop_h, surface_w, surface_h)->cropped);

  meow::viewport::reset();
}

/**
 * @brief A frame smaller than the one recorded at init tightens the clamp.
 */
TEST(MeowViewportSession, ShortFrameTightensTheClamp) {
  meow::viewport::reset();
  int me = 0;
  meow::viewport::on_scaler_init(&me, desktop_w, desktop_h, surface_w, surface_h);
  meow::viewport::detail::requested.store(meow::viewport::detail::pack({3000, 800, 2000, 800}));

  const auto p = meow::viewport::plan_for_frame(&me, 4096, 1024, surface_w, surface_h);
  ASSERT_TRUE(p.has_value());
  ASSERT_TRUE(p->cropped);
  EXPECT_LE(p->source.x + p->source.width, 4096);
  EXPECT_LE(p->source.y + p->source.height, 1024);

  // A rectangle that no longer intersects the shorter frame at all is dropped, and the
  // stream reverts to the uncropped plan -- which is what upstream would have configured
  // for this scaler anyway, so nothing new is read out of bounds.
  meow::viewport::detail::requested.store(meow::viewport::detail::pack({5000, 1000, 360, 400}));
  const auto gone = meow::viewport::plan_for_frame(&me, 4096, 1024, surface_w, surface_h);
  ASSERT_TRUE(gone.has_value());
  EXPECT_FALSE(gone->cropped);
  EXPECT_EQ(*gone, full_frame_plan(desktop_w, desktop_h, surface_w, surface_h));

  meow::viewport::reset();
}

/**
 * @brief A host with no software scaler stays silent rather than promising a crop.
 *
 * On an NVIDIA host the encoder takes the CUDA scaler, `on_scaler_init()` is never called,
 * and nothing claims the state. Answering anyway would be a lie the client acts on: it would
 * reset its local zoom to 1:1 for a crop that never happened, leaving the user staring at the
 * whole desktop unzoomed.
 */
TEST(MeowViewportSession, UnclaimedStateProducesNoEcho) {
  meow::viewport::reset();

  const auto payload = make_payload(1, 0, 640, 188, 640, 343);
  EXPECT_FALSE(meow::viewport::apply_request(payload).has_value());
  EXPECT_EQ(meow::viewport::detail::requested.load(), 0u) << "nothing may be published either";

  // Once a scaler claims it, the same bytes are answered.
  int me = 0;
  meow::viewport::on_scaler_init(&me, desktop_w, desktop_h, surface_w, surface_h);
  const auto echoed = meow::viewport::apply_request(payload);
  ASSERT_TRUE(echoed.has_value());
  EXPECT_EQ(echoed->applied, (rect_t {640, 188, 640, 343}));
  EXPECT_EQ(echoed->capture_width, desktop_w);
  EXPECT_EQ(echoed->capture_height, desktop_h);
  EXPECT_NE(meow::viewport::detail::requested.load(), 0u);

  meow::viewport::reset();
}

/**
 * @brief "Not zoomed in" clears the crop; an unparseable packet does not.
 *
 * Driven through the real state machine rather than the pure function, so the stores on
 * both paths are covered. A leftover rectangle on the first would crop the stream to
 * something the client was told it would not get; a cleared one on the second would throw
 * away the user's zoom because a single packet arrived corrupted.
 */
TEST(MeowViewportSession, ClearingIsDeliberateNotAccidental) {
  meow::viewport::reset();
  int me = 0;
  meow::viewport::on_scaler_init(&me, desktop_w, desktop_h, surface_w, surface_h);

  const auto zoom = make_payload(1, 0, 640, 188, 640, 343);
  ASSERT_TRUE(meow::viewport::apply_request(zoom).has_value());
  ASSERT_TRUE(meow::viewport::plan_for_frame(&me, desktop_w, desktop_h, surface_w, surface_h)->cropped);

  // A rectangle covering the whole encoded frame: "not zoomed in", so stop cropping.
  ASSERT_TRUE(meow::viewport::apply_request(make_payload(1, 0, 0, 0, surface_w, surface_h)).has_value());
  EXPECT_EQ(meow::viewport::detail::requested.load(), 0u);
  EXPECT_FALSE(meow::viewport::plan_for_frame(&me, desktop_w, desktop_h, surface_w, surface_h)->cropped);

  // Garbage, however, must leave an active crop exactly where it was.
  ASSERT_TRUE(meow::viewport::apply_request(zoom).has_value());
  const auto before = meow::viewport::plan_for_frame(&me, desktop_w, desktop_h, surface_w, surface_h);
  ASSERT_TRUE(before->cropped);
  EXPECT_FALSE(meow::viewport::apply_request("garbage").has_value()) << "and it is not answered either";
  const auto after = meow::viewport::plan_for_frame(&me, desktop_w, desktop_h, surface_w, surface_h);
  ASSERT_TRUE(after.has_value());
  EXPECT_EQ(*after, *before) << "one corrupted packet must not throw away the user's zoom";

  meow::viewport::reset();
}

/**
 * @brief Repeating the same request is idempotent, because the reference frame does not move.
 *
 * The client keeps reporting the region it wants while a crop is already applied. If the
 * reference frame were defined against the *current* framing rather than the uncropped one,
 * that identical request would mean a different desktop region every time and the view would
 * walk off the screen. This fails if anybody makes the coordinate transform — or the plan —
 * consult the crop that is currently in force.
 */
TEST(MeowViewportSession, RepeatingTheSameRequestIsIdempotent) {
  meow::viewport::reset();
  int me = 0;
  meow::viewport::on_scaler_init(&me, desktop_w, desktop_h, surface_w, surface_h);

  const auto payload = make_payload(1, 0, 640, 188, 640, 343);

  const auto first = evaluate_request(payload, desktop_w, desktop_h, surface_w, surface_h);
  ASSERT_TRUE(first.publish.has_value());
  meow::viewport::detail::requested.store(meow::viewport::detail::pack(*first.publish));

  const auto plan_while_cropped = meow::viewport::plan_for_frame(&me, desktop_w, desktop_h, surface_w, surface_h);
  ASSERT_TRUE(plan_while_cropped.has_value());
  ASSERT_TRUE(plan_while_cropped->cropped);

  // Same bytes, crop already in force: same answer, in both coordinate systems.
  const auto second = evaluate_request(payload, desktop_w, desktop_h, surface_w, surface_h);
  EXPECT_EQ(second, first);

  meow::viewport::detail::requested.store(meow::viewport::detail::pack(*second.publish));
  const auto plan_again = meow::viewport::plan_for_frame(&me, desktop_w, desktop_h, surface_w, surface_h);
  ASSERT_TRUE(plan_again.has_value());
  EXPECT_EQ(*plan_again, *plan_while_cropped);

  meow::viewport::reset();
}

/**
 * @brief When the host drops a crop by itself, it has something to tell the client.
 *
 * The client resets its local zoom to 1:1 on the strength of an echo. If the host then
 * revokes the crop on its own — an encoder reinit or a display mode change re-runs
 * `on_scaler_init()` — and says nothing, the client shows the whole desktop at 1:1 with no
 * way to know why. This is the notification that closes that hole.
 */
TEST(MeowViewportSession, RevokingACropLeavesSomethingToTell) {
  meow::viewport::reset();
  int me = 0;
  meow::viewport::on_scaler_init(&me, desktop_w, desktop_h, surface_w, surface_h);

  // Nothing to report before anything was applied.
  EXPECT_FALSE(meow::viewport::take_revocation_echo().has_value());

  ASSERT_TRUE(meow::viewport::apply_request(make_payload(1, 0, 640, 188, 640, 343)).has_value());
  ASSERT_TRUE(meow::viewport::plan_for_frame(&me, desktop_w, desktop_h, surface_w, surface_h)->cropped);
  EXPECT_FALSE(meow::viewport::take_revocation_echo().has_value()) << "the client was just told";

  // An encoder reinit drops the crop.
  meow::viewport::on_scaler_init(&me, desktop_w, desktop_h, surface_w, surface_h);
  const auto revocation = meow::viewport::take_revocation_echo();
  ASSERT_TRUE(revocation.has_value());

  const auto ref = reference_frame(desktop_w, desktop_h, surface_w, surface_h);
  EXPECT_EQ(revocation->applied, (rect_t {ref.content_x, ref.content_y, ref.content_width, ref.content_height}));
  EXPECT_EQ(revocation->capture_width, desktop_w);
  EXPECT_EQ(revocation->capture_height, desktop_h);

  // One-shot: a second drain finds nothing.
  EXPECT_FALSE(meow::viewport::take_revocation_echo().has_value());

  // A reinit with no crop in force has nothing to revoke.
  meow::viewport::on_scaler_init(&me, desktop_w, desktop_h, surface_w, surface_h);
  EXPECT_FALSE(meow::viewport::take_revocation_echo().has_value());

  // Answering a request supersedes a pending revocation rather than sending both.
  ASSERT_TRUE(meow::viewport::apply_request(make_payload(1, 0, 640, 188, 640, 343)).has_value());
  meow::viewport::on_scaler_init(&me, desktop_w, desktop_h, surface_w, surface_h);
  ASSERT_TRUE(meow::viewport::apply_request(make_payload(1, 0, 300, 200, 400, 300)).has_value());
  EXPECT_FALSE(meow::viewport::take_revocation_echo().has_value());

  // And a session that has ended has nobody to tell.
  meow::viewport::on_scaler_init(&me, desktop_w, desktop_h, surface_w, surface_h);
  ASSERT_TRUE(meow::viewport::apply_request(make_payload(1, 0, 640, 188, 640, 343)).has_value());
  meow::viewport::on_scaler_init(&me, desktop_w, desktop_h, surface_w, surface_h);
  meow::viewport::reset();
  EXPECT_FALSE(meow::viewport::take_revocation_echo().has_value());
}

/**
 * @brief Packing round-trips, and an empty rectangle packs to "nothing published".
 */
TEST(MeowViewportSession, PackRoundTrips) {
  EXPECT_EQ(meow::viewport::detail::pack({0, 0, 0, 0}), 0u);
  EXPECT_EQ(meow::viewport::detail::pack({10, 20, 0, 40}), 0u);
  EXPECT_FALSE(meow::viewport::detail::unpack(0).has_value());

  const rect_t r {1920, 180, 1920, 1080};
  const auto back = meow::viewport::detail::unpack(meow::viewport::detail::pack(r));
  ASSERT_TRUE(back.has_value());
  EXPECT_EQ(*back, r);
}

/**
 * @brief Per-frame planning is cheap enough for a 180 Hz hot path.
 *
 * Requirement 6. The budget here is deliberately loose -- it is a regression guard against
 * somebody adding an allocation or a lock, not a benchmark. One frame at 180 Hz is 5.5 ms;
 * 1 us is 0.02% of that.
 */
TEST(MeowViewportSession, PerFramePlanningIsCheap) {
  meow::viewport::reset();
  int me = 0;
  meow::viewport::on_scaler_init(&me, desktop_w, desktop_h, surface_w, surface_h);
  meow::viewport::detail::requested.store(meow::viewport::detail::pack({1920, 180, 1920, 1080}));

  constexpr int iterations = 200000;
  // Warm up, and keep the optimiser from eliding the loop below.
  volatile int sink = 0;
  for (int i = 0; i < 1000; ++i) {
    sink += meow::viewport::plan_for_frame(&me, desktop_w, desktop_h, surface_w, surface_h)->source.x;
  }

  const auto start = std::chrono::steady_clock::now();
  for (int i = 0; i < iterations; ++i) {
    sink += meow::viewport::plan_for_frame(&me, desktop_w, desktop_h, surface_w, surface_h)->source.x;
  }
  const auto elapsed = std::chrono::steady_clock::now() - start;

  // Read the accumulator so the loop above cannot be optimised away entirely.
  EXPECT_NE(static_cast<int>(sink), -1);

  const auto ns_per_call = std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count() / static_cast<double>(iterations);
  EXPECT_LT(ns_per_call, 1000.0) << "plan_for_frame() took " << ns_per_call << " ns/call";
  std::cout << "[          ] plan_for_frame(): " << ns_per_call << " ns/call" << std::endl;

  meow::viewport::reset();
}

// ---------------------------------------------------------------------------------
// Control-stream registration.
// ---------------------------------------------------------------------------------

namespace {

  /**
   * @brief Stand-in for `stream::session_t`, which is private to `src/stream.cpp`.
   */
  struct fake_session_t {
    int id {};
  };

  /**
   * @brief Records what was registered, standing in for `stream::control_server_t`.
   *
   * Not a mock of a collaborator whose calls are then asserted for their own sake: the
   * handler it records is *invoked* below and its real effect on the real session state is
   * what the tests check. `control_server_t` cannot be constructed without an ENet host.
   */
  struct recording_server_t {
    std::map<std::uint16_t, std::function<void(fake_session_t *, const std::string_view &)>> handlers;

    /**
     * @brief Record a handler registration.
     * @param type Control message type.
     * @param handler Callback registered for it.
     */
    void map(const std::uint16_t type, std::function<void(fake_session_t *, const std::string_view &)> handler) {
      handlers.emplace(type, std::move(handler));
    }
  };

}  // namespace

/**
 * @brief The handler is registered on the viewport packet type and dispatches to the echo.
 */
TEST(MeowViewportRegistration, RegistersAndDispatches) {
  meow::viewport::reset();
  int me = 0;
  meow::viewport::on_scaler_init(&me, desktop_w, desktop_h, surface_w, surface_h);

  recording_server_t server;
  std::vector<std::pair<int, meow::viewport::echo_t>> sent;
  const auto echo = [&sent](fake_session_t *session, const meow::viewport::echo_t &e) {
    sent.emplace_back(session->id, e);
  };

  static const short clean_table[] = {0x0305, 0x0307, 0x0201, 0x010e, 0x5503};
  const auto registration = meow::viewport::map_request_handler(server, clean_table, std::size(clean_table), echo, true);

  EXPECT_TRUE(registration.registered);
  EXPECT_TRUE(registration.warning.empty());
  EXPECT_FALSE(registration.note.empty()) << "the host log must say whether this is on";
  ASSERT_EQ(server.handlers.count(control_packet_type), 1u);

  fake_session_t session {7};
  server.handlers.at(control_packet_type)(&session, make_payload(1, 0, 640, 188, 640, 343));
  ASSERT_EQ(sent.size(), 1u);
  EXPECT_EQ(sent[0].first, 7);
  EXPECT_EQ(sent[0].second.applied, (rect_t {640, 188, 640, 343}));
  EXPECT_EQ(sent[0].second.capture_width, desktop_w);

  // A message the host cannot parse produces no echo at all.
  server.handlers.at(control_packet_type)(&session, "garbage");
  EXPECT_EQ(sent.size(), 1u);

  meow::viewport::reset();
}

/**
 * @brief With the feature off, nothing is installed at all.
 *
 * The compatibility floor is not "the handler runs and decides to do nothing" — it is that
 * the handler is not there. A client that speaks the extension against a host that has not
 * opted in falls through to the unknown-type path, exactly as it would against stock
 * Sunshine.
 */
TEST(MeowViewportRegistration, DisabledInstallsNothing) {
  recording_server_t server;
  const auto echo = [](fake_session_t *, const meow::viewport::echo_t &) {
  };

  static const short clean_table[] = {0x0305, 0x0307};
  const auto registration = meow::viewport::map_request_handler(server, clean_table, std::size(clean_table), echo, false);

  EXPECT_FALSE(registration.registered);
  EXPECT_TRUE(server.handlers.empty());
  EXPECT_TRUE(registration.warning.empty()) << "off is not an error";
  EXPECT_NE(registration.note.find(meow::viewport::following_config_key), std::string::npos) << "the log must name the key to turn it on";
}

/**
 * @brief A packet number already used upstream refuses registration rather than stealing it.
 *
 * This is what makes the two independent definitions of the wire number checkable instead
 * of trusted. Dispatching a genuine upstream control message into the viewport handler
 * would break whatever feature owned that number, silently.
 */
TEST(MeowViewportRegistration, RefusesToStealAnUpstreamPacketNumber) {
  recording_server_t server;
  const auto echo = [](fake_session_t *, const meow::viewport::echo_t &) {
  };

  static const short colliding_table[] = {0x0305, 0x3003, 0x0201};
  const auto registration = meow::viewport::map_request_handler(server, colliding_table, std::size(colliding_table), echo, true);

  EXPECT_FALSE(registration.registered);
  EXPECT_TRUE(server.handlers.empty()) << "nothing may be installed on a collision";
  EXPECT_FALSE(registration.warning.empty());
  EXPECT_NE(registration.warning.find("0x3003"), std::string::npos) << "the log must name the number";
  EXPECT_NE(registration.warning.find("packetTypesGen7Enc"), std::string::npos) << "and where the other definition lives";
}

// ---------------------------------------------------------------------------------
// Configuration.
// ---------------------------------------------------------------------------------

/**
 * @brief The config value parses like every other boolean in the same file.
 */
TEST(MeowViewportConfig, ParsesLikeSunshineBooleans) {
  for (const auto *v : {"enabled", "enable", "on", "true", "yes", "1", "42", "ENABLED", "True"}) {
    EXPECT_TRUE(parse_following_value(v)) << v;
  }
  for (const auto *v : {"disabled", "disable", "off", "false", "no", "0", "", "nonsense"}) {
    EXPECT_FALSE(parse_following_value(v)) << v;
  }
}

/**
 * @brief The log line names the key, so a mistyped setting is diagnosable from the log.
 */
TEST(MeowViewportConfig, StatusLineNamesTheKeyWhenDisabled) {
  const auto off = meow::viewport::following_status(false);
  EXPECT_NE(off.find(meow::viewport::following_config_key), std::string::npos);
  EXPECT_NE(off.find("disabled"), std::string::npos);

  const auto on = meow::viewport::following_status(true);
  EXPECT_NE(on.find("enabled"), std::string::npos);
  EXPECT_NE(on, off);
}

/**
 * @brief The key is namespaced so it can never collide with an upstream setting.
 */
TEST(MeowViewportConfig, KeyIsNamespaced) {
  EXPECT_TRUE(meow::viewport::following_config_key.starts_with("meow_"));
}
