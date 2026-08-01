// Copyright 2026 ViennaPS
// SPDX-License-Identifier: MIT

#include "levelset_update.hpp"

#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <span>
#include <string>
#include <vector>

namespace {

using viennaps::vulkan::levelset::LevelSetUpdateInput;

[[nodiscard]] bool shouldClamp(const float gradient, const float velocity,
                               const bool checkDissipation) {
  return (checkDissipation && gradient < 0.0F && velocity > 0.0F) ||
         (gradient > 0.0F && velocity < 0.0F);
}

[[nodiscard]] std::vector<float>
cpuReference(const LevelSetUpdateInput &input) {
  std::vector<float> output(input.values.begin(), input.values.end());
  for (std::size_t point = 0; point < input.values.size(); ++point) {
    float &value = output[point];
    if (std::abs(value) > input.integrationCutoff) {
      continue;
    }
    std::uint32_t rateIndex = input.rateOffsets[point];
    float remainingTime = input.timeStep;
    float gradient = input.gradients[rateIndex];
    float velocity = gradient - input.dissipations[rateIndex];
    if (shouldClamp(gradient, velocity, input.checkDissipation)) {
      velocity = 0.0F;
    }
    float rate = remainingTime * velocity;
    while (std::abs(input.stopValues[rateIndex] - value) < std::abs(rate)) {
      remainingTime -=
          std::abs((input.stopValues[rateIndex] - value) / velocity);
      value = input.stopValues[rateIndex++];
      gradient = input.gradients[rateIndex];
      velocity = gradient - input.dissipations[rateIndex];
      if (shouldClamp(gradient, velocity, input.checkDissipation)) {
        velocity = 0.0F;
      }
      rate = remainingTime * velocity;
    }
    value -= rate;
  }
  return output;
}

[[nodiscard]] bool exactEqual(const std::span<const float> lhs,
                              const std::span<const float> rhs) {
  if (lhs.size() != rhs.size()) {
    return false;
  }
  for (std::size_t i = 0; i < lhs.size(); ++i) {
    if (std::bit_cast<std::uint32_t>(lhs[i]) !=
        std::bit_cast<std::uint32_t>(rhs[i])) {
      std::cerr << "mismatch at " << i << ": expected=" << rhs[i]
                << " actual=" << lhs[i] << '\n';
      return false;
    }
  }
  return true;
}

[[nodiscard]] bool
runCase(viennaps::vulkan::runtime::ComputeSession &session,
        const viennaps::vulkan::runtime::SpirvProgram &program,
        const LevelSetUpdateInput &input, const std::string &label) {
  const auto expected = cpuReference(input);
  std::vector<float> output;
  std::string error;
  if (!viennaps::vulkan::levelset::updateLevelSetFp32(session, program, input,
                                                      output, error)) {
    std::cerr << label << " failed: " << error << '\n';
    return false;
  }
  if (!exactEqual(output, expected)) {
    std::cerr << label << " CPU differential failed\n";
    return false;
  }
  std::cout << "[LevelSetUpdate] " << label << " exact PASS\n";
  return true;
}

[[nodiscard]] bool
runUniformCountCase(viennaps::vulkan::runtime::ComputeSession &session,
                    const viennaps::vulkan::runtime::SpirvProgram &program,
                    const std::size_t count) {
  std::vector<float> values(count);
  std::vector<std::uint32_t> offsets(count + 1U);
  std::vector<float> gradients(count, 0.5F);
  std::vector<float> dissipations(count, 0.125F);
  std::vector<float> stops(count, std::numeric_limits<float>::max());
  for (std::size_t i = 0; i < count; ++i) {
    values[i] = static_cast<float>(static_cast<int>(i % 17U) - 8) * 0.03125F;
    offsets[i] = static_cast<std::uint32_t>(i);
  }
  offsets[count] = static_cast<std::uint32_t>(count);
  return runCase(
      session, program,
      {values, offsets, gradients, dissipations, stops, 0.25F, 1.0F, true},
      "count=" + std::to_string(count));
}

[[nodiscard]] bool
runMalformedCases(viennaps::vulkan::runtime::ComputeSession &session,
                  const viennaps::vulkan::runtime::SpirvProgram &program) {
  const std::array<float, 1> values{0.0F};
  const std::array<float, 1> rates{1.0F};
  const std::array<float, 1> sentinel{std::numeric_limits<float>::max()};
  std::vector<float> output{42.0F};
  std::string error;

  const std::array<std::uint32_t, 1> shortOffsets{0U};
  if (viennaps::vulkan::levelset::updateLevelSetFp32(
          session, program,
          {values, shortOffsets, rates, rates, sentinel, 0.1F, 1.0F, false},
          output, error) ||
      output != std::vector<float>{42.0F}) {
    return false;
  }
  const std::array<std::uint32_t, 2> emptyRange{0U, 0U};
  if (viennaps::vulkan::levelset::updateLevelSetFp32(
          session, program, {values, emptyRange, {}, {}, {}, 0.1F, 1.0F, false},
          output, error) ||
      output != std::vector<float>{42.0F}) {
    return false;
  }
  const std::array<std::uint32_t, 2> offsets{0U, 1U};
  const std::array<float, 1> missingSentinel{0.5F};
  if (viennaps::vulkan::levelset::updateLevelSetFp32(
          session, program,
          {values, offsets, rates, rates, missingSentinel, 0.1F, 1.0F, false},
          output, error) ||
      output != std::vector<float>{42.0F}) {
    return false;
  }
  std::cout << "[LevelSetUpdate] malformed CSR transactional PASS\n";
  return true;
}

} // namespace

