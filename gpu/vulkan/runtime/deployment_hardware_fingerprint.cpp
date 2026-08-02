// Copyright 2026 ViennaPS
// SPDX-License-Identifier: MIT

#include "deployment_hardware_fingerprint.hpp"

#include <array>
#include <cstdint>
#include <filesystem>
#include <iomanip>
#include <limits>
#include <sstream>
#include <utility>
#include <vector>

namespace {

[[nodiscard]] std::string uuidToHex(const std::uint8_t *bytes) {
  std::ostringstream out;
  out << std::hex << std::setfill('0');
  for (std::size_t index = 0U; index < VK_UUID_SIZE; ++index) {
    out << std::setw(2) << static_cast<unsigned int>(bytes[index]);
  }
  return out.str();
}

[[nodiscard]] bool findPhysicalDeviceIndex(const VkInstance instance,
                                           const VkPhysicalDevice selected,
                                           std::uint32_t &index,
                                           std::string &error) {
  std::uint32_t count = 0U;
  if (vkEnumeratePhysicalDevices(instance, &count, nullptr) != VK_SUCCESS ||
      count == 0U) {
    error = "Vulkan deployment identity could not enumerate physical devices.";
    return false;
  }
  std::vector<VkPhysicalDevice> devices(count);
  if (vkEnumeratePhysicalDevices(instance, &count, devices.data()) !=
      VK_SUCCESS) {
    error = "Vulkan deployment identity could not list physical devices.";
    return false;
  }
  for (std::uint32_t current = 0U; current < count; ++current) {
    if (devices[current] == selected) {
      index = current;
      return true;
    }
  }
  error = "Vulkan deployment identity selected device is no longer enumerated.";
  return false;
}

} // namespace

namespace viennaps::vulkan::runtime {

bool collectDeploymentHardwareSnapshot(const ComputeSession &session,
                                       DeploymentHardwareSnapshot &snapshot,
                                       std::string &error) {
  error.clear();
  snapshot = {};
  if (!session.isValid()) {
    error = "Vulkan deployment identity requires an initialized session.";
    return false;
  }

  if (!findPhysicalDeviceIndex(session.instanceHandle(),
                               session.selection().handle,
                               snapshot.physicalDeviceIndex, error)) {
    return false;
  }

  VkPhysicalDeviceIDProperties ids{};
  ids.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES;
  VkPhysicalDeviceProperties2 properties{};
  properties.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
  properties.pNext = &ids;
  vkGetPhysicalDeviceProperties2(session.selection().handle, &properties);

  auto &fingerprint = snapshot.fingerprint;
  fingerprint.deviceUuid = uuidToHex(ids.deviceUUID);
  fingerprint.driverUuid = uuidToHex(ids.driverUUID);
  fingerprint.vendorId = properties.properties.vendorID;
  fingerprint.deviceId = properties.properties.deviceID;
  fingerprint.deviceName = properties.properties.deviceName;
  fingerprint.driverVersion =
      std::to_string(properties.properties.driverVersion);
  // Vulkan exposes driver identity but not a portable driver release date.
  // This matches the deployment profile produced by viennaps-device-probe.
  fingerprint.driverDate = "unknown";

  if (fingerprint.deviceUuid.empty() || fingerprint.driverUuid.empty() ||
      fingerprint.vendorId == 0U || fingerprint.deviceId == 0U ||
      fingerprint.deviceName.empty() || fingerprint.driverVersion.empty()) {
    snapshot = {};
    error = "Vulkan deployment identity is incomplete.";
    return false;
  }
  return true;
}

bool collectDeploymentHardwareSnapshot(const ComputeSessionOptions &options,
                                       DeploymentHardwareSnapshot &snapshot,
                                       std::string &error) {
  ComputeSession session;
  if (!session.initialize(error, options)) {
    snapshot = {};
    return false;
  }
  return collectDeploymentHardwareSnapshot(session, snapshot, error);
}

compute::VulkanDeploymentHardwareCollector
makeVulkanDeploymentHardwareCollector(
    VulkanDeploymentHardwareCollectorOptions options) {
  return [options = std::move(options)](
             compute::HardwareFingerprint &fingerprint,
             std::uint32_t &physicalDeviceIndex, std::string &error) {
    ComputeSessionOptions sessionOptions{};
    sessionOptions.manualDeviceIndex = options.manualDeviceIndex;
    sessionOptions.manualDeviceName = options.manualDeviceName;
    sessionOptions.manualDeviceUuid = options.manualDeviceUuid;

    DeploymentHardwareSnapshot snapshot{};
    if (!collectDeploymentHardwareSnapshot(sessionOptions, snapshot, error)) {
      fingerprint = {};
      physicalDeviceIndex = 0U;
      return false;
    }
    fingerprint = std::move(snapshot.fingerprint);
    physicalDeviceIndex = snapshot.physicalDeviceIndex;
    return true;
  };
}

compute::VulkanDeploymentBootstrapResult
bootstrapVulkanDeploymentForSelectedDevice(
    const std::vector<compute::StageWorkload> &workloads,
    const compute::ManualSelectionConfig &selection,
    compute::VulkanDeploymentBootstrapOptions options,
    VulkanDeploymentHardwareCollectorOptions hardwareOptions) {
  if (!options.collector) {
    options.collector =
        makeVulkanDeploymentHardwareCollector(std::move(hardwareOptions));
  }
  if (options.transientOutputDirectory.empty()) {
    std::error_code ec;
    options.transientOutputDirectory = std::filesystem::temp_directory_path(ec);
  }
  return compute::bootstrapVulkanDeploymentProfile(workloads, selection,
                                                   std::move(options));
}

} // namespace viennaps::vulkan::runtime
