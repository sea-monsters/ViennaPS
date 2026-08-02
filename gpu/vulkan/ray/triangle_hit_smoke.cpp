// Copyright 2026 ViennaPS
// SPDX-License-Identifier: MIT

#include "triangle_hit.hpp"

#include <bit>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <limits>
#include <string>
#include <vector>

#ifndef VIENNAPS_VULKAN_TRIANGLE_HIT_SPV_PATH
#define VIENNAPS_VULKAN_TRIANGLE_HIT_SPV_PATH ""
#endif

int main() {
  using viennaps::vulkan::ray::Ray;
  using viennaps::vulkan::ray::Triangle;
  using viennaps::vulkan::ray::TriangleHit;

  const std::vector<Ray> rays{
      {{{0.25F, 0.25F, 1.0F}}, {{0.0F, 0.0F, -1.0F}}, 0.0F, 10.0F}};
  const std::vector<Triangle> triangles{
      {{{0.0F, 0.0F, 0.0F}}, {{1.0F, 0.0F, 0.0F}}, {{0.0F, 1.0F, 0.0F}}}};
  std::vector<TriangleHit> output(1U, TriangleHit::miss());
  std::string error;
  assert(viennaps::vulkan::ray::intersectCpu(rays, triangles, output, error));
  assert(output[0].triangleIndex == 0U && output[0].t == 1.0F);

  const Triangle farther{
      {{0.0F, 0.0F, -2.0F}}, {{1.0F, 0.0F, -2.0F}}, {{0.0F, 1.0F, -2.0F}}};
  const Triangle equalDistance{
      {{0.0F, 0.0F, 0.0F}}, {{1.0F, 0.0F, 0.0F}}, {{0.0F, 1.0F, 0.0F}}};
  const std::vector<Triangle> ordered{farther, equalDistance, triangles[0]};
  output[0] = TriangleHit::miss();
  assert(viennaps::vulkan::ray::intersectCpu(rays, ordered, output, error));
  assert(output[0].triangleIndex == 1U && output[0].t == 1.0F);

  // Misses use a stable finite sentinel; the output tail is never touched.
  std::vector<Ray> misses{
      {{{0.25F, 0.25F, 1.0F}}, {{0.0F, 0.0F, 1.0F}}, 0.0F, 10.0F},
      {{{0.25F, 0.25F, 1.0F}}, {{1.0F, 0.0F, 0.0F}}, 0.0F, 10.0F}};
  std::vector<TriangleHit> guarded(3U, TriangleHit::miss());
  guarded[2].t = 9.0F;
  assert(
      viennaps::vulkan::ray::intersectCpu(misses, triangles, guarded, error));
  assert(guarded[0].isMiss() && guarded[1].isMiss() && guarded[2].t == 9.0F);

  std::vector<TriangleHit> emptyOutput(1U);
  emptyOutput[0].t = 7.0F;
  assert(viennaps::vulkan::ray::intersectCpu({}, {}, emptyOutput, error));
  assert(emptyOutput[0].t == 7.0F);

  auto rejectsWithoutWrite = [&](const std::vector<Ray> &badRays,
                                 const std::vector<Triangle> &badTriangles,
                                 std::span<TriangleHit> destination) {
    const auto before =
        std::vector<TriangleHit>(destination.begin(), destination.end());
    assert(!viennaps::vulkan::ray::intersectCpu(badRays, badTriangles,
                                                destination, error));
    assert(std::equal(destination.begin(), destination.end(), before.begin(),
                      [](const TriangleHit &left, const TriangleHit &right) {
                        return left.t == right.t &&
                               left.triangleIndex == right.triangleIndex &&
                               left.u == right.u && left.v == right.v;
                      }));
  };

  Ray nearMiss = rays[0];
  nearMiss.tMin = 1.01F;
  std::vector<TriangleHit> one(1U, TriangleHit::miss());
  const std::vector<Ray> nearRays{nearMiss};
  assert(viennaps::vulkan::ray::intersectCpu(nearRays, triangles, one, error));
  assert(one[0].isMiss());
  nearMiss = rays[0];
  nearMiss.tMax = 0.99F;
  const std::vector<Ray> farRays{nearMiss};
  assert(viennaps::vulkan::ray::intersectCpu(farRays, triangles, one, error));
  assert(one[0].isMiss());
  const Triangle degenerate{
      {{0.0F, 0.0F, 0.0F}}, {{0.0F, 0.0F, 0.0F}}, {{0.0F, 0.0F, 0.0F}}};
  const std::vector<Triangle> degenerateTriangles{degenerate};
  assert(viennaps::vulkan::ray::intersectCpu(rays, degenerateTriangles, one,
                                             error));
  assert(one[0].isMiss());

  Ray nanRay = rays[0];
  nanRay.origin[0] = std::numeric_limits<float>::quiet_NaN();
  const std::vector<Ray> nanRays{nanRay};
  rejectsWithoutWrite(nanRays, triangles, one);
  rejectsWithoutWrite(rays, triangles, std::span<TriangleHit>{});

  // A deliberately overlapping output view is rejected before any read/write.
  Ray aliasedRay = rays[0];
  const auto aliasedBefore = aliasedRay;
  auto *aliasedOutput = reinterpret_cast<TriangleHit *>(&aliasedRay);
  assert(!viennaps::vulkan::ray::intersectCpu(
      std::span<const Ray>(&aliasedRay, 1U), triangles,
      std::span<TriangleHit>(aliasedOutput, 1U), error));
  assert(aliasedRay.origin == aliasedBefore.origin);

  if (std::string_view(VIENNAPS_VULKAN_TRIANGLE_HIT_SPV_PATH).empty()) {
    return 0;
  }
  viennaps::vulkan::ray::TriangleHitPrimitive primitive;
  if (!primitive.initialize(VIENNAPS_VULKAN_TRIANGLE_HIT_SPV_PATH, error)) {
    std::cout << "triangle hit Vulkan dispatch SKIP: " << error << '\n';
    return 0;
  }
  viennaps::vulkan::runtime::HostVisibleBuffer originBuffer;
  viennaps::vulkan::runtime::HostVisibleBuffer directionBuffer;
  viennaps::vulkan::runtime::HostVisibleBuffer triangleBuffer;
  viennaps::vulkan::runtime::HostVisibleBuffer hitBuffer;
  assert(primitive.createRayBuffer(rays.size(), originBuffer, directionBuffer,
                                   error));
  assert(primitive.createTriangleBuffer(ordered.size(), triangleBuffer, error));
  assert(primitive.createHitBuffer(3U, hitBuffer, error));
  std::vector<std::array<float, 4>> packedOrigins;
  std::vector<std::array<float, 4>> packedDirections;
  std::vector<std::array<float, 4>> packedTriangles;
  for (const auto &ray : rays) {
    packedOrigins.push_back(
        {ray.origin[0], ray.origin[1], ray.origin[2], ray.tMin});
    packedDirections.push_back(
        {ray.direction[0], ray.direction[1], ray.direction[2], ray.tMax});
  }
  for (const auto &triangle : ordered) {
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
  const TriangleHit tailSentinel{9.0F, 77U, 8.0F, 7.0F};
  std::vector<TriangleHit> initialHits(3U, tailSentinel);
  assert(hitBuffer.write(initialHits.data(),
                         initialHits.size() * sizeof(TriangleHit), 0U, error));
  assert(primitive.intersect(originBuffer, directionBuffer, triangleBuffer,
                             rays.size(), ordered.size(), hitBuffer, 3U,
                             error));
  std::vector<TriangleHit> gpuHits(3U, TriangleHit::miss());
  assert(hitBuffer.read(gpuHits.data(), gpuHits.size() * sizeof(TriangleHit),
                        0U, error));
  std::vector<TriangleHit> cpuHits(1U, TriangleHit::miss());
  assert(viennaps::vulkan::ray::intersectCpu(rays, ordered, cpuHits, error));
  const auto exact = [](const float left, const float right) {
    return std::bit_cast<std::uint32_t>(left) ==
           std::bit_cast<std::uint32_t>(right);
  };
  assert(gpuHits[0].triangleIndex == cpuHits[0].triangleIndex &&
         exact(gpuHits[0].t, cpuHits[0].t) &&
         exact(gpuHits[0].u, cpuHits[0].u) &&
         exact(gpuHits[0].v, cpuHits[0].v));
  assert(gpuHits[1].t == tailSentinel.t &&
         gpuHits[1].triangleIndex == tailSentinel.triangleIndex &&
         gpuHits[2].t == tailSentinel.t);

  // The packed origin's fourth component is tMin, not its x coordinate.
  const std::vector<Ray> translatedRays{
      {{{2.25F, 2.25F, 1.0F}}, {{0.0F, 0.0F, -1.0F}}, 0.0F, 10.0F}};
  const std::vector<Triangle> translatedTriangles{
      {{{2.0F, 2.0F, 0.0F}}, {{3.0F, 2.0F, 0.0F}}, {{2.0F, 3.0F, 0.0F}}}};
  const std::array<std::array<float, 4>, 1U> translatedOrigins{{
      {2.25F, 2.25F, 1.0F, 0.0F},
  }};
  const std::array<std::array<float, 4>, 1U> translatedDirections{{
      {0.0F, 0.0F, -1.0F, 10.0F},
  }};
  const std::array<std::array<float, 4>, 3U> translatedVertices{{
      {2.0F, 2.0F, 0.0F, 0.0F},
      {3.0F, 2.0F, 0.0F, 0.0F},
      {2.0F, 3.0F, 0.0F, 0.0F},
  }};
  assert(originBuffer.write(translatedOrigins.data(), sizeof(translatedOrigins),
                            0U, error));
  assert(directionBuffer.write(translatedDirections.data(),
                               sizeof(translatedDirections), 0U, error));
  assert(triangleBuffer.write(translatedVertices.data(),
                              sizeof(translatedVertices), 0U, error));
  assert(primitive.intersect(originBuffer, directionBuffer, triangleBuffer,
                             translatedRays.size(), translatedTriangles.size(),
                             hitBuffer, 3U, error));
  std::vector<TriangleHit> translatedCpu(1U, TriangleHit::miss());
  assert(viennaps::vulkan::ray::intersectCpu(
      translatedRays, translatedTriangles, translatedCpu, error));
  assert(hitBuffer.read(gpuHits.data(), gpuHits.size() * sizeof(TriangleHit),
                        0U, error));
  assert(gpuHits[0].triangleIndex == translatedCpu[0].triangleIndex &&
         exact(gpuHits[0].t, translatedCpu[0].t) &&
         exact(gpuHits[0].u, translatedCpu[0].u) &&
         exact(gpuHits[0].v, translatedCpu[0].v));
  assert(originBuffer.write(packedOrigins.data(),
                            packedOrigins.size() * sizeof(packedOrigins[0]), 0U,
                            error));
  assert(directionBuffer.write(
      packedDirections.data(),
      packedDirections.size() * sizeof(packedDirections[0]), 0U, error));
  assert(triangleBuffer.write(
      packedTriangles.data(),
      packedTriangles.size() * sizeof(packedTriangles[0]), 0U, error));

  viennaps::vulkan::runtime::HostVisibleBuffer missOriginBuffer;
  viennaps::vulkan::runtime::HostVisibleBuffer missDirectionBuffer;
  assert(primitive.createRayBuffer(misses.size(), missOriginBuffer,
                                   missDirectionBuffer, error));
  std::vector<std::array<float, 4>> packedMissOrigins;
  std::vector<std::array<float, 4>> packedMissDirections;
  for (const auto &ray : misses) {
    packedMissOrigins.push_back(
        {ray.origin[0], ray.origin[1], ray.origin[2], ray.tMin});
    packedMissDirections.push_back(
        {ray.direction[0], ray.direction[1], ray.direction[2], ray.tMax});
  }
  assert(missOriginBuffer.write(
      packedMissOrigins.data(),
      packedMissOrigins.size() * sizeof(packedMissOrigins[0]), 0U, error));
  assert(missDirectionBuffer.write(packedMissDirections.data(),
                                   packedMissDirections.size() *
                                       sizeof(packedMissDirections[0]),
                                   0U, error));
  assert(primitive.intersect(missOriginBuffer, missDirectionBuffer,
                             triangleBuffer, misses.size(), ordered.size(),
                             hitBuffer, 3U, error));
  assert(hitBuffer.read(gpuHits.data(), gpuHits.size() * sizeof(TriangleHit),
                        0U, error));
  std::vector<TriangleHit> missCpu(misses.size(), TriangleHit::miss());
  assert(viennaps::vulkan::ray::intersectCpu(misses, ordered, missCpu, error));
  for (std::size_t i = 0U; i < misses.size(); ++i) {
    assert(gpuHits[i].triangleIndex == missCpu[i].triangleIndex &&
           exact(gpuHits[i].t, missCpu[i].t) &&
           exact(gpuHits[i].u, missCpu[i].u) &&
           exact(gpuHits[i].v, missCpu[i].v));
  }
  assert(gpuHits[2].t == tailSentinel.t);
  assert(primitive.intersect(originBuffer, directionBuffer, triangleBuffer,
                             rays.size(), ordered.size(), hitBuffer, 3U,
                             error));
  assert(hitBuffer.read(gpuHits.data(), gpuHits.size() * sizeof(TriangleHit),
                        0U, error));
  const auto unchangedHits = gpuHits;
  assert(!primitive.intersect(originBuffer, directionBuffer, triangleBuffer,
                              rays.size(), ordered.size(), hitBuffer, 0U,
                              error));
  assert(hitBuffer.read(gpuHits.data(), gpuHits.size() * sizeof(TriangleHit),
                        0U, error));
  assert(gpuHits[0].triangleIndex == unchangedHits[0].triangleIndex &&
         exact(gpuHits[0].t, unchangedHits[0].t));
  std::array<float, 4> invalidNear = packedOrigins[0];
  invalidNear[3] = -1.0F;
  assert(originBuffer.write(&invalidNear, sizeof(invalidNear), 0U, error));
  assert(!primitive.intersect(originBuffer, directionBuffer, triangleBuffer,
                              rays.size(), ordered.size(), hitBuffer, 3U,
                              error));
  assert(hitBuffer.read(gpuHits.data(), gpuHits.size() * sizeof(TriangleHit),
                        0U, error));
  assert(gpuHits[0].triangleIndex == unchangedHits[0].triangleIndex &&
         exact(gpuHits[0].t, unchangedHits[0].t));
  const float nan = std::numeric_limits<float>::quiet_NaN();
  std::array<float, 4> invalidOrigin = packedOrigins[0];
  invalidOrigin[0] = nan;
  assert(originBuffer.write(&invalidOrigin, sizeof(invalidOrigin), 0U, error));
  assert(!primitive.intersect(originBuffer, directionBuffer, triangleBuffer,
                              rays.size(), ordered.size(), hitBuffer, 3U,
                              error));
  assert(hitBuffer.read(gpuHits.data(), gpuHits.size() * sizeof(TriangleHit),
                        0U, error));
  assert(gpuHits[0].triangleIndex == unchangedHits[0].triangleIndex &&
         exact(gpuHits[0].t, unchangedHits[0].t));
  assert(originBuffer.write(packedOrigins.data(),
                            packedOrigins.size() * sizeof(packedOrigins[0]), 0U,
                            error));
  assert(primitive.intersect(originBuffer, directionBuffer, triangleBuffer, 0U,
                             ordered.size(), hitBuffer, 3U, error));
  assert(hitBuffer.read(gpuHits.data(), gpuHits.size() * sizeof(TriangleHit),
                        0U, error));
  assert(gpuHits[0].triangleIndex == unchangedHits[0].triangleIndex &&
         exact(gpuHits[0].t, unchangedHits[0].t));
  std::cout << "triangle hit Vulkan dispatch PASS\n";
  return 0;
}
