// Copyright 2026 ViennaPS
// SPDX-License-Identifier: MIT
//
// P5-TEOS-VELOCITY-SUBSTAGE.  This fixture admits only the single-precursor
// reaction-power arithmetic.  The CPU flux, sticking, coverage and Process
// semantics remain the authority; Vulkan output is transactional and oracle
// checked before publication.

#include "teos_velocity_executor.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <limits>
#include <span>
#include <string>
#include <vector>

#ifndef VIENNAPS_VULKAN_TEOS_VELOCITY_SPV_PATH
#error "VIENNAPS_VULKAN_TEOS_VELOCITY_SPV_PATH must be defined"
#endif

namespace {

using T = float;
using Work = viennaps::TEOSVelocityWork<T>;

std::uint32_t bits(const T value) { return std::bit_cast<std::uint32_t>(value); }

std::uint32_t orderedBits(const T value) {
  const auto raw = bits(value);
  return (raw & 0x80000000U) != 0U ? ~raw : raw ^ 0x80000000U;
}

std::uint32_t ulpDistance(const T lhs, const T rhs) {
  const auto a = orderedBits(lhs);
  const auto b = orderedBits(rhs);
  return a >= b ? a - b : b - a;
}

bool sentinel(const std::vector<T> &values, const T expected) {
  for (const auto value : values)
    if (bits(value) != bits(expected))
      return false;
  return true;
}

} // namespace

int main() {
  try {
    const std::vector<T> flux{0.0F, 0.125F, 0.5F, 1.0F, 2.0F, 4.0F};
    constexpr T depositionRate = 0.75F;
    constexpr T reactionOrder = 0.5F;
    std::vector<T> oracle(flux.size());
    for (std::size_t index = 0U; index < flux.size(); ++index)
      oracle[index] = depositionRate * std::pow(flux[index], reactionOrder);
    std::vector<T> output(flux.size(), 19.25F);
    Work work{std::span<const T>(flux), std::span<const T>(oracle),
              std::span<T>(output), {depositionRate, reactionOrder}};

    viennaps::vulkan::levelset::VulkanTEOSVelocityExecutor executor;
    std::string error;
    if (!executor.initialize(VIENNAPS_VULKAN_TEOS_VELOCITY_SPV_PATH, error)) {
      std::cerr << "TEOS velocity initialization failed: " << error << '\n';
      return 1;
    }
    const auto invoke = executor.makeExecutor();
    if (!invoke(work, error) || !work.complete ||
        work.writtenCount != output.size()) {
      std::cerr << "TEOS velocity dispatch failed: " << error << '\n';
      return 1;
    }
    std::uint32_t maxUlp = 0U;
    for (std::size_t index = 0U; index < output.size(); ++index)
      maxUlp = std::max(maxUlp, ulpDistance(output[index], oracle[index]));
    if (maxUlp > 32U || bits(output[0]) != bits(0.0F)) {
      std::cerr << "TEOS velocity CPU oracle mismatch maxUlp=" << maxUlp
                << '\n';
      return 1;
    }

    auto negativeFlux = flux;
    negativeFlux[1] = -1.0F;
    std::vector<T> malformedOutput(flux.size(), -9.5F);
    Work malformed{std::span<const T>(negativeFlux), std::span<const T>(oracle),
                   std::span<T>(malformedOutput), {depositionRate, reactionOrder}};
    if (invoke(malformed, error) || !sentinel(malformedOutput, -9.5F)) {
      std::cerr << "TEOS malformed-input sentinel failed\n";
      return 1;
    }

    auto nanFlux = flux;
    nanFlux[2] = std::numeric_limits<T>::quiet_NaN();
    std::vector<T> nanOutput(flux.size(), 6.75F);
    Work nanWork{std::span<const T>(nanFlux), std::span<const T>(oracle),
                 std::span<T>(nanOutput), {depositionRate, reactionOrder}};
    if (invoke(nanWork, error) || !sentinel(nanOutput, 6.75F)) {
      std::cerr << "TEOS NaN-input sentinel failed\n";
      return 1;
    }

    executor.reset();
    std::vector<T> resetOutput(flux.size(), 3.5F);
    Work resetWork{std::span<const T>(flux), std::span<const T>(oracle),
                   std::span<T>(resetOutput), {depositionRate, reactionOrder}};
    if (invoke(resetWork, error) || !sentinel(resetOutput, 3.5F)) {
      std::cerr << "TEOS reset sentinel failed\n";
      return 1;
    }

    // A caller-owned session reset must invalidate the retained callback for
    // the TEOS seam as well.  This is distinct from resetting the executor's
    // own state above: the generation captured during initialize() is stale
    // even though the callback object is still alive.
    viennaps::vulkan::runtime::ComputeSession externalSession;
    if (!externalSession.initialize(error)) {
      std::cerr << "TEOS external session initialization failed: " << error
                << '\n';
      return 1;
    }
    viennaps::vulkan::levelset::VulkanTEOSVelocityExecutor externalExecutor;
    if (!externalExecutor.initialize(
            externalSession, VIENNAPS_VULKAN_TEOS_VELOCITY_SPV_PATH, error)) {
      std::cerr << "TEOS external executor initialization failed: " << error
                << '\n';
      return 1;
    }
    const auto externalInvoke = externalExecutor.makeExecutor();
    std::vector<T> externalOutput(flux.size(), 4.25F);
    Work externalWork{std::span<const T>(flux), std::span<const T>(oracle),
                      std::span<T>(externalOutput),
                      {depositionRate, reactionOrder}};
    if (!externalInvoke(externalWork, error) || !externalWork.complete ||
        externalWork.writtenCount != externalOutput.size()) {
      std::cerr << "TEOS external-session dispatch failed: " << error << '\n';
      return 1;
    }
    for (std::size_t index = 0U; index < externalOutput.size(); ++index) {
      if (ulpDistance(externalOutput[index], oracle[index]) > 32U) {
        std::cerr << "TEOS external-session oracle mismatch\n";
        return 1;
      }
    }
    externalSession.reset();
    std::fill(externalOutput.begin(), externalOutput.end(), 4.25F);
    Work staleWork{std::span<const T>(flux), std::span<const T>(oracle),
                   std::span<T>(externalOutput),
                   {depositionRate, reactionOrder}};
    if (externalInvoke(staleWork, error) || !sentinel(externalOutput, 4.25F)) {
      std::cerr << "TEOS stale-session sentinel failed\n";
      return 1;
    }
    externalExecutor.reset();

    std::cout << "TEOS velocity Vulkan dispatch PASS (maxUlp=" << maxUlp
              << ", malformed/NaN/reset/stale-session sentinels PASS)\n";
    return 0;
  } catch (const std::exception &exception) {
    std::cerr << "TEOS velocity smoke FAIL: " << exception.what() << '\n';
    return 1;
  }
}
