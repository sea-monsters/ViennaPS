// Copyright 2026 ViennaPS
// SPDX-License-Identifier: MIT
#include "ray_hit_batch.hpp"
#include "ray_record_compaction.hpp"
#include "ray_record_radix_sort.hpp"
#include "triangle_hit_device.hpp"

#include <algorithm>
#include <bit>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <span>
#include <vector>

#ifndef VIENNAPS_VULKAN_RAY_RECORD_RADIX_HISTOGRAM_SPV_PATH
#define VIENNAPS_VULKAN_RAY_RECORD_RADIX_HISTOGRAM_SPV_PATH                    \
  "ray_record_radix_histogram.comp.spv"
#endif
#ifndef VIENNAPS_VULKAN_RAY_RECORD_RADIX_PREFIX_SPV_PATH
#define VIENNAPS_VULKAN_RAY_RECORD_RADIX_PREFIX_SPV_PATH                       \
  "ray_record_radix_prefix.comp.spv"
#endif
#ifndef VIENNAPS_VULKAN_RAY_RECORD_RADIX_SCATTER_SPV_PATH
#define VIENNAPS_VULKAN_RAY_RECORD_RADIX_SCATTER_SPV_PATH                      \
  "ray_record_radix_scatter.comp.spv"
#endif
#ifndef VIENNAPS_VULKAN_RAY_RECORD_COMPACTION_SPV_PATH
#define VIENNAPS_VULKAN_RAY_RECORD_COMPACTION_SPV_PATH                         \
  "ray_record_compaction.comp.spv"
#endif
#ifndef VIENNAPS_VULKAN_REDUCTION_SCAN_SPV_PATH
#define VIENNAPS_VULKAN_REDUCTION_SCAN_SPV_PATH "reduction_scan.comp.spv"
#endif
#ifndef VIENNAPS_VULKAN_TRIANGLE_HIT_DEVICE_SPV_PATH
#define VIENNAPS_VULKAN_TRIANGLE_HIT_DEVICE_SPV_PATH                           \
  "triangle_hit_device.comp.spv"
#endif

using namespace viennaps::vulkan::ray;
using viennaps::vulkan::runtime::ComputeSession;
using viennaps::vulkan::runtime::DeviceBuffer;

