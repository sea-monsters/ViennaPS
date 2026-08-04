// Copyright 2026 ViennaPS
// SPDX-License-Identifier: MIT

#include "device_ray_flux_pipeline.hpp"

#include <algorithm>
#include <bit>
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

  const auto require = [](const bool condition, const std::string_view stage,
                          const std::string &error) {
    if (condition)
      return true;
    std::cerr << "device ray-flux pipeline Vulkan dispatch FAIL [" << stage
              << "]";
    if (!error.empty())
      std::cerr << ": " << error;
    std::cerr << '\n';
    return false;
  };
  const auto require_result = [&require](const bool result,
                                         const std::string_view stage,
                                         const std::string &error) {
    return require(result, stage, error);
  };
  const auto require_equal = [&require](const bool condition,
                                        const std::string_view stage) {
    return require(condition, stage, {});
  };

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
  if (!require_result(pipeline.runCpu(rays, triangles, weights, cpu, error),
                      "cpu normal run", error) ||
      !require_equal(cpu.count == 2U && cpuSurface[0] == 0U &&
                         cpuSurface[1] == 2U,
                     "cpu normal hit/count") ||
      !require_equal(std::bit_cast<std::uint32_t>(cpuWeight[0]) == 0x40400000U,
                     "cpu normal weight[0] bit pattern") ||
      !require_equal(std::bit_cast<std::uint32_t>(cpuWeight[1]) == 0x80000000U,
                     "cpu normal weight[1] bit pattern"))
    return 1;
  for (std::size_t i = cpu.count; i < cpuSurface.size(); ++i)
    if (!require_equal(cpuSurface[i] == kSurfaceSentinel &&
                           cpuWeight[i] == kWeightSentinel,
                       "cpu normal tail sentinel"))
      return 1;

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
      spirv.surfaceSegments.empty() || spirv.surfaceReduce.empty() ||
      spirv.triangleBvh.empty()) {
    std::cerr << "device ray-flux pipeline Vulkan dispatch FAIL [SPIR-V]"
              << ": all required SPIR-V paths must be configured\n";
    return 1;
  }
  if (!require_result(pipeline.initialize(spirv, error), "initialize", error))
    return 1;

  std::vector<std::uint32_t> gpuSurface(rays.size(), kSurfaceSentinel);
  std::vector<float> gpuWeight(rays.size(), kWeightSentinel);
  RayFluxResult gpu{gpuSurface, gpuWeight, 99U};
  if (!require_result(pipeline.runGpu(rays, triangles, weights, gpu, error),
                      "gpu normal run", error) ||
      !require_equal(pipeline.lastComputeSubmissionCount() == 1U,
                     "gpu normal submission count") ||
      !require_equal(gpu.count == cpu.count, "gpu normal count"))
    return 1;
  for (std::size_t i = 0U; i < gpu.count; ++i) {
    if (!require_equal(gpuSurface[i] == cpuSurface[i],
                       "gpu normal surface result") ||
        !require_equal(std::bit_cast<std::uint32_t>(gpuWeight[i]) ==
                           std::bit_cast<std::uint32_t>(cpuWeight[i]),
                       "gpu normal weight bit pattern"))
      return 1;
  }
  for (std::size_t i = gpu.count; i < gpuSurface.size(); ++i)
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
      return 1;
  }

  const std::vector<float> overflowWeights{
      std::numeric_limits<float>::max(), -0.0F, 0.5F,
      std::numeric_limits<float>::max(), 2.0F};
  std::vector<std::uint32_t> overflowCpuSurface(rays.size(), kSurfaceSentinel);
  std::vector<float> overflowCpuWeight(rays.size(), kWeightSentinel);
  RayFluxResult overflowCpu{overflowCpuSurface, overflowCpuWeight, 73U};
  if (!require_result(!pipeline.runCpu(rays, triangles, overflowWeights,
                                       overflowCpu, error),
                      "cpu FLT_MAX fail-closed", error) ||
      !require_equal(overflowCpu.count == 73U &&
                         overflowCpuSurface[0] == kSurfaceSentinel &&
                         overflowCpuWeight[0] == kWeightSentinel,
                     "cpu FLT_MAX output unchanged"))
    return 1;
  std::vector<std::uint32_t> overflowGpuSurface(rays.size(), kSurfaceSentinel);
  std::vector<float> overflowGpuWeight(rays.size(), kWeightSentinel);
  RayFluxResult overflowGpu{overflowGpuSurface, overflowGpuWeight, 73U};
  if (!require_result(!pipeline.runGpu(rays, triangles, overflowWeights,
                                       overflowGpu, error),
                      "gpu FLT_MAX fail-closed", error) ||
      !require_equal(overflowGpu.count == 73U &&
                         overflowGpuSurface[0] == kSurfaceSentinel &&
                         overflowGpuWeight[0] == kWeightSentinel,
                     "gpu FLT_MAX output unchanged"))
    return 1;

  const auto priorSurface = gpuSurface;
  const auto priorWeight = gpuWeight;
  const auto priorCount = gpu.count;
  std::vector<float> malformed = weights;
  malformed[0] = std::numeric_limits<float>::quiet_NaN();
  if (!require_result(!pipeline.runGpu(rays, triangles, malformed, gpu, error),
                      "gpu malformed transaction rejection", error) ||
      !require_equal(gpu.count == priorCount && gpuSurface == priorSurface &&
                         gpuWeight == priorWeight,
                     "gpu malformed transaction unchanged"))
    return 1;

  std::vector<std::uint32_t> shortSurface(1U, 0xfaceb00cU);
  std::vector<float> shortWeight(1U, 19.0F);
  RayFluxResult shortResult{shortSurface, shortWeight, 71U};
  if (!require_result(!pipeline.runCpu(rays, triangles, weights, shortResult,
                                       error),
                      "cpu short transaction rejection", error) ||
      !require_equal(shortResult.count == 71U && shortSurface[0] == 0xfaceb00cU &&
                         shortWeight[0] == 19.0F,
                     "cpu short transaction unchanged") ||
      !require_result(!pipeline.runGpu(rays, triangles, weights, shortResult,
                                       error),
                      "gpu short transaction rejection", error) ||
      !require_equal(shortResult.count == 71U && shortSurface[0] == 0xfaceb00cU &&
                         shortWeight[0] == 19.0F,
                     "gpu short transaction unchanged"))
    return 1;

  std::vector<std::uint32_t> emptySurface(2U, 0x12345678U);
  std::vector<float> emptyWeight(2U, -11.0F);
  RayFluxResult empty{emptySurface, emptyWeight, 42U};
  if (!require_result(pipeline.runGpu({}, {}, {}, empty, error),
                      "gpu empty transaction", error) ||
      !require_equal(empty.count == 42U && emptySurface[0] == 0x12345678U &&
                         emptyWeight[1] == -11.0F,
                     "gpu empty transaction unchanged"))
    return 1;

  std::vector<std::uint32_t> noGeometrySurface(rays.size(), kSurfaceSentinel);
  std::vector<float> noGeometryWeight(rays.size(), kWeightSentinel);
  RayFluxResult noGeometry{noGeometrySurface, noGeometryWeight, 61U};
  if (!require_result(pipeline.runGpu(rays, {}, weights, noGeometry, error),
                      "gpu non-empty rays with empty triangles", error) ||
      !require_equal(noGeometry.count == 0U &&
                         pipeline.lastComputeSubmissionCount() == 0U &&
                         noGeometrySurface == std::vector<std::uint32_t>(
                                                  rays.size(), kSurfaceSentinel) &&
                         noGeometryWeight ==
                             std::vector<float>(rays.size(), kWeightSentinel),
                     "gpu empty geometry output and submission state"))
    return 1;

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
  std::vector<std::uint32_t> tieGpuSurface(tieRays.size(), kSurfaceSentinel);
  std::vector<float> tieGpuWeight(tieRays.size(), kWeightSentinel);
  RayFluxResult tieGpu{tieGpuSurface, tieGpuWeight, 0U};
  if (!require_result(
          pipeline.runCpu(tieRays, tieTriangles, tieWeights, tieCpu, error),
          "tie cpu run", error) ||
      !require_result(
          pipeline.runGpu(tieRays, tieTriangles, tieWeights, tieGpu, error),
          "tie gpu run", error) ||
      !require_equal(pipeline.lastComputeSubmissionCount() == 1U,
                     "tie gpu submission count"))
    return 1;
  const bool tieSurface9 =
      std::find(tieGpuSurface.begin(), tieGpuSurface.begin() + tieGpu.count,
                9U) != tieGpuSurface.begin() + tieGpu.count;
  if (!require_equal(tieGpu.count == tieCpu.count && tieSurface9,
                     "tie hit/count"))
    return 1;
  for (std::size_t i = 0; i < tieGpu.count; ++i)
    if (!require_equal(tieGpuSurface[i] == tieCpuSurface[i],
                       "tie surface bitwise result") ||
        !require_equal(std::bit_cast<std::uint32_t>(tieGpuWeight[i]) ==
                           std::bit_cast<std::uint32_t>(tieCpuWeight[i]),
                       "tie weight bitwise result"))
      return 1;

  if (!require_result(pipeline.prepareGeometry(tieTriangles, error),
                      "BVH prepare", error))
    return 1;
  std::vector<std::uint32_t> preparedSurface(tieRays.size(), kSurfaceSentinel);
  std::vector<float> preparedWeight(tieRays.size(), kWeightSentinel);
  RayFluxResult prepared{preparedSurface, preparedWeight, 0U};
  if (!require_result(pipeline.runGpuPrepared(tieRays, tieWeights, prepared,
                                              error),
                      "prepared BVH run", error) ||
      !require_equal(pipeline.lastComputeSubmissionCount() == 1U,
                     "prepared BVH submission count"))
    return 1;
  const auto preparedSurfaceFirst = preparedSurface;
  const auto preparedWeightFirst = preparedWeight;
  const auto preparedCountFirst = prepared.count;
  auto invalidCountTriangles = tieTriangles;
  invalidCountTriangles.pop_back();
  if (!require_result(!pipeline.refitPreparedGeometry(invalidCountTriangles,
                                                      error),
                      "BVH refit malformed triangle count", error) ||
      !require_result(pipeline.runGpuPrepared(tieRays, tieWeights, prepared,
                                              error),
                      "prepared BVH after malformed refit", error) ||
      !require_equal(prepared.count == preparedCountFirst &&
                         preparedSurface == preparedSurfaceFirst &&
                         preparedWeight == preparedWeightFirst,
                     "prepared BVH unchanged after malformed refit"))
    return 1;
  auto invalidNanTriangles = tieTriangles;
  invalidNanTriangles[0].a[0] = std::numeric_limits<float>::quiet_NaN();
  if (!require_result(!pipeline.refitPreparedGeometry(invalidNanTriangles,
                                                      error),
                      "BVH refit NaN rejection", error) ||
      !require_result(pipeline.runGpuPrepared(tieRays, tieWeights, prepared,
                                              error),
                      "prepared BVH after NaN refit", error) ||
      !require_equal(prepared.count == preparedCountFirst &&
                         preparedSurface == preparedSurfaceFirst &&
                         preparedWeight == preparedWeightFirst,
                     "prepared BVH unchanged after NaN refit"))
    return 1;
  auto invalidSubnormalTriangles = tieTriangles;
  invalidSubnormalTriangles[0].a[0] = std::numeric_limits<float>::denorm_min();
  if (!require_result(
          !pipeline.refitPreparedGeometry(invalidSubnormalTriangles, error),
          "BVH refit subnormal rejection", error) ||
      !require_result(pipeline.runGpuPrepared(tieRays, tieWeights, prepared,
                                              error),
                      "prepared BVH after subnormal refit", error) ||
      !require_equal(prepared.count == preparedCountFirst &&
                         preparedSurface == preparedSurfaceFirst &&
                         preparedWeight == preparedWeightFirst,
                     "prepared BVH unchanged after subnormal refit"))
    return 1;
  auto movedTieTriangles = tieTriangles;
  for (auto &point : {&movedTieTriangles[9].a, &movedTieTriangles[9].b,
                      &movedTieTriangles[9].c})
    (*point)[0] += 8.0F;
  std::vector<std::uint32_t> movedTieCpuSurface(tieRays.size(),
                                                kSurfaceSentinel);
  std::vector<float> movedTieCpuWeight(tieRays.size(), kWeightSentinel);
  RayFluxResult movedTieCpu{movedTieCpuSurface, movedTieCpuWeight, 0U};
  if (!require_result(pipeline.runCpu(tieRays, movedTieTriangles, tieWeights,
                                      movedTieCpu, error),
                      "moved BVH cpu reference", error) ||
      !require_result(pipeline.refitPreparedGeometry(movedTieTriangles, error),
                      "BVH refit moved geometry", error) ||
      !require_equal(pipeline.lastComputeSubmissionCount() == 1U,
                     "BVH refit submission count"))
    return 1;
  std::vector<std::uint32_t> movedTieGpuSurface(tieRays.size(),
                                                kSurfaceSentinel);
  std::vector<float> movedTieGpuWeight(tieRays.size(), kWeightSentinel);
  RayFluxResult movedTieGpu{movedTieGpuSurface, movedTieGpuWeight, 0U};
  if (!require_result(
          pipeline.runGpuPrepared(tieRays, tieWeights, movedTieGpu, error),
          "moved prepared BVH run", error) ||
      !require_equal(pipeline.lastComputeSubmissionCount() == 1U,
                     "moved prepared BVH submission count") ||
      !require_equal(movedTieGpu.count == movedTieCpu.count,
                     "moved prepared BVH count"))
    return 1;
  for (std::size_t i = 0U; i < movedTieGpu.count; ++i)
    if (!require_equal(movedTieGpuSurface[i] == movedTieCpuSurface[i],
                       "moved prepared BVH surface result") ||
        !require_equal(std::bit_cast<std::uint32_t>(movedTieGpuWeight[i]) ==
                           std::bit_cast<std::uint32_t>(movedTieCpuWeight[i]),
                       "moved prepared BVH weight bit pattern"))
      return 1;
  if (!require_result(pipeline.refitPreparedGeometry(tieTriangles, error),
                      "BVH refit reset geometry", error) ||
      !require_equal(pipeline.lastComputeSubmissionCount() == 1U,
                     "BVH reset refit submission count") ||
      !require_result(pipeline.runGpuPrepared(tieRays, tieWeights, prepared,
                                              error),
                      "prepared BVH reset run", error) ||
      !require_equal(pipeline.lastComputeSubmissionCount() == 1U,
                     "prepared BVH reset submission count") ||
      !require_equal(prepared.count == tieCpu.count &&
                         preparedSurface == preparedSurfaceFirst &&
                         preparedWeight == preparedWeightFirst,
                     "prepared BVH reset result"))
    return 1;
  std::vector<std::uint32_t> defaultSurface(tieRays.size(), kSurfaceSentinel);
  std::vector<float> defaultWeight(tieRays.size(), kWeightSentinel);
  RayFluxResult defaultGpu{defaultSurface, defaultWeight, 0U};
  if (!require_result(pipeline.runGpu(tieRays, movedTieTriangles, tieWeights,
                                      defaultGpu, error),
                      "default moved-geometry GPU run", error) ||
      !require_equal(pipeline.lastComputeSubmissionCount() == 1U &&
                         defaultGpu.count == movedTieCpu.count,
                     "default moved-geometry GPU result"))
    return 1;
  for (std::size_t i = 0U; i < defaultGpu.count; ++i)
    if (!require_equal(defaultSurface[i] == movedTieCpuSurface[i],
                       "default moved-geometry surface result") ||
        !require_equal(std::bit_cast<std::uint32_t>(defaultWeight[i]) ==
                           std::bit_cast<std::uint32_t>(movedTieCpuWeight[i]),
                       "default moved-geometry weight bit pattern"))
      return 1;
  std::vector<std::uint32_t> invalidatedSurface(tieRays.size(),
                                                kSurfaceSentinel);
  std::vector<float> invalidatedWeight(tieRays.size(), kWeightSentinel);
  RayFluxResult invalidatedPrepared{invalidatedSurface, invalidatedWeight,
                                    76U};
  if (!require_result(
          !pipeline.runGpuPrepared(tieRays, tieWeights, invalidatedPrepared,
                                   error),
          "invalidated prepared BVH rejection", error) ||
      !require_equal(pipeline.lastComputeSubmissionCount() == 0U &&
                         invalidatedPrepared.count == 76U &&
                         invalidatedSurface[0] == kSurfaceSentinel &&
                         invalidatedWeight[0] == kWeightSentinel,
                     "invalidated prepared BVH unchanged") ||
      !require_result(!pipeline.prepareGeometry({}, error),
                      "empty BVH prepare rejection", error))
    return 1;
  std::vector<std::uint32_t> failedPrepareSurface(tieRays.size(),
                                                  kSurfaceSentinel);
  std::vector<float> failedPrepareWeight(tieRays.size(), kWeightSentinel);
  RayFluxResult failedPrepare{failedPrepareSurface, failedPrepareWeight, 74U};
  if (!require_result(!pipeline.runGpuPrepared(tieRays, tieWeights,
                                               failedPrepare, error),
                      "prepared BVH after failed prepare", error) ||
      !require_equal(failedPrepare.count == 74U &&
                         failedPrepareSurface[0] == kSurfaceSentinel &&
                         failedPrepareWeight[0] == kWeightSentinel,
                     "prepared BVH failed-prepare output unchanged"))
    return 1;
  pipeline.resetPreparedGeometry();
  std::vector<std::uint32_t> resetSurface(tieRays.size(), kSurfaceSentinel);
  std::vector<float> resetWeight(tieRays.size(), kWeightSentinel);
  RayFluxResult resetResult{resetSurface, resetWeight, 73U};
  if (!require_result(!pipeline.runGpuPrepared(tieRays, tieWeights, resetResult,
                                               error),
                      "prepared BVH after reset", error) ||
      !require_equal(resetResult.count == 73U &&
                         resetSurface[0] == kSurfaceSentinel &&
                         resetWeight[0] == kWeightSentinel,
                     "prepared BVH reset output unchanged"))
    return 1;
  std::cout << "device ray-flux pipeline Vulkan dispatch PASS\n";
  return 0;
}
