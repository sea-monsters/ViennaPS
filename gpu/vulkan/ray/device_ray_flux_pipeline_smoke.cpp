// Copyright 2026 ViennaPS
// SPDX-License-Identifier: MIT

#include "device_ray_flux_pipeline.hpp"

#include <bit>
#include <cassert>
#include <cstdint>
#include <iostream>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

#ifndef VIENNAPS_VULKAN_TRIANGLE_HIT_DEVICE_SPV_PATH
#define VIENNAPS_VULKAN_TRIANGLE_HIT_DEVICE_SPV_PATH ""
#endif
#ifndef VIENNAPS_VULKAN_RAY_RECORD_COMPACTION_SPV_PATH
#define VIENNAPS_VULKAN_RAY_RECORD_COMPACTION_SPV_PATH ""
#endif
#ifndef VIENNAPS_VULKAN_REDUCTION_SCAN_SPV_PATH
#define VIENNAPS_VULKAN_REDUCTION_SCAN_SPV_PATH ""
#endif
#ifndef VIENNAPS_VULKAN_RAY_RECORD_RADIX_HISTOGRAM_SPV_PATH
#define VIENNAPS_VULKAN_RAY_RECORD_RADIX_HISTOGRAM_SPV_PATH ""
#endif
#ifndef VIENNAPS_VULKAN_RAY_RECORD_RADIX_PREFIX_SPV_PATH
#define VIENNAPS_VULKAN_RAY_RECORD_RADIX_PREFIX_SPV_PATH ""
#endif
#ifndef VIENNAPS_VULKAN_RAY_RECORD_RADIX_SCATTER_SPV_PATH
#define VIENNAPS_VULKAN_RAY_RECORD_RADIX_SCATTER_SPV_PATH ""
#endif
#ifndef VIENNAPS_VULKAN_RAY_SURFACE_SEGMENTS_SPV_PATH
#define VIENNAPS_VULKAN_RAY_SURFACE_SEGMENTS_SPV_PATH ""
#endif
#ifndef VIENNAPS_VULKAN_RAY_SURFACE_REDUCE_SPV_PATH
#define VIENNAPS_VULKAN_RAY_SURFACE_REDUCE_SPV_PATH ""
#endif

