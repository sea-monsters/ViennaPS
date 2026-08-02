// Copyright 2026 ViennaPS
// SPDX-License-Identifier: MIT
//
// Explicit caller-owned Vulkan bridge for the coverage convergence seam.

#pragma once

#include <memory>
#include <string>
#include <string_view>

#include <process/psCoverageDeltaExecutor.hpp>

namespace viennaps::vulkan::surface {

/// Owns an FP32 Vulkan coverage metric and exposes it through the generic
/// coverage-delta executor seam. Installation on CoverageManager remains an
/// explicit caller decision; this type never changes backend policy.
class VulkanCoverageDeltaExecutor {
public:
  using Executor = viennaps::CoverageDeltaExecutor<float>;

  VulkanCoverageDeltaExecutor();
  ~VulkanCoverageDeltaExecutor();

  VulkanCoverageDeltaExecutor(const VulkanCoverageDeltaExecutor &) = delete;
  VulkanCoverageDeltaExecutor &operator=(
      const VulkanCoverageDeltaExecutor &) = delete;
  VulkanCoverageDeltaExecutor(VulkanCoverageDeltaExecutor &&) = delete;
  VulkanCoverageDeltaExecutor &operator=(VulkanCoverageDeltaExecutor &&) =
      delete;

  [[nodiscard]] bool initialize(std::string_view spirvPath,
                                std::string &error);
  void reset();
  [[nodiscard]] bool isInitialized() const;

  /// Returns a lifetime-safe callback. The callback keeps bridge state alive
  /// after the bridge object itself is destroyed.
  [[nodiscard]] Executor makeExecutor() const;

private:
  struct State;
  std::shared_ptr<State> state_;
};

} // namespace viennaps::vulkan::surface
