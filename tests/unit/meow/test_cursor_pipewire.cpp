/**
 * @file tests/unit/meow/test_cursor_pipewire.cpp
 * @brief Test src/meow/cursor_pipewire.h: the metadata-mode memory path, driven with real
 *        `spa_buffer`s built in memory and a stand-in for `pipewire::stream_data_t`.
 *
 * No PipeWire daemon is involved: the functions under test only read the buffer structures and
 * hand the buffer back through an injected callback, which records it.
 */
// test includes
#include "../../tests_common.h"

// standard includes
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <optional>
#include <vector>

// local includes
#include <src/meow/cursor_pipewire.h>

namespace {

  constexpr int frame_w = 16;  ///< Test frame width.
  constexpr int frame_h = 16;  ///< Test frame height.

  /**
   * @brief The members of `pipewire::stream_data_t` the metadata-mode path touches.
   */
  struct fake_stream_data_t {
    std::mutex frame_mutex;  ///< Frame mutex.
    std::condition_variable frame_cv;  ///< Frame condition variable.
    std::vector<std::uint8_t> buffer_a;  ///< Staging buffer A.
    std::vector<std::uint8_t> buffer_b;  ///< Staging buffer B.
    std::vector<std::uint8_t> *front_buffer = &buffer_a;  ///< Readable staging buffer.
    std::vector<std::uint8_t> *back_buffer = &buffer_b;  ///< Writable staging buffer.
    std::size_t local_stride = 0;  ///< Row pitch of the front buffer.
    bool frame_ready = false;  ///< Whether the capture thread should wake.
    meow::cursor::pipewire::stream_t meow_cursor;  ///< Cursor state under test.
  };

  /**
   * @brief The members of `pipewire::img_descriptor_t` the metadata-mode path fills.
   */
  struct fake_img_t {
    int width = frame_w;  ///< Width.
    int height = frame_h;  ///< Height.
    std::uint8_t *data = nullptr;  ///< Pixels.
    bool data_owned = true;  ///< Ownership flag.
    int row_pitch = 0;  ///< Row pitch.
    int pixel_pitch = 0;  ///< Pixel pitch.
    std::optional<std::chrono::steady_clock::time_point> frame_timestamp;  ///< Timestamp.
    std::optional<std::uint64_t> pts;  ///< PipeWire pts.
    std::optional<std::uint64_t> seq;  ///< PipeWire seq.
    std::optional<bool> pw_damage;  ///< Damage flag.
    std::optional<std::uint32_t> pw_flags;  ///< Chunk flags.
  };

  /**
   * @brief One PipeWire buffer, assembled in memory the way KWin fills it.
   */
  struct built_buffer_t {
    std::vector<std::uint8_t> pixels;  ///< Frame data.
    std::vector<std::uint8_t> cursor_block;  ///< SPA_META_Cursor block.
    spa_meta_header header {};  ///< SPA_META_Header.
    spa_chunk chunk {};  ///< The data chunk.
    spa_data data {};  ///< The data plane.
    spa_meta metas[2] {};  ///< Header + cursor metadata.
    spa_buffer buffer {};  ///< The buffer.
    pw_buffer pw {};  ///< The PipeWire wrapper.

