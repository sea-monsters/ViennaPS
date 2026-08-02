// Copyright 2026 ViennaPS
// SPDX-License-Identifier: MIT
#include "ray_hit_batch.hpp"
#include "ray_record_compaction.hpp"
#include "triangle_hit_device.hpp"

#include <bit>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <span>
#include <vector>

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

[[nodiscard]] bool recordsEqual(const RayRecord &left, const RayRecord &right) {
  return std::memcmp(&left, &right, sizeof(RayRecord)) == 0;
}

[[nodiscard]] bool fillRecords(ComputeSession &session, DeviceBuffer &buffer,
                               std::span<const RayRecord> values,
                               std::string &error) {
  return buffer.upload(session, values.data(),
                       values.size() * sizeof(RayRecord), 0U, error);
}

[[nodiscard]] bool readRecords(ComputeSession &session,
                               const DeviceBuffer &buffer,
                               std::span<RayRecord> values,
                               std::string &error) {
  return buffer.download(session, values.data(),
                         values.size() * sizeof(RayRecord), 0U, error);
}

[[nodiscard]] bool expectSentinel(ComputeSession &session,
                                  const DeviceBuffer &buffer,
                                  std::span<const RayRecord> sentinels,
                                  std::string &error) {
  std::vector<RayRecord> actual(sentinels.size());
  if (!readRecords(session, buffer, actual, error)) {
    return false;
  }
  for (std::size_t i = 0U; i < actual.size(); ++i) {
    if (!recordsEqual(actual[i], sentinels[i])) {
      error = "failed compaction modified caller-owned records";
      return false;
    }
  }
  return true;
}

