// Copyright 2026 ViennaPS
// SPDX-License-Identifier: MIT

#include "coverage_delta_executor.hpp"

#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <limits>
#include <memory>
#include <string>
#include <vector>

namespace {

using Bridge = viennaps::vulkan::surface::VulkanCoverageDeltaExecutor;
using Work = viennaps::CoverageDeltaWork<float>;

[[nodiscard]] bool check(const bool condition, const char *message) {
  if (!condition)
    std::cerr << "[coverage-delta-executor] " << message << '\n';
  return condition;
}

[[nodiscard]] bool exact(const float lhs, const float rhs) {
  return std::bit_cast<std::uint32_t>(lhs) ==
         std::bit_cast<std::uint32_t>(rhs);
}

[[nodiscard]] std::vector<float>
oracle(const std::vector<float> &updated, const std::vector<float> &previous,
       const std::size_t channels, const std::size_t points) {
  std::vector<float> result(channels);
  for (std::size_t channel = 0U; channel < channels; ++channel) {
    volatile float sum = 0.0F;
    for (std::size_t point = 0U; point < points; ++point) {
      volatile float difference =
          updated[channel * points + point] -
          previous[channel * points + point];
      volatile float square = difference * difference;
      sum = sum + square;
    }
    volatile float mean = sum / static_cast<float>(points);
    result[channel] = mean;
  }
  return result;
}

[[nodiscard]] bool runCase(const Bridge::Executor &executor,
                           const std::size_t channels,
                           const std::size_t points) {
  const auto total = channels * points;
  std::vector<float> updated(total);
  std::vector<float> previous(total);
  for (std::size_t channel = 0U; channel < channels; ++channel) {
    for (std::size_t point = 0U; point < points; ++point) {
      const auto index = channel * points + point;
      updated[index] =
          1.0F + static_cast<float>((channel + point) % 7U) * 0.03125F;
      previous[index] =
          0.5F + static_cast<float>((channel * 3U + point) % 5U) * 0.015625F;
    }
  }
  std::vector<std::size_t> offsets(channels + 1U);
  for (std::size_t channel = 0U; channel <= channels; ++channel)
    offsets[channel] = channel * points;
  const auto expected = oracle(updated, previous, channels, points);
  constexpr float sentinel = -12345.25F;
  std::vector<float> output(channels + 3U, sentinel);
  Work work{updated, previous, offsets, output, channels, 99U, false};
  std::string error;
  if (!executor(work, error)) {
    std::cerr << error << '\n';
    return false;
  }
  if (!check(work.complete && work.writtenCount == channels,
             "successful callback did not complete the full output"))
    return false;
  for (std::size_t channel = 0U; channel < channels; ++channel)
    if (!check(exact(output[channel], expected[channel]),
               "CPU/Vulkan coverage mismatch"))
      return false;
  for (std::size_t index = channels; index < output.size(); ++index)
    if (!check(exact(output[index], sentinel),
               "coverage output tail was overwritten"))
      return false;
  return true;
}

template <class Mutator>
[[nodiscard]] bool runRejected(const Bridge::Executor &executor,
                               const std::vector<float> &updated,
                               const std::vector<float> &previous,
                               const std::vector<std::size_t> &offsets,
                               const std::size_t channels, Mutator mutate,
                               const char *message) {
  constexpr float sentinel = 777.5F;
  std::vector<float> output(channels + 2U, sentinel);
  Work work{updated, previous, offsets, output, channels, 41U, true};
  mutate(work);
  std::string error;
  if (!check(!executor(work, error), message))
    return false;
  if (!check(work.writtenCount == 41U && work.complete,
             "rejected callback changed completion metadata"))
    return false;
  for (const float value : output)
    if (!check(exact(value, sentinel), "rejected callback modified output"))
      return false;
  return true;
}

} // namespace

int main() {
  std::string error;
  auto bridge = std::make_unique<Bridge>();
  if (!check(bridge->initialize(VIENNAPS_VULKAN_COVERAGE_DELTA_METRIC_SPV_PATH,
                                error),
             "failed to initialize Vulkan coverage bridge")) {
    std::cerr << error << '\n';
    return 1;
  }
  auto executor = bridge->makeExecutor();
  bridge.reset();

  for (const auto points : std::array<std::size_t, 3U>{1U, 16U, 257U})
    if (!runCase(executor, 3U, points))
      return 1;

  const std::size_t channels = 3U;
  const std::size_t points = 16U;
  const std::vector<float> updated(channels * points, 1.0F);
  const std::vector<float> previous(channels * points, 0.5F);
  const std::vector<std::size_t> offsets{0U, points, points * 2U,
                                         points * 3U};
  if (!runRejected(
          executor, updated, previous,
          std::vector<std::size_t>{0U, points, points + points - 1U,
                                   points * 3U},
          channels, [](Work &) {}, "bad offsets were accepted") ||
      !runRejected(executor, updated, previous, offsets, channels,
                   [](Work &work) { work.updated = work.updated.first(1U); },
                   "bad input length was accepted"))
    return 1;

  auto nan = updated;
  nan.front() = std::numeric_limits<float>::quiet_NaN();
  if (!runRejected(executor, nan, previous, offsets, channels,
                   [](Work &) {}, "NaN coverage was accepted"))
    return 1;
  auto subnormal = updated;
  subnormal.front() = std::numeric_limits<float>::denorm_min();
  if (!runRejected(executor, subnormal, previous, offsets, channels,
                   [](Work &) {}, "subnormal coverage was accepted"))
    return 1;

  bridge = std::make_unique<Bridge>();
  if (!bridge->initialize(VIENNAPS_VULKAN_COVERAGE_DELTA_METRIC_SPV_PATH,
                          error))
    return 1;
  auto resetExecutor = bridge->makeExecutor();
  bridge->reset();
  if (!runRejected(resetExecutor, updated, previous, offsets, channels,
                   [](Work &) {}, "reset callback unexpectedly succeeded"))
    return 1;

  std::cout << "[CoverageDeltaExecutor] CPU/Vulkan exact PASS for N=1,16,257\n";
  return 0;
}
