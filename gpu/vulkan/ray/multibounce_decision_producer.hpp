// Copyright 2026 ViennaPS
// SPDX-License-Identifier: MIT
#pragma once

#include <cstdint>
#include <string>

#include <rayParticle.hpp>
#include <vcPointData.hpp>
#include <vcRNG.hpp>
#include <vcVectorType.hpp>

#include "multibounce_event.hpp"

namespace viennaps::vulkan::ray {

// A validated front-face hit plus caller-owned ViennaRay state. The producer
// mutates only localData and rng, exactly as the ViennaRay callbacks do.
struct MultibounceDecisionInput {
  viennaray::AbstractParticle<float> *particle{nullptr};
  viennacore::RNG *rng{nullptr};
  viennacore::PointData<float> *localData{nullptr};
  const viennacore::PointData<float> *globalData{nullptr};
  viennacore::Vec3D<float> rayDirection{};
  viennacore::Vec3D<float> geometricNormal{};
  viennacore::Vec3D<float> hitPoint{};
  std::uint32_t particleId{0U};
  std::uint32_t bounce{0U};
  std::uint32_t sequence{0U};
  std::uint32_t surfaceId{0U};
  std::uint32_t primitiveId{0U};
  int materialId{0};
  float initialWeight{0.0F};
  float weight{0.0F};
  float contribution{0.0F};
  std::uint32_t reflectionCount{0U};
  std::uint32_t maxReflections{0U};
  bool frontFace{false};
};

class MultibounceDecisionProducer {
public:
  [[nodiscard]] static bool produce(const MultibounceDecisionInput &input,
                                    MultibounceDecision &output,
                                    std::string &error);
};

} // namespace viennaps::vulkan::ray
