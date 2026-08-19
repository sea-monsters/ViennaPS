// Copyright 2026 ViennaPS
// SPDX-License-Identifier: MIT
#include "multibounce_decision_producer.hpp"
#include "multibounce_frontier_queue.hpp"
#include "triangle_hit.hpp"
#include "triangle_hit_device.hpp"

#include <bit>
#include <cstring>
#include <iostream>
#include <random>

#ifndef VIENNAPS_VULKAN_MULTIBOUNCE_FRONTIER_QUEUE_SPV_PATH
#define VIENNAPS_VULKAN_MULTIBOUNCE_FRONTIER_QUEUE_SPV_PATH \
  "multibounce_frontier_queue.comp.spv"
#endif
#ifndef VIENNAPS_VULKAN_TRIANGLE_HIT_DEVICE_SPV_PATH
#define VIENNAPS_VULKAN_TRIANGLE_HIT_DEVICE_SPV_PATH "triangle_hit_device.comp.spv"
#endif

using namespace viennaps::vulkan::ray;
using viennaps::vulkan::runtime::DeviceBuffer;

namespace {
class FixtureParticle final
    : public viennaray::Particle<FixtureParticle, float> {
public:
  explicit FixtureParticle(float stickingValue) : sticking(stickingValue) {}
  void surfaceCollision(float, const viennacore::Vec3D<float> &,
                        const viennacore::Vec3D<float> &, unsigned int, int,
                        viennacore::PointData<float> &,
                        const viennacore::PointData<float> *,
                        viennacore::RNG &rng) final {
    std::uniform_real_distribution<> draw;
    collisionDraw = draw(rng);
  }
  std::pair<float, viennacore::Vec3D<float>>
  surfaceReflection(float, const viennacore::Vec3D<float> &,
                    const viennacore::Vec3D<float> &, unsigned int, int,
                    const viennacore::PointData<float> *,
                    viennacore::RNG &rng) final {
    std::uniform_real_distribution<> draw;
    reflectionDraw = draw(rng);
    return {sticking, {0.25F, 0.5F, 0.75F}};
  }
  float sticking{0.0F};
  double collisionDraw{0.0};
  double reflectionDraw{0.0};
};

MultibounceDecisionInput makeInput(
    FixtureParticle &particle, viennacore::RNG &rng,
    viennacore::PointData<float> &local, const MultibounceEvent &event,
    std::uint32_t reflectionCount, std::uint32_t maxReflections,
    std::uint32_t bounce) {
  MultibounceDecisionInput input;
  input.particle = &particle;
  input.rng = &rng;
  input.localData = &local;
  input.rayDirection = {event.direction[0], event.direction[1],
                        event.direction[2]};
  input.geometricNormal = {0.0F, 0.0F, 1.0F};
  input.hitPoint = {0.0F, 0.0F, 1.0F};
  input.particleId = event.particle;
  input.bounce = bounce;
  input.sequence = event.sequence;
  input.surfaceId = 10U + bounce;
  input.primitiveId = 1U + bounce;
  input.materialId = 0;
  input.initialWeight = 1.0F;
  input.weight = event.weight;
  input.contribution = event.weight;
  input.reflectionCount = reflectionCount;
  input.maxReflections = maxReflections;
  input.frontFace = true;
  return input;
}

bool same(float a, float b) {
  return std::bit_cast<std::uint32_t>(a) == std::bit_cast<std::uint32_t>(b);
}

bool sameNextDraws(viennacore::RNG actual, viennacore::RNG expected) {
  std::uniform_real_distribution<> draw;
  return draw(actual) == draw(expected) && draw(actual) == draw(expected);
}

bool sameResult(const MultibounceFrontierResult &actual,
                const MultibounceFrontierResult &expected) {
  if (actual.rounds != expected.rounds ||
      actual.totalEvents != expected.totalEvents ||
      actual.terminalEvents.size() != expected.terminalEvents.size() ||
      actual.accumulation.size() != expected.accumulation.size())
    return false;
  return (actual.terminalEvents.empty() ||
          std::memcmp(actual.terminalEvents.data(),
                      expected.terminalEvents.data(),
                      actual.terminalEvents.size() *
                          sizeof(MultibounceEvent)) == 0) &&
         (actual.accumulation.empty() ||
          std::memcmp(actual.accumulation.data(), expected.accumulation.data(),
                      actual.accumulation.size() *
                          sizeof(MultibounceAccumulation)) == 0);
}
} // namespace

