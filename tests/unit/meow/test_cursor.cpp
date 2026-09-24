/**
 * @file tests/unit/meow/test_cursor.cpp
 * @brief Test src/meow/cursor.h and src/meow/cursor_runtime.h.
 *
 * Pixel-exact blends on synthetic frames, metadata parsing over hostile byte buffers, the
 * reference-frame mapping on the real 5360x1440 union desktop, and the 60 Hz coalescer on a
 * synthetic clock. No PipeWire, no GPU, no network (CLAUDE.md §5.5).
 */
// test includes
#include "../../tests_common.h"

// standard includes
#include <array>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

// local includes
#include <src/meow/cursor.h>
#include <src/meow/cursor_runtime.h>

namespace {

  using meow::cursor::bitmap_view_t;
  using meow::cursor::blend;
  using meow::cursor::coalescer_t;
  using meow::cursor::image_t;
  using meow::cursor::import_bitmap;
  using meow::cursor::parse_meta;
  using meow::cursor::pixel_format_t;
  using meow::cursor::restore;
  using meow::cursor::save_under_t;
  using meow::cursor::spa_meta_bitmap_layout_t;
  using meow::cursor::spa_meta_cursor_layout_t;
  using meow::cursor::state_t;

  using namespace std::chrono_literals;

  /**
   * @brief Map the two SPA formats these tests use (values are arbitrary test ids).
   * @param format Test format id.
   * @return The byte order.
   */
  pixel_format_t test_formats(const std::uint32_t format) {
    switch (format) {
      case 1:
        return pixel_format_t::rgba;
      case 2:
        return pixel_format_t::bgra;
      default:
        return pixel_format_t::unknown;
    }
  }

  /**
   * @brief Build a `SPA_META_Cursor` block the way a compositor writes one.
   *
   * @param id Cursor id.
   * @param x Hotspot x.
   * @param y Hotspot y.
   * @param hx Hotspot offset x.
   * @param hy Hotspot offset y.
   * @param bw Bitmap width (0 = no bitmap struct).
   * @param bh Bitmap height.
   * @param format Bitmap format id.
   * @param fill Byte every pixel byte is set to.
   * @return The block.
   */
  std::vector<std::uint8_t> make_meta(std::uint32_t id, std::int32_t x, std::int32_t y, std::int32_t hx, std::int32_t hy, std::uint32_t bw, std::uint32_t bh, std::uint32_t format = 1, std::uint8_t fill = 0x80) {
    spa_meta_cursor_layout_t c {id, 0, x, y, hx, hy, 0};
    std::vector<std::uint8_t> out(sizeof(c));
    if (bw > 0) {
      c.bitmap_offset = sizeof(c);
      spa_meta_bitmap_layout_t b {format, bw, bh, static_cast<std::int32_t>(bw * 4), sizeof(spa_meta_bitmap_layout_t)};
      out.resize(sizeof(c) + sizeof(b) + bw * bh * 4, fill);
      std::memcpy(out.data() + sizeof(c), &b, sizeof(b));
    }
    std::memcpy(out.data(), &c, sizeof(c));
    return out;
  }

  /**
   * @brief A solid-colour BGRx frame.
   */
  struct frame_t {
    int width;  ///< Width in pixels.
    int height;  ///< Height in pixels.
    int stride;  ///< Bytes per row.
    std::vector<std::uint8_t> bytes;  ///< Pixels.

