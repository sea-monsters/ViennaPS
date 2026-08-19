// Copyright 2026 ViennaPS
// SPDX-License-Identifier: MIT
#include "multibounce_decision_producer.hpp"
#include "triangle_hit.hpp"
#include "triangle_hit_device.hpp"

#include <bit>
#include <array>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <random>
#include <string>
#include <vector>

#ifndef VIENNAPS_VULKAN_TRIANGLE_HIT_DEVICE_SPV_PATH
#define VIENNAPS_VULKAN_TRIANGLE_HIT_DEVICE_SPV_PATH "triangle_hit_device.comp.spv"
#endif
#ifndef VIENNAPS_VULKAN_MULTIBOUNCE_EVENT_SPV_PATH
#define VIENNAPS_VULKAN_MULTIBOUNCE_EVENT_SPV_PATH "multibounce_event.comp.spv"
#endif

using namespace viennaps::vulkan::ray;
using viennaps::vulkan::runtime::ComputeSession;
using viennaps::vulkan::runtime::DeviceBuffer;

namespace {
class ChainParticle final
    : public viennaray::Particle<ChainParticle, float> {
public:
  void surfaceCollision(float, const viennacore::Vec3D<float> &,
                        const viennacore::Vec3D<float> &, unsigned int, int,
                        viennacore::PointData<float> &, const viennacore::PointData<float> *,
                        viennacore::RNG &) final {}
  std::pair<float, viennacore::Vec3D<float>>
  surfaceReflection(float, const viennacore::Vec3D<float> &,
                    const viennacore::Vec3D<float> &, unsigned int, int,
                    const viennacore::PointData<float> *,
                    viennacore::RNG &) final {
    return {0.0F, {0.0F, 0.0F, 1.0F}};
  }
};

bool sameHit(const TriangleHit &left, const TriangleHit &right) {
  return std::bit_cast<std::uint32_t>(left.t) ==
             std::bit_cast<std::uint32_t>(right.t) &&
         left.triangleIndex == right.triangleIndex &&
         std::bit_cast<std::uint32_t>(left.u) ==
             std::bit_cast<std::uint32_t>(right.u) &&
         std::bit_cast<std::uint32_t>(left.v) ==
             std::bit_cast<std::uint32_t>(right.v);
}

bool runDeviceHit(ComputeSession &session, DeviceTriangleHitPrimitive &primitive,
                  const Ray &ray, const std::vector<Triangle> &triangles,
                  TriangleHit &hit, std::string &error) {
  DeviceBuffer origins, directions, triangleBuffer, hits;
  if (!primitive.createRayBuffers(1U, origins, directions, error) ||
      !primitive.createTriangleBuffer(triangles.size(), triangleBuffer, error) ||
      !primitive.createHitBuffer(1U, hits, error) ||
      !primitive.uploadRays(std::span<const Ray>(&ray, 1U), origins, directions,
                            error) ||
      !primitive.uploadTriangles(triangles, triangleBuffer, error) ||
      !primitive.dispatch(origins, directions, triangleBuffer, 1U,
                          triangles.size(), hits, 1U, error))
    return false;
  std::array<TriangleHit, 1U> downloaded{};
  if (!primitive.downloadHits(1U, hits, downloaded, error)) return false;
  hit = downloaded[0];
  return true;
}

MultibounceDecisionInput makeDecisionInput(
    ChainParticle &particle, viennacore::RNG &rng,
    viennacore::PointData<float> &localData, const Ray &ray,
    const TriangleHit &hit, const Triangle &triangle, std::uint32_t bounce,
    std::uint32_t reflectionCount, float weight) {
  const auto edge1 = std::array<float, 3>{triangle.b[0] - triangle.a[0],
                                          triangle.b[1] - triangle.a[1],
                                          triangle.b[2] - triangle.a[2]};
  const auto edge2 = std::array<float, 3>{triangle.c[0] - triangle.a[0],
                                          triangle.c[1] - triangle.a[1],
                                          triangle.c[2] - triangle.a[2]};
  const auto normal = std::array<float, 3>{edge1[1] * edge2[2] - edge1[2] * edge2[1],
                                          edge1[2] * edge2[0] - edge1[0] * edge2[2],
                                          edge1[0] * edge2[1] - edge1[1] * edge2[0]};
  viennacore::Vec3D<float> hitPoint{ray.origin[0] + hit.t * ray.direction[0],
                                    ray.origin[1] + hit.t * ray.direction[1],
                                    ray.origin[2] + hit.t * ray.direction[2]};
  MultibounceDecisionInput input;
  input.particle = &particle;
  input.rng = &rng;
  input.localData = &localData;
  input.rayDirection = ray.direction;
  input.geometricNormal = normal;
  input.hitPoint = hitPoint;
  input.particleId = 4U;
  input.bounce = bounce;
  input.sequence = 11U;
  input.surfaceId = hit.triangleIndex;
  input.primitiveId = hit.triangleIndex;
  input.materialId = 7;
  input.initialWeight = 1.0F;
  input.weight = weight;
  input.contribution = weight;
  input.reflectionCount = reflectionCount;
  input.maxReflections = 1U;
  input.frontFace = (ray.direction[0] * normal[0] +
                     ray.direction[1] * normal[1] +
                     ray.direction[2] * normal[2]) < 0.0F;
  return input;
}
} // namespace

