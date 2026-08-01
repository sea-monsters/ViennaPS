// Copyright 2026 ViennaPS
// SPDX-License-Identifier: MIT

#include "levelset_process_controller.hpp"

#include <compute/capabilityProfileIO.hpp>

#include <vcTestAsserts.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>

namespace {

using Controller = viennaps::vulkan::levelset::LevelSetProcessController<2>;
using viennaps::compute::ComputeBackend;
using viennaps::compute::HardwareFingerprint;
using viennaps::compute::ManualSelectionConfig;
using viennaps::compute::Precision;
using viennaps::compute::SelectionMode;
using viennaps::compute::Stage;
using viennaps::compute::StageWorkload;
using viennaps::vulkan::runtime::ComputeSession;
using viennaps::vulkan::runtime::ComputeSessionOptions;

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
         ("viennaps-levelset-controller-" + std::to_string(tick) + "-" +
          std::to_string(sequence.fetch_add(1U)));
}

[[nodiscard]] std::string uuidToHex(const std::uint8_t *bytes) {
  std::ostringstream out;
  out << std::hex << std::setfill('0');
  for (std::size_t i = 0; i < VK_UUID_SIZE; ++i) {
    out << std::setw(2) << static_cast<unsigned int>(bytes[i]);
  }
  return out.str();
}

[[nodiscard]] bool collectHardware(HardwareFingerprint &fingerprint,
                                   std::string &error) {
  ComputeSession probeSession;
  if (!probeSession.initialize(error)) {
    return false;
  }
  VkPhysicalDeviceIDProperties idProperties{};
  idProperties.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES;
  VkPhysicalDeviceProperties2 properties{};
  properties.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
  properties.pNext = &idProperties;
  vkGetPhysicalDeviceProperties2(probeSession.selection().handle, &properties);

  fingerprint.deviceUuid = uuidToHex(idProperties.deviceUUID);
  fingerprint.driverUuid = uuidToHex(idProperties.driverUUID);
  fingerprint.vendorId = properties.properties.vendorID;
  fingerprint.deviceId = properties.properties.deviceID;
  fingerprint.deviceName = properties.properties.deviceName;
  fingerprint.driverVersion =
      std::to_string(properties.properties.driverVersion);
  fingerprint.driverDate = "unknown";
  return true;
}

[[nodiscard]] bool writeProfile(const std::filesystem::path &directory,
                                const HardwareFingerprint &hardware,
                                std::string &error) {
  std::error_code filesystemError;
  std::filesystem::create_directories(directory, filesystemError);
  if (filesystemError) {
    error = filesystemError.message();
    return false;
  }

  viennaps::compute::CapabilityProfileRecord record;
  record.recordedAt = "2026-08-02T00:00:00Z";
  record.hardware = hardware;
  record.capabilityProfile.cpuAvailable = true;
  record.capabilityProfile.vulkanAvailable = true;
  record.capabilityProfile.vulkanPrimitiveSuitePass = true;
  record.capabilityProfile.vulkanCompute = true;
  record.capabilityProfile.safeVulkanWorkingSetBytes =
      128ULL * 1024ULL * 1024ULL;
  const auto profilePath = directory / (hardware.deviceUuid + ".json");
  return viennaps::compute::writeCapabilityProfileRecordToFile(
      profilePath.string(), record, &error);
}

[[nodiscard]] Controller::Result configure(
    const ManualSelectionConfig &selection, const HardwareFingerprint &hardware,
    const StageWorkload &workload, const ComputeSessionOptions &manualDevice,
    const std::string_view spirvPath, const std::string_view profilePath) {
  viennaps::Process<float, 2> process;
  Controller controller;
  return controller.configure(process, selection, hardware, workload,
                              manualDevice, spirvPath, profilePath);
}

} // namespace