    /**
     * @brief Construct a frame filled with one colour.
     * @param w Width.
     * @param h Height.
     * @param b Blue.
     * @param g Green.
     * @param r Red.
     * @param pad Extra bytes per row.
     */
    frame_t(int w, int h, std::uint8_t b, std::uint8_t g, std::uint8_t r, int pad = 0):
        width {w},
        height {h},
        stride {w * 4 + pad},
        bytes(static_cast<std::size_t>(stride) * h, 0xEE) {
      for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
          auto *p = at(x, y);
          p[0] = b;
          p[1] = g;
          p[2] = r;
          p[3] = 0x7F;
        }
      }
    }

    /**
     * @brief Pixel address.
     * @param x Column.
     * @param y Row.
     * @return Pointer to the pixel.
     */
    std::uint8_t *at(int x, int y) {
      return bytes.data() + static_cast<std::ptrdiff_t>(y) * stride + static_cast<std::ptrdiff_t>(x) * 4;
    }
  };

  /**
   * @brief A 2x2 premultiplied cursor: opaque red, opaque blue, 50% white, transparent.
   * @param hx Hotspot x.
   * @param hy Hotspot y.
   * @return The image.
   */
  image_t two_by_two(int hx = 0, int hy = 0) {
    image_t img;
    img.width = 2;
    img.height = 2;
    img.hotspot_x = hx;
    img.hotspot_y = hy;
    img.bgra = {
      0,
      0,
      255,
      255,  // red
      255,
      0,
      0,
      255,  // blue
      128,
      128,
      128,
      128,  // 50% white, premultiplied
      0,
      0,
      0,
      0  // transparent
    };
    return img;
  }

}  // namespace

// ---------------------------------------------------------------------------------
// Wire format: the client's test vectors, byte for byte.
// ---------------------------------------------------------------------------------

TEST(MeowCursorWire, PositionMatchesTheClientTestVector) {
  // `POS` from tests/meow/test_meow_protocol.c: version 1, visible, seq 0xBEEF, x 5359, y 1439.
  const std::array<std::uint8_t, 8> expected {0x01, 0x01, 0xEF, 0xBE, 0xEF, 0x14, 0x9F, 0x05};
  std::array<std::uint8_t, 9> out {};
  out.fill(0xCC);
  meow::cursor::write_position(5359, 1439, true, 0xBEEF, out.data());
  EXPECT_TRUE(std::equal(expected.begin(), expected.end(), out.begin()));
  EXPECT_EQ(out[8], 0xCC) << "exactly 8 bytes";

  meow::cursor::write_position(5359, 1439, false, 0xBEEF, out.data());
  EXPECT_EQ(out[1], 0x00) << "hidden";
  EXPECT_EQ(meow::cursor::control_packet_type, 0x3004);
}

TEST(MeowCursorWire, SubscribeParsesTheClientEncoding) {
  // meowEncodeCursorSubscribe(true) == {0x01, 0x01}; (false) == {0x01, 0x00}.
  EXPECT_EQ(meow::cursor::parse_subscribe(std::string("\x01\x01", 2)), true);
  EXPECT_EQ(meow::cursor::parse_subscribe(std::string("\x01\x00", 2)), false);
  EXPECT_EQ(meow::cursor::parse_subscribe(std::string("\x01\xFE", 2)), false) << "unknown flag bits are ignored";
  EXPECT_FALSE(meow::cursor::parse_subscribe(std::string()).has_value());
  EXPECT_FALSE(meow::cursor::parse_subscribe(std::string("\x01", 1)).has_value());
  EXPECT_FALSE(meow::cursor::parse_subscribe(std::string("\x01\x01\x00", 3)).has_value()) << "oversize";
  EXPECT_FALSE(meow::cursor::parse_subscribe(std::string("\x02\x01", 2)).has_value()) << "version";
  EXPECT_FALSE(meow::cursor::parse_subscribe(std::string("\x00\x01", 2)).has_value());
}

// ---------------------------------------------------------------------------------
// SPA_META_Cursor parsing: compositor bytes, bounds-checked.
// ---------------------------------------------------------------------------------

TEST(MeowCursorMeta, ParsesPositionHotspotAndBitmap) {
  const auto block = make_meta(1, 100, 200, 3, 4, 8, 6, 1);
  const auto meta = parse_meta(block.data(), block.size(), test_formats);
  ASSERT_TRUE(meta.has_value());
  EXPECT_TRUE(meta->visible);
  EXPECT_EQ(meta->x, 100);
  EXPECT_EQ(meta->y, 200);
  EXPECT_EQ(meta->hotspot_x, 3);
  EXPECT_EQ(meta->hotspot_y, 4);
  ASSERT_TRUE(meta->bitmap.has_value());
  EXPECT_EQ(meta->bitmap->format, pixel_format_t::rgba);
  EXPECT_EQ(meta->bitmap->width, 8);
  EXPECT_EQ(meta->bitmap->height, 6);
  EXPECT_EQ(meta->bitmap->pixels, block.data() + sizeof(spa_meta_cursor_layout_t) + sizeof(spa_meta_bitmap_layout_t));
}