int main() {
  using namespace viennaps::vulkan::ray;

  const std::vector<Ray> rays{
      {{{0.25F, 0.25F, 1.0F}}, {{0.0F, 0.0F, -1.0F}}, 0.0F, 10.0F},
      {{{2.25F, 2.25F, 1.0F}}, {{0.0F, 0.0F, -1.0F}}, 0.0F, 10.0F},
      {{{0.25F, 0.25F, 1.0F}}, {{0.0F, 0.0F, 1.0F}}, 0.0F, 10.0F},
      {{{0.30F, 0.20F, 1.0F}}, {{0.0F, 0.0F, -1.0F}}, 0.0F, 10.0F},
      {{{0.75F, 0.10F, 1.0F}}, {{0.0F, 0.0F, -1.0F}}, 0.0F, 10.0F}};
  const std::vector<Triangle> triangles{
      {{{0.0F, 0.0F, 0.0F}}, {{1.0F, 0.0F, 0.0F}}, {{0.0F, 1.0F, 0.0F}}},
      {{{0.0F, 0.0F, -2.0F}}, {{1.0F, 0.0F, -2.0F}}, {{0.0F, 1.0F, -2.0F}}},
      {{{2.0F, 2.0F, 0.0F}}, {{3.0F, 2.0F, 0.0F}}, {{2.0F, 3.0F, 0.0F}}}};
  const std::vector<float> weights{1.0F, -0.0F, 0.5F, -0.0F, 2.0F};
  constexpr std::uint32_t kSurfaceSentinel = 0xdeadbeefU;
  constexpr float kWeightSentinel = -7.0F;

  std::string error;
  DeviceRayFluxPipeline pipeline;
  std::vector<std::uint32_t> cpuSurface(rays.size(), kSurfaceSentinel);
  std::vector<float> cpuWeight(rays.size(), kWeightSentinel);
  RayFluxResult cpu{cpuSurface, cpuWeight, 99U};
  assert(pipeline.runCpu(rays, triangles, weights, cpu, error));
  assert(cpu.count == 2U && cpuSurface[0] == 0U && cpuSurface[1] == 2U);
  assert(std::bit_cast<std::uint32_t>(cpuWeight[0]) == 0x40400000U);
  assert(std::bit_cast<std::uint32_t>(cpuWeight[1]) == 0x80000000U);
  for (std::size_t i = cpu.count; i < cpuSurface.size(); ++i)
    assert(cpuSurface[i] == kSurfaceSentinel &&
           cpuWeight[i] == kWeightSentinel);

  const DeviceRayFluxSpirv spirv{
      VIENNAPS_VULKAN_TRIANGLE_HIT_DEVICE_SPV_PATH,
      VIENNAPS_VULKAN_RAY_RECORD_COMPACTION_SPV_PATH,
      VIENNAPS_VULKAN_REDUCTION_SCAN_SPV_PATH,
      VIENNAPS_VULKAN_RAY_RECORD_RADIX_HISTOGRAM_SPV_PATH,
      VIENNAPS_VULKAN_RAY_RECORD_RADIX_PREFIX_SPV_PATH,
      VIENNAPS_VULKAN_RAY_RECORD_RADIX_SCATTER_SPV_PATH,
      VIENNAPS_VULKAN_RAY_SURFACE_SEGMENTS_SPV_PATH,
      VIENNAPS_VULKAN_RAY_SURFACE_REDUCE_SPV_PATH};
  if (spirv.triangleHit.empty() || spirv.recordCompaction.empty() ||
      spirv.reductionScan.empty() || spirv.radixHistogram.empty() ||
      spirv.radixPrefix.empty() || spirv.radixScatter.empty() ||
      spirv.surfaceSegments.empty() || spirv.surfaceReduce.empty()) {
    std::cout << "device ray-flux pipeline Vulkan dispatch SKIP (no SPIR-V)\n";
    return 0;
  }
  assert(pipeline.initialize(spirv, error));

  std::vector<std::uint32_t> gpuSurface(rays.size(), kSurfaceSentinel);
  std::vector<float> gpuWeight(rays.size(), kWeightSentinel);
  RayFluxResult gpu{gpuSurface, gpuWeight, 99U};
  assert(pipeline.runGpu(rays, triangles, weights, gpu, error));
  assert(pipeline.lastComputeSubmissionCount() == 1U);
  assert(gpu.count == cpu.count);
  for (std::size_t i = 0U; i < gpu.count; ++i) {
    assert(gpuSurface[i] == cpuSurface[i]);
    assert(std::bit_cast<std::uint32_t>(gpuWeight[i]) ==
           std::bit_cast<std::uint32_t>(cpuWeight[i]));
  }
  for (std::size_t i = gpu.count; i < gpuSurface.size(); ++i)
    assert(gpuSurface[i] == kSurfaceSentinel &&
           gpuWeight[i] == kWeightSentinel);

  const std::vector<float> overflowWeights{
      std::numeric_limits<float>::max(), -0.0F, 0.5F,
      std::numeric_limits<float>::max(), 2.0F};
  std::vector<std::uint32_t> overflowCpuSurface(rays.size(), kSurfaceSentinel);
  std::vector<float> overflowCpuWeight(rays.size(), kWeightSentinel);
  RayFluxResult overflowCpu{overflowCpuSurface, overflowCpuWeight, 73U};
  assert(!pipeline.runCpu(rays, triangles, overflowWeights, overflowCpu, error));
  assert(overflowCpu.count == 73U &&
         overflowCpuSurface[0] == kSurfaceSentinel &&
         overflowCpuWeight[0] == kWeightSentinel);
  std::vector<std::uint32_t> overflowGpuSurface(rays.size(), kSurfaceSentinel);
  std::vector<float> overflowGpuWeight(rays.size(), kWeightSentinel);
  RayFluxResult overflowGpu{overflowGpuSurface, overflowGpuWeight, 73U};
  assert(!pipeline.runGpu(rays, triangles, overflowWeights, overflowGpu, error));
  assert(overflowGpu.count == 73U &&
         overflowGpuSurface[0] == kSurfaceSentinel &&
         overflowGpuWeight[0] == kWeightSentinel);

  const auto priorSurface = gpuSurface;
  const auto priorWeight = gpuWeight;
  const auto priorCount = gpu.count;
  std::vector<float> malformed = weights;
  malformed[0] = std::numeric_limits<float>::quiet_NaN();
  assert(!pipeline.runGpu(rays, triangles, malformed, gpu, error));
  assert(gpu.count == priorCount && gpuSurface == priorSurface &&
         gpuWeight == priorWeight);

  std::vector<std::uint32_t> shortSurface(1U, 0xfaceb00cU);
  std::vector<float> shortWeight(1U, 19.0F);
  RayFluxResult shortResult{shortSurface, shortWeight, 71U};
  assert(!pipeline.runCpu(rays, triangles, weights, shortResult, error));
  assert(shortResult.count == 71U && shortSurface[0] == 0xfaceb00cU &&
         shortWeight[0] == 19.0F);
  assert(!pipeline.runGpu(rays, triangles, weights, shortResult, error));
  assert(shortResult.count == 71U && shortSurface[0] == 0xfaceb00cU &&
         shortWeight[0] == 19.0F);

  std::vector<std::uint32_t> emptySurface(2U, 0x12345678U);
  std::vector<float> emptyWeight(2U, -11.0F);
  RayFluxResult empty{emptySurface, emptyWeight, 42U};
  assert(pipeline.runGpu({}, {}, {}, empty, error));
  assert(empty.count == 42U && emptySurface[0] == 0x12345678U &&
         emptyWeight[1] == -11.0F);
  std::cout << "device ray-flux pipeline Vulkan dispatch PASS\n";
}
