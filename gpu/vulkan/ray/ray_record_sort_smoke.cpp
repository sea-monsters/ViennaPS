// Copyright 2026 ViennaPS
// SPDX-License-Identifier: MIT
#include "ray_hit_batch.hpp"
#include "ray_record_compaction.hpp"
#include "ray_record_sort.hpp"
#include "triangle_hit_device.hpp"

#include <algorithm>
#include <bit>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <span>
#include <string>
#include <vector>

#ifndef VIENNAPS_VULKAN_RAY_RECORD_SORT_SPV_PATH
#define VIENNAPS_VULKAN_RAY_RECORD_SORT_SPV_PATH "ray_record_sort.comp.spv"
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

namespace {

[[nodiscard]] bool sameRecord(const RayRecord &left, const RayRecord &right) {
  return std::memcmp(&left, &right, sizeof(RayRecord)) == 0;
}

[[nodiscard]] bool readRecords(ComputeSession &session,
                               const DeviceBuffer &buffer,
                               std::span<RayRecord> records,
                               std::string &error) {
  return buffer.download(session, records.data(),
                         records.size() * sizeof(RayRecord), 0U, error);
}

[[nodiscard]] bool unchanged(ComputeSession &session,
                             const DeviceBuffer &buffer,
                             std::span<const RayRecord> expected,
                             std::string &error) {
  std::vector<RayRecord> actual(expected.size());
  if (!readRecords(session, buffer, actual, error)) {
    return false;
  }
  for (std::size_t i = 0U; i < actual.size(); ++i) {
    if (!sameRecord(actual[i], expected[i])) {
      error = "rejected ray-record sort modified caller output";
      return false;
    }
  }
  return true;
}

[[nodiscard]] bool expectRejected(const bool result, const std::string &error,
                                  const char *label) {
  if (!result && !error.empty()) {
    return true;
  }
  std::cerr << label << " did not fail closed\n";
  return false;
}

} // namespace