    /**
     * @brief Build a buffer.
     * @param colour Fill byte for a data frame; ignored when `cursor_only`.
     * @param cursor_only A KWin cursor-only update (SPA_CHUNK_FLAG_CORRUPTED).
     * @param cursor_x Cursor hotspot x, or -1 for no cursor metadata at all.
     * @param cursor_y Cursor hotspot y.
     * @param with_bitmap Whether to attach a 2x2 opaque white bitmap.
     * @param pts Header pts.
     */
    built_buffer_t(std::uint8_t colour, bool cursor_only, int cursor_x, int cursor_y, bool with_bitmap, std::uint64_t pts) {
      pixels.assign(static_cast<std::size_t>(frame_w) * frame_h * 4, colour);
      chunk.size = cursor_only ? 0 : static_cast<std::uint32_t>(pixels.size());
      chunk.stride = frame_w * 4;
      chunk.flags = cursor_only ? SPA_CHUNK_FLAG_CORRUPTED : 0;
      data.type = SPA_DATA_MemPtr;
      data.data = pixels.data();
      data.maxsize = static_cast<std::uint32_t>(pixels.size());
      data.chunk = &chunk;
      header.pts = static_cast<std::int64_t>(pts);
      header.seq = pts;
      metas[0] = {SPA_META_Header, sizeof(header), &header};
      std::uint32_t n_metas = 1;
      if (cursor_x >= 0) {
        spa_meta_cursor c {};
        c.id = 1;
        c.position = {cursor_x, cursor_y};
        c.hotspot = {0, 0};
        cursor_block.resize(sizeof(spa_meta_cursor));
        if (with_bitmap) {
          c.bitmap_offset = sizeof(spa_meta_cursor);
          spa_meta_bitmap b {};
          b.format = SPA_VIDEO_FORMAT_RGBA;
          b.size = {2, 2};
          b.stride = 8;
          b.offset = sizeof(spa_meta_bitmap);
          cursor_block.resize(sizeof(spa_meta_cursor) + sizeof(spa_meta_bitmap) + 16, 0xFF);
          std::memcpy(cursor_block.data() + sizeof(spa_meta_cursor), &b, sizeof(b));
        }
        std::memcpy(cursor_block.data(), &c, sizeof(c));
        metas[1] = {SPA_META_Cursor, static_cast<std::uint32_t>(cursor_block.size()), cursor_block.data()};
        n_metas = 2;
      }
      buffer.n_metas = n_metas;
      buffer.metas = metas;
      buffer.n_datas = 1;
      buffer.datas = &data;
      pw.buffer = &buffer;
    }
  };

  /**
   * @brief Fresh stream state in metadata mode with a BGRx frame format.
   */
  struct MeowCursorPipewireTest: testing::Test {
    void SetUp() override {
      d.meow_cursor.enabled = true;
      d.meow_cursor.frame_format = meow::cursor::pixel_format_t::bgrx;
    }

    void TearDown() override {
      meow::cursor::deactivate();
    }

    /**
     * @brief Run `process_memory()` and count requeues.
     * @param b The buffer.
     */
    void process(built_buffer_t &b) {
      meow::cursor::pipewire::process_memory(&d, &b.pw, [this](pw_buffer *) {
        ++requeued;
      });
    }

    /**
     * @brief The blue byte of a pixel of the image handed out.
     * @param img The image.
     * @param x Column.
     * @param y Row.
     * @return The byte.
     */
    static int blue(const fake_img_t &img, int x, int y) {
      return img.data[y * img.row_pitch + x * 4];
    }

    fake_stream_data_t d;  ///< Stream data under test.
    int requeued = 0;  ///< Buffers handed back to PipeWire.
  };

}  // namespace

TEST_F(MeowCursorPipewireTest, ADataFrameIsCopiedAndHandedOutWithTheCursorDrawnIn) {
  built_buffer_t frame(0x10, false, 4, 5, true, 1000);
  process(frame);
  EXPECT_EQ(requeued, 1) << "the buffer goes straight back to PipeWire";
  EXPECT_TRUE(d.frame_ready);
  ASSERT_EQ(d.front_buffer, &d.buffer_b) << "copied into the back buffer, then swapped";

  fake_img_t img;
  meow::cursor::pipewire::fill_memory_img(d, img);
  ASSERT_EQ(img.data, d.front_buffer->data());
  EXPECT_FALSE(img.data_owned);
  EXPECT_EQ(img.row_pitch, frame_w * 4);
  EXPECT_EQ(img.pts, 1000u) << "metadata recorded at copy time, not read from a requeued buffer";
  EXPECT_EQ(img.pw_flags, 0u);
  EXPECT_EQ(blue(img, 4, 5), 0xFF) << "cursor drawn at its hotspot";
  EXPECT_EQ(blue(img, 5, 6), 0xFF);
  EXPECT_EQ(blue(img, 3, 5), 0x10) << "and nowhere else";
  EXPECT_EQ(frame.pixels[(5 * frame_w + 4) * 4], 0x10) << "the compositor's buffer is never written";

  const auto published = meow::cursor::current();
  ASSERT_TRUE(published.has_value());
  EXPECT_EQ(published->x, 4);
  EXPECT_EQ(published->y, 5);
  EXPECT_TRUE(published->visible);
}

