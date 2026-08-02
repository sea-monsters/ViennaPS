// Copyright 2026 ViennaPS
// SPDX-License-Identifier: MIT

#include "ray_flux_pipeline.hpp"

#include <array>
#include <bit>
#include <cassert>
#include <cstdint>
#include <iostream>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

#ifndef VIENNAPS_VULKAN_TRIANGLE_HIT_SPV_PATH
#define VIENNAPS_VULKAN_TRIANGLE_HIT_SPV_PATH ""
#endif
#ifndef VIENNAPS_VULKAN_RAY_HIT_BATCH_SPV_PATH
#define VIENNAPS_VULKAN_RAY_HIT_BATCH_SPV_PATH ""
#endif
#ifndef VIENNAPS_VULKAN_RAY_SPV_PATH
#define VIENNAPS_VULKAN_RAY_SPV_PATH ""
#endif

int main() {
  using namespace viennaps::vulkan::ray;
  const std::vector<Ray> rays{
      {{{0.25F, 0.25F, 1.0F}}, {{0.0F, 0.0F, -1.0F}}, 0.0F, 10.0F},
      {{{0.30F, 0.20F, 1.0F}}, {{0.0F, 0.0F, -1.0F}}, 0.0F, 10.0F},
      {{{0.25F, 0.25F, 1.0F}}, {{0.0F, 0.0F, 1.0F}}, 0.0F, 10.0F},
      {{{0.75F, 0.10F, 1.0F}}, {{0.0F, 0.0F, -1.0F}}, 0.0F, 10.0F},
      {{{2.25F, 2.25F, 1.0F}}, {{0.0F, 0.0F, -1.0F}}, 0.0F, 10.0F}};
  const std::vector<Triangle> triangles{
      {{{0.0F, 0.0F, 0.0F}}, {{1.0F, 0.0F, 0.0F}}, {{0.0F, 1.0F, 0.0F}}},
      {{{0.0F, 0.0F, -2.0F}}, {{1.0F, 0.0F, -2.0F}}, {{0.0F, 1.0F, -2.0F}}},
      {{{2.0F, 2.0F, 0.0F}}, {{3.0F, 2.0F, 0.0F}}, {{2.0F, 3.0F, 0.0F}}}};
  const std::vector<float> weights{1.0F, -0.0F, 0.5F, 2.0F, -0.0F};
  std::string error;
  RayFluxPipeline pipeline;
  std::vector<std::uint32_t> cpuSurface(3U, 0xdeadbeefu);
  std::vector<float> cpuWeight(3U, -7.0F);
  RayFluxResult cpu{cpuSurface, cpuWeight, 99U};
  assert(pipeline.runCpu(rays, triangles, weights, cpu, error));
  assert(cpu.count == 2U && cpuSurface[0] == 0U && cpuSurface[1] == 2U);
  assert(cpuWeight[0] == 3.0F && std::bit_cast<std::uint32_t>(cpuWeight[1]) ==
                                     std::bit_cast<std::uint32_t>(-0.0F));

  // A pipeline with no shader paths is skipped for the GPU half; the CPU
  // composition above remains the deterministic oracle.
  if (std::string_view(VIENNAPS_VULKAN_TRIANGLE_HIT_SPV_PATH).empty() ||
      std::string_view(VIENNAPS_VULKAN_RAY_HIT_BATCH_SPV_PATH).empty() ||
      std::string_view(VIENNAPS_VULKAN_RAY_SPV_PATH).empty()) {
    std::cout << "ray flux pipeline Vulkan dispatch SKIP (no SPIR-V)\n";
    return 0;
  }
  assert(pipeline.initialize(VIENNAPS_VULKAN_TRIANGLE_HIT_SPV_PATH,
                             VIENNAPS_VULKAN_RAY_HIT_BATCH_SPV_PATH,
                             VIENNAPS_VULKAN_RAY_SPV_PATH, error));
  if (!pipeline.isInitialized()) {
    std::cout << "ray flux pipeline Vulkan dispatch SKIP: " << error << '\n';
    return 0;
  }
  std::vector<std::uint32_t> gpuSurface(3U, 0xdeadbeefu);
  std::vector<float> gpuWeight(3U, -7.0F);
  RayFluxResult gpu{gpuSurface, gpuWeight, 99U};
  const bool gpuRan = pipeline.runGpu(rays, triangles, weights, gpu, error);
  if (!gpuRan) {
    std::cerr << "ray flux pipeline GPU failure: " << error << '\n';
  }
  assert(gpuRan);
  assert(gpu.count == cpu.count);
  for (std::size_t i = 0U; i < gpu.count; ++i) {
    assert(gpuSurface[i] == cpuSurface[i]);
    assert(std::bit_cast<std::uint32_t>(gpuWeight[i]) ==
           std::bit_cast<std::uint32_t>(cpuWeight[i]));
  }
  assert(gpuSurface[2] == 0xdeadbeefu && gpuWeight[2] == -7.0F);

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
  assert(pipeline.runCpu({}, {}, {}, empty, error));
  assert(empty.count == 42U && emptySurface[0] == 0x12345678U &&
         emptyWeight[0] == -11.0F);
  assert(pipeline.runGpu({}, {}, {}, empty, error));
  assert(empty.count == 42U && emptySurface[1] == 0x12345678U &&
         emptyWeight[1] == -11.0F);
  std::cout << "ray flux pipeline Vulkan dispatch PASS\n";
}
