// Copyright 2026 ViennaPS
// SPDX-License-Identifier: MIT
#include "multibounce_decision_producer.hpp"

#include <bit>
#include <iostream>
#include <random>
#include <string>

#ifndef VIENNAPS_VULKAN_MULTIBOUNCE_EVENT_SPV_PATH
#define VIENNAPS_VULKAN_MULTIBOUNCE_EVENT_SPV_PATH "multibounce_event.comp.spv"
#endif

using namespace viennaps::vulkan::ray;

namespace {
class FixtureParticle final
    : public viennaray::Particle<FixtureParticle, float> {
public:
  explicit FixtureParticle(float stickingValue = 0.95F)
      : sticking(stickingValue) {}
  void surfaceCollision(float, const viennacore::Vec3D<float> &,
                        const viennacore::Vec3D<float> &, unsigned int, int,
                        viennacore::PointData<float> &, const viennacore::PointData<float> *,
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
  float sticking{0.95F};
  double collisionDraw{0.0};
  double reflectionDraw{0.0};
};

MultibounceDecisionInput makeInput(FixtureParticle &particle,
                                   viennacore::RNG &rng,
                                   std::uint32_t reflectionCount,
                                   std::uint32_t maxReflections) {
  static viennacore::PointData<float> localData;
  MultibounceDecisionInput input;
  input.particle = &particle;
  input.rng = &rng;
  input.localData = &localData;
  input.rayDirection = {0.0F, 0.0F, -1.0F};
  input.geometricNormal = {0.0F, 0.0F, 1.0F};
  input.hitPoint = {2.0F, 3.0F, 4.0F};
  input.particleId = 7U;
  input.bounce = 1U;
  input.sequence = 19U;
  input.surfaceId = 3U;
  input.primitiveId = 3U;
  input.initialWeight = 1.0F;
  input.weight = 1.0F;
  input.contribution = 1.0F;
  input.reflectionCount = reflectionCount;
  input.maxReflections = maxReflections;
  input.frontFace = true;
  return input;
}
} // namespace

int main() {
  std::string error;
  FixtureParticle particle;
  viennacore::RNG rng(42U);
  auto input = makeInput(particle, rng, 1U, 1U);
  MultibounceDecision decision{};
  const float expectedPostReflectionWeight = 1.0F - 1.0F * 0.95F;
  if (!MultibounceDecisionProducer::produce(input, decision, error) ||
      decision.action != static_cast<std::uint32_t>(MultibounceAction::terminate) ||
      std::bit_cast<std::uint32_t>(decision.nextWeight) !=
          std::bit_cast<std::uint32_t>(expectedPostReflectionWeight) ||
      std::bit_cast<std::uint32_t>(decision.successorOrigin[0]) !=
          std::bit_cast<std::uint32_t>(2.0F) ||
      std::bit_cast<std::uint32_t>(decision.successorDirection[2]) !=
          std::bit_cast<std::uint32_t>(0.75F)) {
    std::cerr << "multibounce decision producer FAIL [limit/order] " << error
              << '\n';
    return 1;
  }
  const double collisionDraw = particle.collisionDraw;
  const double reflectionDraw = particle.reflectionDraw;
  std::uniform_real_distribution<> dist;
  viennacore::RNG expectedRng(42U);
  if (dist(expectedRng) != collisionDraw || dist(expectedRng) != reflectionDraw) {
    std::cerr << "multibounce decision producer FAIL [callback RNG order]\n";
    return 1;
  }
  if (dist(rng) != dist(expectedRng)) {
    std::cerr << "multibounce decision producer FAIL [pre-limit roulette draw]\n";
    return 1;
  }
  const auto hasExactlyThreeDraws = [](viennacore::RNG &actual,
                                       const std::uint64_t seed) {
    viennacore::RNG expected(seed);
    std::uniform_real_distribution<> reference;
    reference(expected); // surfaceCollision callback draw
    reference(expected); // surfaceReflection callback draw
    reference(expected); // roulette draw
    return reference(actual) == reference(expected) &&
           reference(actual) == reference(expected);
  };
  FixtureParticle survivor;
  viennacore::RNG survivorRng(1U);
  auto survivorInput = makeInput(survivor, survivorRng, 0U, 1U);
  MultibounceDecision survivorDecision{};
  if (!MultibounceDecisionProducer::produce(survivorInput, survivorDecision,
                                             error) ||
      survivorDecision.action !=
          static_cast<std::uint32_t>(MultibounceAction::continueRay) ||
      std::bit_cast<std::uint32_t>(survivorDecision.nextWeight) !=
          std::bit_cast<std::uint32_t>(0.3F) ||
      !hasExactlyThreeDraws(survivorRng, 1U)) {
    std::cerr << "multibounce decision producer FAIL [roulette branch] "
              << error << " action=" << survivorDecision.action << '\n';
    return 1;
  }
  FixtureParticle rejected;
  viennacore::RNG rejectedRng(42U);
  auto rejectedInput = makeInput(rejected, rejectedRng, 0U, 1U);
  MultibounceDecision rejectedDecision{};
  if (!MultibounceDecisionProducer::produce(rejectedInput, rejectedDecision,
                                             error) ||
      rejectedDecision.action !=
          static_cast<std::uint32_t>(MultibounceAction::rouletteReject) ||
      !hasExactlyThreeDraws(rejectedRng, 42U)) {
    std::cerr << "multibounce decision producer FAIL [roulette reject] "
              << error << '\n';
    return 1;
  }
  auto invalid = input;
  invalid.frontFace = false;
  const auto sentinel = decision;
  if (MultibounceDecisionProducer::produce(invalid, decision, error) ||
      std::bit_cast<std::uint32_t>(decision.nextWeight) !=
          std::bit_cast<std::uint32_t>(sentinel.nextWeight)) {
    std::cerr << "multibounce decision producer FAIL [invalid hit]\n";
    return 1;
  }

  // Feed two CPU-produced decision streams directly into Slice-01. The first
  // lane is high-weight continuation; the second is low-weight roulette
  // survival (seed 1). The second event in each lane is pre-limit termination.
  viennacore::RNG highRng(7U);
  viennacore::RNG lowRng(1U);
  FixtureParticle highParticle(0.0F);
  FixtureParticle lowParticle(0.95F);
  auto highInput = makeInput(highParticle, highRng, 0U, 1U);
  auto lowInput = makeInput(lowParticle, lowRng, 0U, 1U);
  highInput.bounce = 0U;
  lowInput.bounce = 0U;
  highInput.particleId = 7U;
  highInput.sequence = 19U;
  lowInput.particleId = 8U;
  lowInput.sequence = 20U;
  MultibounceDecision highFirst{}, lowFirst{};
  if (!MultibounceDecisionProducer::produce(highInput, highFirst, error) ||
      !MultibounceDecisionProducer::produce(lowInput, lowFirst, error) ||
      highFirst.action != static_cast<std::uint32_t>(MultibounceAction::continueRay) ||
      lowFirst.action != static_cast<std::uint32_t>(MultibounceAction::continueRay)) {
    std::cerr << "multibounce decision producer FAIL [GPU input decisions] "
              << error << '\n';
    return 1;
  }
  auto highSecondInput = highInput;
  highSecondInput.bounce = 1U;
  highSecondInput.reflectionCount = 1U;
  highSecondInput.weight = highFirst.nextWeight;
  auto lowSecondInput = lowInput;
  lowSecondInput.bounce = 1U;
  lowSecondInput.reflectionCount = 1U;
  lowSecondInput.weight = lowFirst.nextWeight;
  MultibounceDecision highSecond{}, lowSecond{};
  if (!MultibounceDecisionProducer::produce(highSecondInput, highSecond,
                                            error) ||
      !MultibounceDecisionProducer::produce(lowSecondInput, lowSecond, error)) {
    std::cerr << "multibounce decision producer FAIL [GPU second decisions] "
              << error << '\n';
    return 1;
  }
  viennacore::RNG gpuSessionSeed(99U);
  (void)gpuSessionSeed;
  viennaps::vulkan::runtime::ComputeSession session;
  DeviceMultibounceSlice deviceSlice;
  if (!session.initialize(error) ||
      !deviceSlice.initialize(session, VIENNAPS_VULKAN_MULTIBOUNCE_EVENT_SPV_PATH,
                              error)) {
    std::cerr << "multibounce decision producer FAIL [GPU initialize] "
              << error << '\n';
    return 1;
  }
  std::vector<MultibounceEvent> events(2U);
  events[0].particle = 7U;
  events[0].sequence = 19U;
  events[0].activeFlag = 1U;
  events[0].weight = 1.0F;
  events[1].particle = 8U;
  events[1].sequence = 20U;
  events[1].activeFlag = 1U;
  events[1].weight = 1.0F;
  std::vector<MultibounceDecision> gpuDecisions{highFirst, highSecond,
                                                lowFirst, lowSecond};
  MultibounceSliceResult gpuResult;
  if (!deviceSlice.run(events, gpuDecisions, 2U, 1U, gpuResult, error) ||
      gpuResult.events.size() != 2U || gpuResult.accumulation.size() != 4U ||
      gpuResult.events[0].action != highSecond.action ||
      gpuResult.events[1].action != lowSecond.action ||
      std::bit_cast<std::uint32_t>(gpuResult.events[0].origin[0]) !=
          std::bit_cast<std::uint32_t>(highSecond.successorOrigin[0]) ||
      std::bit_cast<std::uint32_t>(gpuResult.events[1].direction[2]) !=
          std::bit_cast<std::uint32_t>(lowSecond.successorDirection[2]) ||
      gpuResult.accumulation[0].weightBits !=
          std::bit_cast<std::uint32_t>(highFirst.contribution) ||
      gpuResult.accumulation[1].weightBits !=
          std::bit_cast<std::uint32_t>(lowFirst.contribution)) {
    std::cerr << "multibounce decision producer FAIL [GPU raw-bit oracle] "
              << error << " first=" << highFirst.bounce << "/" << highFirst.sequence
              << " second=" << highSecond.bounce << "/" << highSecond.sequence
              << " low=" << lowFirst.bounce << "/" << lowFirst.sequence << "/"
              << lowSecond.bounce << "/" << lowSecond.sequence << '\n';
    return 1;
  }
  std::cout << "multibounce decision producer PASS\n";
  return 0;
}
