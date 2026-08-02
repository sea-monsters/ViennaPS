// Copyright 2026 ViennaPS
// SPDX-License-Identifier: MIT

#include "deployment_hardware_fingerprint.hpp"

#include <cstdlib>
#include <iostream>
#include <string>

namespace {

[[nodiscard]] bool
complete(const viennaps::compute::HardwareFingerprint &fingerprint) {
  return !fingerprint.deviceUuid.empty() && !fingerprint.driverUuid.empty() &&
         fingerprint.vendorId != 0U && fingerprint.deviceId != 0U &&
         !fingerprint.deviceName.empty() &&
         !fingerprint.driverVersion.empty() && !fingerprint.driverDate.empty();
}

} // namespace

int main() {
  using namespace viennaps::vulkan::runtime;

  std::string error;
  DeploymentHardwareSnapshot first{};
  if (!collectDeploymentHardwareSnapshot(ComputeSessionOptions{}, first,
                                         error) ||
      !complete(first.fingerprint)) {
    std::cerr << "[DeploymentHardware] " << error << '\n';
    return EXIT_FAILURE;
  }

  ComputeSessionOptions selectedOptions{};
  selectedOptions.manualDeviceUuid = first.fingerprint.deviceUuid;
  DeploymentHardwareSnapshot selected{};
  if (!collectDeploymentHardwareSnapshot(selectedOptions, selected, error) ||
      !complete(selected.fingerprint) ||
      selected.fingerprint.deviceUuid != first.fingerprint.deviceUuid ||
      selected.fingerprint.driverUuid != first.fingerprint.driverUuid ||
      selected.fingerprint.vendorId != first.fingerprint.vendorId ||
      selected.fingerprint.deviceId != first.fingerprint.deviceId ||
      selected.physicalDeviceIndex != first.physicalDeviceIndex) {
    std::cerr << "[DeploymentHardware] "
              << (error.empty() ? "manual UUID identity mismatch" : error)
              << '\n';
    return EXIT_FAILURE;
  }

  VulkanDeploymentHardwareCollectorOptions collectorOptions{};
  collectorOptions.manualDeviceUuid = first.fingerprint.deviceUuid;
  auto collector =
      makeVulkanDeploymentHardwareCollector(std::move(collectorOptions));
  viennaps::compute::HardwareFingerprint collected{};
  std::uint32_t collectedIndex = 0U;
  if (!collector(collected, collectedIndex, error) ||
      collected.deviceUuid != selected.fingerprint.deviceUuid ||
      collected.driverUuid != selected.fingerprint.driverUuid ||
      collectedIndex != selected.physicalDeviceIndex) {
    std::cerr << "[DeploymentHardware] "
              << (error.empty() ? "collector identity mismatch" : error)
              << '\n';
    return EXIT_FAILURE;
  }

  std::cout << "[DeploymentHardware] selected-device collector identity PASS\n";
  return EXIT_SUCCESS;
}
