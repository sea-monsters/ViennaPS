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
#ifndef VIENNAPS_VULKAN_TRIANGLE_BVH_HIT_SPV_PATH
#define VIENNAPS_VULKAN_TRIANGLE_BVH_HIT_SPV_PATH ""
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
      VIENNAPS_VULKAN_RAY_SURFACE_REDUCE_SPV_PATH,
      VIENNAPS_VULKAN_TRIANGLE_BVH_HIT_SPV_PATH};
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
  assert(
      !pipeline.runCpu(rays, triangles, overflowWeights, overflowCpu, error));
  assert(overflowCpu.count == 73U &&
         overflowCpuSurface[0] == kSurfaceSentinel &&
         overflowCpuWeight[0] == kWeightSentinel);
  std::vector<std::uint32_t> overflowGpuSurface(rays.size(), kSurfaceSentinel);
  std::vector<float> overflowGpuWeight(rays.size(), kWeightSentinel);
  RayFluxResult overflowGpu{overflowGpuSurface, overflowGpuWeight, 73U};
  assert(
      !pipeline.runGpu(rays, triangles, overflowWeights, overflowGpu, error));
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

  std::vector<Triangle> tieTriangles;
  for (int i = 0; i < 9; ++i) {
    const float x = -10.0F - static_cast<float>(i);
    tieTriangles.push_back({{{x - 0.25F, -0.25F, 0.0F}},
                            {{x + 0.25F, -0.25F, 0.0F}},
                            {{x, 0.25F, 0.0F}}});
  }
  tieTriangles.push_back(
      {{{-1.0F, -1.0F, 0.0F}}, {{1.0F, -1.0F, 0.0F}}, {{0.0F, 1.0F, 0.0F}}});
  tieTriangles.push_back(tieTriangles.back());
  for (int i = 0; i < 9; ++i) {
    const float x = 10.0F + static_cast<float>(i);
    tieTriangles.push_back({{{x - 0.25F, -0.25F, 0.0F}},
                            {{x + 0.25F, -0.25F, 0.0F}},
                            {{x, 0.25F, 0.0F}}});
  }
  const std::vector<Ray> tieRays{
      {{{0.0F, 0.0F, 2.0F}}, {{0.0F, 0.0F, -1.0F}}, 0.0F, 10.0F},
      {{{0.9F, 0.8F, 2.0F}}, {{0.0F, 0.0F, -1.0F}}, 0.0F, 10.0F},
      {{{20.0F, 20.0F, 2.0F}}, {{0.0F, 0.0F, -1.0F}}, 0.0F, 10.0F},
      {{{-10.0F, 0.0F, 2.0F}}, {{0.0F, 0.0F, -1.0F}}, 0.0F, 10.0F},
      {{{0.0F, 0.0F, 2.0F}}, {{0.0F, 0.0F, -1.0F}}, 2.0F, 10.0F},
      {{{0.0F, 0.0F, 2.0F}}, {{0.0F, 0.0F, 1.0F}}, 0.0F, 10.0F}};
  const std::vector<float> tieWeights{1.0F, 2.0F, 3.0F, 4.0F, 5.0F, 6.0F};
  std::vector<std::uint32_t> tieCpuSurface(tieRays.size(), kSurfaceSentinel);
  std::vector<float> tieCpuWeight(tieRays.size(), kWeightSentinel);
  RayFluxResult tieCpu{tieCpuSurface, tieCpuWeight, 0U};
  assert(pipeline.runCpu(tieRays, tieTriangles, tieWeights, tieCpu, error));
  std::vector<std::uint32_t> tieGpuSurface(tieRays.size(), kSurfaceSentinel);
  std::vector<float> tieGpuWeight(tieRays.size(), kWeightSentinel);
  RayFluxResult tieGpu{tieGpuSurface, tieGpuWeight, 0U};
  assert(pipeline.runGpu(tieRays, tieTriangles, tieWeights, tieGpu, error));
  assert(pipeline.lastComputeSubmissionCount() == 1U);
  const bool tieSurface9 =
      std::find(tieGpuSurface.begin(), tieGpuSurface.begin() + tieGpu.count,
                9U) != tieGpuSurface.begin() + tieGpu.count;
  if (!(tieGpu.count == tieCpu.count && tieSurface9)) {
    std::cerr << "tie result gpu count=" << tieGpu.count
              << " cpu count=" << tieCpu.count << " first="
              << (tieGpu.count == 0U ? 0xffffffffU : tieGpuSurface[0])
              << " cpu-first="
              << (tieCpu.count == 0U ? 0xffffffffU : tieCpuSurface[0]) << '\n';
    return 1;
  }
  for (std::size_t i = 0; i < tieGpu.count; ++i) {
    assert(tieGpuSurface[i] == tieCpuSurface[i]);
    assert(std::bit_cast<std::uint32_t>(tieGpuWeight[i]) ==
           std::bit_cast<std::uint32_t>(tieCpuWeight[i]));
  }
  assert(pipeline.prepareGeometry(tieTriangles, error));
  std::vector<std::uint32_t> preparedSurface(tieRays.size(), kSurfaceSentinel);
  std::vector<float> preparedWeight(tieRays.size(), kWeightSentinel);
  RayFluxResult prepared{preparedSurface, preparedWeight, 0U};
  assert(pipeline.runGpuPrepared(tieRays, tieWeights, prepared, error));
  assert(pipeline.lastComputeSubmissionCount() == 1U);
  const auto preparedSurfaceFirst = preparedSurface;
  const auto preparedWeightFirst = preparedWeight;
  const auto preparedCountFirst = prepared.count;
  auto invalidCountTriangles = tieTriangles;
  invalidCountTriangles.pop_back();
  assert(!pipeline.refitPreparedGeometry(invalidCountTriangles, error));
  assert(pipeline.runGpuPrepared(tieRays, tieWeights, prepared, error));
  assert(prepared.count == preparedCountFirst &&
         preparedSurface == preparedSurfaceFirst &&
         preparedWeight == preparedWeightFirst);
  auto invalidNanTriangles = tieTriangles;
  invalidNanTriangles[0].a[0] = std::numeric_limits<float>::quiet_NaN();
  assert(!pipeline.refitPreparedGeometry(invalidNanTriangles, error));
  assert(pipeline.runGpuPrepared(tieRays, tieWeights, prepared, error));
  assert(prepared.count == preparedCountFirst &&
         preparedSurface == preparedSurfaceFirst &&
         preparedWeight == preparedWeightFirst);
  auto invalidSubnormalTriangles = tieTriangles;
  invalidSubnormalTriangles[0].a[0] =
      std::numeric_limits<float>::denorm_min();
  assert(!pipeline.refitPreparedGeometry(invalidSubnormalTriangles, error));
  assert(pipeline.runGpuPrepared(tieRays, tieWeights, prepared, error));
  assert(prepared.count == preparedCountFirst &&
         preparedSurface == preparedSurfaceFirst &&
         preparedWeight == preparedWeightFirst);
  auto movedTieTriangles = tieTriangles;
  for (auto &point : {&movedTieTriangles[9].a, &movedTieTriangles[9].b,
                      &movedTieTriangles[9].c})
    (*point)[0] += 8.0F;
  std::vector<std::uint32_t> movedTieCpuSurface(tieRays.size(),
                                                kSurfaceSentinel);
  std::vector<float> movedTieCpuWeight(tieRays.size(), kWeightSentinel);
  RayFluxResult movedTieCpu{movedTieCpuSurface, movedTieCpuWeight, 0U};
  assert(pipeline.runCpu(tieRays, movedTieTriangles, tieWeights, movedTieCpu,
                         error));
  assert(pipeline.refitPreparedGeometry(movedTieTriangles, error));
  assert(pipeline.lastComputeSubmissionCount() == 1U);
  std::vector<std::uint32_t> movedTieGpuSurface(tieRays.size(),
                                                kSurfaceSentinel);
  std::vector<float> movedTieGpuWeight(tieRays.size(), kWeightSentinel);
  RayFluxResult movedTieGpu{movedTieGpuSurface, movedTieGpuWeight, 0U};
  assert(pipeline.runGpuPrepared(tieRays, tieWeights, movedTieGpu, error));
  assert(pipeline.lastComputeSubmissionCount() == 1U);
  assert(movedTieGpu.count == movedTieCpu.count);
  for (std::size_t i = 0U; i < movedTieGpu.count; ++i) {
    assert(movedTieGpuSurface[i] == movedTieCpuSurface[i]);
    assert(std::bit_cast<std::uint32_t>(movedTieGpuWeight[i]) ==
           std::bit_cast<std::uint32_t>(movedTieCpuWeight[i]));
  }
  assert(pipeline.refitPreparedGeometry(tieTriangles, error));
  assert(pipeline.lastComputeSubmissionCount() == 1U);
  assert(pipeline.runGpuPrepared(tieRays, tieWeights, prepared, error));
  assert(pipeline.lastComputeSubmissionCount() == 1U);
  assert(prepared.count == tieCpu.count &&
         preparedSurface == preparedSurfaceFirst &&
         preparedWeight == preparedWeightFirst);
  std::vector<std::uint32_t> defaultSurface(tieRays.size(), kSurfaceSentinel);
  std::vector<float> defaultWeight(tieRays.size(), kWeightSentinel);
  RayFluxResult defaultGpu{defaultSurface, defaultWeight, 0U};
  assert(pipeline.runGpu(tieRays, movedTieTriangles, tieWeights, defaultGpu,
                         error));
  assert(pipeline.lastComputeSubmissionCount() == 1U &&
         defaultGpu.count == movedTieCpu.count);
  for (std::size_t i = 0U; i < defaultGpu.count; ++i) {
    assert(defaultSurface[i] == movedTieCpuSurface[i]);
    assert(std::bit_cast<std::uint32_t>(defaultWeight[i]) ==
           std::bit_cast<std::uint32_t>(movedTieCpuWeight[i]));
  }
  std::vector<std::uint32_t> invalidatedSurface(tieRays.size(),
                                                kSurfaceSentinel);
  std::vector<float> invalidatedWeight(tieRays.size(), kWeightSentinel);
  RayFluxResult invalidatedPrepared{invalidatedSurface, invalidatedWeight,
                                    76U};
  assert(!pipeline.runGpuPrepared(tieRays, tieWeights, invalidatedPrepared,
                                  error));
  assert(pipeline.lastComputeSubmissionCount() == 0U &&
         invalidatedPrepared.count == 76U &&
         invalidatedSurface[0] == kSurfaceSentinel &&
         invalidatedWeight[0] == kWeightSentinel);
  assert(!pipeline.prepareGeometry({}, error));
  std::vector<std::uint32_t> failedPrepareSurface(tieRays.size(),
                                                  kSurfaceSentinel);
  std::vector<float> failedPrepareWeight(tieRays.size(), kWeightSentinel);
  RayFluxResult failedPrepare{failedPrepareSurface, failedPrepareWeight, 74U};
  assert(!pipeline.runGpuPrepared(tieRays, tieWeights, failedPrepare, error));
  assert(failedPrepare.count == 74U &&
         failedPrepareSurface[0] == kSurfaceSentinel &&
         failedPrepareWeight[0] == kWeightSentinel);
  pipeline.resetPreparedGeometry();
  std::vector<std::uint32_t> resetSurface(tieRays.size(), kSurfaceSentinel);
  std::vector<float> resetWeight(tieRays.size(), kWeightSentinel);
  RayFluxResult resetResult{resetSurface, resetWeight, 73U};
  assert(!pipeline.runGpuPrepared(tieRays, tieWeights, resetResult, error));
  assert(resetResult.count == 73U && resetSurface[0] == kSurfaceSentinel &&
         resetWeight[0] == kWeightSentinel);
  std::cout << "device ray-flux pipeline Vulkan dispatch PASS\n";
}
    if (!require_equal(gpuSurface[i] == kSurfaceSentinel &&
                           gpuWeight[i] == kWeightSentinel,
                       "gpu normal tail sentinel"))
      return 1;

  for (std::size_t iteration = 0U; iteration < 40U; ++iteration) {
    std::vector<std::uint32_t> reuseSurface(rays.size(), kSurfaceSentinel);
    std::vector<float> reuseWeight(rays.size(), kWeightSentinel);
    RayFluxResult reuse{reuseSurface, reuseWeight, 0U};
    if (!require_result(pipeline.runGpu(rays, triangles, weights, reuse, error),
                        "descriptor lease reuse run", error) ||
        !require_equal(pipeline.lastComputeSubmissionCount() == 1U &&
                           reuse.count == cpu.count && reuseSurface == cpuSurface &&
                           std::equal(reuseWeight.begin(), reuseWeight.end(),
                                      cpuWeight.begin(),
                                      [](const float lhs, const float rhs) {
                                        return std::bit_cast<std::uint32_t>(lhs) ==
                                               std::bit_cast<std::uint32_t>(rhs);
                                      }),
                       "descriptor lease reuse result"))
