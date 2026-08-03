// Copyright 2026 ViennaPS
// SPDX-License-Identifier: MIT

#include "levelset_process_controller.hpp"

#include <compute/capabilityProfileIO.hpp>

#include <vcTestAsserts.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace {

using Controller = viennaps::vulkan::levelset::LevelSetProcessController<2>;
using RebuildSpirvPaths = Controller::RebuildSpirvPaths;
using Advect = viennals::Advect<float, 2>;
using viennaps::LevelSetUpdateFailurePolicy;
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
  record.capabilityProfile.vulkanFp32NumericalSmoke.status =
      viennaps::compute::VulkanNumericalSmokeStatus::PASS;
  record.capabilityProfile.vulkanFp32NumericalSmoke.contractId =
      std::string(viennaps::compute::kVulkanFp32NumericalSmokeContract);
  record.capabilityProfile.vulkanFp32NumericalSmoke.caseCount = 18U;
  record.capabilityProfile.vulkanFp32NumericalSmoke.mismatchCount = 0U;
  record.capabilityProfile.vulkanFp32NumericalSmoke.maxUlp = 0U;
  record.capabilityProfile.vulkanFp32NumericalSmoke.watchdogMs =
      viennaps::compute::kVulkanFp32NumericalSmokeWatchdogMs;
  record.capabilityProfile.vulkanFp32NumericalSmoke.elapsedMs = 1U;
  record.capabilityProfile.safeVulkanWorkingSetBytes =
      128ULL * 1024ULL * 1024ULL;
  const auto profilePath = directory / (hardware.deviceUuid + ".json");
  return viennaps::compute::writeCapabilityProfileRecordToFile(
      profilePath.string(), record, &error);
}

struct ConfigureResult {
  Controller::Result controller;
  LevelSetUpdateFailurePolicy failurePolicy;
  bool updateInstalled = false;
  bool rebuildInstalled = false;
};

[[nodiscard]] ConfigureResult
configure(const ManualSelectionConfig &selection,
          const HardwareFingerprint &hardware, const StageWorkload &workload,
          const ComputeSessionOptions &manualDevice,
          const std::string_view spirvPath, const std::string_view profilePath,
          const RebuildSpirvPaths &rebuildPaths = {}) {
  viennaps::Process<float, 2> process;
  Controller controller;
  auto result =
      controller.configure(process, selection, hardware, workload, manualDevice,
                           spirvPath, profilePath, rebuildPaths);
  return {std::move(result), process.getLevelSetUpdateFailurePolicy(),
          static_cast<bool>(process.getLevelSetUpdateExecutor()),
          static_cast<bool>(process.getLevelSetRebuildExecutor())};
}

} // namespace