int main() {
  HardwareFingerprint hardware;
  std::string error;
  if (!collectHardware(hardware, error)) {
    std::cout << "[LevelSetController] SKIP: " << error << '\n';
    return EXIT_SUCCESS;
  }

  const StageWorkload workload{Stage::LEVEL_SET,
                               Precision::FP32,
                               4096U,
                               false,
                               viennaps::compute::RayMode::NONE,
                               true};
  const TempDirectoryGuard missingProfile{uniqueTempDirectory()};
  const auto missingResult =
      configure({}, hardware, workload, {}, {}, missingProfile.path.string());
  VC_TEST_ASSERT(missingResult.ok);
  VC_TEST_ASSERT(missingResult.prepared);
  VC_TEST_ASSERT(missingResult.degraded);
  VC_TEST_ASSERT(!missingResult.usingVulkan);
  VC_TEST_ASSERT(missingResult.selectedBackend == ComputeBackend::CPU);

  const TempDirectoryGuard validProfile{uniqueTempDirectory()};
  VC_TEST_ASSERT(writeProfile(validProfile.path, hardware, error));

  const auto autoVulkan =
      configure({}, hardware, workload, {}, VIENNAPS_LEVELSET_UPDATE_SPV_PATH,
                validProfile.path.string());
  VC_TEST_ASSERT(autoVulkan.ok);
  VC_TEST_ASSERT(autoVulkan.prepared);
  VC_TEST_ASSERT(autoVulkan.usingVulkan);
  VC_TEST_ASSERT(!autoVulkan.degraded);
  VC_TEST_ASSERT(autoVulkan.selectedBackend == ComputeBackend::VULKAN);

  const auto autoShaderFallback =
      configure({}, hardware, workload, {}, "missing-controller-shader.spv",
                validProfile.path.string());
  VC_TEST_ASSERT(autoShaderFallback.ok);
  VC_TEST_ASSERT(autoShaderFallback.prepared);
  VC_TEST_ASSERT(autoShaderFallback.degraded);
  VC_TEST_ASSERT(!autoShaderFallback.usingVulkan);
  VC_TEST_ASSERT(autoShaderFallback.selectedBackend == ComputeBackend::CPU);

  ManualSelectionConfig manualCpu;
  manualCpu.selectionMode = SelectionMode::MANUAL;
  manualCpu.globalBackend = ComputeBackend::CPU;
  const auto cpuResult = configure(manualCpu, hardware, workload, {}, {},
                                   validProfile.path.string());
  VC_TEST_ASSERT(cpuResult.ok);
  VC_TEST_ASSERT(cpuResult.prepared);
  VC_TEST_ASSERT(!cpuResult.degraded);
  VC_TEST_ASSERT(!cpuResult.usingVulkan);
  VC_TEST_ASSERT(cpuResult.selectedBackend == ComputeBackend::CPU);

  ManualSelectionConfig manualVulkan;
  manualVulkan.selectionMode = SelectionMode::MANUAL;
  manualVulkan.globalBackend = ComputeBackend::VULKAN;
  const auto manualShaderError =
      configure(manualVulkan, hardware, workload, {},
                "missing-controller-shader.spv", validProfile.path.string());
  VC_TEST_ASSERT(!manualShaderError.ok);
  VC_TEST_ASSERT(manualShaderError.prepared);
  VC_TEST_ASSERT(!manualShaderError.degraded);
  VC_TEST_ASSERT(!manualShaderError.usingVulkan);

  ComputeSessionOptions selectedDevice;
  selectedDevice.manualDeviceName = hardware.deviceName;
  const auto manualVulkanResult =
      configure(manualVulkan, hardware, workload, selectedDevice,
                VIENNAPS_LEVELSET_UPDATE_SPV_PATH, validProfile.path.string());
  VC_TEST_ASSERT(manualVulkanResult.ok);
  VC_TEST_ASSERT(manualVulkanResult.prepared);
  VC_TEST_ASSERT(manualVulkanResult.usingVulkan);
  VC_TEST_ASSERT(manualVulkanResult.selectedBackend == ComputeBackend::VULKAN);

  auto staleHardware = hardware;
  staleHardware.driverVersion += "-stale";
  const auto staleResult = configure({}, staleHardware, workload, {}, {},
                                     validProfile.path.string());
  VC_TEST_ASSERT(staleResult.ok);
  VC_TEST_ASSERT(staleResult.prepared);
  VC_TEST_ASSERT(staleResult.degraded);
  VC_TEST_ASSERT(!staleResult.usingVulkan);
  VC_TEST_ASSERT(staleResult.selectedBackend == ComputeBackend::CPU);

  auto invalidWorkload = workload;
  invalidWorkload.stage = Stage::RAY_TRACING;
  const auto invalidResult = configure({}, hardware, invalidWorkload, {}, {},
                                       validProfile.path.string());
  VC_TEST_ASSERT(!invalidResult.ok);
  VC_TEST_ASSERT(!invalidResult.prepared);
  VC_TEST_ASSERT(!invalidResult.message.empty());

  auto zeroWorkload = workload;
  zeroWorkload.estimatedBytes = 0U;
  const auto zeroWorkloadResult =
      configure({}, hardware, zeroWorkload, {}, {}, validProfile.path.string());
  VC_TEST_ASSERT(!zeroWorkloadResult.ok);
  VC_TEST_ASSERT(!zeroWorkloadResult.prepared);
  VC_TEST_ASSERT(!zeroWorkloadResult.message.empty());

  ManualSelectionConfig fp64Selection;
  fp64Selection.precision = Precision::FP64;
  const auto fp64Result = configure(fp64Selection, hardware, workload, {}, {},
                                    validProfile.path.string());
  VC_TEST_ASSERT(!fp64Result.ok);
  VC_TEST_ASSERT(!fp64Result.prepared);
  VC_TEST_ASSERT(!fp64Result.message.empty());

  std::cout << "[LevelSetController] auto/manual/stale/shader/gates PASS\n";
  return EXIT_SUCCESS;
}
