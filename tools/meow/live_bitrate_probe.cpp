/**
 * @file tools/meow/live_bitrate_probe.cpp
 * @brief Measure whether an FFmpeg encoder honours a bitrate change on a *running* session.
 *
 * `src/meow/adaptive_bitrate_encoder.h` changes the bitrate by mutating `bit_rate`,
 * `rc_max_rate`, `rc_min_rate` and `rc_buffer_size` on the live `AVCodecContext`, which is the
 * only runtime interface FFmpeg offers. FFmpeg gives no way to ask whether an encoder honours
 * it - one that ignores it reports success and keeps its old rate - so this measures it.
 *
 * It opens an encoder with the rate-control fields `video.cpp` sets (CBR: `rc_min_rate ==
 * rc_max_rate == bit_rate`, VBV of one frame for the software encoder, none for VA-API), feeds
 * desktop-like content (a static gradient with a moving window whose contents change every
 * frame, busy enough that 8 Mbps is rate-limited rather than quality-limited), and prints the produced rate and keyframe count per 30-frame window while
 * it lowers the bitrate from 8 to 3 Mbps at frame 180 and raises it back at frame 360.
 *
 * Not part of the build (it needs a GPU for the hardware encoders). Build against the FFmpeg
 * bundle the gate build downloaded, from the repository root:
 *
 * ```bash
 * F=cmake-build-gate/_deps/ffmpeg
 * g++ -std=c++20 -O2 tools/meow/live_bitrate_probe.cpp -I$F/include -o /var/tmp/live_bitrate_probe \
 *   $F/lib/libavcodec.a $F/lib/libswscale.a $F/lib/libavutil.a $F/lib/libx264.a $F/lib/libx265.a \
 *   $F/lib/libSvtAv1Enc.a $F/lib/libcbs.a -lva -lva-drm -ldrm -lz -lpthread -ldl -lm -lstdc++
 * /var/tmp/live_bitrate_probe libx264
 * /var/tmp/live_bitrate_probe h264_vaapi /dev/dri/renderD128
 * /var/tmp/live_bitrate_probe h264_nvenc
 * /var/tmp/live_bitrate_probe h264_nvenc --static
 * ```
 */
// standard includes
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <string>
#include <unistd.h>

// lib includes
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_vaapi.h>
#include <libavutil/opt.h>
}
#include <va/va_drm.h>

namespace {

  int width = 1280;  ///< Frame width (`size=WxH` overrides).
  int height = 720;  ///< Frame height.
  constexpr int fps = 60;  ///< Frame rate.
  constexpr int window = 30;  ///< Frames per reported window.

  /**
   * @brief Paint one desktop-like NV12/YUV420 frame into system memory.
   * @param frame Destination (`AV_PIX_FMT_NV12` or `AV_PIX_FMT_YUV420P`).
   * @param n Frame number.
   */
  void paint(AVFrame *frame, const int n) {
    for (int y = 0; y < height; ++y) {
      auto *row = frame->data[0] + static_cast<std::ptrdiff_t>(y) * frame->linesize[0];
      for (int x = 0; x < width; ++x) {
        int v = 40 + (x + y) / 16;  // background gradient
        const int wx = (n * 4) % std::max(width - 300, 1);
        if (x >= wx && x < wx + 300 && y >= 200 && y < 500) {
          // A window being dragged whose contents change every frame (a video, a scrolling
          // terminal): busy enough that 8 Mbps is actually rate-limited, not quality-limited.
          auto h = static_cast<std::uint32_t>(x * 73856093) ^ static_cast<std::uint32_t>(y * 19349663) ^ static_cast<std::uint32_t>(n * 83492791);
          h ^= h >> 13;
          h *= 0x5bd1e995u;
          h ^= h >> 15;
          v = 64 + static_cast<int>(h & 0x7F);
        }
        row[x] = static_cast<std::uint8_t>(v);
      }
    }
    for (int p = 1; p < (frame->format == AV_PIX_FMT_NV12 ? 2 : 3); ++p) {
      const int plane_width = frame->format == AV_PIX_FMT_NV12 ? width : width / 2;
      for (int y = 0; y < height / 2; ++y) {
        std::memset(frame->data[p] + static_cast<std::ptrdiff_t>(y) * frame->linesize[p], 128, plane_width);
      }
    }
  }

