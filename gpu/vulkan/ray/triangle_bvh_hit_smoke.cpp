// Copyright 2026 ViennaPS
// SPDX-License-Identifier: MIT
#include "triangle_bvh_hit.hpp"

#include <bit>
#include <cstdint>
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