int main() try {
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
  const auto missingConfiguration =
      configure({}, hardware, workload, {}, {}, missingProfile.path.string());
  const auto &missingResult = missingConfiguration.controller;
  VC_TEST_ASSERT(missingResult.ok);
  VC_TEST_ASSERT(missingResult.prepared);
  VC_TEST_ASSERT(missingResult.degraded);
  VC_TEST_ASSERT(!missingResult.usingVulkan);
  VC_TEST_ASSERT(missingResult.selectedBackend == ComputeBackend::CPU);
  VC_TEST_ASSERT(missingConfiguration.failurePolicy ==
                 LevelSetUpdateFailurePolicy::FALLBACK);

  const TempDirectoryGuard validProfile{uniqueTempDirectory()};
  VC_TEST_ASSERT(writeProfile(validProfile.path, hardware, error));

  const auto autoConfiguration =
      configure({}, hardware, workload, {}, VIENNAPS_LEVELSET_UPDATE_SPV_PATH,
                validProfile.path.string());
  const auto &autoVulkan = autoConfiguration.controller;
  VC_TEST_ASSERT(autoVulkan.ok);
  VC_TEST_ASSERT(autoVulkan.prepared);
  VC_TEST_ASSERT(autoVulkan.usingVulkan);
  VC_TEST_ASSERT(!autoVulkan.degraded);
  VC_TEST_ASSERT(autoVulkan.selectedBackend == ComputeBackend::VULKAN);
  VC_TEST_ASSERT(autoVulkan.updateSessionGeneration != 0U);
  VC_TEST_ASSERT(autoVulkan.updateSessionGeneration ==
                 autoVulkan.rebuildSessionGeneration);
  VC_TEST_ASSERT(!autoVulkan.updateSessionDeviceName.empty());
  VC_TEST_ASSERT(autoVulkan.updateSessionDeviceName ==
                 autoVulkan.rebuildSessionDeviceName);
  VC_TEST_ASSERT(autoConfiguration.failurePolicy ==
                 LevelSetUpdateFailurePolicy::FALLBACK);
  VC_TEST_ASSERT(autoConfiguration.updateInstalled);
  VC_TEST_ASSERT(autoConfiguration.rebuildInstalled);

  RebuildSpirvPaths badRebuildPaths;
  badRebuildPaths.classification = "missing-controller-rebuild.spv";
  const auto autoRebuildFallback =
      configure({}, hardware, workload, {}, VIENNAPS_LEVELSET_UPDATE_SPV_PATH,
                validProfile.path.string(), badRebuildPaths);
  VC_TEST_ASSERT(autoRebuildFallback.controller.ok);
  VC_TEST_ASSERT(autoRebuildFallback.controller.degraded);
  VC_TEST_ASSERT(!autoRebuildFallback.updateInstalled);
  VC_TEST_ASSERT(!autoRebuildFallback.rebuildInstalled);

  const auto autoShaderConfiguration =
      configure({}, hardware, workload, {}, "missing-controller-shader.spv",
                validProfile.path.string());
  const auto &autoShaderFallback = autoShaderConfiguration.controller;
  VC_TEST_ASSERT(autoShaderFallback.ok);
  VC_TEST_ASSERT(autoShaderFallback.prepared);
  VC_TEST_ASSERT(autoShaderFallback.degraded);
  VC_TEST_ASSERT(!autoShaderFallback.usingVulkan);
  VC_TEST_ASSERT(autoShaderFallback.selectedBackend == ComputeBackend::CPU);
  VC_TEST_ASSERT(autoShaderConfiguration.failurePolicy ==
                 LevelSetUpdateFailurePolicy::FALLBACK);

  ManualSelectionConfig manualCpu;
  manualCpu.selectionMode = SelectionMode::MANUAL;
  manualCpu.globalBackend = ComputeBackend::CPU;
  const auto cpuConfiguration = configure(manualCpu, hardware, workload, {}, {},
                                          validProfile.path.string());
  const auto &cpuResult = cpuConfiguration.controller;
  VC_TEST_ASSERT(cpuResult.ok);
  VC_TEST_ASSERT(cpuResult.prepared);
  VC_TEST_ASSERT(!cpuResult.degraded);
  VC_TEST_ASSERT(!cpuResult.usingVulkan);
  VC_TEST_ASSERT(cpuResult.selectedBackend == ComputeBackend::CPU);
  VC_TEST_ASSERT(cpuConfiguration.failurePolicy ==
                 LevelSetUpdateFailurePolicy::FALLBACK);

  ManualSelectionConfig manualVulkan;
  manualVulkan.selectionMode = SelectionMode::MANUAL;
  manualVulkan.globalBackend = ComputeBackend::VULKAN;
  const auto manualShaderConfiguration =
      configure(manualVulkan, hardware, workload, {},
                "missing-controller-shader.spv", validProfile.path.string());
  const auto &manualShaderError = manualShaderConfiguration.controller;
  VC_TEST_ASSERT(!manualShaderError.ok);
  VC_TEST_ASSERT(manualShaderError.prepared);
  VC_TEST_ASSERT(!manualShaderError.degraded);
  VC_TEST_ASSERT(!manualShaderError.usingVulkan);
  VC_TEST_ASSERT(manualShaderConfiguration.failurePolicy ==
                 LevelSetUpdateFailurePolicy::FALLBACK);

  const auto manualRebuildError = configure(
      manualVulkan, hardware, workload, {}, VIENNAPS_LEVELSET_UPDATE_SPV_PATH,
      validProfile.path.string(), badRebuildPaths);
  VC_TEST_ASSERT(!manualRebuildError.controller.ok);
  VC_TEST_ASSERT(manualRebuildError.controller.prepared);
  VC_TEST_ASSERT(!manualRebuildError.updateInstalled);
  VC_TEST_ASSERT(!manualRebuildError.rebuildInstalled);

  viennaps::Process<float, 2> preservedProcess;
  const auto preservedUpdate =
      [](const Advect::LevelSetUpdateContext &, Advect::LevelSetUpdateOutput &,
         std::string &) { return Advect::LevelSetUpdateStatus::ERROR; };
  const auto preservedRebuild = [](const Advect::LevelSetRebuildContext &,
                                   Advect::LevelSetRebuildOutput &,
                                   std::string &) {
    return Advect::LevelSetRebuildStatus::ERROR;
  };
  preservedProcess.setLevelSetUpdateExecutor(preservedUpdate);
  preservedProcess.setLevelSetRebuildExecutor(preservedRebuild);
  preservedProcess.setLevelSetUpdateFailurePolicy(
      LevelSetUpdateFailurePolicy::FAIL);
  Controller preservedController;
  const auto preservedFailure = preservedController.configure(
      preservedProcess, manualVulkan, hardware, workload, {},
      VIENNAPS_LEVELSET_UPDATE_SPV_PATH, validProfile.path.string(),
      badRebuildPaths);
  VC_TEST_ASSERT(!preservedFailure.ok);
  VC_TEST_ASSERT(
      static_cast<bool>(preservedProcess.getLevelSetUpdateExecutor()));
  VC_TEST_ASSERT(
      static_cast<bool>(preservedProcess.getLevelSetRebuildExecutor()));
  VC_TEST_ASSERT(preservedProcess.getLevelSetUpdateFailurePolicy() ==
                 LevelSetUpdateFailurePolicy::FAIL);

  auto invalidPreservedWorkload = workload;
  invalidPreservedWorkload.stage = Stage::RAY_TRACING;
  const auto invalidPreservedFailure = preservedController.configure(
      preservedProcess, manualVulkan, hardware, invalidPreservedWorkload, {},
      VIENNAPS_LEVELSET_UPDATE_SPV_PATH, validProfile.path.string());
  VC_TEST_ASSERT(!invalidPreservedFailure.ok);
  VC_TEST_ASSERT(
      static_cast<bool>(preservedProcess.getLevelSetUpdateExecutor()));
  VC_TEST_ASSERT(
      static_cast<bool>(preservedProcess.getLevelSetRebuildExecutor()));
  VC_TEST_ASSERT(preservedProcess.getLevelSetUpdateFailurePolicy() ==
                 LevelSetUpdateFailurePolicy::FAIL);

  ComputeSessionOptions selectedDevice;
  selectedDevice.manualDeviceName = hardware.deviceName;
  const auto manualVulkanConfiguration =
      configure(manualVulkan, hardware, workload, selectedDevice,
                VIENNAPS_LEVELSET_UPDATE_SPV_PATH, validProfile.path.string());
  const auto &manualVulkanResult = manualVulkanConfiguration.controller;
  VC_TEST_ASSERT(manualVulkanResult.ok);
  VC_TEST_ASSERT(manualVulkanResult.prepared);
  VC_TEST_ASSERT(manualVulkanResult.usingVulkan);
  VC_TEST_ASSERT(manualVulkanResult.selectedBackend == ComputeBackend::VULKAN);
  VC_TEST_ASSERT(manualVulkanConfiguration.failurePolicy ==
                 LevelSetUpdateFailurePolicy::FAIL);

  viennaps::Process<float, 2> clearProcess;
  Controller clearController;
  const auto clearConfiguration = clearController.configure(
      clearProcess, manualVulkan, hardware, workload, selectedDevice,
      VIENNAPS_LEVELSET_UPDATE_SPV_PATH, validProfile.path.string());
  VC_TEST_ASSERT(clearConfiguration.ok);
  VC_TEST_ASSERT(clearProcess.getLevelSetUpdateFailurePolicy() ==
                 LevelSetUpdateFailurePolicy::FAIL);
  VC_TEST_ASSERT(static_cast<bool>(clearProcess.getLevelSetUpdateExecutor()));
  VC_TEST_ASSERT(static_cast<bool>(clearProcess.getLevelSetRebuildExecutor()));
  clearController.clear(clearProcess);
  VC_TEST_ASSERT(clearProcess.getLevelSetUpdateFailurePolicy() ==
                 LevelSetUpdateFailurePolicy::FALLBACK);
  VC_TEST_ASSERT(!static_cast<bool>(clearProcess.getLevelSetUpdateExecutor()));
  VC_TEST_ASSERT(!static_cast<bool>(clearProcess.getLevelSetRebuildExecutor()));

  auto staleHardware = hardware;
  staleHardware.driverVersion += "-stale";
  const auto staleConfiguration = configure({}, staleHardware, workload, {}, {},
                                            validProfile.path.string());
  const auto &staleResult = staleConfiguration.controller;
  VC_TEST_ASSERT(staleResult.ok);
  VC_TEST_ASSERT(staleResult.prepared);
  VC_TEST_ASSERT(staleResult.degraded);
  VC_TEST_ASSERT(!staleResult.usingVulkan);
  VC_TEST_ASSERT(staleResult.selectedBackend == ComputeBackend::CPU);
  VC_TEST_ASSERT(staleConfiguration.failurePolicy ==
                 LevelSetUpdateFailurePolicy::FALLBACK);

  auto invalidWorkload = workload;
  invalidWorkload.stage = Stage::RAY_TRACING;
  const auto invalidConfiguration = configure({}, hardware, invalidWorkload, {},
                                              {}, validProfile.path.string());
  const auto &invalidResult = invalidConfiguration.controller;
  VC_TEST_ASSERT(!invalidResult.ok);
  VC_TEST_ASSERT(!invalidResult.prepared);
  VC_TEST_ASSERT(!invalidResult.message.empty());
  VC_TEST_ASSERT(invalidConfiguration.failurePolicy ==
                 LevelSetUpdateFailurePolicy::FALLBACK);

  auto zeroWorkload = workload;
  zeroWorkload.estimatedBytes = 0U;
  const auto zeroWorkloadConfiguration =
      configure({}, hardware, zeroWorkload, {}, {}, validProfile.path.string());
  const auto &zeroWorkloadResult = zeroWorkloadConfiguration.controller;
  VC_TEST_ASSERT(!zeroWorkloadResult.ok);
  VC_TEST_ASSERT(!zeroWorkloadResult.prepared);
  VC_TEST_ASSERT(!zeroWorkloadResult.message.empty());
  VC_TEST_ASSERT(zeroWorkloadConfiguration.failurePolicy ==
                 LevelSetUpdateFailurePolicy::FALLBACK);

  ManualSelectionConfig fp64Selection;
  fp64Selection.precision = Precision::FP64;
  const auto fp64Configuration = configure(fp64Selection, hardware, workload,
                                           {}, {}, validProfile.path.string());
  const auto &fp64Result = fp64Configuration.controller;
  VC_TEST_ASSERT(!fp64Result.ok);
  VC_TEST_ASSERT(!fp64Result.prepared);
  VC_TEST_ASSERT(!fp64Result.message.empty());
  VC_TEST_ASSERT(fp64Configuration.failurePolicy ==
                 LevelSetUpdateFailurePolicy::FALLBACK);

  std::cout << "[LevelSetController] auto/manual/stale/shader/gates PASS\n";
  return EXIT_SUCCESS;
} catch (const std::exception &error) {
  std::cerr << "[LevelSetController] " << error.what() << '\n';
  return EXIT_FAILURE;
}