TEST_F(MeowCursorPipewireTest, ACursorOnlyUpdateRedrawsTheSameFrameWithoutATrail) {
  built_buffer_t frame(0x10, false, 4, 5, true, 1000);
  process(frame);
  fake_img_t first;
  meow::cursor::pipewire::fill_memory_img(d, first);
  d.frame_ready = false;
  auto *const front = d.front_buffer;

  built_buffer_t moved(0xEE, true, 9, 10, false, 1001);
  process(moved);
  EXPECT_EQ(requeued, 2);
  EXPECT_TRUE(d.frame_ready) << "a moved cursor wakes the capture thread";
  EXPECT_EQ(d.front_buffer, front) << "no copy and no swap for a cursor-only buffer";

  fake_img_t second;
  meow::cursor::pipewire::fill_memory_img(d, second);
  EXPECT_EQ(blue(second, 4, 5), 0x10) << "the old cursor was restored";
  EXPECT_EQ(blue(second, 5, 6), 0x10);
  EXPECT_EQ(blue(second, 9, 10), 0xFF) << "and the same image drawn at the new position";
  EXPECT_FALSE(second.pts.has_value()) << "no pts, so the duplicate filter cannot drop the refresh";
  EXPECT_EQ(second.pw_flags, 0u) << "the CORRUPTED flag of the cursor-only buffer is not passed on";
  for (const auto byte : *d.front_buffer) {
    EXPECT_NE(byte, 0xEE) << "the cursor-only buffer's contents are never copied";
  }
}

TEST_F(MeowCursorPipewireTest, ACursorOnlyUpdateThatChangesNothingDoesNotWake) {
  built_buffer_t frame(0x10, false, 4, 5, true, 1000);
  process(frame);
  d.frame_ready = false;
  built_buffer_t same(0xEE, true, 4, 5, false, 1001);
  process(same);
  EXPECT_FALSE(d.frame_ready);
  EXPECT_EQ(requeued, 2) << "but the buffer still goes back";
}

TEST_F(MeowCursorPipewireTest, ANewFrameStartsCleanAndCarriesItsOwnMetadata) {
  built_buffer_t frame(0x10, false, 4, 5, true, 1000);
  process(frame);
  fake_img_t img;
  meow::cursor::pipewire::fill_memory_img(d, img);
  built_buffer_t moved(0xEE, true, 9, 10, false, 1001);
  process(moved);
  meow::cursor::pipewire::fill_memory_img(d, img);

  built_buffer_t next(0x20, false, 9, 10, false, 1002);
  process(next);
  EXPECT_EQ(d.front_buffer, &d.buffer_a) << "swapped";
  meow::cursor::pipewire::fill_memory_img(d, img);
  EXPECT_EQ(img.pts, 1002u);
  EXPECT_EQ(blue(img, 9, 10), 0xFF) << "cursor drawn fresh into the new frame";
  EXPECT_EQ(blue(img, 4, 5), 0x20) << "nothing restored into it from the previous frame";
}

TEST_F(MeowCursorPipewireTest, AHiddenCursorIsNeitherDrawnNorReportedVisible) {
  built_buffer_t frame(0x10, false, 4, 5, true, 1000);
  process(frame);
  // KWin reports "no cursor over this stream" with id 0.
  built_buffer_t hidden(0xEE, true, 4, 5, false, 1001);
  spa_meta_cursor c {};
  std::memcpy(&c, hidden.cursor_block.data(), sizeof(c));
  c.id = 0;
  std::memcpy(hidden.cursor_block.data(), &c, sizeof(c));
  process(hidden);
  fake_img_t img;
  meow::cursor::pipewire::fill_memory_img(d, img);
  EXPECT_EQ(blue(img, 4, 5), 0x10);
  const auto published = meow::cursor::current();
  ASSERT_TRUE(published.has_value());
  EXPECT_FALSE(published->visible);
}

TEST_F(MeowCursorPipewireTest, NoFrameYetHandsOutNothing) {
  built_buffer_t moved(0xEE, true, 9, 10, true, 1);
  process(moved);
  EXPECT_FALSE(d.frame_ready) << "a cursor with no frame to draw it into is not a frame";
  fake_img_t img;
  meow::cursor::pipewire::fill_memory_img(d, img);
  EXPECT_EQ(img.data, nullptr);
}

