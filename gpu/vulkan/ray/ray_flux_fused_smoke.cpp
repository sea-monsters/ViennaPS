// Copyright 2026 ViennaPS
// SPDX-License-Identifier: MIT

#include "ray_flux_fused.hpp"

#include <array>
#include <bit>
#include <cassert>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <string>
#include <vector>

#ifndef VIENNAPS_VULKAN_RAY_FLUX_FUSED_SPV_PATH
#error "The fused ray-flux smoke requires a generated SPIR-V path."
#endif

using namespace viennaps::vulkan::ray;

namespace {
std::uint32_t bits(const float value) {
  return std::bit_cast<std::uint32_t>(value);
}

void require(bool value, const char *message) {
  if (!value) {
    std::cerr << "fused ray flux smoke failure: " << message << '\n';
    std::exit(1);
  }
}
} // namespace

int main() {
  const std::vector<Triangle> triangles = {
      {{{-2.0F, -1.0F, 0.0F}}, {{-1.0F, -1.0F, 0.0F}}, {{-2.0F, 1.0F, 0.0F}}},
      {{{0.0F, -1.0F, 0.0F}}, {{1.0F, -1.0F, 0.0F}}, {{0.0F, 1.0F, 0.0F}}},
      {{{2.0F, -1.0F, 0.0F}}, {{3.0F, -1.0F, 0.0F}}, {{2.0F, 1.0F, 0.0F}}}};
  const std::vector<Ray> rays = {
      {{{-1.75F, 0.0F, -1.0F}}, {{0.0F, 0.0F, 1.0F}}, 0.0F, 10.0F},
      {{{-1.5F, 0.0F, -1.0F}}, {{0.0F, 0.0F, 1.0F}}, 0.0F, 10.0F},
      {{{0.25F, 0.0F, -1.0F}}, {{0.0F, 0.0F, 1.0F}}, 0.0F, 10.0F},
      {{{2.25F, 0.0F, -1.0F}}, {{0.0F, 0.0F, 1.0F}}, 0.0F, 10.0F},
      {{{5.0F, 0.0F, -1.0F}}, {{0.0F, 0.0F, 1.0F}}, 0.0F, 10.0F}};
  const std::vector<float> weights = {1.0F, 2.0F, -0.0F, -1.0F, 4.0F};

  FusedRayFluxPrimitive primitive;
  std::string error;
  require(primitive.initialize(VIENNAPS_VULKAN_RAY_FLUX_FUSED_SPV_PATH, error),
          error.c_str());

  std::array<std::uint32_t, 8U> cpuSurface{};
  std::array<float, 8U> cpuWeight{};
  RayFluxFusedResult cpu{cpuSurface, cpuWeight, 0U};
  require(primitive.runCpu(rays, triangles, weights, cpu, error),
          error.c_str());
  require(cpu.count == 3U && cpuSurface[0] == 0U && cpuSurface[1] == 1U &&
              cpuSurface[2] == 2U && bits(cpuWeight[0]) == bits(3.0F) &&
              bits(cpuWeight[1]) == bits(-0.0F) &&
              bits(cpuWeight[2]) == bits(-1.0F),
          "CPU fixture mismatch");

  std::array<std::uint32_t, 8U> gpuSurface{};
  std::array<float, 8U> gpuWeight{};
  gpuSurface.fill(0xdecafbadU);
  gpuWeight.fill(std::numeric_limits<float>::quiet_NaN());
  RayFluxFusedResult gpu{gpuSurface, gpuWeight, 91U};
  require(primitive.runGpu(rays, triangles, weights, gpu, error),
          error.c_str());
  require(gpu.count == cpu.count, "GPU count mismatch");
  for (std::size_t i = 0U; i < cpu.count; ++i)
    require(gpuSurface[i] == cpuSurface[i] &&
                bits(gpuWeight[i]) == bits(cpuWeight[i]),
            "GPU bitwise result mismatch");
  require(gpuSurface[7] == 0xdecafbadU, "GPU tail surface changed");

  const std::vector<Triangle> noTriangles;
  gpuSurface.fill(0xfeedfaceU);
  gpuWeight.fill(-0.0F);
  gpu.count = 73U;
  const bool noTriangleSucceeded =
      primitive.runGpu(rays, noTriangles, weights, gpu, error);
  require(!noTriangleSucceeded, "empty surface domain accepted");
  require(gpu.count == 73U && gpuSurface[0] == 0xfeedfaceU &&
              bits(gpuWeight[0]) == bits(-0.0F),
          "empty surface domain mutated output data");

  std::array<std::uint32_t, 1U> smallSurface{{0xaaaaU}};
  std::array<float, 1U> smallWeight{{7.0F}};
  RayFluxFusedResult small{smallSurface, smallWeight, 44U};
  require(!primitive.runGpu(rays, triangles, weights, small, error),
          "insufficient capacity accepted");
  require(small.count == 44U && smallSurface[0] == 0xaaaaU &&
              smallWeight[0] == 7.0F,
          "capacity failure mutated output");

  auto badWeights = weights;
  badWeights[0] = std::numeric_limits<float>::quiet_NaN();
  gpu.count = 55U;
  gpuSurface[0] = 0xbeefU;
  require(!primitive.runGpu(rays, triangles, badWeights, gpu, error),
          "NaN weight accepted");
  require(gpu.count == 55U && gpuSurface[0] == 0xbeefU,
          "NaN failure mutated output");

  std::array<std::uint32_t, 1U> emptySurface{{0x1234U}};
  std::array<float, 1U> emptyWeight{{-0.0F}};
  RayFluxFusedResult empty{emptySurface, emptyWeight, 17U};
  require(primitive.runCpu({}, triangles, {}, empty, error), error.c_str());
  require(primitive.runGpu({}, triangles, {}, empty, error), error.c_str());
  require(empty.count == 17U && emptySurface[0] == 0x1234U &&
              bits(emptyWeight[0]) == bits(-0.0F),
          "N=0 changed output");
  std::cout << "fused ray flux Vulkan dispatch PASS\n";
  return 0;
}
