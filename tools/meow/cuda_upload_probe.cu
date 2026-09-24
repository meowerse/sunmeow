/**
 * @file tools/meow/cuda_upload_probe.cu
 * @brief Measure what the cropped upload in `cuda_ram_t` saves per frame.
 *
 * `cuda_ram_t::convert()` copies the whole captured frame from system memory into a CUDA
 * array on every frame (`sws_t::load_ram()`, a pageable `cudaMemcpy2DToArray`). With a crop
 * active only the cropped span is sampled, so `meow_viewport_load()` copies just that span
 * (`meow::viewport::cuda_upload_rect()`). This times both copies exactly as the encoder issues
 * them - same array shape, same pageable source, same 2D copy - on the reference desktop size.
 *
 * Not part of the build. From the repository root:
 *
 * ```bash
 * /opt/cuda/bin/nvcc -O2 -std=c++17 tools/meow/cuda_upload_probe.cu -o /var/tmp/cuda_upload_probe
 * /var/tmp/cuda_upload_probe
 * ```
 */
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <vector>

#include <cuda_runtime.h>

namespace {
  /**
   * @brief A region to copy.
   */
  struct region_t {
    int x;  ///< Left edge.
    int y;  ///< Top edge.
    int w;  ///< Width.
    int h;  ///< Height.
  };

  /**
   * @brief Time one 2D host-to-array copy of `r`, the way `load_ram` issues it.
   * @return Milliseconds.
   */
  double time_copy(cudaArray_t array, const std::vector<unsigned char> &host, int pitch, const region_t &r) {
    const auto start = std::chrono::steady_clock::now();
    const auto *src = host.data() + (size_t) r.y * pitch + (size_t) r.x * 4;
    if (cudaMemcpy2DToArray(array, (size_t) r.x * 4, r.y, src, pitch, (size_t) r.w * 4, r.h, cudaMemcpyHostToDevice) != cudaSuccess) {
      std::fprintf(stderr, "copy failed\n");
      return -1;
    }
    cudaDeviceSynchronize();
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
  }

  /**
   * @brief Median of a sample.
   * @param v Samples (reordered).
   * @return The median.
   */
  double median(std::vector<double> &v) {
    std::sort(v.begin(), v.end());
    return v[v.size() / 2];
  }
}  // namespace

/**
 * @brief Probe entry point.
 * @return 0 on success.
 */
int main() {
  constexpr int width = 5360;
  constexpr int height = 1440;
  constexpr int pitch = width * 4;
  std::vector<unsigned char> host((size_t) pitch * height, 0x5A);

  // Same shape as `tex_t::make(height, width * 4)` in src/platform/linux/cuda.cu.
  cudaChannelFormatDesc desc = cudaCreateChannelDesc<uchar4>();
  cudaArray_t array;
  if (cudaMallocArray(&array, &desc, width * 4, height, cudaArrayDefault) != cudaSuccess) {
    std::fprintf(stderr, "no CUDA device\n");
    return 1;
  }
  cudaDeviceProp prop {};
  cudaGetDeviceProperties(&prop, 0);
  std::printf("device: %s\n", prop.name);

  // Interleaved, one copy of each per round, median of 300 rounds: the GPU is shared with the
  // desktop (and on the reference machine with a live stream), so back-to-back batches of one
  // kind would measure whatever else happened to be running during that batch.
  const region_t regions[] = {
    {0, 0, width, height},  // full frame, 30.9 MB
    {1918, 178, 1924, 1084},  // 16:9 crop of the HDMI monitor + 2-texel margin, 8.3 MB
    {2398, 358, 1284, 724},  // ~2x zoom on a phone-shaped view, 3.7 MB
  };
  std::vector<double> samples[3];
  for (int round = 0; round < 305; ++round) {
    for (int k = 0; k < 3; ++k) {
      const double ms = time_copy(array, host, pitch, regions[k]);
      if (round >= 5) {
        samples[k].push_back(ms);
      }
    }
  }
  const double full = median(samples[0]);
  const double crop = median(samples[1]);
  const double zoom = median(samples[2]);
  std::printf("full frame 5360x1440 (30.9 MB): %.3f ms/frame (median of 300)\n", full);
  std::printf("crop 1924x1084 (8.3 MB):         %.3f ms/frame (%.0f%% of full)\n", crop, 100.0 * crop / full);
  std::printf("crop 1284x724 (3.7 MB):          %.3f ms/frame (%.0f%% of full)\n", zoom, 100.0 * zoom / full);
  cudaFreeArray(array);
  return 0;
}
