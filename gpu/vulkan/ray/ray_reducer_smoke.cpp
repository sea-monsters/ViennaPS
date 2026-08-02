// Copyright 2026 ViennaPS
// SPDX-License-Identifier: MIT

#include "ray_reducer.hpp"

#include <bit>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <limits>
#include <span>
#include <string>
#include <vector>

#ifndef VIENNAPS_VULKAN_RAY_SPV_PATH
#define VIENNAPS_VULKAN_RAY_SPV_PATH ""
#endif

int main() {
  using viennaps::vulkan::ray::RayRecordSoA;
  using viennaps::vulkan::ray::RayReduction;
  const std::vector<std::uint32_t> rayIds{3U, 1U, 2U, 0U};
  const std::vector<std::uint32_t> surfaces{4U, 2U, 4U, 2U};
  const std::vector<float> weights{0.25F, 1.0F, 0.5F, 2.0F};
  const RayRecordSoA records{rayIds, surfaces, weights, {}};
  std::vector<std::uint32_t> outputSurfaces(2U);
  std::vector<float> outputWeights(2U);
  RayReduction output{outputSurfaces, outputWeights, 0U};
  std::string error;
  if (!viennaps::vulkan::ray::reduceCpu(records, 8U, output, error)) {
    std::cerr << error << '\n';
    return 1;
  }
  assert(output.count == 2U && outputSurfaces[0] == 2U &&
         outputSurfaces[1] == 4U);
  // Unaligned tail and repeated execution are part of the oracle contract.
  std::vector<std::uint32_t> tailSurfaces(4U, 99U);
  std::vector<float> tailWeights(4U, -7.0F);
  const std::vector<std::uint32_t> tailRayIds{5U, 4U, 6U};
  const std::vector<std::uint32_t> tailInputSurfaces{1U, 1U, 1U};
  const std::vector<float> tailInputWeights{0.25F, 0.5F, 1.0F};
  RayReduction tail{tailSurfaces, tailWeights, 0U};
  assert(viennaps::vulkan::ray::reduceCpu(
      RayRecordSoA{tailRayIds, tailInputSurfaces, tailInputWeights, {}}, 2U,
      tail, error));
  assert(tail.count == 1U && tailSurfaces[0] == 1U && tailWeights[0] == 1.75F);

  std::vector<std::uint32_t> emptySurface(1U, 77U);
  std::vector<float> emptyWeight(1U, 88.0F);
  RayReduction empty{emptySurface, emptyWeight, 9U};
  assert(viennaps::vulkan::ray::reduceCpu(RayRecordSoA{{}, {}, {}, {}}, 0U,
                                          empty, error));
  assert(empty.count == 0U && emptySurface[0] == 77U &&
         emptyWeight[0] == 88.0F);

  auto rejectsWithoutWrite = [&](const RayRecordSoA &bad,
                                 const std::uint32_t domain,
                                 const std::size_t capacity) {
    std::vector<std::uint32_t> ids(capacity, 0xdeadbeefU);
    std::vector<float> values(capacity, 123.0F);
    RayReduction rejected{ids, values, 123U};
    assert(!viennaps::vulkan::ray::reduceCpu(bad, domain, rejected, error));
    assert(rejected.count == 123U &&
           ids == std::vector<std::uint32_t>(capacity, 0xdeadbeefU));
    assert(values == std::vector<float>(capacity, 123.0F));
  };
  const std::vector<std::uint32_t> badRay{0U};
  const std::vector<std::uint32_t> badSurface{9U};
  const std::vector<float> badWeight{1.0F};
  rejectsWithoutWrite(RayRecordSoA{badRay, badSurface, badWeight, {}}, 2U, 1U);
  const std::vector<std::uint32_t> nanSurface{1U};
  const std::vector<float> nanWeight{std::numeric_limits<float>::quiet_NaN()};
  rejectsWithoutWrite(RayRecordSoA{badRay, nanSurface, nanWeight, {}}, 2U, 1U);
  const std::vector<std::uint32_t> overRay{0U, 1U};
  const std::vector<std::uint32_t> overSurface{0U, 1U};
  const std::vector<float> overWeight{1.0F, 2.0F};
  rejectsWithoutWrite(RayRecordSoA{overRay, overSurface, overWeight, {}}, 2U,
                      1U);

  // Explicit aliasing is rejected before any output write.
  std::vector<std::uint32_t> aliasedIds{0U};
  std::vector<float> aliasedWeights{4.0F};
  RayReduction aliased{std::span<std::uint32_t>(aliasedIds), aliasedWeights,
                       8U};
  assert(!viennaps::vulkan::ray::reduceCpu(
      RayRecordSoA{aliasedIds, aliasedIds, aliasedWeights, {}}, 1U, aliased,
      error));
  assert(aliased.count == 8U);

  // When a Vulkan SDK/compiler is available, exercise the real one-dispatch
  // path against the same oracle.  A missing physical device is reported as a
  // skipped runtime check rather than changing the CPU contract.
  if (std::string_view(VIENNAPS_VULKAN_RAY_SPV_PATH).empty()) {
    std::cout << "ray reducer Vulkan dispatch SKIP (no SPIR-V)\n";
  } else {
    viennaps::vulkan::ray::DeterministicRayReducer reducer;
    if (!reducer.initialize(VIENNAPS_VULKAN_RAY_SPV_PATH, error)) {
      std::cout << "ray reducer Vulkan dispatch SKIP: " << error << '\n';
    } else {
      viennaps::vulkan::runtime::HostVisibleBuffer idsBuffer;
      viennaps::vulkan::runtime::HostVisibleBuffer surfacesBuffer;
      viennaps::vulkan::runtime::HostVisibleBuffer weightsBuffer;
      viennaps::vulkan::runtime::HostVisibleBuffer outputSurfaceBuffer;
      viennaps::vulkan::runtime::HostVisibleBuffer outputWeightBuffer;
      assert(reducer.createRayIdBuffer(rayIds.size(), idsBuffer, error));
      assert(reducer.createSurfaceIdBuffer(surfaces.size(), surfacesBuffer,
                                           error));
      assert(reducer.createWeightBuffer(weights.size(), weightsBuffer, error));
      assert(reducer.createSurfaceIdBuffer(3U, outputSurfaceBuffer, error));
      assert(reducer.createWeightBuffer(3U, outputWeightBuffer, error));
      assert(idsBuffer.write(rayIds.data(),
                             rayIds.size() * sizeof(std::uint32_t), 0U, error));
      assert(surfacesBuffer.write(
          surfaces.data(), surfaces.size() * sizeof(std::uint32_t), 0U, error));
      assert(weightsBuffer.write(weights.data(), weights.size() * sizeof(float),
                                 0U, error));
      const std::vector<std::uint32_t> surfaceSentinels{0xfeedU, 0xfeedU,
                                                        0xfeedU};
      const std::vector<float> weightSentinels{-99.0F, -99.0F, -99.0F};
      assert(outputSurfaceBuffer.write(
          surfaceSentinels.data(),
          surfaceSentinels.size() * sizeof(std::uint32_t), 0U, error));
      assert(outputWeightBuffer.write(weightSentinels.data(),
                                      weightSentinels.size() * sizeof(float),
                                      0U, error));
      std::size_t outputCount = 0U;
      assert(reducer.reduce(idsBuffer, surfacesBuffer, weightsBuffer,
                            rayIds.size(), 8U, outputSurfaceBuffer,
                            outputWeightBuffer, 3U, outputCount, error));
      assert(outputCount == 2U);
      std::vector<std::uint32_t> gpuSurfaces(3U, 0xfeedU);
      std::vector<float> gpuWeights(3U, -99.0F);
      assert(outputSurfaceBuffer.read(
          gpuSurfaces.data(), gpuSurfaces.size() * sizeof(std::uint32_t), 0U,
          error));
      assert(outputWeightBuffer.read(
          gpuWeights.data(), gpuWeights.size() * sizeof(float), 0U, error));
      assert(gpuSurfaces[0] == outputSurfaces[0] &&
             gpuSurfaces[1] == outputSurfaces[1] && gpuSurfaces[2] == 0xfeedU);
      assert(std::bit_cast<std::uint32_t>(gpuWeights[0]) ==
                 std::bit_cast<std::uint32_t>(outputWeights[0]) &&
             std::bit_cast<std::uint32_t>(gpuWeights[1]) ==
                 std::bit_cast<std::uint32_t>(outputWeights[1]) &&
             gpuWeights[2] == -99.0F);

      // Re-run with a shuffled order and require bit-exact, deterministic
      // output.
      const std::vector<std::uint32_t> shuffledRay{2U, 0U, 3U, 1U};
      const std::vector<std::uint32_t> shuffledSurface{4U, 2U, 4U, 2U};
      const std::vector<float> shuffledWeight{0.5F, 2.0F, 0.25F, 1.0F};
      assert(idsBuffer.write(shuffledRay.data(),
                             shuffledRay.size() * sizeof(std::uint32_t), 0U,
                             error));
      assert(surfacesBuffer.write(
          shuffledSurface.data(),
          shuffledSurface.size() * sizeof(std::uint32_t), 0U, error));
      assert(weightsBuffer.write(shuffledWeight.data(),
                                 shuffledWeight.size() * sizeof(float), 0U,
                                 error));
      const auto priorCount = outputCount;
      assert(reducer.reduce(idsBuffer, surfacesBuffer, weightsBuffer,
                            shuffledRay.size(), 8U, outputSurfaceBuffer,
                            outputWeightBuffer, 3U, outputCount, error));
      assert(outputCount == priorCount);
      std::vector<float> shuffledOutput(3U, -99.0F);
      assert(outputWeightBuffer.read(shuffledOutput.data(),
                                     shuffledOutput.size() * sizeof(float), 0U,
                                     error));
      assert(std::bit_cast<std::uint32_t>(shuffledOutput[0]) ==
                 std::bit_cast<std::uint32_t>(gpuWeights[0]) &&
             std::bit_cast<std::uint32_t>(shuffledOutput[1]) ==
                 std::bit_cast<std::uint32_t>(gpuWeights[1]) &&
             shuffledOutput[2] == -99.0F);

      // Capacity failure is transactional: count and output sentinels remain.
      const auto unchangedCount = outputCount;
      const auto unchangedTail = shuffledOutput[2];
      assert(!reducer.reduce(idsBuffer, surfacesBuffer, weightsBuffer,
                             shuffledRay.size(), 8U, outputSurfaceBuffer,
                             outputWeightBuffer, 1U, outputCount, error));
      assert(outputCount == unchangedCount);
      std::vector<float> failedOutput(3U, 0.0F);
      assert(outputWeightBuffer.read(
          failedOutput.data(), failedOutput.size() * sizeof(float), 0U, error));
      assert(failedOutput[2] == unchangedTail);

      const std::vector<std::uint32_t> invalidSurface{99U, 1U, 4U, 2U};
      assert(surfacesBuffer.write(invalidSurface.data(),
                                  invalidSurface.size() * sizeof(std::uint32_t),
                                  0U, error));
      const auto invalidCount = outputCount;
      assert(!reducer.reduce(idsBuffer, surfacesBuffer, weightsBuffer,
                             shuffledRay.size(), 8U, outputSurfaceBuffer,
                             outputWeightBuffer, 3U, outputCount, error));
      assert(outputCount == invalidCount);
      std::cout << "ray reducer Vulkan dispatch PASS\n";
    }
  }
  std::cout << "ray reducer CPU oracle PASS\n";
}