TEST(MeowCursorMeta, PositionOnlyAndHiddenBlocks) {
  const auto moved = make_meta(1, 5, 6, 0, 0, 0, 0);
  const auto meta = parse_meta(moved.data(), moved.size(), test_formats);
  ASSERT_TRUE(meta.has_value());
  EXPECT_TRUE(meta->visible);
  EXPECT_FALSE(meta->bitmap.has_value()) << "no bitmap: keep the current image";

  const auto hidden = make_meta(0, 5, 6, 0, 0, 8, 8);
  const auto h = parse_meta(hidden.data(), hidden.size(), test_formats);
  ASSERT_TRUE(h.has_value());
  EXPECT_FALSE(h->visible);
  EXPECT_FALSE(h->bitmap.has_value());

  // An empty bitmap (no image data) hides the shape without hiding the position.
  auto empty = make_meta(1, 5, 6, 0, 0, 4, 4);
  spa_meta_bitmap_layout_t b;
  std::memcpy(&b, empty.data() + sizeof(spa_meta_cursor_layout_t), sizeof(b));
  b.offset = 0;
  std::memcpy(empty.data() + sizeof(spa_meta_cursor_layout_t), &b, sizeof(b));
  const auto e = parse_meta(empty.data(), empty.size(), test_formats);
  ASSERT_TRUE(e.has_value());
  ASSERT_TRUE(e->bitmap.has_value());
  EXPECT_EQ(e->bitmap->width, 0);

  // Format 0 means "no new image information", not "hide".
  b.offset = sizeof(b);
  b.format = 0;
  std::memcpy(empty.data() + sizeof(spa_meta_cursor_layout_t), &b, sizeof(b));
  const auto f = parse_meta(empty.data(), empty.size(), test_formats);
  ASSERT_TRUE(f.has_value());
  EXPECT_FALSE(f->bitmap.has_value());
}

TEST(MeowCursorMeta, NeverReadsOutsideTheBlock) {
  const auto good = make_meta(1, 10, 10, 1, 1, 16, 16);

  // Every truncation: either rejected, or the bitmap dropped - never a view past the end.
  for (std::size_t len = 0; len < good.size(); ++len) {
    std::vector<std::uint8_t> exact(good.begin(), good.begin() + static_cast<std::ptrdiff_t>(len));
    const auto meta = parse_meta(exact.empty() ? nullptr : exact.data(), exact.size(), test_formats);
    if (len < sizeof(spa_meta_cursor_layout_t)) {
      EXPECT_FALSE(meta.has_value()) << len;
      continue;
    }
    ASSERT_TRUE(meta.has_value()) << len;
    EXPECT_FALSE(meta->bitmap.has_value()) << "a bitmap that does not fit is dropped, len " << len;
  }

  // Hostile offsets and sizes.
  const auto patch_bitmap = [&good](auto &&edit) {
    auto block = good;
    spa_meta_bitmap_layout_t b;
    std::memcpy(&b, block.data() + sizeof(spa_meta_cursor_layout_t), sizeof(b));
    edit(b);
    std::memcpy(block.data() + sizeof(spa_meta_cursor_layout_t), &b, sizeof(b));
    return block;
  };
  for (const auto &block : {
         patch_bitmap([](auto &b) {
           b.width = 0xFFFFFFFFu;
         }),
         patch_bitmap([](auto &b) {
           b.height = 4096;
         }),
         patch_bitmap([](auto &b) {
           b.stride = -64;
         }),
         patch_bitmap([](auto &b) {
           b.stride = 4;
         }),
         patch_bitmap([](auto &b) {
           b.offset = 0xFFFFFFF0u;
         }),
         patch_bitmap([](auto &b) {
           b.offset = 4;
         }),
         patch_bitmap([](auto &b) {
           b.format = 99;
         }),
       }) {
    const auto meta = parse_meta(block.data(), block.size(), test_formats);
    ASSERT_TRUE(meta.has_value());
    EXPECT_FALSE(meta->bitmap.has_value());
    EXPECT_EQ(meta->x, 10) << "the position survives a bad bitmap";
  }

  auto bad_offset = good;
  spa_meta_cursor_layout_t c;
  std::memcpy(&c, bad_offset.data(), sizeof(c));
  for (const std::uint32_t off : {4u, 0xFFFFFFFFu, static_cast<std::uint32_t>(good.size())}) {
    c.bitmap_offset = off;
    std::memcpy(bad_offset.data(), &c, sizeof(c));
    const auto meta = parse_meta(bad_offset.data(), bad_offset.size(), test_formats);
    ASSERT_TRUE(meta.has_value());
    EXPECT_FALSE(meta->bitmap.has_value()) << off;
  }
}