int main() {
  using namespace viennaps::vulkan::runtime;
  std::string error;
  ComputeSession session{};
  if (!session.initialize(error)) {
    std::cerr << error << '\n';
    return EXIT_FAILURE;
  }
  SpirvProgram program{};
  if (!readSpirv(VIENNAPS_LEVELSET_UPDATE_SPV_PATH, program, error)) {
    std::cerr << error << '\n';
    return EXIT_FAILURE;
  }

  bool pass = true;
  for (const std::size_t count : {0U, 1U, 257U, 65'535U}) {
    pass = runUniformCountCase(session, program, count) && pass;
  }

  const float sentinel = std::numeric_limits<float>::max();
  const std::array<float, 4> values{0.25F, 2.0F, -0.2F, 0.0F};
  const std::array<std::uint32_t, 5> offsets{0U, 1U, 2U, 3U, 5U};
  const std::array<float, 5> gradients{0.5F, 0.5F, 0.2F, 1.0F, 2.0F};
  const std::array<float, 5> dissipations{0.1F, 0.0F, 0.5F, 0.0F, 0.0F};
  const std::array<float, 5> stops{sentinel, sentinel, sentinel, -0.25F,
                                   sentinel};
  pass = runCase(session, program,
                 {values, offsets, gradients, dissipations, stops, 1.0F, 1.0F,
                  false},
                 "cutoff-clamp-multi-material") &&
         pass;

  const std::array<float, 1> negativeGradient{-0.2F};
  const std::array<float, 1> negativeDissipation{-0.5F};
  const std::array<float, 1> singleValue{0.0F};
  const std::array<std::uint32_t, 2> singleOffsets{0U, 1U};
  const std::array<float, 1> singleStop{sentinel};
  pass = runCase(session, program,
                 {singleValue, singleOffsets, negativeGradient,
                  negativeDissipation, singleStop, 1.0F, 1.0F, true},
                 "negative-gradient-dissipation-clamp") &&
         pass;
  pass = runMalformedCases(session, program) && pass;

  std::cout << "[LevelSetUpdate] " << (pass ? "PASS" : "FAIL") << '\n';
  return pass ? EXIT_SUCCESS : EXIT_FAILURE;
}
