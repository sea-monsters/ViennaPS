// Copyright 2026 ViennaPS
// SPDX-License-Identifier: MIT

#include "deployment_compute_context.hpp"

#include <compute/capabilityProfileIO.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <system_error>
#include <vector>

namespace {

[[nodiscard]] std::string uuidToHex(const std::uint8_t *bytes) {
  std::ostringstream out;
  out << std::hex << std::setfill('0');
  for (std::size_t i = 0; i < VK_UUID_SIZE; ++i) {
    out << std::setw(2) << static_cast<unsigned int>(bytes[i]);
  }
  return out.str();
}

void reportFailure(const std::string &message) {
  std::cerr << "[DeploymentContext] " << message << '\n';
}

struct TempDirectoryGuard {
  std::filesystem::path path;
  ~TempDirectoryGuard() {
    std::error_code ignored;
    std::filesystem::remove_all(path, ignored);
  }
};

[[nodiscard]] std::filesystem::path uniqueTempDirectory() {
  static std::atomic_uint64_t sequence{0U};
  const auto tick = std::chrono::steady_clock::now().time_since_epoch().count();
  return std::filesystem::temp_directory_path() /
         ("viennaps-deployment-context-" + std::to_string(tick) + "-" +
          std::to_string(sequence.fetch_add(1U)));
}

} // namespace

int main() {
  using namespace viennaps;
  using namespace viennaps::compute;
  using namespace viennaps::vulkan::runtime;

  std::string error;
  ComputeSession probeSession{};
  if (!probeSession.initialize(error)) {
    std::cerr << error << '\n';
    return EXIT_FAILURE;
  }
  VkPhysicalDeviceIDProperties idProperties{};
  idProperties.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES;
  VkPhysicalDeviceProperties2 properties2{};
  properties2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
  properties2.pNext = &idProperties;
  vkGetPhysicalDeviceProperties2(probeSession.selection().handle, &properties2);

  HardwareFingerprint fingerprint{};
  fingerprint.deviceUuid = uuidToHex(idProperties.deviceUUID);
  fingerprint.driverUuid = uuidToHex(idProperties.driverUUID);
  fingerprint.vendorId = properties2.properties.vendorID;
  fingerprint.deviceId = properties2.properties.deviceID;
  fingerprint.deviceName = properties2.properties.deviceName;
  fingerprint.driverVersion =
      std::to_string(properties2.properties.driverVersion);
  fingerprint.driverDate = "unknown";
  probeSession.reset();

  CapabilityProfileRecord record{};
  record.recordedAt = "2026-08-02T00:00:00Z";
  record.hardware = fingerprint;
  record.capabilityProfile.cpuAvailable = true;
  record.capabilityProfile.vulkanAvailable = true;
  record.capabilityProfile.vulkanPrimitiveSuitePass = true;
  record.capabilityProfile.vulkanFp64SuitePass = false;
  record.capabilityProfile.vulkanCompute = true;
  record.capabilityProfile.safeVulkanWorkingSetBytes =
      128ULL * 1024ULL * 1024ULL;

  const TempDirectoryGuard tempDirectory{uniqueTempDirectory()};
  const auto &profileDirectory = tempDirectory.path;
  const auto profilePath =
      profileDirectory / (fingerprint.deviceUuid + ".json");
  std::error_code filesystemError;
  std::filesystem::create_directories(profileDirectory, filesystemError);
  if (filesystemError || !writeCapabilityProfileRecordToFile(
                             profilePath.string(), record, &error)) {
    std::cerr << "profile setup failed: " << error << '\n';
    return EXIT_FAILURE;
  }

  const std::array<StageWorkload, 1> workloads = {StageWorkload{
      Stage::LEVEL_SET, Precision::FP32, 4096U, false, RayMode::NONE, true}};
  DeploymentComputeContext context{};
  bool pass = true;
  pass = context.prepare(fingerprint, workloads, ManualSelectionConfig{},
                         profileDirectory.string(), error) &&
         pass;
  pass = (context.isPrepared() && context.hasVulkanSession() &&
          context.session() != nullptr &&
          context.backendFor(Stage::LEVEL_SET) == ComputeBackend::VULKAN &&
          context.decision().state == DeploymentProfileState::VALID) &&
         pass;

  ManualSelectionConfig manualCpu{};
  manualCpu.selectionMode = SelectionMode::MANUAL;
  manualCpu.globalBackend = ComputeBackend::CPU;
  // A prepared context is cached and must not silently change mid-simulation.
  pass = context.prepare(fingerprint, workloads, manualCpu,
                         profileDirectory.string(), error) &&
         context.backendFor(Stage::LEVEL_SET) == ComputeBackend::VULKAN && pass;

  context.reset();
  pass = context.prepare(fingerprint, workloads, manualCpu,
                         profileDirectory.string(), error) &&
         context.isPrepared() && !context.hasVulkanSession() &&
         context.backendFor(Stage::LEVEL_SET) == ComputeBackend::CPU && pass;

  context.reset();
  HardwareFingerprint staleFingerprint = fingerprint;
  staleFingerprint.driverVersion += "-changed";
  pass = context.prepare(staleFingerprint, workloads, ManualSelectionConfig{},
                         profileDirectory.string(), error) &&
         context.decision().requiresProbe &&
         context.decision().state == DeploymentProfileState::STALE &&
         context.backendFor(Stage::LEVEL_SET) == ComputeBackend::CPU &&
         !context.hasVulkanSession() && pass;

  context.reset();
  ManualSelectionConfig unavailableManualVulkan{};
  unavailableManualVulkan.selectionMode = SelectionMode::MANUAL;
  unavailableManualVulkan.globalBackend = ComputeBackend::VULKAN;
  const bool rejected =
      !context.prepare(staleFingerprint, workloads, unavailableManualVulkan,
                       profileDirectory.string(), error);
  pass = rejected && !context.isPrepared() && !context.hasVulkanSession() &&
         !context.decision().plan.ok && pass;

  context.reset();
  ComputeSessionOptions manualDevice{};
  manualDevice.manualDeviceName = fingerprint.deviceName;
  pass = context.prepare(fingerprint, workloads, ManualSelectionConfig{},
                         profileDirectory.string(), error, manualDevice) &&
         context.hasVulkanSession() && pass;

  if (!pass) {
    reportFailure(error.empty() ? "policy/session checks failed" : error);
    return EXIT_FAILURE;
  }
  std::cout << "[DeploymentContext] auto/manual/cache/stale PASS\n";
  return EXIT_SUCCESS;
}