// ---------------------------------------------------------------------------------
// Bitmap import and the blend: pixel exact.
// ---------------------------------------------------------------------------------

TEST(MeowCursorBlend, ImportConvertsEveryByteOrderToPremultipliedBgra) {
  const std::array<std::uint8_t, 4> rgba {10, 20, 30, 255};
  bitmap_view_t view {pixel_format_t::rgba, 1, 1, 4, rgba.data()};
  image_t img;
  import_bitmap(view, 0, 0, true, img);
  EXPECT_EQ(img.bgra, (std::vector<std::uint8_t> {30, 20, 10, 255}));

  const std::array<std::uint8_t, 4> argb {255, 30, 20, 10};
  view = {pixel_format_t::argb, 1, 1, 4, argb.data()};
  import_bitmap(view, 0, 0, true, img);
  EXPECT_EQ(img.bgra, (std::vector<std::uint8_t> {10, 20, 30, 255}));

  // Straight alpha is premultiplied on import.
  const std::array<std::uint8_t, 4> straight {200, 100, 50, 128};
  view = {pixel_format_t::rgba, 1, 1, 4, straight.data()};
  import_bitmap(view, 0, 0, false, img);
  EXPECT_EQ(img.bgra, (std::vector<std::uint8_t> {25, 50, 100, 128}));

  // A malformed premultiplied pixel (colour above alpha) is clamped, so the blend cannot wrap.
  const std::array<std::uint8_t, 4> malformed {255, 255, 255, 10};
  view = {pixel_format_t::rgba, 1, 1, 4, malformed.data()};
  import_bitmap(view, 0, 0, true, img);
  EXPECT_EQ(img.bgra, (std::vector<std::uint8_t> {10, 10, 10, 10}));
}

TEST(MeowCursorBlend, DrawsPixelExactAtTheHotspot) {
  frame_t f(8, 8, 0, 0, 0);  // black BGRx
  save_under_t save;
  // Hotspot (1,1) at frame (4,4): the 2x2 image's top-left lands at (3,3).
  ASSERT_TRUE(blend(f.bytes.data(), f.width, f.height, f.stride, pixel_format_t::bgrx, two_by_two(1, 1), 4, 4, &save));

  const auto px = [&f](int x, int y) {
    const auto *p = f.at(x, y);
    return std::array<int, 4> {p[0], p[1], p[2], p[3]};
  };
  EXPECT_EQ(px(3, 3), (std::array<int, 4> {0, 0, 255, 0x7F})) << "opaque red; x byte untouched";
  EXPECT_EQ(px(4, 3), (std::array<int, 4> {255, 0, 0, 0x7F})) << "opaque blue";
  EXPECT_EQ(px(3, 4), (std::array<int, 4> {128, 128, 128, 0x7F})) << "50% white over black";
  EXPECT_EQ(px(4, 4), (std::array<int, 4> {0, 0, 0, 0x7F})) << "transparent leaves the frame";
  EXPECT_EQ(px(2, 3), (std::array<int, 4> {0, 0, 0, 0x7F})) << "nothing outside the image";
  EXPECT_EQ(save.x, 3);
  EXPECT_EQ(save.y, 3);
  EXPECT_EQ(save.width, 2);
  EXPECT_EQ(save.height, 2);
}

