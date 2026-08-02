// Copyright 2026 ViennaPS
// SPDX-License-Identifier: MIT

#include "surface_diffusion_executor.hpp"

#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <limits>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace {

using Bridge = viennaps::vulkan::surface::VulkanSurfaceDiffusionExecutor;
using Work = viennaps::SurfaceDiffusionWork<float>;
using Status = viennaps::SurfaceDiffusionExecutionStatus;

[[nodiscard]] bool check(const bool condition, const char *message) {
  if (!condition)
    std::cerr << "[surface-diffusion-executor] " << message << '\n';
  return condition;
}

[[nodiscard]] bool exact(const float lhs, const float rhs) {
  return std::bit_cast<std::uint32_t>(lhs) ==
         std::bit_cast<std::uint32_t>(rhs);
}

[[nodiscard]] std::vector<float>
oracle(const std::vector<std::uint32_t> &offsets,
       const std::vector<std::uint32_t> &columns,
       const std::vector<float> &weights, const std::vector<float> &field,
       const float step) {
  std::vector<float> result(field.size());
  for (std::size_t row = 0U; row < field.size(); ++row) {
    volatile float sum = 0.0F;
    for (std::size_t edge = offsets[row]; edge < offsets[row + 1U]; ++edge) {
      volatile float product = weights[edge] * field[columns[edge]];
      sum = sum + product;
    }
    volatile float scaled = step * sum;
    volatile float value = field[row] + scaled;
    result[row] = value;
  }
  return result;
}

[[nodiscard]] bool runCase(const Bridge::Executor &executor,
                           const std::size_t count) {
  std::vector<std::uint32_t> offsets(count + 1U);
  std::vector<std::uint32_t> columns;
  std::vector<float> weights;
  std::vector<float> field(count);
  for (std::size_t row = 0U; row < count; ++row) {
    offsets[row] = static_cast<std::uint32_t>(columns.size());
    field[row] = 1.0F + static_cast<float>(row % 11U) * 0.03125F;
    columns.push_back(static_cast<std::uint32_t>(row));
    weights.push_back(0.125F);
    if (row > 0U) {
      columns.push_back(static_cast<std::uint32_t>(row - 1U));
      weights.push_back(-0.0625F);
    }
  }
  offsets[count] = static_cast<std::uint32_t>(columns.size());
  constexpr float step = 0.5F;
  const auto expected = oracle(offsets, columns, weights, field, step);
  constexpr float sentinel = -991.25F;
  std::vector<float> output(count, sentinel);
  Work work{offsets, columns, weights, field, output, step, 19U, false};
  std::string error;
  if (!check(executor(work, error) == Status::SUCCESS, "valid case failed")) {
    std::cerr << error << '\n';
    return false;
  }
  if (!check(work.complete && work.writtenCount == count,
             "valid case did not complete"))
    return false;
  for (std::size_t i = 0U; i < count; ++i)
    if (!check(exact(output[i], expected[i]), "CPU/Vulkan mismatch"))
      return false;
  return true;
}

template <class Mutator>
[[nodiscard]] bool runRejected(const Bridge::Executor &executor,
                               std::vector<std::uint32_t> offsets,
                               std::vector<std::uint32_t> columns,
                               std::vector<float> weights,
                               std::vector<float> field, Mutator mutate,
                               const char *message) {
  constexpr float sentinel = 777.5F;
  std::vector<float> output(field.size(), sentinel);
  Work work{offsets, columns, weights, field, output, 0.5F, 41U, true};
  mutate(work);
  std::string error;
  if (!check(executor(work, error) == Status::FAILURE, message))
    return false;
  if (!check(work.writtenCount == 41U && work.complete,
             "rejected call changed completion metadata"))
    return false;
  for (const float value : output)
    if (!check(exact(value, sentinel), "rejected call modified output"))
      return false;
  return true;
}

} // namespace

int main() {
  std::string error;
  auto bridge = std::make_unique<Bridge>();
  if (!check(bridge->initialize(VIENNAPS_VULKAN_GRAPH_DIFFUSION_SPV_PATH,
                                error),
             "failed to initialize Vulkan bridge")) {
    std::cerr << error << '\n';
    return 1;
  }
  auto executor = bridge->makeExecutor();
  bridge.reset();

  for (const auto count : std::array<std::size_t, 3U>{0U, 1U, 257U})
    if (!runCase(executor, count))
      return 1;

  const std::vector<std::uint32_t> offsets{0U, 2U, 3U};
  const std::vector<std::uint32_t> columns{0U, 1U, 1U};
  const std::vector<float> weights{0.25F, -0.125F, 0.5F};
  const std::vector<float> field{1.0F, 1.5F};
  if (!runRejected(executor, {0U, 1U, 2U}, columns, weights, field,
                   [](Work &) {}, "bad CSR offsets accepted") ||
      !runRejected(executor, offsets, columns, weights, field,
                   [](Work &work) {
                     work.output = std::span<float>(
                         const_cast<float *>(work.field.data()),
                         work.field.size());
                   },
                   "aliased field/output accepted"))
    return 1;

  auto nan = field;
  nan.front() = std::numeric_limits<float>::quiet_NaN();
  if (!runRejected(executor, offsets, columns, weights, nan,
                   [](Work &) {}, "NaN field accepted"))
    return 1;
  auto subnormal = weights;
  subnormal.front() = std::numeric_limits<float>::denorm_min();
  if (!runRejected(executor, offsets, columns, subnormal, field,
                   [](Work &) {}, "subnormal weight accepted"))
    return 1;

  auto resetBridge = std::make_unique<Bridge>();
  if (!check(resetBridge->initialize(VIENNAPS_VULKAN_GRAPH_DIFFUSION_SPV_PATH,
                                     error),
             "failed to initialize reset bridge"))
    return 1;
  auto resetExecutor = resetBridge->makeExecutor();
  resetBridge->reset();
  if (!runRejected(resetExecutor, offsets, columns, weights, field,
                   [](Work &) {}, "reset executor unexpectedly succeeded"))
    return 1;

  std::cout << "[SurfaceDiffusionExecutor] CPU/Vulkan exact PASS for N=0,1,257\n";
  return 0;
}