TEST_F(MeowCursorPipewireTest, ATrustedPipewirePtsBecomesTheCaptureTimestamp) {
  built_buffer_t frame(0x10, false, 4, 5, true, 1000);
  process(frame);
  fake_img_t img;
  meow::cursor::pipewire::fill_memory_img(d, img, true);
  ASSERT_TRUE(img.frame_timestamp.has_value());
  EXPECT_EQ(*img.frame_timestamp, std::chrono::steady_clock::time_point(std::chrono::nanoseconds(1000)))
    << "upstream's pts passthrough applies to the metadata-mode path too";
  EXPECT_EQ(img.pts, 1000u);
}

TEST_F(MeowCursorPipewireTest, AnUntrustedPipewirePtsIsNotTheCaptureTimestamp) {
  built_buffer_t frame(0x10, false, 4, 5, true, 1000);
  process(frame);
  const auto before = std::chrono::steady_clock::now();
  fake_img_t img;
  meow::cursor::pipewire::fill_memory_img(d, img, false);
  ASSERT_TRUE(img.frame_timestamp.has_value());
  EXPECT_GE(*img.frame_timestamp, before) << "sampled at capture time, as upstream does off its whitelist";
  EXPECT_EQ(img.pts, 1000u) << "the pts itself is still reported for the duplicate filter";
}

TEST_F(MeowCursorPipewireTest, ACursorRefreshIsStampedNowNotWithTheFramesPts) {
  built_buffer_t frame(0x10, false, 4, 5, true, 1000);
  process(frame);
  fake_img_t first;
  meow::cursor::pipewire::fill_memory_img(d, first, true);

  built_buffer_t moved(0xEE, true, 9, 10, false, 1001);
  process(moved);
  const auto before = std::chrono::steady_clock::now();
  fake_img_t second;
  meow::cursor::pipewire::fill_memory_img(d, second, true);
  EXPECT_FALSE(second.pts.has_value());
  ASSERT_TRUE(second.frame_timestamp.has_value());
  EXPECT_GE(*second.frame_timestamp, before) << "a refresh is a new image now; re-using the frame's pts would duplicate a timestamp";
  EXPECT_NE(*second.frame_timestamp, *first.frame_timestamp);
}

TEST_F(MeowCursorPipewireTest, AZeroPtsIsNotAUsableTimestamp) {
  built_buffer_t frame(0x10, false, 4, 5, true, 0);
  process(frame);
  const auto before = std::chrono::steady_clock::now();
  fake_img_t img;
  img.pts = 77;  // Stale value from an earlier use of this image; must not survive.
  meow::cursor::pipewire::fill_memory_img(d, img, true);
  EXPECT_FALSE(img.pts.has_value()) << "upstream's fill_img_metadata() drops a pts of 0 the same way";
  ASSERT_TRUE(img.frame_timestamp.has_value());
  EXPECT_GE(*img.frame_timestamp, before);
  EXPECT_EQ(img.seq, 0u) << "seq is kept as reported";
}

TEST_F(MeowCursorPipewireTest, AFrameAfterARefreshIsNeverStampedEarlierThanTheRefresh) {
  built_buffer_t frame(0x10, false, 4, 5, true, 1000);
  process(frame);
  fake_img_t img;
  meow::cursor::pipewire::fill_memory_img(d, img, true);

  built_buffer_t moved(0xEE, true, 9, 10, false, 1001);
  process(moved);
  fake_img_t refreshed;
  meow::cursor::pipewire::fill_memory_img(d, refreshed, true);

  // Composed before the refresh was handed out: its compositor pts is earlier than now().
  built_buffer_t next(0x20, false, 9, 10, false, 1002);
  process(next);
  fake_img_t after;
  meow::cursor::pipewire::fill_memory_img(d, after, true);
  EXPECT_EQ(after.pts, 1002u) << "the frame still reports its own pts";
  ASSERT_TRUE(after.frame_timestamp.has_value());
  EXPECT_EQ(*after.frame_timestamp, *refreshed.frame_timestamp) << "held at the refresh's stamp instead of going backwards";
}