TEST(MeowCursorBlend, HalfAlphaOverAColourRoundsToNearest) {
  frame_t f(1, 1, 200, 100, 50);
  image_t img;
  img.width = 1;
  img.height = 1;
  img.bgra = {64, 64, 64, 128};  // 50% grey-ish, premultiplied
  ASSERT_TRUE(blend(f.bytes.data(), 1, 1, f.stride, pixel_format_t::bgra, img, 0, 0, nullptr));
  // out = s + d * 127 / 255, rounded: 64 + 99.6 -> 64 + 100; 64 + 49.8 -> 114; 64 + 24.9 -> 89
  EXPECT_EQ(f.at(0, 0)[0], 164);
  EXPECT_EQ(f.at(0, 0)[1], 114);
  EXPECT_EQ(f.at(0, 0)[2], 89);
  EXPECT_EQ(f.at(0, 0)[3], 128 + (0x7F * 127 + 127) / 255) << "alpha of an A format is blended too";
}

TEST(MeowCursorBlend, RgbFramesGetTheirChannelsSwapped) {
  frame_t f(2, 1, 0, 0, 0);
  ASSERT_TRUE(blend(f.bytes.data(), 2, 1, f.stride, pixel_format_t::rgbx, two_by_two(), 0, 0, nullptr));
  EXPECT_EQ(f.at(0, 0)[0], 255) << "red lands in byte 0 of an RGB frame";
  EXPECT_EQ(f.at(0, 0)[2], 0);
  EXPECT_EQ(f.at(1, 0)[2], 255) << "blue in byte 2";
}

TEST(MeowCursorBlend, ClipsAtEveryEdgeAndCorner) {
  for (const auto &[x, y, drawn] : std::vector<std::tuple<int, int, int>> {
         {-1, -1, 1},  // only the bottom-right pixel of the image is inside
         {7, 7, 1},  // only the top-left
         {-1, 4, 2},
         {7, 4, 2},
         {4, -1, 2},
         {4, 7, 2},
         {-2, 4, 0},
         {8, 4, 0},
         {-100000, 5, 0},
         {2147483647, 2147483647, 0},
         {-2147483647, -2147483647, 0},
       }) {
    frame_t f(8, 8, 0, 0, 0, 12);  // padded rows: the blend must use the stride
    image_t img;
    img.width = 2;
    img.height = 2;
    img.bgra.assign(16, 255);
    save_under_t save;
    const bool any = blend(f.bytes.data(), f.width, f.height, f.stride, pixel_format_t::bgrx, img, x, y, &save);
    EXPECT_EQ(any, drawn > 0) << x << "," << y;
    EXPECT_EQ(save.width * save.height, drawn) << x << "," << y;
    int white = 0;
    for (int yy = 0; yy < f.height; ++yy) {
      for (int xx = 0; xx < f.width; ++xx) {
        white += f.at(xx, yy)[0] == 255;
      }
      // The row padding is never touched.
      for (int pad = f.width * 4; pad < f.stride; ++pad) {
        EXPECT_EQ(f.bytes[static_cast<std::size_t>(yy) * f.stride + pad], 0xEE);
      }
    }
    EXPECT_EQ(white, drawn) << x << "," << y;
  }
}

TEST(MeowCursorBlend, RestoreUndoesTheBlendExactly) {
  frame_t f(16, 16, 11, 22, 33);
  for (int i = 0; i < 16 * 16; ++i) {
    f.bytes[static_cast<std::size_t>(i) * 4] = static_cast<std::uint8_t>(i);  // a non-uniform frame
  }
  const auto pristine = f.bytes;
  save_under_t save;
  image_t img = two_by_two(1, 1);

  // A cursor moving across an idle frame: restore then re-draw, many times, in the same frame.
  for (int step = 0; step < 20; ++step) {
    restore(f.bytes.data(), f.stride, save);
    ASSERT_TRUE(blend(f.bytes.data(), f.width, f.height, f.stride, pixel_format_t::bgrx, img, step % 15, (step * 7) % 15, &save));
  }
  restore(f.bytes.data(), f.stride, save);
  EXPECT_EQ(f.bytes, pristine) << "no trail: every blended pixel came back";
  restore(f.bytes.data(), f.stride, save);  // Nothing saved: a no-op.
  EXPECT_EQ(f.bytes, pristine);
}