int main() {
  std::string error;
  viennaps::vulkan::runtime::ComputeSession session;
  if (!session.initialize(error)) {
    std::cerr << "multibounce frontier queue FAIL [session]: " << error << '\n';
    return 1;
  }
  MultibounceFrontierQueue queue;
  if (!queue.initialize(session, VIENNAPS_VULKAN_MULTIBOUNCE_FRONTIER_QUEUE_SPV_PATH,
                        error)) {
    std::cerr << "multibounce frontier queue FAIL [initialize]: " << error
              << '\n';
    return 1;
  }
  DeviceTriangleHitPrimitive hitPrimitive;
  if (!hitPrimitive.initialize(session, VIENNAPS_VULKAN_TRIANGLE_HIT_DEVICE_SPV_PATH,
                               error)) {
    std::cerr << "multibounce frontier queue FAIL [hit initialize]: " << error
              << '\n';
    return 1;
  }
  const std::vector<Triangle> triangles{{{-10.0F, -10.0F, 1.0F},
                                         {10.0F, -10.0F, 1.0F},
                                         {-10.0F, 10.0F, 1.0F}}};

  std::vector<MultibounceEvent> events(2U);
  events[0].particle = 1U;
  events[0].sequence = 100U;
  events[0].activeFlag = 1U;
  events[0].weight = 1.0F;
  events[1].particle = 2U;
  events[1].sequence = 200U;
  events[1].activeFlag = 1U;
  events[1].weight = 1.0F;
  events[0].direction[2] = -1.0F;
  events[1].direction[2] = -1.0F;

  FixtureParticle high(0.0F);
  FixtureParticle low(0.95F);
  viennacore::RNG highRng(7U);
  viennacore::RNG lowRng(1U);
  viennacore::PointData<float> highLocal;
  viennacore::PointData<float> lowLocal;
  const auto limits = MultibounceFrontierLimits{2U, 2U, 1U, 2U};
  MultibounceFrontierResult published;

  for (std::uint32_t round = 0U; round < 3U; ++round) {
    std::vector<Ray> rays;
    for (const auto &event : events)
      rays.push_back({{event.origin[0], event.origin[1], event.origin[2]},
                      {event.direction[0], event.direction[1], event.direction[2]},
                      0.0F, 100.0F});
    std::vector<TriangleHit> cpuHits(rays.size());
    if (!intersectCpu(rays, triangles, cpuHits, error)) {
      std::cerr << "multibounce frontier queue FAIL [CPU hit]: " << error << '\n';
      return 1;
    }
    DeviceBuffer origins, directions, triangleBuffer, hitBuffer;
    if (!hitPrimitive.createRayBuffers(rays.size(), origins, directions, error) ||
        !hitPrimitive.createTriangleBuffer(triangles.size(), triangleBuffer, error) ||
        !hitPrimitive.createHitBuffer(rays.size(), hitBuffer, error) ||
        !hitPrimitive.uploadRays(rays, origins, directions, error) ||
        !hitPrimitive.uploadTriangles(triangles, triangleBuffer, error) ||
        !hitPrimitive.dispatch(origins, directions, triangleBuffer, rays.size(),
                               triangles.size(), hitBuffer, rays.size(), error)) {
      std::cerr << "multibounce frontier queue FAIL [GPU hit]: " << error << '\n';
      return 1;
    }
    std::vector<TriangleHit> gpuHits(rays.size());
    if (!hitPrimitive.downloadHits(rays.size(), hitBuffer, gpuHits, error)) {
      std::cerr << "multibounce frontier queue FAIL [GPU hit download]: " << error
                << '\n';
      return 1;
    }
    for (std::size_t i = 0U; i < rays.size(); ++i) {
      if (events[i].activeFlag != 0U &&
          (std::bit_cast<std::uint32_t>(gpuHits[i].t) !=
               std::bit_cast<std::uint32_t>(cpuHits[i].t) ||
           gpuHits[i].triangleIndex != cpuHits[i].triangleIndex ||
           std::bit_cast<std::uint32_t>(gpuHits[i].u) !=
               std::bit_cast<std::uint32_t>(cpuHits[i].u) ||
           std::bit_cast<std::uint32_t>(gpuHits[i].v) !=
               std::bit_cast<std::uint32_t>(cpuHits[i].v))) {
        std::cerr << "multibounce frontier queue FAIL [hit raw oracle]\n";
        return 1;
      }
    }
    MultibounceFrontier frontier;
    frontier.events = events;
    frontier.decisionStride = 3U;
    frontier.decisions.resize(events.size() * frontier.decisionStride);
    for (std::size_t lane = 0U; lane < events.size(); ++lane) {
      auto &event = frontier.events[lane];
      auto &slot = frontier.decisions[lane * frontier.decisionStride + event.bounce];
      slot.particle = event.particle;
      slot.bounce = event.bounce;
      slot.sequence = event.sequence;
      slot.weight = event.weight;
      slot.nextWeight = event.weight;
      slot.action = static_cast<std::uint32_t>(MultibounceAction::terminate);
      if (event.activeFlag == 0U) continue;
      auto input = lane == 0U
                       ? makeInput(high, highRng, highLocal, event, round, 2U,
                                   event.bounce)
                       : makeInput(low, lowRng, lowLocal, event, round, 1U,
                                   event.bounce);
      MultibounceDecision decision{};
      if (!MultibounceDecisionProducer::produce(input, decision, error)) {
        std::cerr << "multibounce frontier queue FAIL [producer]: " << error
                  << '\n';
        return 1;
      }
      slot = decision;
    }

    // Seed-1 low-weight survival consumes exactly collision + reflection +
    // roulette. The high-weight path consumes only its two callback draws.
    if (round == 0U) {
      std::uniform_real_distribution<> draw;
      viennacore::RNG expected(1U);
      draw(expected);
      draw(expected);
      draw(expected);
      if (!same(frontier.decisions[1U * frontier.decisionStride].nextWeight,
                0.3F) || !sameNextDraws(lowRng, expected)) {
        std::cerr << "multibounce frontier queue FAIL [RNG survival]\n";
        return 1;
      }
    }
    if (round == 1U) {
      std::uniform_real_distribution<> draw;
      viennacore::RNG expected(1U);
      // First frontier: collision, reflection, roulette. Second frontier:
      // collision and reflection only; max-reflection termination consumes no
      // roulette value.
      draw(expected);
      draw(expected);
      draw(expected);
      draw(expected);
      draw(expected);
      if (!sameNextDraws(lowRng, expected)) {
        std::cerr << "multibounce frontier queue FAIL [RNG limit]\n";
        return 1;
      }
    }
    if (!queue.run(std::span<const MultibounceFrontier>(&frontier, 1U), limits,
                   published, error) ||
        published.rounds != 1U || published.terminalEvents.size() != 2U ||
        published.accumulation.size() != 2U) {
      std::cerr << "multibounce frontier queue FAIL [GPU round]: " << error
                << '\n';
      return 1;
    }
    if (round == 0U &&
        (published.terminalEvents[0].bounce != 1U ||
         published.terminalEvents[1].bounce != 1U ||
         published.terminalEvents[0].activeFlag == 0U ||
         published.terminalEvents[1].activeFlag == 0U ||
         !same(published.terminalEvents[1].weight, 0.3F))) {
      std::cerr << "multibounce frontier queue FAIL [first frontier oracle]\n";
      return 1;
    }
    if (round == 1U && published.terminalEvents[1].activeFlag != 0U) {
      std::cerr << "multibounce frontier queue FAIL [limit terminal]\n";
      return 1;
    }
    events = published.terminalEvents;
  }

  if (published.terminalEvents[0].activeFlag != 0U ||
      published.terminalEvents[0].bounce != 3U ||
      published.terminalEvents[1].activeFlag != 0U) {
    std::cerr << "multibounce frontier queue FAIL [three-round terminal]\n";
    return 1;
  }
  {
    std::uniform_real_distribution<> draw;
    viennacore::RNG expected(7U);
    // Three high-weight frontiers consume the two callback draws only.
    for (std::uint32_t count = 0U; count < 6U; ++count)
      draw(expected);
    if (!sameNextDraws(highRng, expected)) {
      std::cerr << "multibounce frontier queue FAIL [RNG high]\n";
      return 1;
    }
  }

  auto bad = MultibounceFrontier{};
  bad.events = events;
  bad.decisionStride = 3U;
  bad.decisions.resize(events.size() * 3U);
  auto sentinel = published;
  if (queue.run(std::span<const MultibounceFrontier>(&bad, 1U),
                MultibounceFrontierLimits{2U, 1U, 1U, 2U}, published, error) ||
      !sameResult(published, sentinel)) {
    std::cerr << "multibounce frontier queue FAIL [capacity sentinel]\n";
    return 1;
  }
  // A malformed decision is a device-status check, so its event must be
  // active. The terminal frontier above intentionally contains inactive lanes.
  bad.events[0].activeFlag = 1U;
  bad.events[0].bounce = 0U;
  bad.events[0].weight = 1.0F;
  bad.events[0].nextWeight = 1.0F;
  bad.events[0].action =
      static_cast<std::uint32_t>(MultibounceAction::continueRay);
  bad.decisions[0].particle = 99U;
  published = sentinel;
  if (queue.run(std::span<const MultibounceFrontier>(&bad, 1U), limits,
                published, error) || !sameResult(published, sentinel)) {
    std::cerr << "multibounce frontier queue FAIL [malformed sentinel]\n";
    return 1;
  }
  MultibounceDecisionInput miss{};
  miss.frontFace = false;
  MultibounceDecision missDecision{};
  if (MultibounceDecisionProducer::produce(miss, missDecision, error)) {
    std::cerr << "multibounce frontier queue FAIL [backface]\n";
    return 1;
  }
  queue.reset();
  published = sentinel;
  if (queue.run(std::span<const MultibounceFrontier>(&bad, 1U), limits,
                published, error) || !sameResult(published, sentinel)) {
    std::cerr << "multibounce frontier queue FAIL [session sentinel]\n";
    return 1;
  }

  // The session can be reset and reinitialized while an external queue object
  // still exists. Its old VkDevice handles must never be used against the new
  // generation, and the caller-owned output must remain untouched.
  MultibounceFrontierQueue staleQueue;
  if (!staleQueue.initialize(session,
                             VIENNAPS_VULKAN_MULTIBOUNCE_FRONTIER_QUEUE_SPV_PATH,
                             error)) {
    std::cerr << "multibounce frontier queue FAIL [stale initialize]: " << error
              << '\n';
    return 1;
  }
  hitPrimitive.reset();
  session.reset();
  if (!session.initialize(error)) {
    std::cerr << "multibounce frontier queue FAIL [session reinitialize]: "
              << error << '\n';
    return 1;
  }
  published = sentinel;
  if (staleQueue.run(std::span<const MultibounceFrontier>(&bad, 1U), limits,
                     published, error) ||
      error != "frontier queue belongs to a stale compute session generation" ||
      !sameResult(published, sentinel)) {
    std::cerr << "multibounce frontier queue FAIL [stale generation sentinel]: "
              << error << '\n';
    return 1;
  }
  staleQueue.reset();
  std::cout << "multibounce frontier queue Vulkan dispatch PASS\n";
  return 0;
}
