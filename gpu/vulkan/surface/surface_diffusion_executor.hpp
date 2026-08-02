// Copyright 2026 ViennaPS
// SPDX-License-Identifier: MIT

#pragma once

#include <memory>
#include <string>
#include <string_view>

#include <process/psSurfaceDiffusionExecutor.hpp>

#include "../runtime/compute_session.hpp"

namespace viennaps::vulkan::surface {

/// Caller-owned, FP32 Vulkan bridge for the surface-diffusion executor seam.
/// Installation on Process is explicit through
/// Process::setSurfaceDiffusionStatusExecutor().
class VulkanSurfaceDiffusionExecutor {
public:
  using Executor = viennaps::SurfaceDiffusionStatusExecutor<float>;

  VulkanSurfaceDiffusionExecutor();
  ~VulkanSurfaceDiffusionExecutor();

  VulkanSurfaceDiffusionExecutor(const VulkanSurfaceDiffusionExecutor &) =
      delete;
  VulkanSurfaceDiffusionExecutor &operator=(
      const VulkanSurfaceDiffusionExecutor &) = delete;
  VulkanSurfaceDiffusionExecutor(VulkanSurfaceDiffusionExecutor &&) = delete;
  VulkanSurfaceDiffusionExecutor &operator=(VulkanSurfaceDiffusionExecutor &&) =
      delete;

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

  /// The callback owns a shared state snapshot and is safe to invoke after
  /// this bridge object has been destroyed.
  [[nodiscard]] Executor makeExecutor() const;

private:
  struct State;
  std::shared_ptr<State> state_;
};

} // namespace viennaps::vulkan::surface