TEST(MeowCursorBlend, RefusesFormatsItCannotWrite) {
  frame_t f(4, 4, 0, 0, 0);
  const auto before = f.bytes;
  for (const auto format : {pixel_format_t::unknown, pixel_format_t::argb, pixel_format_t::abgr}) {
    EXPECT_FALSE(blend(f.bytes.data(), 4, 4, f.stride, format, two_by_two(), 1, 1, nullptr));
  }
  EXPECT_FALSE(blend(f.bytes.data(), 4, 4, 8, pixel_format_t::bgrx, two_by_two(), 1, 1, nullptr)) << "stride too small";
  EXPECT_FALSE(blend(nullptr, 4, 4, f.stride, pixel_format_t::bgrx, two_by_two(), 1, 1, nullptr));
  EXPECT_FALSE(blend(f.bytes.data(), 4, 4, f.stride, pixel_format_t::bgrx, image_t {}, 1, 1, nullptr));
  EXPECT_EQ(f.bytes, before);
}

// ---------------------------------------------------------------------------------
// Reference-frame mapping on the real desktop.
// ---------------------------------------------------------------------------------

TEST(MeowCursorMapping, UnionDesktopMapsIntoTheLetterboxedContentArea) {
  // 5360x1440 (1920x1200 eDP + 3440x1440 HDMI) in a 1280x720 stream: the desktop occupies
  // 1280x343 at y = 188, with padding above and below - the same space as 0x3003.
  const auto ref = meow::viewport::reference_frame(5360, 1440, 1280, 720);
  ASSERT_EQ(ref.content_y, 188);
  ASSERT_EQ(ref.content_height, 343);

  EXPECT_EQ(meow::cursor::to_reference(0, 0, 5360, 1440, 1280, 720), (std::pair<int, int> {0, 188}));
  EXPECT_EQ(meow::cursor::to_reference(5359, 1439, 5360, 1440, 1280, 720), (std::pair<int, int> {1279, 530}));
  // The monitor seam at x = 1920 lands at 1920 * 1280 / 5360 = 458.5 -> 459.
  EXPECT_EQ(meow::cursor::to_reference(1920, 0, 5360, 1440, 1280, 720)->first, 459);
  // Centre of the HDMI monitor.
  const auto centre = meow::cursor::to_reference(1920 + 1720, 720, 5360, 1440, 1280, 720);
  EXPECT_EQ(centre, (std::pair<int, int> {869, 360}));
  // Out-of-frame positions are clamped, never sent into the padding.
  EXPECT_EQ(meow::cursor::to_reference(-50, -50, 5360, 1440, 1280, 720), (std::pair<int, int> {0, 188}));
  EXPECT_EQ(meow::cursor::to_reference(99999, 99999, 5360, 1440, 1280, 720), (std::pair<int, int> {1279, 530}));
  EXPECT_FALSE(meow::cursor::to_reference(1, 1, 0, 0, 1280, 720).has_value());
}

TEST(MeowCursorMapping, AgreesWithTheViewportTransformForEveryPixelColumn) {
  // The point mapping must be the rectangle mapping's left edge: the client pans with one
  // and crops with the other, so they may not disagree by even a pixel.
  for (int x = 0; x < 5360; x += 7) {
    const auto point = meow::cursor::to_reference(x, 700, 5360, 1440, 1280, 720);
    const auto rect = meow::viewport::to_reference({x, 700, 64, 64}, 5360, 1440, 1280, 720);
    ASSERT_TRUE(point.has_value());
    EXPECT_EQ(point->first, rect.x) << x;
    EXPECT_EQ(point->second, rect.y) << x;
  }
}

