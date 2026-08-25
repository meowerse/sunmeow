/**
 * @file src/platform/linux/cuda.h
 * @brief Definitions for CUDA implementation.
 */
#pragma once

#if defined(SUNSHINE_BUILD_CUDA)
  // standard includes
  #include <cstdint>
  #include <memory>
  #include <optional>
  #include <string>
  #include <vector>

  // local includes
  #include "src/video_colorspace.h"

namespace platf {
  struct avcodec_encode_device_t;
  struct img_t;
}  // namespace platf

namespace cuda {

  namespace nvfbc {
    std::vector<std::string> display_names();
  }

  std::unique_ptr<platf::avcodec_encode_device_t> make_avcodec_encode_device(int width, int height, bool vram);

  /**
   * @brief Create a GL->CUDA encoding device for consuming captured dmabufs.
   * @param in_width Width of captured frames.
   * @param in_height Height of captured frames.
   * @param offset_x Offset of content in captured frame.
   * @param offset_y Offset of content in captured frame.
   * @return FFmpeg encoding device context.
   */
  std::unique_ptr<platf::avcodec_encode_device_t> make_avcodec_gl_encode_device(int width, int height, int offset_x, int offset_y);

  int init();
}  // namespace cuda

typedef struct cudaArray *cudaArray_t;

  #if !defined(__CUDACC__)
typedef struct CUstream_st *cudaStream_t;
typedef unsigned long long cudaTextureObject_t;
  #else /* defined(__CUDACC__) */
typedef __location__(device_builtin) struct CUstream_st *cudaStream_t;
typedef __location__(device_builtin) unsigned long long cudaTextureObject_t;
  #endif /* !defined(__CUDACC__) */

namespace cuda {

  class freeCudaPtr_t {
  public:
    void operator()(void *ptr);
  };

  class freeCudaStream_t {
  public:
    void operator()(cudaStream_t ptr);
  };

  using ptr_t = std::unique_ptr<void, freeCudaPtr_t>;
  using stream_t = std::unique_ptr<CUstream_st, freeCudaStream_t>;

  stream_t make_stream(int flags = 0);

  struct viewport_t {
    int width;
    int height;
    int offsetX;
    int offsetY;
  };

  /**
   * @brief MEOW-TOUCH(viewport-cuda): where the scaler reads from, in captured texels.
   *
   * `viewport_t` above is a **destination** rectangle -- where the scaled image lands inside
   * the encode surface. This is the other half of the mapping and is purely **source** space:
   * the texel the first destination pixel samples, and how far the sample point advances per
   * destination pixel. Keeping them apart is the point; an offset added to the wrong one is
   * silent and looks like a scaling bug.
   *
   * Upstream's single `sws_t::scale` is the special case `{0, 0, scale, scale}`. The step is
   * per axis because a crop's scaled extents are even-aligned independently
   * (`meow::viewport::plan()`), after which the two axis ratios genuinely differ.
   *
   * Geometry, validation and the bounds proof live in `src/meow/viewport_cuda.h` and are unit
   * tested without a GPU.
   */
  struct source_t {
    float originX;  ///< Texel column sampled by the first destination column.
    float originY;  ///< Texel row sampled by the first destination row.
    float stepX;  ///< Texel columns advanced per destination column.
    float stepY;  ///< Texel rows advanced per destination row.
  };

  class tex_t {
  public:
    static std::optional<tex_t> make(int height, int pitch);

    tex_t();
    tex_t(tex_t &&);

    tex_t &operator=(tex_t &&other);

    ~tex_t();

    int copy(std::uint8_t *src, int height, int pitch);

    cudaArray_t array;

    struct texture {
      cudaTextureObject_t point;
      cudaTextureObject_t linear;
    } texture;
  };

  class sws_t {
  public:
    sws_t() = default;
    sws_t(int in_width, int in_height, int out_width, int out_height, int pitch, int threadsPerBlock, ptr_t &&color_matrix);

    /**
     * in_width, in_height -- The width and height of the captured image in pixels
     * out_width, out_height -- the width and height of the NV12 image in pixels
     *
     * pitch -- The size of a single row of pixels in bytes
     */
    static std::optional<sws_t> make(int in_width, int in_height, int out_width, int out_height, int pitch);

    // Converts loaded image into a CUDevicePtr
    int convert_nv12(std::uint8_t *Y, std::uint8_t *UV, std::uint32_t pitchY, std::uint32_t pitchUV, cudaTextureObject_t texture, stream_t::pointer stream);
    int convert_nv12(std::uint8_t *Y, std::uint8_t *UV, std::uint32_t pitchY, std::uint32_t pitchUV, cudaTextureObject_t texture, stream_t::pointer stream, const viewport_t &viewport);
    int convert_yuv444(std::uint8_t *Y, std::uint8_t *U, std::uint8_t *V, std::uint32_t pitch, cudaTextureObject_t texture, stream_t::pointer stream);
    int convert_yuv444(std::uint8_t *Y, std::uint8_t *U, std::uint8_t *V, std::uint32_t pitch, cudaTextureObject_t texture, stream_t::pointer stream, const viewport_t &viewport);

    void apply_colorspace(const video::sunshine_colorspace_t &colorspace);

    int load_ram(platf::img_t &img, cudaArray_t array);

    ptr_t color_matrix;

    int threadsPerBlock;

    viewport_t viewport;

    float scale;

    /**
     * @brief MEOW-TOUCH(viewport-cuda): source sampling map used by the conversion kernels.
     *
     * Initialised by the constructor to `{0, 0, scale, scale}`, which is bit-identical to the
     * mapping upstream computed from `scale` alone. `cuda_t::convert()` overwrites it once
     * per frame from `meow::viewport::cuda_scaler_config()` when the client has asked for a
     * crop, and puts the baseline back when it has not.
     */
    source_t source;
  };
}  // namespace cuda

#endif
