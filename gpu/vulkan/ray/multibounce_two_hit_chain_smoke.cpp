// Copyright 2026 ViennaPS
// SPDX-License-Identifier: MIT
#include "multibounce_decision_producer.hpp"
#include "triangle_hit.hpp"
#include "triangle_hit_device.hpp"

#include <array>
#include <bit>
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
class TwoHitParticle final
    : public viennaray::Particle<TwoHitParticle, float> {
public:
  void surfaceCollision(float, const viennacore::Vec3D<float> &,
                        const viennacore::Vec3D<float> &, unsigned int, int,
                        viennacore::PointData<float> &, const viennacore::PointData<float> *,
                        viennacore::RNG &rng) final {
    std::uniform_real_distribution<> draw;
    collisionDraws.push_back(draw(rng));
  }
  std::pair<float, viennacore::Vec3D<float>>
  surfaceReflection(float, const viennacore::Vec3D<float> &,
                    const viennacore::Vec3D<float> &, unsigned int, int,
                    const viennacore::PointData<float> *,
                    viennacore::RNG &rng) final {
    std::uniform_real_distribution<> draw;
    reflectionDraws.push_back(draw(rng));
    return {0.0F, {0.0F, 0.0F, 1.0F}};
  }
  std::vector<double> collisionDraws;
  std::vector<double> reflectionDraws;
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

bool runDeviceHit(DeviceTriangleHitPrimitive &primitive, const Ray &ray,
                  const std::vector<Triangle> &triangles, TriangleHit &hit,
                  std::string &error) {
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

viennacore::Vec3D<float> normalOf(const Triangle &triangle) {
  const auto e1 = std::array<float, 3>{triangle.b[0] - triangle.a[0],
                                      triangle.b[1] - triangle.a[1],
                                      triangle.b[2] - triangle.a[2]};
  const auto e2 = std::array<float, 3>{triangle.c[0] - triangle.a[0],
                                      triangle.c[1] - triangle.a[1],
                                      triangle.c[2] - triangle.a[2]};
  return {e1[1] * e2[2] - e1[2] * e2[1],
          e1[2] * e2[0] - e1[0] * e2[2],
          e1[0] * e2[1] - e1[1] * e2[0]};
}

MultibounceDecisionInput makeInput(
    TwoHitParticle &particle, viennacore::RNG &rng,
    viennacore::PointData<float> &localData, const Ray &ray,
    const TriangleHit &hit, const Triangle &triangle, std::uint32_t bounce,
    std::uint32_t reflectionCount, float weight) {
  const auto normal = normalOf(triangle);
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
  input.particleId = 6U;
  input.bounce = bounce;
  input.sequence = 17U;
  input.surfaceId = hit.triangleIndex;
  input.primitiveId = hit.triangleIndex;
  input.materialId = 9;
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

Ray successorRay(const MultibounceDecision &decision) {
  return {{{decision.successorOrigin[0], decision.successorOrigin[1],
            decision.successorOrigin[2]}},
          {{decision.successorDirection[0], decision.successorDirection[1],
            decision.successorDirection[2]}},
          0.0F, 10.0F};
}
} // namespace

int main() {
  std::string error;
  ComputeSession session;
  DeviceTriangleHitPrimitive hitPrimitive;
  if (!session.initialize(error) ||
      !hitPrimitive.initialize(session, VIENNAPS_VULKAN_TRIANGLE_HIT_DEVICE_SPV_PATH,
                               error)) {
    std::cerr << "multibounce two-hit chain FAIL [initialize]: " << error << '\n';
    return 1;
  }
  const std::vector<Triangle> triangles{
      {{{0.0F, 0.0F, 0.0F}}, {{1.0F, 0.0F, 0.0F}}, {{0.0F, 1.0F, 0.0F}}},
      {{{0.0F, 1.0F, 2.0F}}, {{1.0F, 0.0F, 2.0F}}, {{0.0F, 0.0F, 2.0F}}}};
  const Ray firstRay{{0.25F, 0.25F, 1.0F}, {0.0F, 0.0F, -1.0F}, 0.0F, 10.0F};
  std::vector<TriangleHit> cpuFirst(1U, TriangleHit::miss());
  TriangleHit gpuFirst{};
  if (!intersectCpu(std::span<const Ray>(&firstRay, 1U), triangles, cpuFirst,
                    error) ||
      !runDeviceHit(hitPrimitive, firstRay, triangles, gpuFirst, error) ||
      !sameHit(cpuFirst[0], gpuFirst) || cpuFirst[0].triangleIndex != 0U) {
    std::cerr << "multibounce two-hit chain FAIL [first hit oracle]: " << error
              << '\n';
    return 1;
  }

  TwoHitParticle particle;
  viennacore::RNG rng(13U);
  viennacore::PointData<float> localData;
  auto firstInput = makeInput(particle, rng, localData, firstRay, gpuFirst,
                              triangles[0], 0U, 0U, 1.0F);
  MultibounceDecision firstDecision{};
  if (!MultibounceDecisionProducer::produce(firstInput, firstDecision, error) ||
      firstDecision.action !=
          static_cast<std::uint32_t>(MultibounceAction::continueRay)) {
    std::cerr << "multibounce two-hit chain FAIL [first decision]: " << error
              << '\n';
    return 1;
  }

  // The second device ray is reconstructed byte-for-byte from the host
  // callback decision, not from a host bounce loop or a recomputed direction.
  const Ray secondRay = successorRay(firstDecision);
  std::vector<Triangle> secondGeometry{triangles[1]};
  std::vector<TriangleHit> cpuSecond(1U, TriangleHit::miss());
  TriangleHit gpuSecond{};
  if (!intersectCpu(std::span<const Ray>(&secondRay, 1U), secondGeometry,
                    cpuSecond, error) ||
      !runDeviceHit(hitPrimitive, secondRay, secondGeometry, gpuSecond, error) ||
      !sameHit(cpuSecond[0], gpuSecond) || cpuSecond[0].triangleIndex != 0U) {
    std::cerr << "multibounce two-hit chain FAIL [second hit oracle]: " << error
              << '\n';
    return 1;
  }
  auto secondInput = makeInput(particle, rng, localData, secondRay, gpuSecond,
                               secondGeometry[0], 1U, 1U,
                               firstDecision.nextWeight);
  MultibounceDecision secondDecision{};
  if (!MultibounceDecisionProducer::produce(secondInput, secondDecision,
                                             error) ||
      secondDecision.action !=
          static_cast<std::uint32_t>(MultibounceAction::terminate)) {
    std::cerr << "multibounce two-hit chain FAIL [limit termination]: " << error
              << '\n';
    return 1;
  }
  std::uniform_real_distribution<> expectedDraw;
  viennacore::RNG expectedRng(13U);
  for (int i = 0; i < 4; ++i) expectedDraw(expectedRng);
  if (expectedDraw(rng) != expectedDraw(expectedRng)) {
    std::cerr << "multibounce two-hit chain FAIL [second limit roulette draw]\n";
    return 1;
  }

  DeviceMultibounceSlice slice;
  if (!slice.initialize(session, VIENNAPS_VULKAN_MULTIBOUNCE_EVENT_SPV_PATH,
                        error)) {
    std::cerr << "multibounce two-hit chain FAIL [slice initialize]: " << error
              << '\n';
    return 1;
  }
  MultibounceEvent event{};
  event.origin[0] = firstRay.origin[0];
  event.origin[1] = firstRay.origin[1];
  event.origin[2] = firstRay.origin[2];
  event.direction[2] = firstRay.direction[2];
  event.particle = firstDecision.particle;
  event.sequence = firstDecision.sequence;
  event.activeFlag = 1U;
  event.weight = 1.0F;
  const std::vector<MultibounceDecision> decisions{firstDecision, secondDecision};
  MultibounceSliceResult result;
  if (!slice.run(std::span<const MultibounceEvent>(&event, 1U), decisions, 2U,
                 1U, result, error) ||
      result.events.size() != 1U || result.accumulation.size() != 2U ||
      result.events[0].action != secondDecision.action ||
      std::bit_cast<std::uint32_t>(result.events[0].origin[2]) !=
          std::bit_cast<std::uint32_t>(secondDecision.successorOrigin[2]) ||
      std::bit_cast<std::uint32_t>(result.events[0].direction[2]) !=
          std::bit_cast<std::uint32_t>(secondDecision.successorDirection[2]) ||
      result.accumulation[0].weightBits !=
          std::bit_cast<std::uint32_t>(firstDecision.contribution)) {
    std::cerr << "multibounce two-hit chain FAIL [GPU raw oracle]: " << error
              << '\n';
    return 1;
  }

  const Ray miss{{0.25F, 0.25F, 1.0F}, {1.0F, 0.0F, 0.0F}, 0.0F, 10.0F};
  TriangleHit missHit{};
  const auto sentinel = secondDecision;
  auto missDecision = sentinel;
  if (!runDeviceHit(hitPrimitive, miss, secondGeometry, missHit, error) ||
      !missHit.isMiss() || std::memcmp(&missDecision, &sentinel,
                                       sizeof(sentinel)) != 0) {
    std::cerr << "multibounce two-hit chain FAIL [second miss sentinel]\n";
    return 1;
  }
  const Ray back{{0.25F, 0.25F, 3.0F}, {0.0F, 0.0F, -1.0F}, 0.0F, 10.0F};
  TriangleHit backHit{};
  auto backInput = makeInput(particle, rng, localData, back, gpuSecond,
                             secondGeometry[0], 1U, 1U, 1.0F);
  backInput.frontFace = false;
  auto backDecision = sentinel;
  if (!runDeviceHit(hitPrimitive, back, secondGeometry, backHit, error) ||
      backHit.isMiss() ||
      MultibounceDecisionProducer::produce(backInput, backDecision, error) ||
      std::memcmp(&backDecision, &sentinel, sizeof(sentinel)) != 0) {
    std::cerr << "multibounce two-hit chain FAIL [second backface sentinel]\n";
    return 1;
  }
  slice.reset();
  auto lost = result;
  if (slice.run(std::span<const MultibounceEvent>(&event, 1U), decisions, 2U,
                1U, lost, error) || lost.events.size() != result.events.size() ||
      lost.events[0].sequence != result.events[0].sequence) {
    std::cerr << "multibounce two-hit chain FAIL [session loss]\n";
    return 1;
  }
  std::cout << "multibounce two-hit chain PASS\n";
  return 0;
}
