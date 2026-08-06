#pragma once

#include <vcRNG.hpp>

#include <random>

namespace viennaps::ray {

using namespace viennacore;

/// Factors that define the Russian-roulette window. They are hard-coded to match
/// ViennaRay's `rayTraceKernel.hpp` and the GPU `continueRay` helper so that the
/// estimator stays unbiased across backends.
inline constexpr float kRussianRouletteLowerThresholdFactor = 0.1f;
inline constexpr float kRussianRouletteRenewWeightFactor = 0.3f;

/// Russian-roulette continuation matching the CPU trace kernel in
/// `rayTraceKernel.hpp` and the GPU `continueRay` helper in ViennaRay.
///
/// Returns true if the ray should continue, false if it is killed. When
/// continuing, `rayWeight` may be boosted to `renewWeight` to keep the
/// estimator unbiased.
[[nodiscard]] inline bool russianRoulette(float &rayWeight,
                                          const float initialWeight, RNG &rng) {
  if (initialWeight <= 0.0f) {
    return false;
  }
  const float lowerThreshold =
      kRussianRouletteLowerThresholdFactor * initialWeight;
  if (rayWeight >= lowerThreshold) {
    return true;
  }
  const float renewWeight = kRussianRouletteRenewWeightFactor * initialWeight;
  const float killProbability = 1.0f - rayWeight / renewWeight;
  std::uniform_real_distribution<double> dist(0.0, 1.0);
  if (dist(rng) < killProbability) {
    return false;
  }
  rayWeight = renewWeight;
  return true;
}

} // namespace viennaps::ray
