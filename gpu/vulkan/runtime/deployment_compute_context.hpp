// Copyright 2026 ViennaPS
// SPDX-License-Identifier: MIT

#pragma once

#include "compute_session.hpp"

#include <compute/deploymentProfile.hpp>

#include <span>
#include <string>
#include <string_view>

namespace viennaps::vulkan::runtime {

// Process-lifetime cache for deployment-profile backend selection. prepare()
// reads and validates the profile once; reset() is required before changing
// policy or applying a different manual override. This object is intentionally
// not thread-safe and should be configured before simulation workers start.
class DeploymentComputeContext {
public:
  [[nodiscard]] bool
  prepare(const compute::HardwareFingerprint &currentHardware,
          std::span<const compute::StageWorkload> workloads,
          const compute::ManualSelectionConfig &selectionConfig,
          std::string_view configuredProfilePath, std::string &error,
          const ComputeSessionOptions &manualDevice = {});

  // Consume a decision resolved by the deployment/configuration thread. This
  // path is cache-only: it does not read the profile path or invoke a probe.
  [[nodiscard]] bool
  prepare(const compute::DeploymentProfileDecision &resolvedDecision,
          const compute::HardwareFingerprint &currentHardware,
          std::span<const compute::StageWorkload> workloads,
          const compute::ManualSelectionConfig &selectionConfig,
          std::string &error, const ComputeSessionOptions &manualDevice = {});

  void reset();
  [[nodiscard]] bool isPrepared() const;
  [[nodiscard]] bool hasVulkanSession() const;
  [[nodiscard]] compute::ComputeBackend backendFor(compute::Stage stage) const;

  [[nodiscard]] const compute::DeploymentProfileDecision &decision() const;
  [[nodiscard]] ComputeSession *session();
  [[nodiscard]] const ComputeSession *session() const;

private:
  compute::DeploymentProfileDecision decision_{};
  ComputeSession session_{};
  bool prepared_ = false;
};

} // namespace viennaps::vulkan::runtime