int main() {
  std::string error;
  ComputeSession session;
  if (!session.initialize(error)) {
    std::cerr << error << '\n';
    return 1;
  }
  DeviceTriangleHitPrimitive hit;
  DeviceRayRecordCompactor compactor;
  DeviceRayRecordSort sorter;
  if (!hit.initialize(session, VIENNAPS_VULKAN_TRIANGLE_HIT_DEVICE_SPV_PATH,
                      error) ||
      !compactor.initialize(session,
                            VIENNAPS_VULKAN_RAY_RECORD_COMPACTION_SPV_PATH,
                            VIENNAPS_VULKAN_REDUCTION_SCAN_SPV_PATH, error) ||
      !sorter.initialize(session, VIENNAPS_VULKAN_RAY_RECORD_SORT_SPV_PATH,
                         error)) {
    std::cerr << error << '\n';
    return 1;
  }

  const Triangle first{{-1, -1, 2}, {1, -1, 2}, {0, 1, 2}};
  const Triangle second{{-1, -1, 4}, {1, -1, 4}, {0, 1, 4}};
  const std::vector<Triangle> triangles{first, second};
  std::vector<Ray> rays(6U);
  for (auto &ray : rays) {
    ray.direction = {0, 0, 1};
    ray.tMax = 10;
  }
  rays[1].origin = {2, 0, 0}; // Miss.
  rays[2].tMin = 3;
  rays[3].tMin = 3; // Same surface: ascending ray IDs are required.
  rays[4].origin = {0.5F, 0, 0};

  DeviceBuffer origins, directions, triangleBuffer, hitBuffer, weights;
  if (!hit.createRayBuffers(rays.size(), origins, directions, error) ||
      !hit.createTriangleBuffer(triangles.size(), triangleBuffer, error) ||
      !hit.createHitBuffer(rays.size(), hitBuffer, error) ||
      !hit.uploadRays(rays, origins, directions, error) ||
      !hit.uploadTriangles(triangles, triangleBuffer, error)) {
    std::cerr << error << '\n';
    return 1;
  }
  const std::vector<float> hostWeights{
      1.0F, 2.0F, std::bit_cast<float>(0x80000000U), 4.0F, 5.0F, 6.0F};
  if (!weights.create(session, hostWeights.size() * sizeof(float), error) ||
      !weights.upload(session, hostWeights.data(),
                      hostWeights.size() * sizeof(float), 0U, error) ||
      !hit.dispatch(origins, directions, triangleBuffer, rays.size(),
                    triangles.size(), hitBuffer, rays.size(), error)) {
    std::cerr << error << '\n';
    return 1;
  }

  RayRecordCompactionDeviceOutput compacted;
  if (!compactor.createRecordBuffer(rays.size(), compacted.records, error) ||
      !compactor.compact(hitBuffer, weights, rays.size(), 2U, rays.size(),
                         compacted, error)) {
    std::cerr << error << '\n';
    return 1;
  }

  const RayRecord sentinel{0xdecafbadU, 0xabcdef01U, 0x13579bdfU, 0x2468ace0U};
  const std::vector<RayRecord> sentinels(rays.size(), sentinel);
  DeviceBuffer sorted;
  if (!compactor.createRecordBuffer(rays.size(), sorted, error) ||
      !sorted.upload(session, sentinels.data(),
                     sentinels.size() * sizeof(RayRecord), 0U, error) ||
      !sorter.sort(compacted.records, compacted.count, rays.size(), sorted,
                   rays.size(), error)) {
    std::cerr << error << '\n';
    return 1;
  }

  // The only downloads from the P5-I -> P5-JA -> P5-JB pipeline occur here.
  std::uint32_t count = 0U;
  std::vector<RayRecord> actual(rays.size());
  std::vector<TriangleHit> hits(rays.size());
  if (!compacted.count.download(session, &count, sizeof(count), 0U, error) ||
      !readRecords(session, sorted, actual, error) ||
      !hit.downloadHits(rays.size(), hitBuffer, hits, error)) {
    std::cerr << error << '\n';
    return 1;
  }
  std::vector<std::uint32_t> rayIds(rays.size()), surfaces(rays.size());
  std::vector<float> compactWeights(rays.size());
  RayHitBatch oracle{std::span<std::uint32_t>(rayIds),
                     std::span<std::uint32_t>(surfaces),
                     std::span<float>(compactWeights), 0U};
  if (!compactCpu(hits, hostWeights, 2U, oracle, error)) {
    std::cerr << error << '\n';
    return 1;
  }
  std::vector<RayRecord> expected;
  expected.reserve(oracle.count);
  for (std::size_t i = 0U; i < oracle.count; ++i) {
    expected.push_back({oracle.rayId[i], oracle.surfaceId[i],
                        std::bit_cast<std::uint32_t>(compactWeights[i]), 0U});
  }
  std::stable_sort(expected.begin(), expected.end(),
                   [](const RayRecord &left, const RayRecord &right) {
                     return left.surfaceId < right.surfaceId ||
                            (left.surfaceId == right.surfaceId &&
                             left.rayId < right.rayId);
                   });
  if (count != expected.size()) {
    std::cerr << "CPU/GPU ray-record count mismatch\n";
    return 1;
  }
  for (std::size_t i = 0U; i < expected.size(); ++i) {
    if (!sameRecord(actual[i], expected[i])) {
      std::cerr << "CPU/GPU ray-record sort mismatch\n";
      return 1;
    }
  }
  for (std::size_t i = expected.size(); i < actual.size(); ++i) {
    if (!sameRecord(actual[i], sentinel)) {
      std::cerr << "ray-record sort overwrote output tail\n";
      return 1;
    }
  }

  std::string rejected;
  if (!expectRejected(sorter.sort(sorted, compacted.count, rays.size(), sorted,
                                  rays.size(), rejected),
                      rejected, "input/output alias validation") ||
      !unchanged(session, sorted, actual, error) ||
      !expectRejected(sorter.sort(compacted.records, compacted.count,
                                  rays.size(), sorted, rays.size() - 1U,
                                  rejected),
                      rejected, "capacity validation") ||
      !unchanged(session, sorted, actual, error) ||
      !sorter.sort(compacted.records, compacted.count, 0U, sorted, 0U,
                   rejected) ||
      !unchanged(session, sorted, actual, error)) {
    std::cerr << error << '\n';
    return 1;
  }

  ComputeSession foreign;
  DeviceBuffer foreignRecords;
  if (!foreign.initialize(error) ||
      !foreignRecords.create(foreign, rays.size() * sizeof(RayRecord), error) ||
      !expectRejected(sorter.sort(foreignRecords, compacted.count, rays.size(),
                                  sorted, rays.size(), rejected),
                      rejected, "foreign-session validation") ||
      !unchanged(session, sorted, actual, error)) {
    std::cerr << error << '\n';
    return 1;
  }

  std::cout << "ray record sort Vulkan dispatch PASS\n";
  return 0;
}
