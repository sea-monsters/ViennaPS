// Copyright 2026 ViennaPS
// SPDX-License-Identifier: MIT
#include "triangle_bvh_hit.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <limits>
#include <vector>

using namespace viennaps::vulkan::ray;
namespace {
bool same(const TriangleHit &a, const TriangleHit &b) {
  return std::bit_cast<std::uint32_t>(a.t) ==
             std::bit_cast<std::uint32_t>(b.t) &&
         a.triangleIndex == b.triangleIndex &&
         std::bit_cast<std::uint32_t>(a.u) ==
             std::bit_cast<std::uint32_t>(b.u) &&
         std::bit_cast<std::uint32_t>(a.v) == std::bit_cast<std::uint32_t>(b.v);
}
} // namespace
int main() {
  std::string error;
  viennaps::vulkan::runtime::ComputeSession session;
  if (!session.initialize(error)) {
    std::cerr << "session: " << error << '\n';
    return 1;
  }
  TriangleBvhHitPrimitive primitive;
  if (!primitive.initialize(session, VIENNAPS_VULKAN_TRIANGLE_BVH_HIT_SPV_PATH,
                            error)) {
    std::cerr << "init: " << error << '\n';
    return 1;
  }
  std::vector<Triangle> triangles;
  for (int i = 0; i < 9; ++i) {
    const float x = -10.0F - static_cast<float>(i);
    triangles.push_back(
        {{x - 0.25F, -0.25F, 0}, {x + 0.25F, -0.25F, 0}, {x, 0.25F, 0}});
  }
  triangles.push_back({{-1, -1, 0}, {1, -1, 0}, {0, 1, 0}});
  triangles.push_back({{-1, -1, 0}, {1, -1, 0}, {0, 1, 0}});
  for (int i = 0; i < 9; ++i) {
    const float x = 10.0F + static_cast<float>(i);
    triangles.push_back(
        {{x - 0.25F, -0.25F, 0}, {x + 0.25F, -0.25F, 0}, {x, 0.25F, 0}});
  }
  if (!primitive.build(triangles, error)) {
    std::cerr << "build: " << error << '\n';
    return 1;
  }
  // P5-R5 RED contract: a fixed-topology device refit accepts a caller-owned
  // packed-vertex buffer plus its strict host mirror and records into a
  // caller-owned command buffer only.
  viennaps::vulkan::runtime::DeviceBuffer invalidRefitVertices;
  if (primitive.recordRefit(VK_NULL_HANDLE, invalidRefitVertices,
                             std::span<const Triangle>(triangles), error)) {
    std::cerr << "invalid refit unexpectedly succeeded\n";
    return 1;
  }
  std::vector<Ray> rays = {{{0, 0, 2}, {0, 0, -1}, 0, 10},
                           {{0.9F, 0.8F, 2}, {0, 0, -1}, 0, 10},
                           {{4.1F, 0.1F, 2}, {0, 0, -1}, 0, 10},
                           {{-4, 0, 2}, {0, 0, -1}, 0, 10},
                           {{0, 0, 2}, {0, 0, -1}, 2, 10},
                           {{20, 20, 2}, {0, 0, -1}, 0, 10}};
  std::vector<TriangleHit> cpu(rays.size(), TriangleHit::miss());
  if (!intersectCpu(rays, triangles, cpu, error)) {
    std::cerr << "cpu: " << error << '\n';
    return 1;
  }
  const TriangleHit sentinel{13.0F, 77U, 2.0F, 3.0F};
  std::vector<TriangleHit> gpu(rays.size(), sentinel);
  if (!primitive.intersect(rays, gpu, error)) {
    std::cerr << "gpu: " << error << '\n';
    return 1;
  }
  std::size_t mismatches = 0;
  for (std::size_t i = 0; i < gpu.size(); ++i)
    if (!same(cpu[i], gpu[i]))
      ++mismatches;
  if (mismatches != 0U) {
    std::cerr << "mismatches=" << mismatches << '\n';
    return 1;
  }
  if (gpu.front().triangleIndex != 9U) {
    std::cerr << "cross-root tie selected wrong triangle\n";
    return 1;
  }
  std::vector<Triangle> movedTriangles = triangles;
  for (auto &point : {&movedTriangles[0].a, &movedTriangles[0].b,
                      &movedTriangles[0].c})
    (*point)[0] -= 20.0F;
  for (auto &point : {&movedTriangles[9].a, &movedTriangles[9].b,
                      &movedTriangles[9].c})
    (*point)[0] += 8.0F;
  std::vector<std::array<float, 4>> movedPacked;
  movedPacked.reserve(movedTriangles.size() * 3U);
  for (const auto &triangle : movedTriangles)
    for (const auto point : {triangle.a, triangle.b, triangle.c})
      movedPacked.push_back({point[0], point[1], point[2], 0.0F});
  viennaps::vulkan::runtime::DeviceBuffer refitVertices;
  if (!refitVertices.create(
          session, movedPacked.size() * sizeof(movedPacked[0]), error) ||
      !refitVertices.upload(session, movedPacked.data(),
                            movedPacked.size() * sizeof(movedPacked[0]), 0U,
                            error)) {
    std::cerr << "refit vertex setup failed: " << error << '\n';
    return 1;
  }
  std::vector<std::array<float, 4>> originPacked;
  std::vector<std::array<float, 4>> directionPacked;
  for (const auto &ray : rays) {
    originPacked.push_back(
        {ray.origin[0], ray.origin[1], ray.origin[2], ray.tMin});
    directionPacked.push_back(
        {ray.direction[0], ray.direction[1], ray.direction[2], ray.tMax});
  }
  viennaps::vulkan::runtime::DeviceBuffer recordOrigins, recordDirections,
      recordHits;
  if (!recordOrigins.create(
          session, originPacked.size() * sizeof(originPacked[0]), error) ||
      !recordDirections.create(
          session, directionPacked.size() * sizeof(directionPacked[0]),
          error) ||
      !recordHits.create(session, gpu.size() * sizeof(TriangleHit), error) ||
      !recordOrigins.upload(session, originPacked.data(),
                            originPacked.size() * sizeof(originPacked[0]), 0U,
                            error) ||
      !recordDirections.upload(
          session, directionPacked.data(),
          directionPacked.size() * sizeof(directionPacked[0]), 0U, error)) {
    std::cerr << "record buffer setup failed: " << error << '\n';
    return 1;
  }
  std::vector<TriangleHit> recordSentinel(rays.size(), sentinel);
  if (!recordHits.upload(session, recordSentinel.data(),
                         recordSentinel.size() * sizeof(TriangleHit), 0U,
                         error)) {
    std::cerr << "record sentinel upload failed: " << error << '\n';
    return 1;
  }
  VkCommandBuffer recordCommand = VK_NULL_HANDLE;
  viennaps::vulkan::runtime::Fence recordFence;
  if (!session.commandContext().allocatePrimary(recordCommand, error) ||
      !recordFence.create(session.device(), error)) {
    std::cerr << "record command setup failed: " << error << '\n';
    return 1;
  }
  if (primitive.recordDispatch(VK_NULL_HANDLE, recordOrigins, recordDirections,
                               rays.size(), recordHits, rays.size(), error)) {
    std::cerr << "invalid record unexpectedly succeeded\n";
    return 1;
  }
  std::vector<TriangleHit> preserved(rays.size());
  if (!recordHits.download(session, preserved.data(),
                           preserved.size() * sizeof(TriangleHit), 0U, error)) {
    std::cerr << "record sentinel download failed: " << error << '\n';
    return 1;
  }
  for (const auto &hit : preserved)
    if (!same(hit, sentinel)) {
      std::cerr << "invalid record modified output sentinel\n";
      return 1;
    }
  VkCommandBufferBeginInfo recordBegin{
      VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  recordBegin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  if (vkBeginCommandBuffer(recordCommand, &recordBegin) != VK_SUCCESS ||
      !primitive.recordDispatch(recordCommand, recordOrigins, recordDirections,
                                rays.size(), recordHits, rays.size(), error) ||
      vkEndCommandBuffer(recordCommand) != VK_SUCCESS) {
    std::cerr << "record dispatch failed: " << error << '\n';
    return 1;
  }
  VkSubmitInfo recordSubmit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
  recordSubmit.commandBufferCount = 1U;
  recordSubmit.pCommandBuffers = &recordCommand;
  if (vkQueueSubmit(session.device().computeQueue(), 1U, &recordSubmit,
                    recordFence.get()) != VK_SUCCESS ||
      !recordFence.wait(10'000'000'000ULL, error)) {
    std::cerr << "record submit failed: " << error << '\n';
    return 1;
  }
  recordFence.reset();
  std::vector<TriangleHit> recorded(rays.size());
  if (!recordHits.download(session, recorded.data(),
                           recorded.size() * sizeof(TriangleHit), 0U, error)) {
    std::cerr << "record hit download failed: " << error << '\n';
    return 1;
  }
  for (std::size_t i = 0; i < recorded.size(); ++i)
    if (!same(cpu[i], recorded[i])) {
      std::cerr << "record raw-bit mismatch at " << i << '\n';
      return 1;
    }
  std::vector<TriangleBvhNode> originalNodes;
  if (!primitive.snapshotNodes(originalNodes, error) || originalNodes.empty()) {
    std::cerr << "original node snapshot failed: " << error << '\n';
    return 1;
  }
  auto expectRejected = [&](const char *label, auto &&attempt) {
    error.clear();
    std::vector<TriangleHit> sentinels(rays.size(), sentinel);
    if (!recordHits.upload(session, sentinels.data(),
                           sentinels.size() * sizeof(TriangleHit), 0U,
                           error))
      return false;
    if (vkResetCommandBuffer(recordCommand, 0U) != VK_SUCCESS ||
        vkBeginCommandBuffer(recordCommand, &recordBegin) != VK_SUCCESS) {
      std::cerr << label << " command setup failed\n";
      return false;
    }
    const bool accepted = attempt(recordCommand);
    if (vkEndCommandBuffer(recordCommand) != VK_SUCCESS)
      return false;
    if (accepted) {
      std::cerr << label << " unexpectedly accepted\n";
      return false;
    }
    if (vkResetCommandBuffer(recordCommand, 0U) != VK_SUCCESS)
      return false;
    std::vector<TriangleHit> actual(sentinels.size());
    if (!recordHits.download(session, actual.data(),
                             actual.size() * sizeof(TriangleHit), 0U, error))
      return false;
    for (const auto &hit : actual)
      if (!same(hit, sentinel)) {
        std::cerr << label << " modified output sentinel\n";
        return false;
      }
    std::vector<TriangleBvhNode> nodes;
    if (!primitive.snapshotNodes(nodes, error) ||
        nodes.size() != originalNodes.size() ||
        std::memcmp(nodes.data(), originalNodes.data(),
                    nodes.size() * sizeof(TriangleBvhNode)) != 0) {
      std::cerr << label << " modified BVH nodes\n";
      return false;
    }
    return true;
  };
  if (!expectRejected("null command", [&](const VkCommandBuffer) {
        return primitive.recordRefit(VK_NULL_HANDLE, refitVertices,
                                     std::span<const Triangle>(triangles),
                                     error);
      }) ||
      !expectRejected("host mirror count", [&](const VkCommandBuffer command) {
        return primitive.recordRefit(
            command, refitVertices,
            std::span<const Triangle>(triangles).first(triangles.size() - 1U),
            error);
      }))
    return 1;
  auto nanMirror = movedTriangles;
  nanMirror[0].a[0] = std::numeric_limits<float>::quiet_NaN();
  if (!expectRejected("NaN host mirror", [&](const VkCommandBuffer command) {
        return primitive.recordRefit(command, refitVertices,
                                     std::span<const Triangle>(nanMirror),
                                     error);
      }))
    return 1;
  auto subnormalMirror = movedTriangles;
  subnormalMirror[0].a[0] = std::numeric_limits<float>::denorm_min();
  if (!expectRejected(
          "subnormal host mirror", [&](const VkCommandBuffer command) {
            return primitive.recordRefit(
                command, refitVertices,
                std::span<const Triangle>(subnormalMirror), error);
          }))
    return 1;
  viennaps::vulkan::runtime::DeviceBuffer undersizedRefitVertices;
  if (!undersizedRefitVertices.create(
          session, refitVertices.size() - 1U, error) ||
      !expectRejected("undersized dynamic buffer",
                      [&](const VkCommandBuffer command) {
                        return primitive.recordRefit(
                            command, undersizedRefitVertices,
                            std::span<const Triangle>(movedTriangles), error);
                      }))
    return 1;
  viennaps::vulkan::runtime::ComputeSession foreignSession;
  viennaps::vulkan::runtime::DeviceBuffer foreignRefitVertices;
  if (!foreignSession.initialize(error) ||
      !foreignRefitVertices.create(foreignSession, refitVertices.size(),
                                    error) ||
      !expectRejected("foreign dynamic buffer",
                      [&](const VkCommandBuffer command) {
                        return primitive.recordRefit(
                            command, foreignRefitVertices,
                            std::span<const Triangle>(movedTriangles), error);
                      }))
    return 1;
  viennaps::vulkan::runtime::ComputeSession staleSession;
  viennaps::vulkan::runtime::DeviceBuffer staleRefitVertices;
  if (!staleSession.initialize(error) ||
      !staleRefitVertices.create(staleSession, refitVertices.size(), error))
    return 1;
  staleSession.reset();
  if (!expectRejected("stale dynamic buffer",
                      [&](const VkCommandBuffer command) {
                        return primitive.recordRefit(
                            command, staleRefitVertices,
                            std::span<const Triangle>(movedTriangles), error);
                      }))
    return 1;
  std::vector<TriangleHit> movedCpu(rays.size(), TriangleHit::miss());
  if (!intersectCpu(rays, movedTriangles, movedCpu, error)) {
    std::cerr << "moved CPU setup failed: " << error << '\n';
    return 1;
  }
  if (vkResetCommandBuffer(recordCommand, 0U) != VK_SUCCESS ||
      vkBeginCommandBuffer(recordCommand, &recordBegin) != VK_SUCCESS ||
      !primitive.recordRefit(recordCommand, refitVertices,
                             std::span<const Triangle>(movedTriangles),
                             error) ||
      !primitive.recordDispatch(recordCommand, recordOrigins, recordDirections,
                                rays.size(), recordHits, rays.size(), error) ||
      vkEndCommandBuffer(recordCommand) != VK_SUCCESS) {
    std::cerr << "refit record failed: " << error << '\n';
    return 1;
  }
  if (vkQueueSubmit(session.device().computeQueue(), 1U, &recordSubmit,
                    recordFence.get()) != VK_SUCCESS ||
      !recordFence.wait(10'000'000'000ULL, error)) {
    std::cerr << "refit submit failed: " << error << '\n';
    return 1;
  }
  recordFence.reset();
  std::vector<TriangleHit> movedGpu(rays.size(), sentinel);
  if (!recordHits.download(session, movedGpu.data(),
                           movedGpu.size() * sizeof(TriangleHit), 0U,
                           error)) {
    std::cerr << "refit hit download failed: " << error << '\n';
    return 1;
  }
  for (std::size_t i = 0; i < movedGpu.size(); ++i)
    if (!same(movedCpu[i], movedGpu[i])) {
      std::cerr << "refit hit mismatch at " << i << '\n';
      return 1;
    }
  if (movedGpu.front().triangleIndex != 10U) {
    std::cerr << "refit did not update triangle hit ownership\n";
    return 1;
  }
  std::vector<TriangleBvhNode> nodes;
  if (!primitive.snapshotNodes(nodes, error) || nodes.empty()) {
    std::cerr << "refit node snapshot failed: " << error << '\n';
    return 1;
  }
  float movedMinX = std::numeric_limits<float>::max();
  float movedMaxX = -std::numeric_limits<float>::max();
  for (const auto &triangle : movedTriangles)
    for (const auto point : {triangle.a, triangle.b, triangle.c}) {
      movedMinX = std::min(movedMinX, point[0]);
      movedMaxX = std::max(movedMaxX, point[0]);
    }
  if (std::bit_cast<std::uint32_t>(nodes.front().minX) !=
          std::bit_cast<std::uint32_t>(std::nextafter(
              movedMinX, -std::numeric_limits<float>::infinity())) ||
      std::bit_cast<std::uint32_t>(nodes.front().maxX) !=
          std::bit_cast<std::uint32_t>(std::nextafter(
              movedMaxX, std::numeric_limits<float>::infinity()))) {
    std::cerr << "refit root bounds mismatch\n";
    return 1;
  }
  std::vector<std::array<float, 4>> restoredPacked;
  restoredPacked.reserve(triangles.size() * 3U);
  for (const auto &triangle : triangles)
    for (const auto point : {triangle.a, triangle.b, triangle.c})
      restoredPacked.push_back({point[0], point[1], point[2], 0.0F});
  if (!refitVertices.upload(session, restoredPacked.data(),
                            restoredPacked.size() * sizeof(restoredPacked[0]),
                            0U, error) ||
      vkResetCommandBuffer(recordCommand, 0U) != VK_SUCCESS ||
      vkBeginCommandBuffer(recordCommand, &recordBegin) != VK_SUCCESS ||
      !primitive.recordRefit(recordCommand, refitVertices,
                             std::span<const Triangle>(triangles), error) ||
      !primitive.recordDispatch(recordCommand, recordOrigins, recordDirections,
                                rays.size(), recordHits, rays.size(), error) ||
      vkEndCommandBuffer(recordCommand) != VK_SUCCESS ||
      vkQueueSubmit(session.device().computeQueue(), 1U, &recordSubmit,
                    recordFence.get()) != VK_SUCCESS ||
      !recordFence.wait(10'000'000'000ULL, error)) {
    std::cerr << "second refit submit failed: " << error << '\n';
    return 1;
  }
  recordFence.reset();
  std::vector<TriangleHit> restoredGpu(rays.size(), sentinel);
  if (!recordHits.download(session, restoredGpu.data(),
                           restoredGpu.size() * sizeof(TriangleHit), 0U,
                           error)) {
    std::cerr << "second refit hit download failed: " << error << '\n';
    return 1;
  }
  for (std::size_t i = 0; i < restoredGpu.size(); ++i)
    if (!same(cpu[i], restoredGpu[i])) {
      std::cerr << "second refit hit mismatch at " << i << '\n';
      return 1;
    }
  std::vector<TriangleBvhNode> restoredNodes;
  if (!primitive.snapshotNodes(restoredNodes, error) || restoredNodes.empty()) {
    std::cerr << "second refit node mismatch\n";
    return 1;
  }
  float restoredMinX = std::numeric_limits<float>::max();
  float restoredMaxX = -std::numeric_limits<float>::max();
  for (const auto &triangle : triangles)
    for (const auto point : {triangle.a, triangle.b, triangle.c}) {
      restoredMinX = std::min(restoredMinX, point[0]);
      restoredMaxX = std::max(restoredMaxX, point[0]);
    }
  if (std::bit_cast<std::uint32_t>(restoredNodes.front().minX) !=
          std::bit_cast<std::uint32_t>(std::nextafter(
              restoredMinX, -std::numeric_limits<float>::infinity())) ||
      std::bit_cast<std::uint32_t>(restoredNodes.front().maxX) !=
          std::bit_cast<std::uint32_t>(std::nextafter(
              restoredMaxX, std::numeric_limits<float>::infinity()))) {
    std::cerr << "second refit root bounds mismatch\n";
    return 1;
  }
  std::vector<TriangleHit> shortOut(1U, sentinel);
  if (primitive.intersect(rays, shortOut, error) ||
      shortOut[0].triangleIndex != sentinel.triangleIndex) {
    std::cerr << "malformed transaction failed\n";
    return 1;
  }
  std::vector<TriangleHit> noOp(1U, sentinel);
  if (!primitive.intersect(std::span<const Ray>{}, noOp, error) ||
      !same(noOp.front(), sentinel)) {
    std::cerr << "zero-ray no-op failed\n";
    return 1;
  }
  const float tiny = 1.0e-31F;
  const std::vector<Triangle> tinyTriangles = {{{tiny, -5.0e12F, -5.0e12F},
                                                {tiny, 5.0e12F, -5.0e12F},
                                                {tiny, -5.0e12F, 5.0e12F}}};
  const std::vector<Ray> tinyRays = {{{0, 0, 0}, {tiny, 0, 0}, 0, 2}};
  std::vector<TriangleHit> tinyCpu(1U, TriangleHit::miss());
  std::vector<TriangleHit> tinyGpu(1U, sentinel);
  if (!primitive.build(tinyTriangles, error) ||
      !intersectCpu(tinyRays, tinyTriangles, tinyCpu, error) ||
      !primitive.intersect(tinyRays, tinyGpu, error) ||
      !same(tinyCpu.front(), tinyGpu.front()) ||
      tinyGpu.front().triangleIndex != 0U) {
    std::cerr << "tiny-normal differential failed: " << error << '\n';
    return 1;
  }
  const std::vector<Triangle> emptyTriangles;
  std::vector<TriangleHit> emptyCpu(rays.size(), TriangleHit::miss());
  std::vector<TriangleHit> emptyGpu(rays.size(), sentinel);
  if (!primitive.build(emptyTriangles, error) ||
      !intersectCpu(rays, emptyTriangles, emptyCpu, error) ||
      !primitive.intersect(rays, emptyGpu, error)) {
    std::cerr << "empty BVH differential execution failed: " << error << '\n';
    return 1;
  }
  for (std::size_t i = 0U; i < emptyGpu.size(); ++i) {
    if (!same(emptyCpu[i], emptyGpu[i])) {
      std::cerr << "empty BVH differential failed\n";
      return 1;
    }
  }
  std::uint32_t state = 0x5EEDU;
  const auto next = [&state]() {
    state = state * 1664525U + 1013904223U;
    return static_cast<float>(state & 0xffffU) / 65535.0F;
  };
  std::vector<Triangle> randomTriangles;
  for (int i = 0; i < 32; ++i) {
    const float x = 4.0F * next() - 2.0F;
    const float y = 4.0F * next() - 2.0F;
    const float z = 0.5F + next();
    randomTriangles.push_back({{x, y, z}, {x + 0.2F, y, z}, {x, y + 0.2F, z}});
  }
  std::vector<Ray> randomRays;
  for (int i = 0; i < 16; ++i)
    randomRays.push_back({{4.0F * next() - 2.0F, 4.0F * next() - 2.0F, 3.0F},
                          {0, 0, -1},
                          0,
                          10});
  if (!primitive.build(randomTriangles, error)) {
    std::cerr << "random build: " << error << '\n';
    return 1;
  }
  std::vector<TriangleHit> randomCpu(randomRays.size(), TriangleHit::miss());
  std::vector<TriangleHit> randomGpu(randomRays.size(), sentinel);
  if (!intersectCpu(randomRays, randomTriangles, randomCpu, error) ||
      !primitive.intersect(randomRays, randomGpu, error)) {
    std::cerr << "random differential execution failed: " << error << '\n';
    return 1;
  }
  std::size_t randomMismatches = 0;
  for (std::size_t i = 0; i < randomGpu.size(); ++i)
    randomMismatches += !same(randomCpu[i], randomGpu[i]);
  if (randomMismatches != 0U) {
    std::cerr << "random mismatches=" << randomMismatches << '\n';
    return 1;
  }
  std::cout << "triangle_bvh_hit smoke: tie-rays=" << rays.size()
            << " tie-triangles=" << triangles.size() << " tiny=1"
            << " mismatches=0 random-rays=" << randomRays.size()
            << " random-triangles=" << randomTriangles.size()
            << " seed=0x5EED\n";
  return 0;
}