[[nodiscard]] bool expectFailure(bool result, const std::string &error,
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
  DeviceTriangleHitPrimitive hitsPrimitive;
  DeviceRayRecordCompactor compactor;
  if (!hitsPrimitive.initialize(
          session, VIENNAPS_VULKAN_TRIANGLE_HIT_DEVICE_SPV_PATH, error) ||
      !compactor.initialize(session,
                            VIENNAPS_VULKAN_RAY_RECORD_COMPACTION_SPV_PATH,
                            VIENNAPS_VULKAN_REDUCTION_SCAN_SPV_PATH, error)) {
    std::cerr << error << '\n';
    return 1;
  }

  const Triangle first{{-1, -1, 2}, {1, -1, 2}, {0, 1, 2}};
  const Triangle second{{-1, -1, 4}, {1, -1, 4}, {0, 1, 4}};
  const Triangle third{{-1, -1, 6}, {1, -1, 6}, {0, 1, 6}};
  const std::vector<Triangle> triangles{first, second, third};
  std::vector<Ray> rays(5U);
  for (auto &ray : rays) {
    ray.direction = {0, 0, 1};
    ray.tMax = 10;
  }
  rays[1].origin = {2, 0, 0};
  rays[2].tMin = 3;
  rays[3].tMin = 5;
  rays[4].origin = {0.5F, 0, 0};

  DeviceBuffer origins, directions, triangleBuffer, hitBuffer;
  if (!hitsPrimitive.createRayBuffers(rays.size(), origins, directions,
                                      error) ||
      !hitsPrimitive.createTriangleBuffer(triangles.size(), triangleBuffer,
                                          error) ||
      !hitsPrimitive.createHitBuffer(rays.size(), hitBuffer, error) ||
      !hitsPrimitive.uploadRays(rays, origins, directions, error) ||
      !hitsPrimitive.uploadTriangles(triangles, triangleBuffer, error) ||
      !hitsPrimitive.dispatch(origins, directions, triangleBuffer, rays.size(),
                              triangles.size(), hitBuffer, rays.size(),
                              error)) {
    std::cerr << error << '\n';
    return 1;
  }

  const std::vector<float> weights{
      1.0F, 2.0F, std::bit_cast<float>(0x80000000U), 4.0F, 5.0F};
  DeviceBuffer weightBuffer;
  if (!weightBuffer.create(session, weights.size() * sizeof(float), error) ||
      !weightBuffer.upload(session, weights.data(),
                           weights.size() * sizeof(float), 0U, error)) {
    std::cerr << error << '\n';
    return 1;
  }

  const RayRecord sentinel{0x12345678U, 0x9abcdef0U, 0x13579bdfU, 0x2468ace0U};
  const std::vector<RayRecord> sentinels(rays.size(), sentinel);
  RayRecordCompactionDeviceOutput output;
  if (!compactor.createRecordBuffer(rays.size(), output.records, error) ||
      !fillRecords(session, output.records, sentinels, error) ||
      !compactor.compact(hitBuffer, weightBuffer, rays.size(), 3U, rays.size(),
                         output, error)) {
    std::cerr << error << '\n';
    return 1;
  }

  std::uint32_t count = 0U;
  std::vector<RayRecord> records(rays.size());
  if (!output.count.download(session, &count, sizeof(count), 0U, error) ||
      !readRecords(session, output.records, records, error)) {
    std::cerr << error << '\n';
    return 1;
  }
  // This is the first host read from the P5-I -> P5-JA data path.
  std::vector<TriangleHit> hostHits(rays.size());
  if (!hitsPrimitive.downloadHits(rays.size(), hitBuffer, hostHits, error)) {
    std::cerr << error << '\n';
    return 1;
  }
  std::vector<std::uint32_t> expectedRay(rays.size());
  std::vector<std::uint32_t> expectedSurface(rays.size());
  std::vector<float> expectedWeight(rays.size());
  RayHitBatch expected{std::span<std::uint32_t>(expectedRay),
                       std::span<std::uint32_t>(expectedSurface),
                       std::span<float>(expectedWeight), 0U};
  if (!compactCpu(hostHits, weights, 3U, expected, error) ||
      count != expected.count) {
    std::cerr << "CPU/GPU count mismatch\n";
    return 1;
  }
  for (std::size_t i = 0U; i < count; ++i) {
    if (records[i].rayId != expected.rayId[i] ||
        records[i].surfaceId != expected.surfaceId[i] ||
        records[i].weightBits !=
            std::bit_cast<std::uint32_t>(expected.weight[i]) ||
        records[i].reserved != 0U) {
      std::cerr << "CPU/GPU record mismatch\n";
      return 1;
    }
  }
  for (std::size_t i = count; i < records.size(); ++i) {
    if (!recordsEqual(records[i], sentinel)) {
      std::cerr << "compaction overwrote record tail\n";
      return 1;
    }
  }

  RayRecordCompactionDeviceOutput failedOutput;
  if (!compactor.createRecordBuffer(rays.size(), failedOutput.records, error) ||
      !fillRecords(session, failedOutput.records, sentinels, error)) {
    std::cerr << error << '\n';
    return 1;
  }
  if (!expectFailure(compactor.compact(hitBuffer, hitBuffer, rays.size(), 3U,
                                       rays.size(), failedOutput, error),
                     error, "alias validation") ||
      !expectSentinel(session, failedOutput.records, sentinels, error) ||
      !fillRecords(session, failedOutput.records, sentinels, error) ||
      !expectFailure(compactor.compact(hitBuffer, failedOutput.records,
                                       rays.size(), 3U, rays.size(),
                                       failedOutput, error),
                     error, "output-alias validation") ||
      !expectSentinel(session, failedOutput.records, sentinels, error) ||
      !fillRecords(session, failedOutput.records, sentinels, error) ||
      !expectFailure(compactor.compact(hitBuffer, weightBuffer, rays.size(), 0U,
                                       rays.size(), failedOutput, error),
                     error, "surface-domain validation") ||
      !expectSentinel(session, failedOutput.records, sentinels, error) ||
      !fillRecords(session, failedOutput.records, sentinels, error) ||
      !expectFailure(compactor.compact(hitBuffer, weightBuffer, rays.size(), 3U,
                                       rays.size() - 1U, failedOutput, error),
                     error, "capacity validation") ||
      !expectSentinel(session, failedOutput.records, sentinels, error) ||
      !fillRecords(session, failedOutput.records, sentinels, error) ||
      !compactor.compact(hitBuffer, weightBuffer, 0U, 0U, 0U, failedOutput,
                         error) ||
      !expectSentinel(session, failedOutput.records, sentinels, error)) {
    std::cerr << error << '\n';
    return 1;
  }

  ComputeSession foreignSession;
  DeviceBuffer foreignHits;
  if (!foreignSession.initialize(error) ||
      !foreignHits.create(foreignSession, rays.size() * sizeof(TriangleHit),
                          error) ||
      !fillRecords(session, failedOutput.records, sentinels, error) ||
      !expectFailure(compactor.compact(foreignHits, weightBuffer, rays.size(),
                                       3U, rays.size(), failedOutput, error),
                     error, "foreign-session validation") ||
      !expectSentinel(session, failedOutput.records, sentinels, error)) {
    std::cerr << error << '\n';
    return 1;
  }

  std::cout << "ray record compaction Vulkan dispatch PASS\n";
  return 0;
}