int main() {
  std::string error;
  ComputeSession session;
  DeviceTriangleHitPrimitive hit;
  DeviceRayRecordCompactor compactor;
  DeviceRayRecordRadixSort sorter;
  if (!session.initialize(error) ||
      !hit.initialize(session, VIENNAPS_VULKAN_TRIANGLE_HIT_DEVICE_SPV_PATH,
                      error) ||
      !compactor.initialize(session,
                            VIENNAPS_VULKAN_RAY_RECORD_COMPACTION_SPV_PATH,
                            VIENNAPS_VULKAN_REDUCTION_SCAN_SPV_PATH, error) ||
      !sorter.initialize(
          session, VIENNAPS_VULKAN_RAY_RECORD_RADIX_HISTOGRAM_SPV_PATH,
          VIENNAPS_VULKAN_RAY_RECORD_RADIX_PREFIX_SPV_PATH,
          VIENNAPS_VULKAN_RAY_RECORD_RADIX_SCATTER_SPV_PATH, error)) {
    std::cerr << error << '\n';
    return 1;
  }
  constexpr std::size_t rayCount = 16'448U;
  const Triangle triangle{{-1, -1, 2}, {1, -1, 2}, {0, 1, 2}};
  std::vector<Triangle> triangles{triangle};
  std::vector<Ray> rays(rayCount);
  for (std::size_t i = 0U; i < rayCount; ++i) {
    const float x = -0.8F + 1.6F * static_cast<float>(i % 128U) / 127.0F;
    const float y = -0.8F + 0.8F * static_cast<float>((i / 128U) % 64U) / 63.0F;
    rays[i].origin = {x, y, 0.0F};
    rays[i].direction = {0.0F, 0.0F, 1.0F};
    rays[i].tMax = 10.0F;
  }
  DeviceBuffer origins, directions, triangleBuffer, hitBuffer, weights;
  if (!hit.createRayBuffers(rayCount, origins, directions, error) ||
      !hit.createTriangleBuffer(triangles.size(), triangleBuffer, error) ||
      !hit.createHitBuffer(rayCount, hitBuffer, error) ||
      !hit.uploadRays(rays, origins, directions, error) ||
      !hit.uploadTriangles(triangles, triangleBuffer, error)) {
    std::cerr << error << '\n';
    return 1;
  }
  std::vector<float> hostWeights(rayCount, 1.0F);
  hostWeights[0] = std::bit_cast<float>(0x80000000U);
  if (!weights.create(session, rayCount * sizeof(float), error) ||
      !weights.upload(session, hostWeights.data(), rayCount * sizeof(float), 0U,
                      error) ||
      !hit.dispatch(origins, directions, triangleBuffer, rayCount,
                    triangles.size(), hitBuffer, rayCount, error)) {
    std::cerr << error << '\n';
    return 1;
  }
  RayRecordCompactionDeviceOutput compacted;
  DeviceBuffer sorted;
  if (!compactor.createRecordBuffer(rayCount, compacted.records, error) ||
      !compactor.createRecordBuffer(rayCount + 4U, sorted, error) ||
      !compactor.compact(hitBuffer, weights, rayCount, 1U, rayCount, compacted,
                         error)) {
    std::cerr << error << '\n';
    return 1;
  }
  std::vector<RayRecord> sentinel(
      rayCount + 4U, {0xdecafbadU, 0xabcdef01U, 0x13579bdfU, 0x2468ace0U});
  if (!sorted.upload(session, sentinel.data(),
                     sentinel.size() * sizeof(RayRecord), 0U, error) ||
      !sorter.sort(compacted.records, compacted.count, rayCount, sorted,
                   rayCount + 4U, error)) {
    std::cerr << error << '\n';
    return 1;
  }
  std::uint32_t active = 0U;
  std::vector<TriangleHit> hits(rayCount);
  std::vector<RayRecord> actual(rayCount + 4U);
  if (!compacted.count.download(session, &active, sizeof(active), 0U, error) ||
      !sorted.download(session, actual.data(),
                       actual.size() * sizeof(RayRecord), 0U, error) ||
      !hit.downloadHits(rayCount, hitBuffer, hits, error)) {
    std::cerr << error << '\n';
    return 1;
  }
  std::vector<std::uint32_t> ids(rayCount), surfaces(rayCount);
  std::vector<float> compactWeights(rayCount);
  RayHitBatch oracle{std::span<std::uint32_t>(ids),
                     std::span<std::uint32_t>(surfaces),
                     std::span<float>(compactWeights), 0U};
  if (!compactCpu(hits, hostWeights, 1U, oracle, error)) {
    std::cerr << error << '\n';
    return 1;
  }
  std::vector<RayRecord> expected;
  for (std::size_t i = 0U; i < oracle.count; ++i)
    expected.push_back({oracle.rayId[i], oracle.surfaceId[i],
                        std::bit_cast<std::uint32_t>(compactWeights[i]), 0U});
  std::stable_sort(expected.begin(), expected.end(),
                   [](const RayRecord &a, const RayRecord &b) {
                     return a.surfaceId < b.surfaceId ||
                            (a.surfaceId == b.surfaceId && a.rayId < b.rayId);
                   });
  const auto sameRecord = [](const RayRecord &a, const RayRecord &b) {
    return std::memcmp(&a, &b, sizeof(RayRecord)) == 0;
  };
  const auto mismatch = std::mismatch(expected.begin(), expected.end(),
                                      actual.begin(), sameRecord);
  if (active != expected.size() || mismatch.first != expected.end()) {
    std::cerr << "P5-I/P5-JA/P5-JB2 radix mismatch: active=" << active
              << ", expected=" << expected.size();
    if (mismatch.first != expected.end()) {
      const auto index =
          static_cast<std::size_t>(mismatch.first - expected.begin());
      const auto &expectedRecord = *mismatch.first;
      const auto &actualRecord = *mismatch.second;
      std::cerr << ", index=" << index << ", expected=(" << expectedRecord.rayId
                << ',' << expectedRecord.surfaceId << ','
                << expectedRecord.weightBits << ',' << expectedRecord.reserved
                << "), actual=(" << actualRecord.rayId << ','
                << actualRecord.surfaceId << ',' << actualRecord.weightBits
                << ',' << actualRecord.reserved << ')';
    }
    std::cerr << '\n';
    return 1;
  }
  for (std::size_t i = rayCount; i < actual.size(); ++i)
    if (std::memcmp(&actual[i], &sentinel[i], sizeof(RayRecord)) != 0)
      return 1;

  // The real P5-I -> P5-JA path above exercises a two-tile data volume. This
  // direct device-buffer case adds repeated keys and arbitrary record words so
  // stability and the surface-primary order cannot be masked by hit order.
  constexpr std::size_t permutationCount = 513U;
  std::vector<RayRecord> permutation(permutationCount);
  for (std::size_t i = 0U; i < permutation.size(); ++i) {
    permutation[i] = {static_cast<std::uint32_t>((i * 7U) % 31U),
                      static_cast<std::uint32_t>((i * 5U) % 3U),
                      i == 0U ? 0x80000000U
                              : static_cast<std::uint32_t>(0x3f000000U + i),
                      static_cast<std::uint32_t>(0xace00000U + i)};
  }
  std::vector<RayRecord> permutationExpected = permutation;
  std::stable_sort(permutationExpected.begin(), permutationExpected.end(),
                   [](const RayRecord &a, const RayRecord &b) {
                     return a.surfaceId < b.surfaceId ||
                            (a.surfaceId == b.surfaceId && a.rayId < b.rayId);
                   });
  DeviceBuffer permutationRecords, permutationCountBuffer;
  const auto permutationCountWord =
      static_cast<std::uint32_t>(permutation.size());
  if (!compactor.createRecordBuffer(permutation.size(), permutationRecords,
                                    error) ||
      !permutationCountBuffer.create(session, sizeof(permutationCountWord),
                                     error) ||
      !permutationRecords.upload(session, permutation.data(),
                                 permutation.size() * sizeof(RayRecord), 0U,
                                 error) ||
      !permutationCountBuffer.upload(session, &permutationCountWord,
                                     sizeof(permutationCountWord), 0U, error) ||
      !sorted.upload(session, sentinel.data(),
                     sentinel.size() * sizeof(RayRecord), 0U, error) ||
      !sorter.sort(permutationRecords, permutationCountBuffer,
                   permutation.size(), sorted, sentinel.size(), error)) {
    std::cerr << error << '\n';
    return 1;
  }
  std::vector<RayRecord> permutationActual(sentinel.size());
  if (!sorted.download(session, permutationActual.data(),
                       permutationActual.size() * sizeof(RayRecord), 0U,
                       error)) {
    std::cerr << error << '\n';
    return 1;
  }
  if (!std::equal(permutationExpected.begin(), permutationExpected.end(),
                  permutationActual.begin(), sameRecord)) {
    std::cerr << "P5-JB2 radix stable permutation mismatch\n";
    return 1;
  }
  for (std::size_t i = permutation.size(); i < permutationActual.size(); ++i)
    if (!sameRecord(permutationActual[i], sentinel[i])) {
      std::cerr << "P5-JB2 radix wrote a permutation tail sentinel\n";
      return 1;
    }
  std::string rejected;
  constexpr std::size_t hierarchyOverflowCapacity = 4'194'305U;
  if (sorter.sort(sorted, permutationCountBuffer, permutation.size(), sorted,
                  sentinel.size(), rejected) ||
      sorter.sort(permutationRecords, permutationCountBuffer,
                  permutation.size(), sorted, permutation.size() - 1U,
                  rejected) ||
      sorter.sort(permutationRecords, permutationCountBuffer,
                  hierarchyOverflowCapacity, sorted, hierarchyOverflowCapacity,
                  rejected) ||
      !sorter.sort(permutationRecords, permutationCountBuffer, 0U, sorted, 0U,
                   rejected))
    return 1;
  ComputeSession foreign;
  DeviceBuffer foreignRecords;
  if (!foreign.initialize(error) ||
      !foreignRecords.create(foreign, permutation.size() * sizeof(RayRecord),
                             error) ||
      sorter.sort(foreignRecords, permutationCountBuffer, permutation.size(),
                  sorted, sentinel.size(), rejected))
    return 1;
  std::vector<RayRecord> afterRejected(sentinel.size());
  if (!sorted.download(session, afterRejected.data(),
                       afterRejected.size() * sizeof(RayRecord), 0U, error) ||
      !std::equal(permutationActual.begin(), permutationActual.end(),
                  afterRejected.begin(), sameRecord)) {
    std::cerr << "rejected P5-JB2 radix call modified caller output\n";
    return 1;
  }
  std::cout << "ray record radix sort Vulkan dispatch PASS\n";
  return 0;
}
