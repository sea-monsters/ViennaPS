#include <chrono>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#include <compute/capabilityProfileIO.hpp>
#include <compute/vulkanDeploymentBootstrap.hpp>
#include <vcTestAsserts.hpp>

namespace {
using namespace viennaps::compute;

HardwareFingerprint hardware() {
  return {
      "device-bootstrap", "driver-bootstrap", 1U, 2U, "fixture", "1.0", "2026"};
}

CapabilityProfileRecord validRecord(const HardwareFingerprint &fingerprint) {
  CapabilityProfileRecord record;
  record.recordedAt = "2026-08-03T00:00:00Z";
  record.hardware = fingerprint;
  record.capabilityProfile.cpuAvailable = true;
  auto &smoke = record.capabilityProfile.vulkanFp32NumericalSmoke;
  smoke.status = VulkanNumericalSmokeStatus::PASS;
  smoke.contractId = std::string(kVulkanFp32NumericalSmokeContract);
  smoke.caseCount = 18U;
  smoke.watchdogMs = kVulkanFp32NumericalSmokeWatchdogMs;
  smoke.elapsedMs = 1U;
  return record;
}

CapabilityProfileRecord validRecord() { return validRecord(hardware()); }

std::filesystem::path tempDirectory() {
  const auto path = std::filesystem::temp_directory_path() /
                    "viennaps-vulkan-bootstrap-cpu-test";
  std::error_code ec;
  std::filesystem::remove_all(path, ec);
  std::filesystem::create_directories(path, ec);
  return path;
}

std::vector<StageWorkload> workloads() {
  return {{Stage::LEVEL_SET, Precision::FP32, 64U, false, RayMode::NONE, true}};
}

void successAndSingleProbe() {
  const auto temp = tempDirectory();
  const auto selectedHardware = HardwareFingerprint{
      "selected-device", "selected-driver", 9U, 10U, "selected-fixture",
      "2.0", "2026-08-03"};
  std::size_t collectorCalls = 0U;
  std::size_t launcherCalls = 0U;
  VulkanDeploymentBootstrapOptions options;
  options.profilePath = (temp / "profile.json").string();
  options.probeExecutable = "fake-probe";
  options.transientOutputDirectory = temp / "transient";
  std::filesystem::create_directories(options.transientOutputDirectory);
  options.collector = [&](HardwareFingerprint &out, std::uint32_t &index,
                          std::string &) {
    ++collectorCalls;
    out = selectedHardware;
    index = 7U;
    return true;
  };
  options.launcher = [&](const std::vector<std::string> &argv,
                         std::chrono::milliseconds, std::string &error) {
    ++launcherCalls;
    VC_TEST_ASSERT(argv.size() == 7U && argv.at(0) == "fake-probe" &&
                   argv.at(1) == "--strict-fp32-smoke" &&
                   argv.at(2) == "--write-deployment-profile" &&
                   argv.at(4) == "--validate-profile" &&
                   argv.at(5) == "--strict-fp32-device-index" &&
                   argv.at(6) == "7");
    return writeCapabilityProfileRecordToFile(argv.at(3),
                                              validRecord(selectedHardware),
                                              &error);
  };
  const auto result = bootstrapVulkanDeploymentProfile(
      workloads(), ManualSelectionConfig{}, std::move(options));
  VC_TEST_ASSERT(result.ok && result.probeInvoked && collectorCalls == 1U &&
                 launcherCalls == 1U);
  VC_TEST_ASSERT(
      result.hardware.deviceUuid == "selected-device" &&
      result.hardware.driverUuid == "selected-driver" &&
      result.hardware.vendorId == 9U && result.hardware.deviceId == 10U &&
      result.hardware.deviceName == "selected-fixture" &&
      result.hardware.driverVersion == "2.0" &&
      result.hardware.driverDate == "2026-08-03");
  VC_TEST_ASSERT(
      result.provisioning.decision.activeRecord.hardware.deviceUuid ==
          "selected-device" &&
      result.provisioning.decision.activeRecord.hardware.driverUuid ==
          "selected-driver" &&
      result.provisioning.decision.activeRecord.hardware.vendorId == 9U &&
      result.provisioning.decision.activeRecord.hardware.deviceId == 10U &&
      result.provisioning.decision.activeRecord.hardware.deviceName ==
          "selected-fixture" &&
      result.provisioning.decision.activeRecord.hardware.driverVersion == "2.0" &&
      result.provisioning.decision.activeRecord.hardware.driverDate ==
          "2026-08-03");
  VC_TEST_ASSERT(std::filesystem::is_directory(temp / "transient") &&
                 std::filesystem::directory_iterator(temp / "transient") ==
                     std::filesystem::directory_iterator{});
  std::error_code ec;
  std::filesystem::remove_all(temp, ec);
}

void failureIsCpuFallback() {
  const auto temp = tempDirectory();
  VulkanDeploymentBootstrapOptions options;
  options.profilePath = (temp / "profile.json").string();
  options.probeExecutable = "fake-probe";
  options.transientOutputDirectory = temp / "transient";
  std::filesystem::create_directories(options.transientOutputDirectory);
  options.collector = [](HardwareFingerprint &, std::uint32_t &,
                         std::string &error) {
    error = "collector failure";
    return false;
  };
  const auto result = bootstrapVulkanDeploymentProfile(
      workloads(), ManualSelectionConfig{}, std::move(options));
  VC_TEST_ASSERT(
      !result.ok && result.collectorInvoked && !result.probeInvoked &&
      result.error == "collector failure" &&
      result.provisioning.error == "collector failure" &&
      result.provisioning.decision.plan.stages.front().selectedBackend ==
          ComputeBackend::CPU);
  VC_TEST_ASSERT(std::filesystem::is_directory(temp / "transient") &&
                 std::filesystem::directory_iterator(temp / "transient") ==
                     std::filesystem::directory_iterator{});
  std::error_code ec;
  std::filesystem::remove_all(temp, ec);
}

void probeFailureIsCpuFallbackAndCleansOutput() {
  const auto temp = tempDirectory();
  const auto selectedHardware = hardware();
  VulkanDeploymentBootstrapOptions options;
  options.profilePath = (temp / "profile.json").string();
  options.probeExecutable = "fake-probe";
  options.transientOutputDirectory = temp / "transient";
  std::filesystem::create_directories(options.transientOutputDirectory);
  options.collector = [&](HardwareFingerprint &out, std::uint32_t &index,
                          std::string &) {
    out = selectedHardware;
    index = 7U;
    return true;
  };
  options.launcher = [&](const std::vector<std::string> &argv,
                         std::chrono::milliseconds, std::string &error) {
    std::string writeError;
    if (!writeCapabilityProfileRecordToFile(argv.at(3),
                                            validRecord(selectedHardware),
                                            &writeError)) {
      error = writeError;
      return false;
    }
    error = "probe failure";
    return false;
  };
  const auto result = bootstrapVulkanDeploymentProfile(
      workloads(), ManualSelectionConfig{}, std::move(options));
  VC_TEST_ASSERT(
      !result.ok && result.collectorInvoked && result.probeInvoked &&
      result.error == "probe failure" &&
      result.provisioning.error == "probe failure" &&
      result.provisioning.decision.plan.stages.front().selectedBackend ==
          ComputeBackend::CPU);
  VC_TEST_ASSERT(std::filesystem::is_directory(temp / "transient") &&
                 std::filesystem::directory_iterator(temp / "transient") ==
                     std::filesystem::directory_iterator{});
  std::error_code ec;
  std::filesystem::remove_all(temp, ec);
}

void manualCpuBypassesEverything() {
  ManualSelectionConfig config;
  config.selectionMode = SelectionMode::MANUAL;
  config.globalBackend = ComputeBackend::CPU;
  VulkanDeploymentBootstrapOptions options;
  const auto result =
      bootstrapVulkanDeploymentProfile(workloads(), config, std::move(options));
  VC_TEST_ASSERT(result.ok && result.manualCpuBypass &&
                 !result.collectorInvoked && !result.probeInvoked);

  config.perStageBackend.at(static_cast<std::size_t>(Stage::LEVEL_SET)) =
      ComputeBackend::VULKAN;
  const auto overridden = bootstrapVulkanDeploymentProfile(
      workloads(), config, VulkanDeploymentBootstrapOptions{});
  VC_TEST_ASSERT(!overridden.ok && !overridden.manualCpuBypass &&
                 !overridden.collectorInvoked && !overridden.probeInvoked);
}

void missingTransientDirectoryFailsClosed() {
  const auto temp = tempDirectory();
  std::size_t collectorCalls = 0U;
  VulkanDeploymentBootstrapOptions options;
  options.profilePath = (temp / "profile.json").string();
  options.probeExecutable = "fake-probe";
  options.transientOutputDirectory = temp / "missing";
  options.collector = [&](HardwareFingerprint &out, std::uint32_t &index,
                          std::string &) {
    ++collectorCalls;
    out = hardware();
    index = 0U;
    return true;
  };
  const auto result = bootstrapVulkanDeploymentProfile(
      workloads(), ManualSelectionConfig{}, std::move(options));
  VC_TEST_ASSERT(
      !result.ok && result.collectorInvoked && !result.probeInvoked &&
      collectorCalls == 1U &&
      result.provisioning.decision.plan.stages.front().selectedBackend ==
          ComputeBackend::CPU);
  std::error_code ec;
  std::filesystem::remove_all(temp, ec);
}
} // namespace

int main() {
  try {
    successAndSingleProbe();
    failureIsCpuFallback();
    probeFailureIsCpuFallbackAndCleansOutput();
    manualCpuBypassesEverything();
    missingTransientDirectoryFailsClosed();
  } catch (const std::exception &exception) {
    std::cerr << exception.what() << '\n';
    return 1;
  }
  return 0;
}
