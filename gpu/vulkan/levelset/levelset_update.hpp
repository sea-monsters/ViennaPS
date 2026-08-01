// Copyright 2026 ViennaPS
// SPDX-License-Identifier: MIT

#pragma once

#include "../runtime/compute_session.hpp"

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace viennaps::vulkan::levelset {

// CSR representation of ViennaLS TempRatesStop data. Each point owns at least
// one rate entry and its final stopValue must be +/- max(float), matching the
// sentinel consumed by Advect::updateLevelSet().
struct LevelSetUpdateInput {
  std::span<const float> values;
  std::span<const std::uint32_t> rateOffsets;
  std::span<const float> gradients;
  std::span<const float> dissipations;
  std::span<const float> stopValues;
  float timeStep = 0.0F;
  float integrationCutoff = 0.0F;
  bool checkDissipation = false;
};

// Executes only the value-update phase of ViennaLS Forward Euler. The caller
// owns the session and SPIR-V program. Output is transactional: it is replaced
// only after a successful dispatch and status readback.
[[nodiscard]] bool updateLevelSetFp32(runtime::ComputeSession &session,
                                      const runtime::SpirvProgram &program,
                                      const LevelSetUpdateInput &input,
                                      std::vector<float> &output,
                                      std::string &error);

} // namespace viennaps::vulkan::levelset