int main() {
  std::string error;
  ComputeSession session;
  DeviceTriangleHitPrimitive hitPrimitive;
  if (!session.initialize(error) ||
      !hitPrimitive.initialize(session, VIENNAPS_VULKAN_TRIANGLE_HIT_DEVICE_SPV_PATH,
                               error)) {
    std::cerr << "multibounce hit decision chain FAIL [initialize]: " << error
              << '\n';
    return 1;
  }
  const std::vector<Triangle> triangles{
      {{{0.0F, 0.0F, 0.0F}}, {{1.0F, 0.0F, 0.0F}}, {{0.0F, 1.0F, 0.0F}}}};
  const Ray front{{0.25F, 0.25F, 1.0F}, {0.0F, 0.0F, -1.0F}, 0.0F, 10.0F};
  std::vector<TriangleHit> cpuHits(1U, TriangleHit::miss());
  TriangleHit gpuHit{};
  if (!intersectCpu(std::span<const Ray>(&front, 1U), triangles, cpuHits,
                    error) ||
      !runDeviceHit(session, hitPrimitive, front, triangles, gpuHit, error) ||
      !sameHit(cpuHits[0], gpuHit) || cpuHits[0].isMiss()) {
    std::cerr << "multibounce hit decision chain FAIL [hit oracle]: " << error
              << '\n';
    return 1;
  }

  ChainParticle particle;
  viennacore::RNG rng(9U);
  viennacore::PointData<float> localData;
  auto firstInput = makeDecisionInput(particle, rng, localData, front, gpuHit,
                                       triangles[0], 0U, 0U, 1.0F);
  MultibounceDecision firstDecision{};
  if (!MultibounceDecisionProducer::produce(firstInput, firstDecision, error) ||
      firstDecision.action !=
          static_cast<std::uint32_t>(MultibounceAction::continueRay)) {
    std::cerr << "multibounce hit decision chain FAIL [CPU decision]: " << error
              << '\n';
    return 1;
  }
  // Slice-01 consumes a bounded two-slot decision stream. The first slot is
  // the sole producer handoff for this card; the second is a fixed terminal
  // record, avoiding a host per-bounce loop.
  MultibounceDecision secondDecision = firstDecision;
  secondDecision.bounce = 1U;
  secondDecision.weight = firstDecision.nextWeight;
  secondDecision.nextWeight = firstDecision.nextWeight;
  secondDecision.contribution = 0.0F;
  secondDecision.action =
      static_cast<std::uint32_t>(MultibounceAction::terminate);

  DeviceMultibounceSlice slice;
  if (!slice.initialize(session, VIENNAPS_VULKAN_MULTIBOUNCE_EVENT_SPV_PATH,
                        error)) {
    std::cerr << "multibounce hit decision chain FAIL [slice initialize]: "
              << error << '\n';
    return 1;
  }
  MultibounceEvent event{};
  event.origin[0] = front.origin[0];
  event.origin[1] = front.origin[1];
  event.origin[2] = front.origin[2];
  event.direction[0] = front.direction[0];
  event.direction[1] = front.direction[1];
  event.direction[2] = front.direction[2];
  event.particle = firstDecision.particle;
  event.sequence = firstDecision.sequence;
  event.activeFlag = 1U;
  event.weight = 1.0F;
  MultibounceSliceResult result;
  const std::vector<MultibounceDecision> decisions{firstDecision,
                                                    secondDecision};
  if (!slice.run(std::span<const MultibounceEvent>(&event, 1U), decisions, 2U,
                 1U, result, error) ||
      result.events.size() != 1U || result.accumulation.size() != 2U ||
      result.events[0].action != secondDecision.action ||
      std::bit_cast<std::uint32_t>(result.events[0].origin[0]) !=
          std::bit_cast<std::uint32_t>(secondDecision.successorOrigin[0]) ||
      std::bit_cast<std::uint32_t>(result.events[0].direction[2]) !=
          std::bit_cast<std::uint32_t>(secondDecision.successorDirection[2]) ||
      result.accumulation[0].weightBits !=
          std::bit_cast<std::uint32_t>(firstDecision.contribution)) {
    std::cerr << "multibounce hit decision chain FAIL [GPU raw oracle]: "
              << error << '\n';
    return 1;
  }

  // A miss never enters the producer and preserves the caller sentinel.
  const Ray miss{{2.0F, 2.0F, 1.0F}, {0.0F, 0.0F, -1.0F}, 0.0F, 10.0F};
  TriangleHit missHit{};
  MultibounceDecision sentinel = firstDecision;
  const auto published = sentinel;
  if (!runDeviceHit(session, hitPrimitive, miss, triangles, missHit, error) ||
      !missHit.isMiss() || std::memcmp(&sentinel, &published, sizeof(sentinel)) != 0) {
    std::cerr << "multibounce hit decision chain FAIL [miss fail-closed]\n";
    return 1;
  }

  // A back-face hit is rejected before callback/output publication.
  const Ray back{{0.25F, 0.25F, -1.0F}, {0.0F, 0.0F, 1.0F}, 0.0F, 10.0F};
  TriangleHit backHit{};
  auto backInput = makeDecisionInput(particle, rng, localData, back, backHit,
                                     triangles[0], 0U, 0U, 1.0F);
  backInput.frontFace = false;
  auto backSentinel = sentinel;
  if (!runDeviceHit(session, hitPrimitive, back, triangles, backHit, error) ||
      backHit.isMiss() ||
      MultibounceDecisionProducer::produce(backInput, backSentinel, error) ||
      std::memcmp(&backSentinel, &sentinel, sizeof(sentinel)) != 0) {
    std::cerr << "multibounce hit decision chain FAIL [backface fail-closed]\n";
    return 1;
  }

  slice.reset();
  auto lostSentinel = result;
  if (slice.run(std::span<const MultibounceEvent>(&event, 1U), decisions, 2U,
                1U, lostSentinel, error) ||
      lostSentinel.events.size() != result.events.size() ||
      lostSentinel.events[0].sequence != result.events[0].sequence) {
    std::cerr << "multibounce hit decision chain FAIL [session-loss]\n";
    return 1;
  }
  std::cout << "multibounce hit decision chain PASS\n";
  return 0;
}
