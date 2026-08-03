// Copyright 2026 ViennaPS
// SPDX-License-Identifier: MIT
//
// Explicit caller-owned Vulkan bridge for the neutral-transport velocity seam.

#pragma once

#include <memory>
#include <string>
#include <string_view>

#include <models/psNeutralTransportVelocityExecutor.hpp>

#include "../runtime/compute_session.hpp"

namespace viennaps::vulkan::surface {

/// Owns an FP32 Vulkan surface primitive and exposes it through the generic
/// neutral-transport executor seam. The caller must explicitly install the
/// returned executor on a CPU neutral-transport surface model.
class VulkanNeutralTransportVelocityExecutor {
public:
  using Executor = viennaps::NeutralTransportVelocityExecutor<float>;

  VulkanNeutralTransportVelocityExecutor();
  ~VulkanNeutralTransportVelocityExecutor();

  VulkanNeutralTransportVelocityExecutor(
      const VulkanNeutralTransportVelocityExecutor &) = delete;
  VulkanNeutralTransportVelocityExecutor &operator=(
      const VulkanNeutralTransportVelocityExecutor &) = delete;
  VulkanNeutralTransportVelocityExecutor(
      VulkanNeutralTransportVelocityExecutor &&) = delete;
  VulkanNeutralTransportVelocityExecutor &operator=(
      VulkanNeutralTransportVelocityExecutor &&) = delete;

  [[nodiscard]] bool initialize(std::string_view spirvPath,
                                std::string &error);
  /// Initializes against a caller-owned session. The session must remain
  /// valid until every callback made by makeExecutor() is released; this
  /// bridge never resets or owns the borrowed session.
  [[nodiscard]] bool initialize(runtime::ComputeSession &session,
                                std::string_view spirvPath,
                                std::string &error);
  void reset();
  [[nodiscard]] bool isInitialized() const;

  /// Returns a lifetime-safe callback. The callback keeps the bridge state
  /// alive after the bridge object itself is destroyed.
  [[nodiscard]] Executor makeExecutor() const;

private:
  struct State;
  std::shared_ptr<State> state_;
};

} // namespace viennaps::vulkan::surface