  /**
   * @brief Set the rate-control fields the way `video.cpp` does for a CBR encoder.
   * @param ctx Codec context.
   * @param kbps Bitrate.
   * @param vbv Whether to set a one-frame VBV buffer.
   */
  void set_rate(AVCodecContext *ctx, const int kbps, const bool vbv) {
    const std::int64_t bits = static_cast<std::int64_t>(kbps) * 1000;
    ctx->rc_max_rate = bits;
    ctx->bit_rate = bits;
    ctx->rc_min_rate = bits;
    if (vbv) {
      ctx->rc_buffer_size = static_cast<int>(bits / fps);
    }
  }

}  // namespace

/**
 * @brief Probe entry point.
 * @param argc Argument count.
 * @param argv `encoder [vaapi-device]`.
 * @return 0 on success.
 */
int main(int argc, char **argv) {
  if (argc < 2) {
    std::fprintf(stderr, "usage: %s <encoder> [vaapi device]\n", argv[0]);
    return 2;
  }
  const std::string name = argv[1];
  // `--static` repeats one frame: what re-encoding an idle desktop at the minimum frame rate
  // (Sunshine's `minimum_fps_target`) costs on the wire.
  const bool still = std::string(argv[argc - 1]) == "--static";
  // `preset=pN` overrides NVENC's preset, to weigh encode latency against quality per bit.
  std::string preset = "p1";
  for (int i = 2; i < argc; ++i) {
    if (std::string(argv[i]).starts_with("preset=")) {
      preset = std::string(argv[i]).substr(7);
    } else if (std::string(argv[i]).starts_with("size=")) {
      std::sscanf(argv[i] + 5, "%dx%d", &width, &height);
    }
  }
  const bool vaapi = name.find("vaapi") != std::string::npos;
  const bool nvenc = name.find("nvenc") != std::string::npos;

  const AVCodec *codec = avcodec_find_encoder_by_name(name.c_str());
  if (!codec) {
    std::fprintf(stderr, "encoder %s not in this FFmpeg build\n", name.c_str());
    return 1;
  }
  AVCodecContext *ctx = avcodec_alloc_context3(codec);
  ctx->width = width;
  ctx->height = height;
  ctx->time_base = {1, fps};
  ctx->framerate = {fps, 1};
  ctx->gop_size = 1 << 30;  // Sunshine: infinite GOP, IDR on demand only
  ctx->max_b_frames = 0;
  ctx->pix_fmt = nvenc ? AV_PIX_FMT_NV12 : AV_PIX_FMT_YUV420P;

  AVBufferRef *device = nullptr;
  AVBufferRef *frames_ref = nullptr;
  if (vaapi) {
    // The bundled FFmpeg cannot open a DRM VA display by itself; do what src/platform/linux/vaapi.cpp does.
    const int fd = open(argc > 2 ? argv[2] : "/dev/dri/renderD128", O_RDWR);
    VADisplay display = fd >= 0 ? vaGetDisplayDRM(fd) : nullptr;
    int major = 0;
    int minor = 0;
    if (!display || vaInitialize(display, &major, &minor) != VA_STATUS_SUCCESS) {
      std::fprintf(stderr, "no VA-API device\n");
      return 1;
    }
    std::printf("VA-API %d.%d: %s\n", major, minor, vaQueryVendorString(display));
    device = av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_VAAPI);
    reinterpret_cast<AVVAAPIDeviceContext *>(reinterpret_cast<AVHWDeviceContext *>(device->data)->hwctx)->display = display;
    if (av_hwdevice_ctx_init(device) < 0) {
      std::fprintf(stderr, "VA-API device init failed\n");
      return 1;
    }
    frames_ref = av_hwframe_ctx_alloc(device);
    auto *frames = reinterpret_cast<AVHWFramesContext *>(frames_ref->data);
    frames->format = AV_PIX_FMT_VAAPI;
    frames->sw_format = AV_PIX_FMT_NV12;
    frames->width = width;
    frames->height = height;
    frames->initial_pool_size = 4;
    if (av_hwframe_ctx_init(frames_ref) < 0) {
      std::fprintf(stderr, "hwframe init failed\n");
      return 1;
    }
    ctx->pix_fmt = AV_PIX_FMT_VAAPI;
    ctx->hw_frames_ctx = av_buffer_ref(frames_ref);
  }

  AVDictionary *options = nullptr;
  if (nvenc) {
    av_dict_set(&options, "preset", preset.c_str(), 0);
    av_dict_set(&options, "forced-idr", "1", 0);
    av_dict_set(&options, "cbr_padding", "0", 0);
    av_dict_set(&options, "multipass", "qres", 0);
    av_dict_set(&options, "tune", "ull", 0);
    av_dict_set(&options, "rc", "cbr", 0);
    av_dict_set(&options, "zerolatency", "1", 0);
    av_dict_set(&options, "delay", "0", 0);
    av_dict_set(&options, "surfaces", "1", 0);
  } else if (vaapi) {
    av_dict_set(&options, "async_depth", "1", 0);
    av_dict_set(&options, "idr_interval", "2147483647", 0);
    av_dict_set(&options, "rc_mode", "CBR", 0);
  } else {
    av_dict_set(&options, "preset", "superfast", 0);
    av_dict_set(&options, "tune", "zerolatency", 0);
  }
  set_rate(ctx, 8000, !vaapi);
  if (const int err = avcodec_open2(ctx, codec, &options); err < 0) {
    char msg[AV_ERROR_MAX_STRING_SIZE];
    std::fprintf(stderr, "open failed: %s\n", av_make_error_string(msg, sizeof(msg), err));
    return 1;
  }

  // Anything left in the dictionary was not recognised by this encoder and silently ignored.
  for (const AVDictionaryEntry *e = nullptr; (e = av_dict_iterate(options, e));) {
    std::printf("  (option '%s' ignored by %s)\n", e->key, name.c_str());
  }

  AVFrame *sw = av_frame_alloc();
  sw->format = vaapi ? AV_PIX_FMT_NV12 : ctx->pix_fmt;
  sw->width = width;
  sw->height = height;
  av_frame_get_buffer(sw, 0);
  AVPacket *pkt = av_packet_alloc();

  std::int64_t bytes = 0;
  double encode_ms = 0.0;
  int keyframes = 0;
  int total_keyframes = 0;
  for (int n = 0; n < 540; ++n) {
    if (n == 180) {
      set_rate(ctx, 3000, !vaapi);
      std::printf("  -- lowered to 3000 kbps on the live context --\n");
    } else if (n == 360) {
      set_rate(ctx, 8000, !vaapi);
      std::printf("  -- raised to 8000 kbps on the live context --\n");
    }
    av_frame_make_writable(sw);
    paint(sw, still ? 0 : n);
    sw->pts = n;
    AVFrame *in = sw;
    AVFrame *hw = nullptr;
    if (vaapi) {
      hw = av_frame_alloc();
      av_hwframe_get_buffer(frames_ref, hw, 0);
      av_hwframe_transfer_data(hw, sw, 0);
      hw->pts = n;
      in = hw;
    }
    const auto sent_at = std::chrono::steady_clock::now();
    if (avcodec_send_frame(ctx, in) < 0) {
      std::fprintf(stderr, "send failed at %d\n", n);
      return 1;
    }
    av_frame_free(&hw);
    while (avcodec_receive_packet(ctx, pkt) == 0) {
      // Frame sizes either side of each change: what one reconfiguration costs.
      if ((pkt->pts >= 176 && pkt->pts <= 186) || (pkt->pts >= 356 && pkt->pts <= 366)) {
        std::printf("    frame %3lld: %6d bytes%s\n", static_cast<long long>(pkt->pts), pkt->size, (pkt->flags & AV_PKT_FLAG_KEY) ? "  IDR" : "");
      }
      bytes += pkt->size;
      if (pkt->flags & AV_PKT_FLAG_KEY) {
        ++keyframes;
        ++total_keyframes;
      }
      av_packet_unref(pkt);
    }
    encode_ms += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - sent_at).count();
    if ((n + 1) % window == 0) {
      std::printf("  f%3d-%3d  %6.3f Mbps  IDR=%d\n", n + 1 - window, n, static_cast<double>(bytes) * 8.0 * fps / window / 1e6, keyframes);
      bytes = 0;
      keyframes = 0;
    }
  }
  std::printf("%s: total IDRs %d / 540 frames, %.2f ms per frame from send to packet\n", name.c_str(), total_keyframes, encode_ms / 540.0);

  av_packet_free(&pkt);
  av_frame_free(&sw);
  avcodec_free_context(&ctx);
  av_buffer_unref(&frames_ref);
  av_buffer_unref(&device);
  av_dict_free(&options);
  return 0;
}
