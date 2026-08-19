// Copyright 2026 ViennaPS
// SPDX-License-Identifier: MIT
#include "multibounce_decision_producer.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <random>

namespace viennaps::vulkan::ray {
namespace {
bool fail(std::string &error, const char *message) {
  error = message;
  return false;
}
bool valid(float value) {
  return std::isfinite(value) && (value == 0.0F || std::isnormal(value));
}
bool finiteVec(const viennacore::Vec3D<float> &value) {
  for (const float component : value)
    if (!valid(component)) return false;
  return true;
}
void copyVec(const viennacore::Vec3D<float> &source, float (&target)[4]) {
  target[0] = source[0];
  target[1] = source[1];
  target[2] = source[2];
  target[3] = 0.0F;
}
} // namespace

bool MultibounceDecisionProducer::produce(
    const MultibounceDecisionInput &input, MultibounceDecision &output,
    std::string &error) {
  error.clear();
  if (input.particle == nullptr || input.rng == nullptr ||
      input.localData == nullptr)
    return fail(error, "particle, RNG, and local data are required");
  if (!input.frontFace)
    return fail(error, "decision producer requires a validated front-face hit");
  if (input.reflectionCount > input.maxReflections)
    return fail(error, "reflection count exceeds configured limit");
  if (input.reflectionCount == std::numeric_limits<std::uint32_t>::max())
    return fail(error, "reflection count would overflow");
  if (!valid(input.initialWeight) || !valid(input.weight) ||
      !valid(input.contribution) || input.initialWeight < 0.0F ||
      input.weight < 0.0F || !finiteVec(input.rayDirection) ||
      !finiteVec(input.geometricNormal) || !finiteVec(input.hitPoint))
    return fail(error, "decision producer received invalid FP32 state");

  MultibounceDecision staged{};
  staged.particle = input.particleId;
  staged.bounce = input.bounce;
  staged.sequence = input.sequence;
  staged.surfaceId = input.surfaceId;
  staged.weight = input.weight;
  staged.contribution = input.contribution;

  // This order is intentionally identical to rayTraceKernel.hpp: surface
  // collision, surface reflection, weight update, limit, then roulette.
  input.particle->surfaceCollision(
      input.weight, input.rayDirection, input.geometricNormal,
      input.primitiveId, input.materialId, *input.localData, input.globalData,
      *input.rng);
  const auto stickingDirection = input.particle->surfaceReflection(
      input.weight, input.rayDirection, input.geometricNormal,
      input.primitiveId, input.materialId, input.globalData, *input.rng);
  const float sticking = stickingDirection.first;
  if (!valid(sticking) || sticking < 0.0F || sticking > 1.0F ||
      !finiteVec(stickingDirection.second))
    return fail(error, "particle callback returned invalid reflection state");

  float nextWeight = input.weight - input.weight * sticking;
  if (!valid(nextWeight))
    return fail(error, "weight update left the FP32 domain");
  copyVec(input.hitPoint, staged.successorOrigin);
  copyVec(stickingDirection.second, staged.successorDirection);
  staged.nextWeight = nextWeight;

  if (nextWeight <= 0.0F) {
    staged.action = static_cast<std::uint32_t>(MultibounceAction::terminate);
    output = staged;
    return true;
  }
  const auto nextReflectionCount = input.reflectionCount + 1U;
  if (nextReflectionCount > input.maxReflections) {
    staged.action = static_cast<std::uint32_t>(MultibounceAction::terminate);
    output = staged;
    return true;
  }

  const float lowerThreshold = 0.1F * input.initialWeight;
  const float renewWeight = 0.3F * input.initialWeight;
  if (nextWeight < lowerThreshold) {
    std::uniform_real_distribution<> distribution;
    const double killProbability = 1.0 - nextWeight / renewWeight;
    if (distribution(*input.rng) < killProbability) {
      staged.action = static_cast<std::uint32_t>(
          MultibounceAction::rouletteReject);
      output = staged;
      return true;
    }
    nextWeight = renewWeight;
  }
  staged.nextWeight = nextWeight;
  staged.action = static_cast<std::uint32_t>(MultibounceAction::continueRay);
  output = staged;
  return true;
}

} // namespace viennaps::vulkan::ray
