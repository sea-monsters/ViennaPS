// Copyright 2026 ViennaPS
// SPDX-License-Identifier: MIT
//
// Deployment-time Vulkan hardware identity collection. This is deliberately
// separate from worker execution so profile provisioning can bind to the
// actual device selected for a future compute session.

#pragma once

#include "compute_session.hpp"

#include <compute/capabilityProfileIO.hpp>
#include <compute/vulkanDeploymentBootstrap.hpp>

#include <cstdint>
#include <limits>
#include <string>
#include <vector>

namespace viennaps::vulkan::runtime {

struct DeploymentHardwareSnapshot {
  compute::HardwareFingerprint fingerprint{};
  std::uint32_t physicalDeviceIndex = 0U;
};

struct VulkanDeploymentHardwareCollectorOptions {
  std::uint32_t manualDeviceIndex = std::numeric_limits<std::uint32_t>::max();
  std::string manualDeviceName;
  std::string manualDeviceUuid;
};

// Reads the identity of an already initialized session. The index is the
// Vulkan physical-device enumeration index and can be forwarded to a strict
// child probe to guarantee it validates the same device.
[[nodiscard]] bool
collectDeploymentHardwareSnapshot(const ComputeSession &session,
                                  DeploymentHardwareSnapshot &snapshot,
                                  std::string &error);

// Creates only a short-lived deployment/configuration session, selects a
// device with the same options that later execution will use, then collects
// its identity. No worker callback or simulation state is touched.
[[nodiscard]] bool
collectDeploymentHardwareSnapshot(const ComputeSessionOptions &options,
                                  DeploymentHardwareSnapshot &snapshot,
                                  std::string &error);

// Produces a durable callback that owns its manual-device selector strings.
// It is passed to the pure deployment bootstrap and initializes Vulkan only
// when automatic/manual-Vulkan provisioning actually needs a fingerprint.
[[nodiscard]] compute::VulkanDeploymentHardwareCollector
makeVulkanDeploymentHardwareCollector(
    VulkanDeploymentHardwareCollectorOptions options = {});

// Runtime-facing deployment facade. If callers do not inject a collector, it
// uses the selected-device collector above; the pure bootstrap preserves an
// explicit manual CPU bypass before that callback executes.
[[nodiscard]] compute::VulkanDeploymentBootstrapResult
bootstrapVulkanDeploymentForSelectedDevice(
    const std::vector<compute::StageWorkload> &workloads,
    const compute::ManualSelectionConfig &selection,
    compute::VulkanDeploymentBootstrapOptions options,
    VulkanDeploymentHardwareCollectorOptions hardwareOptions = {});

} // namespace viennaps::vulkan::runtime