// ---------------------------------------------------------------------------------
// Coalescing.
// ---------------------------------------------------------------------------------

TEST(MeowCursorCoalescer, SendsImmediatelyOnSubscribeAndVisibilityAndAtMost60Hz) {
  coalescer_t c;
  auto now = std::chrono::steady_clock::time_point {} + 1h;
  state_t s {10, 10, true, 1};
  c.force();
  ASSERT_TRUE(c.due(s, now)) << "once immediately on subscribe";
  c.sent(s, now);
  EXPECT_FALSE(c.due(s, now + 1s)) << "nothing changed, nothing sent";

  // Movement inside 1/60 s is held, then the latest state goes out.
  int sends = 0;
  for (int i = 0; i < 1000; ++i) {
    now += 1ms;
    s = {10 + i, 10, true, s.generation + 1};
    if (c.due(s, now)) {
      ++sends;
      c.sent(s, now);
    }
  }
  EXPECT_LE(sends, 61) << "<= 60 Hz over one second of 1 kHz motion";
  EXPECT_GE(sends, 55);
  EXPECT_TRUE(c.due({s.x + 1, s.y, true, s.generation + 1}, now + coalescer_t::min_interval)) << "the last change is not lost";

  // Visibility changes bypass the interval.
  c.sent(s, now);
  EXPECT_TRUE(c.due({s.x, s.y, false, s.generation + 1}, now + 1ms));
  c.sent({s.x, s.y, false, s.generation + 1}, now + 1ms);
  EXPECT_FALSE(c.due({s.x + 5, s.y, false, s.generation + 2}, now + 1s)) << "motion while hidden is useless to the client";
  EXPECT_TRUE(c.due({s.x + 5, s.y, true, s.generation + 3}, now + 1s + 1ms));
}

// ---------------------------------------------------------------------------------
// The cross-thread word.
// ---------------------------------------------------------------------------------

TEST(MeowCursorRuntime, PublishesAtomicallyAndCountsChanges) {
  meow::cursor::deactivate();
  EXPECT_FALSE(meow::cursor::current().has_value()) << "no metadata stream, no positions";

  meow::cursor::publish(5359, 1439, true);
  const auto a = meow::cursor::current();
  ASSERT_TRUE(a.has_value());
  EXPECT_EQ(a->x, 5359);
  EXPECT_EQ(a->y, 1439);
  EXPECT_TRUE(a->visible);

  meow::cursor::publish(5359, 1439, true);
  EXPECT_EQ(meow::cursor::current()->generation, a->generation) << "republishing the same state is not a change";

  meow::cursor::publish(-5, 70000, false);
  const auto b = meow::cursor::current();
  EXPECT_EQ(b->x, 0);
  EXPECT_EQ(b->y, 0xFFFF);
  EXPECT_FALSE(b->visible);
  EXPECT_NE(b->generation, a->generation);

  meow::cursor::deactivate();
  EXPECT_FALSE(meow::cursor::current().has_value());
}

TEST(MeowCursorRuntime, MetadataModeOnlyForMemoryPathsWhileEnabled) {
  const bool saved = config::video.cursor_reporting;
  const bool refused = meow::cursor::detail::metadata_refused.load();
  meow::cursor::detail::metadata_refused = false;

  config::video.cursor_reporting = true;
  EXPECT_TRUE(meow::cursor::metadata_mode_wanted(true));
  EXPECT_FALSE(meow::cursor::metadata_mode_wanted(false)) << "a DMA-BUF path keeps the embedded cursor";
  config::video.cursor_reporting = false;
  EXPECT_FALSE(meow::cursor::metadata_mode_wanted(true)) << "the off switch";
  config::video.cursor_reporting = true;
  meow::cursor::refuse_metadata();
  EXPECT_FALSE(meow::cursor::metadata_mode_wanted(true)) << "sticky once blending proved impossible";

  meow::cursor::detail::metadata_refused = refused;
  config::video.cursor_reporting = saved;
}