TEST_F(MeowCursorPipewireTest, AFrameArrivingBeforeAPendingRefreshIsHandedOutKeepsItsPts) {
  built_buffer_t frame(0x10, false, 4, 5, true, 1000);
  process(frame);
  fake_img_t img;
  meow::cursor::pipewire::fill_memory_img(d, img, true);

  built_buffer_t moved(0xEE, true, 9, 10, false, 1001);
  process(moved);
  built_buffer_t next(0x20, false, 9, 10, false, 5000);
  process(next);  // before the capture thread picked the refresh up

  meow::cursor::pipewire::fill_memory_img(d, img, true);
  EXPECT_EQ(img.pts, 5000u) << "the new frame supersedes the refresh";
  ASSERT_TRUE(img.frame_timestamp.has_value());
  EXPECT_EQ(*img.frame_timestamp, std::chrono::steady_clock::time_point(std::chrono::nanoseconds(5000)));
  EXPECT_EQ(blue(img, 9, 10), 0xFF) << "with the cursor at its latest position";
}

TEST(MeowCursorPipewireTimestamp, MirrorsUpstreamsChoice) {
  const auto now = std::chrono::steady_clock::time_point(std::chrono::seconds(5));
  const auto pts = std::chrono::steady_clock::time_point(std::chrono::nanoseconds(4'000'000'000));
  EXPECT_EQ(meow::cursor::pipewire::frame_timestamp(4'000'000'000u, true, now), pts);
  EXPECT_EQ(meow::cursor::pipewire::frame_timestamp(4'000'000'000u, false, now), now);
  EXPECT_EQ(meow::cursor::pipewire::frame_timestamp(std::nullopt, true, now), now);
  EXPECT_EQ(meow::cursor::pipewire::frame_timestamp(std::nullopt, false, now), now);
}

TEST(MeowCursorPipewireFormat, RefusesWhatTheBlendCannotWriteAndStopsAsking) {
  const bool saved = meow::cursor::detail::metadata_refused.load();
  meow::cursor::detail::metadata_refused = false;

  meow::cursor::pipewire::stream_t ok;
  ok.enabled = true;
  EXPECT_TRUE(meow::cursor::pipewire::on_format(ok, SPA_VIDEO_FORMAT_BGRx, false));
  EXPECT_EQ(ok.frame_format, meow::cursor::pixel_format_t::bgrx);
  EXPECT_FALSE(meow::cursor::detail::metadata_refused.load());

  meow::cursor::pipewire::stream_t ten_bit;
  ten_bit.enabled = true;
  EXPECT_FALSE(meow::cursor::pipewire::on_format(ten_bit, SPA_VIDEO_FORMAT_xBGR_210LE, false));
  EXPECT_FALSE(ten_bit.enabled);
  EXPECT_TRUE(meow::cursor::detail::metadata_refused.load()) << "later streams ask for the embedded cursor";

  meow::cursor::detail::metadata_refused = false;
  meow::cursor::pipewire::stream_t dmabuf;
  dmabuf.enabled = true;
  EXPECT_FALSE(meow::cursor::pipewire::on_format(dmabuf, SPA_VIDEO_FORMAT_BGRx, true));
  EXPECT_TRUE(meow::cursor::detail::metadata_refused.load());

  meow::cursor::pipewire::stream_t off;
  EXPECT_TRUE(meow::cursor::pipewire::on_format(off, SPA_VIDEO_FORMAT_xBGR_210LE, true)) << "inert unless metadata mode was chosen";

  meow::cursor::detail::metadata_refused = saved;
}

TEST(MeowCursorPipewireFormat, MemoryPathMirrorsEnsureStream) {
  using platf::mem_type_e;
  EXPECT_TRUE(meow::cursor::pipewire::memory_path(mem_type_e::cuda, 4, false)) << "hybrid laptop: NVENC fed from memory";
  EXPECT_FALSE(meow::cursor::pipewire::memory_path(mem_type_e::cuda, 4, true)) << "pure NVIDIA: DMA-BUF";
  EXPECT_FALSE(meow::cursor::pipewire::memory_path(mem_type_e::vaapi, 4, false));
  EXPECT_TRUE(meow::cursor::pipewire::memory_path(mem_type_e::vaapi, 0, false)) << "no DMA-BUF formats at all";
  EXPECT_TRUE(meow::cursor::pipewire::memory_path(mem_type_e::system, 4, false));
}
