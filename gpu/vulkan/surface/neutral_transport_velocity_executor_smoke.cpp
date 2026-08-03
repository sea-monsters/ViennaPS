// Copyright 2026 ViennaPS
// SPDX-License-Identifier: MIT

#include "neutral_transport_velocity_executor.hpp"

#include "../runtime/compute_session.hpp"

#include <models/psNeutralTransportVelocityExecutor.hpp>

#include <algorithm>
#include <bit>
#include <cstdint>
#include <iostream>
#include <limits>
#include <string>
#include <vector>

namespace {

using Bridge = viennaps::vulkan::surface::VulkanNeutralTransportVelocityExecutor;
using Work = viennaps::NeutralTransportVelocityWork<float>;
using Parameters = viennaps::NeutralTransportVelocityParameters<float>;

[[nodiscard]] bool rawEqual(const float lhs, const float rhs) {
  return std::bit_cast<std::uint32_t>(lhs) ==
         std::bit_cast<std::uint32_t>(rhs);
}

[[nodiscard]] float legacyVelocity(const float coverage, const float material,
                                    const Parameters &parameters) {
  if (material != static_cast<float>(parameters.etchFrontMaterialId))
    return 0.0F;
  const auto etchVelocity =
      parameters.siliconDensity > 0.0F
          ? parameters.kEtch * parameters.surfaceSiteDensity * coverage /
                parameters.siliconDensity
          : 0.0F;
  return -etchVelocity * parameters.timeToSecond /
         parameters.lengthToMeter;
}

[[nodiscard]] bool unchanged(const std::vector<float> &values,
                             const float sentinel) {
  return std::all_of(values.begin(), values.end(),
                     [sentinel](const float value) {
                       return rawEqual(value, sentinel);
                     });
}

[[nodiscard]] bool check(const bool condition, const char *message) {
  if (!condition)
    std::cerr << "[neutral-transport-executor] " << message << '\n';
  return condition;
}

int runSmoke() {
  Parameters parameters{2.0F, 3.0F, 6.0F, 1.0F, 1.0F, 10};
  const std::vector<float> coverage{0.25F, 0.5F, 0.75F, 0.0F};
  const std::vector<float> materials{10.0F, 1.0F, 10.0F, 10.0F};

  Bridge bridge;
  std::string error;
  viennaps::vulkan::runtime::ComputeSession borrowedSession;
  if (!borrowedSession.initialize(error))
    return 1;
  Bridge borrowedBridge;
  if (!borrowedBridge.initialize(
          borrowedSession, VIENNAPS_VULKAN_NEUTRAL_TRANSPORT_SPV_PATH,
          error))
    return 1;
  auto borrowedExecutor = borrowedBridge.makeExecutor();
  if (!bridge.initialize(VIENNAPS_VULKAN_NEUTRAL_TRANSPORT_SPV_PATH, error)) {
    std::cerr << "bridge initialize failed: " << error << '\n';
    return 1;
  }
  auto executor = bridge.makeExecutor();

  std::vector<float> output(coverage.size(), -91.0F);
  Work work{coverage, materials, output, parameters};
  if (!executor(work, error) || !work.complete ||
      work.writtenCount != output.size())
    return 1;
  for (std::size_t index = 0U; index < output.size(); ++index) {
    if (!check(rawEqual(output[index],
                        legacyVelocity(coverage[index], materials[index],
                                       parameters)),
               "GPU output differs from the legacy CPU oracle"))
      return 1;
  }

  std::fill(output.begin(), output.end(), -91.0F);
  Work borrowedWork{coverage, materials, output, parameters};
  if (!borrowedExecutor(borrowedWork, error) || !borrowedWork.complete ||
      borrowedWork.writtenCount != output.size())
    return 1;
  for (std::size_t index = 0U; index < output.size(); ++index) {
    if (!check(rawEqual(output[index],
                        legacyVelocity(coverage[index], materials[index],
                                       parameters)),
               "borrowed callback output differs from the legacy CPU oracle"))
      return 1;
  }

  viennaps::vulkan::runtime::ComputeSession invalidSession;
  Bridge invalidBorrowed;
  error.clear();
  if (invalidBorrowed.initialize(
          invalidSession, VIENNAPS_VULKAN_NEUTRAL_TRANSPORT_SPV_PATH,
          error) || invalidBorrowed.isInitialized() || error.empty())
    return 1;

  Bridge missingSpirv;
  error.clear();
  if (missingSpirv.initialize(borrowedSession, "", error) ||
      missingSpirv.isInitialized() || error.empty() ||
      !borrowedSession.isValid())
    return 1;

  borrowedBridge.reset();
  if (!check(!borrowedBridge.isInitialized() && borrowedSession.isValid(),
             "borrowed reset invalidated caller session"))
    return 1;
  std::fill(output.begin(), output.end(), -91.0F);
  Work afterBorrowedReset{coverage, materials, output, parameters};
  if (borrowedExecutor(afterBorrowedReset, error) ||
      afterBorrowedReset.complete || !unchanged(output, -91.0F))
    return 1;

  Bridge::Executor borrowedLifetimeExecutor;
  {
    Bridge transient;
    if (!transient.initialize(
            borrowedSession, VIENNAPS_VULKAN_NEUTRAL_TRANSPORT_SPV_PATH,
            error))
      return 1;
    borrowedLifetimeExecutor = transient.makeExecutor();
  }
  std::fill(output.begin(), output.end(), -91.0F);
  Work afterBorrowedLifetime{coverage, materials, output, parameters};
  if (!borrowedLifetimeExecutor(afterBorrowedLifetime, error) ||
      !afterBorrowedLifetime.complete ||
      afterBorrowedLifetime.writtenCount != output.size())
    return 1;
  if (!check(borrowedSession.isValid(),
             "borrowed bridge destruction invalidated caller session"))
    return 1;

  std::vector<float> growthCoverage(257U, 0.5F);
  std::vector<float> growthMaterials(257U, 10.0F);
  std::vector<float> growthOutput(257U, -17.0F);
  Work growth{growthCoverage, growthMaterials, growthOutput, parameters};
  if (!executor(growth, error) || !growth.complete ||
      growth.writtenCount != growthOutput.size())
    return 1;
  for (std::size_t index = 0U; index < growthOutput.size(); ++index) {
    if (!check(rawEqual(growthOutput[index],
                        legacyVelocity(growthCoverage[index],
                                       growthMaterials[index], parameters)),
               "reusable growth output differs from the legacy CPU oracle"))
      return 1;
  }

  Parameters zeroDensity = parameters;
  zeroDensity.siliconDensity = 0.0F;
  std::fill(output.begin(), output.end(), -91.0F);
  Work zeroWork{coverage, materials, output, zeroDensity};
  if (!executor(zeroWork, error) || !zeroWork.complete ||
      !rawEqual(output[0], -0.0F) || !rawEqual(output[1], 0.0F) ||
      !rawEqual(output[2], -0.0F) || !rawEqual(output[3], -0.0F))
    return 1;

  const std::vector<float> subnormalCoverage{
      std::numeric_limits<float>::denorm_min(), 0.5F, 0.5F, 0.5F};
  std::fill(output.begin(), output.end(), -91.0F);
  Work subnormal{subnormalCoverage, materials, output, parameters};
  if (executor(subnormal, error) || subnormal.complete || subnormal.writtenCount != 0U ||
      !unchanged(output, -91.0F))
    return 1;

  const std::vector<float> unknownMaterials{999.0F, 1.0F, 10.0F, 10.0F};
  std::fill(output.begin(), output.end(), -91.0F);
  Work unknown{coverage, unknownMaterials, output, parameters};
  if (executor(unknown, error) || unknown.complete || !unchanged(output, -91.0F))
    return 1;

  const std::vector<float> negativeMaterials{-1.0F, 1.0F, 10.0F, 10.0F};
  std::fill(output.begin(), output.end(), -91.0F);
  Work negative{coverage, negativeMaterials, output, parameters};
  if (executor(negative, error) || negative.complete ||
      !unchanged(output, -91.0F))
    return 1;

  Parameters nanParameters = parameters;
  nanParameters.kEtch = std::numeric_limits<float>::quiet_NaN();
  std::fill(output.begin(), output.end(), -91.0F);
  Work nan{coverage, materials, output, nanParameters};
  if (executor(nan, error) || nan.complete || !unchanged(output, -91.0F))
    return 1;

  bridge.reset();
  if (!check(!bridge.isInitialized(), "reset left bridge initialized"))
    return 1;
  std::fill(output.begin(), output.end(), -91.0F);
  Work afterReset{coverage, materials, output, parameters};
  if (executor(afterReset, error) || afterReset.complete ||
      !unchanged(output, -91.0F))
    return 1;

  Bridge::Executor lifetimeExecutor;
  {
    Bridge transient;
    if (!transient.initialize(VIENNAPS_VULKAN_NEUTRAL_TRANSPORT_SPV_PATH,
                              error))
      return 1;
    lifetimeExecutor = transient.makeExecutor();
  }
  std::fill(output.begin(), output.end(), -91.0F);
  Work afterLifetime{coverage, materials, output, parameters};
  if (!lifetimeExecutor(afterLifetime, error) || !afterLifetime.complete ||
      afterLifetime.writtenCount != output.size())
    return 1;
  for (std::size_t index = 0U; index < output.size(); ++index) {
    if (!check(rawEqual(output[index],
                        legacyVelocity(coverage[index], materials[index],
                                       parameters)),
               "lifetime callback output differs from the legacy CPU oracle"))
      return 1;
  }
  return 0;
}

} // namespace

int main() { return runSmoke(); }
