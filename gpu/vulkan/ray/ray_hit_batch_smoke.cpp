// Copyright 2026 ViennaPS
// SPDX-License-Identifier: MIT

#include "ray_hit_batch.hpp"
#include "triangle_hit.hpp"

#include <array>
#include <bit>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <limits>
#include <string>
#include <vector>

#ifndef VIENNAPS_VULKAN_RAY_HIT_BATCH_SPV_PATH
#define VIENNAPS_VULKAN_RAY_HIT_BATCH_SPV_PATH ""
#endif
#ifndef VIENNAPS_VULKAN_TRIANGLE_HIT_SPV_PATH
#define VIENNAPS_VULKAN_TRIANGLE_HIT_SPV_PATH ""
#endif

int main() {
  using viennaps::vulkan::ray::RayHitBatch;
  using viennaps::vulkan::ray::TriangleHit;

  const std::vector<TriangleHit> hits{{1.0F, 2U, 0.25F, 0.25F},
                                      TriangleHit::miss(),
                                      {2.0F, 2U, 0.0F, 0.5F},
                                      {3.0F, 1U, 0.5F, 0.0F},
                                      TriangleHit::miss()};
  const std::vector<float> weights{1.0F, 0.0F, -0.0F, 2.0F, 4.0F};
  std::vector<std::uint32_t> rayId(5U, 0xdeadbeefu);
  std::vector<std::uint32_t> surfaceId(5U, 0xdeadbeefu);
  std::vector<float> outputWeight(5U, -7.0F);
  RayHitBatch output{rayId, surfaceId, outputWeight, 99U};
  std::string error;
  assert(viennaps::vulkan::ray::compactCpu(hits, weights, 4U, output, error));
  assert(output.count == 3U);
  assert(rayId[0] == 0U && rayId[1] == 2U && rayId[2] == 3U);
  assert(surfaceId[0] == 2U && surfaceId[1] == 2U && surfaceId[2] == 1U);
  assert(std::bit_cast<std::uint32_t>(outputWeight[1]) ==
         std::bit_cast<std::uint32_t>(-0.0F));
  assert(rayId[3] == 0xdeadbeefu && outputWeight[4] == -7.0F);

  const auto beforeRay = rayId;
  const auto beforeSurface = surfaceId;
  const auto beforeWeight = outputWeight;
  const auto beforeCount = output.count;
  RayHitBatch tooSmall{std::span<std::uint32_t>(rayId.data(), 2U),
                       std::span<std::uint32_t>(surfaceId.data(), 2U),
                       std::span<float>(outputWeight.data(), 2U), beforeCount};
  assert(
      !viennaps::vulkan::ray::compactCpu(hits, weights, 4U, tooSmall, error));
  assert(rayId == beforeRay && surfaceId == beforeSurface &&
         outputWeight == beforeWeight && tooSmall.count == beforeCount);

  std::uint32_t zeroRay = 17U;
  std::uint32_t zeroSurface = 18U;
  float zeroWeight = 19.0F;
  RayHitBatch empty{std::span<std::uint32_t>(&zeroRay, 1U),
                    std::span<std::uint32_t>(&zeroSurface, 1U),
                    std::span<float>(&zeroWeight, 1U), 23U};
  assert(viennaps::vulkan::ray::compactCpu({}, {}, 0U, empty, error));
  assert(zeroRay == 17U && zeroSurface == 18U && zeroWeight == 19.0F &&
         empty.count == 23U);

  auto malformed = hits;
  malformed[1].u = 1.0F;
  malformed[1].triangleIndex = std::numeric_limits<std::uint32_t>::max();
  malformed[1].t = 2.0F;
  assert(!viennaps::vulkan::ray::compactCpu(malformed, weights, 4U, output,
                                            error));
  assert(rayId == beforeRay && surfaceId == beforeSurface &&
         outputWeight == beforeWeight && output.count == beforeCount);

  if (std::string_view(VIENNAPS_VULKAN_RAY_HIT_BATCH_SPV_PATH).empty()) {
    return 0;
  }
  viennaps::vulkan::runtime::ComputeSession sharedSession;
  if (!sharedSession.initialize(error)) {
    std::cout << "ray hit batch Vulkan dispatch SKIP: " << error << '\n';
    return 0;
  }
  viennaps::vulkan::ray::TriangleHitPrimitive hitPrimitive;
  if (!hitPrimitive.initialize(sharedSession,
                               VIENNAPS_VULKAN_TRIANGLE_HIT_SPV_PATH, error)) {
    std::cout << "ray hit batch Vulkan dispatch SKIP: " << error << '\n';
    return 0;
  }
  viennaps::vulkan::ray::RayHitBatchPrimitive primitive;
  if (!primitive.initialize(sharedSession,
                            VIENNAPS_VULKAN_RAY_HIT_BATCH_SPV_PATH, error)) {
    std::cout << "ray hit batch Vulkan dispatch SKIP: " << error << '\n';
    return 0;
  }
  using viennaps::vulkan::ray::Ray;
  using viennaps::vulkan::ray::Triangle;
  const std::vector<Ray> gpuRays{
      {{{0.25F, 0.25F, 1.0F}}, {{0.0F, 0.0F, -1.0F}}, 0.0F, 10.0F},
      {{{0.25F, 0.25F, 1.0F}}, {{0.0F, 0.0F, 1.0F}}, 0.0F, 10.0F},
      {{{0.3F, 0.2F, 1.0F}}, {{0.0F, 0.0F, -1.0F}}, 0.0F, 10.0F},
      {{{0.75F, 0.1F, 1.0F}}, {{0.0F, 0.0F, -1.0F}}, 0.0F, 10.0F},
      {{{0.25F, 0.25F, 1.0F}}, {{0.0F, 0.0F, 1.0F}}, 0.0F, 10.0F}};
  const std::vector<Triangle> gpuTriangles{
      {{{0.0F, 0.0F, -3.0F}}, {{1.0F, 0.0F, -3.0F}}, {{0.0F, 1.0F, -3.0F}}},
      {{{0.0F, 0.0F, -2.0F}}, {{1.0F, 0.0F, -2.0F}}, {{0.0F, 1.0F, -2.0F}}},
      {{{0.0F, 0.0F, 0.0F}}, {{0.6F, 0.0F, 0.0F}}, {{0.0F, 0.6F, 0.0F}}}};
  std::vector<TriangleHit> cpuHits(gpuRays.size(), TriangleHit::miss());
  assert(viennaps::vulkan::ray::intersectCpu(gpuRays, gpuTriangles, cpuHits,
                                             error));
  RayHitBatch expectedBatch{rayId, surfaceId, outputWeight, 0U};
  assert(viennaps::vulkan::ray::compactCpu(cpuHits, weights, 4U, expectedBatch,
                                           error));
  viennaps::vulkan::runtime::HostVisibleBuffer originBuffer;
  viennaps::vulkan::runtime::HostVisibleBuffer directionBuffer;
  viennaps::vulkan::runtime::HostVisibleBuffer triangleBuffer;
  viennaps::vulkan::runtime::HostVisibleBuffer hitBuffer;
  viennaps::vulkan::runtime::HostVisibleBuffer weightBuffer;
  viennaps::vulkan::runtime::HostVisibleBuffer rayBuffer;
  viennaps::vulkan::runtime::HostVisibleBuffer surfaceBuffer;
  viennaps::vulkan::runtime::HostVisibleBuffer batchWeightBuffer;
  assert(hitPrimitive.createRayBuffer(gpuRays.size(), originBuffer,
                                      directionBuffer, error));
  assert(hitPrimitive.createTriangleBuffer(gpuTriangles.size(), triangleBuffer,
                                           error));
  assert(hitPrimitive.createHitBuffer(gpuRays.size(), hitBuffer, error));
  assert(primitive.createWeightBuffer(weights.size(), weightBuffer, error));
  assert(primitive.createRayIdBuffer(5U, rayBuffer, error));
  assert(primitive.createSurfaceIdBuffer(5U, surfaceBuffer, error));
  assert(primitive.createWeightBuffer(5U, batchWeightBuffer, error));
  std::vector<std::array<float, 4>> packedOrigins;
  std::vector<std::array<float, 4>> packedDirections;
  std::vector<std::array<float, 4>> packedTriangles;
  for (const auto &ray : gpuRays) {
    packedOrigins.push_back(
        {ray.origin[0], ray.origin[1], ray.origin[2], ray.tMin});
    packedDirections.push_back(
        {ray.direction[0], ray.direction[1], ray.direction[2], ray.tMax});
  }
  for (const auto &triangle : gpuTriangles) {
    packedTriangles.push_back(
        {triangle.a[0], triangle.a[1], triangle.a[2], 0.0F});
    packedTriangles.push_back(
        {triangle.b[0], triangle.b[1], triangle.b[2], 0.0F});
    packedTriangles.push_back(
        {triangle.c[0], triangle.c[1], triangle.c[2], 0.0F});
  }
  assert(originBuffer.write(packedOrigins.data(),
                            packedOrigins.size() * sizeof(packedOrigins[0]), 0U,
                            error));
  assert(directionBuffer.write(
      packedDirections.data(),
      packedDirections.size() * sizeof(packedDirections[0]), 0U, error));
  assert(triangleBuffer.write(
      packedTriangles.data(),
      packedTriangles.size() * sizeof(packedTriangles[0]), 0U, error));
  assert(hitPrimitive.intersect(originBuffer, directionBuffer, triangleBuffer,
                                gpuRays.size(), gpuTriangles.size(), hitBuffer,
                                gpuRays.size(), error));
  assert(weightBuffer.write(weights.data(), weights.size() * sizeof(float), 0U,
                            error));
  const std::uint32_t idTail = 0xdeadbeefu;
  const float weightTail = -7.0F;
  std::vector<std::uint32_t> initialId(5U, idTail);
  std::vector<std::uint32_t> initialSurface(5U, idTail);
  std::vector<float> initialWeight(5U, weightTail);
  assert(rayBuffer.write(initialId.data(), initialId.size() * sizeof(idTail),
                         0U, error));
  assert(surfaceBuffer.write(initialSurface.data(),
                             initialSurface.size() * sizeof(idTail), 0U,
                             error));
  assert(batchWeightBuffer.write(initialWeight.data(),
                                 initialWeight.size() * sizeof(weightTail), 0U,
                                 error));
  std::size_t gpuCount = 99U;
  assert(primitive.compact(hitBuffer, weightBuffer, gpuRays.size(), 4U,
                           rayBuffer, surfaceBuffer, batchWeightBuffer, 5U,
                           gpuCount, error));
  std::vector<std::uint32_t> gpuRay(5U, 0U);
  std::vector<std::uint32_t> gpuSurface(5U, 0U);
  std::vector<float> gpuWeight(5U, 0.0F);
  assert(rayBuffer.read(gpuRay.data(), gpuRay.size() * sizeof(gpuRay[0]), 0U,
                        error));
  assert(surfaceBuffer.read(
      gpuSurface.data(), gpuSurface.size() * sizeof(gpuSurface[0]), 0U, error));
  assert(batchWeightBuffer.read(
      gpuWeight.data(), gpuWeight.size() * sizeof(gpuWeight[0]), 0U, error));
  assert(gpuCount == expectedBatch.count);
  for (std::size_t i = 0U; i < gpuCount; ++i) {
    assert(gpuRay[i] == rayId[i] && gpuSurface[i] == surfaceId[i] &&
           std::bit_cast<std::uint32_t>(gpuWeight[i]) ==
               std::bit_cast<std::uint32_t>(outputWeight[i]));
  }
  assert(gpuRay[4] == idTail && gpuSurface[4] == idTail &&
         gpuWeight[4] == weightTail);

  const auto snapshotRay = gpuRay;
  const auto snapshotSurface = gpuSurface;
  const auto snapshotWeight = gpuWeight;
  const auto snapshotCount = gpuCount;
  std::vector<TriangleHit> snapshotHits(gpuRays.size());
  assert(hitBuffer.read(snapshotHits.data(),
                        snapshotHits.size() * sizeof(snapshotHits[0]), 0U,
                        error));
  const auto assertUnchanged = [&]() {
    std::vector<std::uint32_t> currentRay(snapshotRay.size());
    std::vector<std::uint32_t> currentSurface(snapshotSurface.size());
    std::vector<float> currentWeight(snapshotWeight.size());
    assert(rayBuffer.read(currentRay.data(),
                          currentRay.size() * sizeof(currentRay[0]), 0U,
                          error));
    assert(surfaceBuffer.read(currentSurface.data(),
                              currentSurface.size() * sizeof(currentSurface[0]),
                              0U, error));
    assert(batchWeightBuffer.read(
        currentWeight.data(), currentWeight.size() * sizeof(currentWeight[0]),
        0U, error));
    assert(currentRay == snapshotRay && currentSurface == snapshotSurface);
    for (std::size_t i = 0U; i < currentWeight.size(); ++i) {
      assert(std::bit_cast<std::uint32_t>(currentWeight[i]) ==
             std::bit_cast<std::uint32_t>(snapshotWeight[i]));
    }
    assert(gpuCount == snapshotCount);
  };
  assert(!primitive.compact(hitBuffer, weightBuffer, gpuRays.size(), 4U,
                            rayBuffer, surfaceBuffer, batchWeightBuffer, 0U,
                            gpuCount, error));
  assertUnchanged();
  auto invalidWeights = weights;
  invalidWeights[0] = std::numeric_limits<float>::quiet_NaN();
  assert(weightBuffer.write(invalidWeights.data(),
                            invalidWeights.size() * sizeof(invalidWeights[0]),
                            0U, error));
  assert(!primitive.compact(hitBuffer, weightBuffer, gpuRays.size(), 4U,
                            rayBuffer, surfaceBuffer, batchWeightBuffer, 5U,
                            gpuCount, error));
  assertUnchanged();
  assert(weightBuffer.write(weights.data(), weights.size() * sizeof(weights[0]),
                            0U, error));
  auto invalidHits = snapshotHits;
  invalidHits[0] = {2.0F, std::numeric_limits<std::uint32_t>::max(), 1.0F,
                    0.0F};
  assert(hitBuffer.write(invalidHits.data(),
                         invalidHits.size() * sizeof(invalidHits[0]), 0U,
                         error));
  assert(!primitive.compact(hitBuffer, weightBuffer, gpuRays.size(), 4U,
                            rayBuffer, surfaceBuffer, batchWeightBuffer, 5U,
                            gpuCount, error));
  assertUnchanged();
  assert(hitBuffer.write(snapshotHits.data(),
                         snapshotHits.size() * sizeof(snapshotHits[0]), 0U,
                         error));
  assert(primitive.compact(hitBuffer, weightBuffer, 0U, 0U, rayBuffer,
                           surfaceBuffer, batchWeightBuffer, 5U, gpuCount,
                           error));
  assertUnchanged();
  std::cout << "ray hit batch Vulkan dispatch PASS\n";
  return 0;
}
