#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>

#include <compute/capabilityProfileIO.hpp>
#include <compute/deploymentProfile.hpp>
#include <compute/vulkanDeploymentProbe.hpp>
#include <vcTestAsserts.hpp>

namespace viennacore {
using namespace viennaps::compute;

namespace {

struct TempDir {
  std::filesystem::path path;
  ~TempDir() {
    std::error_code ignored;
    std::filesystem::remove_all(path, ignored);
  }
};

TempDir tempDir() {
  static std::uint64_t sequence = 0U;
  const auto tick = std::chrono::steady_clock::now().time_since_epoch().count();
  const auto path = std::filesystem::temp_directory_path() /
                    ("viennaps-vulkan-deployment-probe-" +
                     std::to_string(tick) + "-" + std::to_string(sequence++));
  std::filesystem::create_directories(path);
  return {path};
}

HardwareFingerprint hardware() {
  return {"device-1", "driver-1", 1U, 2U, "fixture", "1.0", "2026"};
}

CapabilityProfileRecord validRecord(const HardwareFingerprint &fingerprint) {
  CapabilityProfileRecord record;
  record.recordedAt = "2026-08-02T00:00:00Z";
  record.hardware = fingerprint;
  record.capabilityProfile.cpuAvailable = true;
  record.capabilityProfile.vulkanAvailable = true;
  auto &smoke = record.capabilityProfile.vulkanFp32NumericalSmoke;
  smoke.status = VulkanNumericalSmokeStatus::PASS;
  smoke.contractId = std::string(kVulkanFp32NumericalSmokeContract);
  smoke.caseCount = 1U;
  smoke.mismatchCount = 0U;
  smoke.maxUlp = 0U;
  smoke.watchdogMs = kVulkanFp32NumericalSmokeWatchdogMs;
  smoke.elapsedMs = 1U;
  return record;
}

void TestSuccessInvokesStrictProbeOnceAndCleansOutput() {
  const auto temp = tempDir();
  std::size_t calls = 0U;
  std::vector<std::string> argv;
  VulkanDeploymentProbeOptions options;
  options.executable = "fake-viennaps-device-probe";
  options.outputPath = temp.path / "probe-output" / "strict-profile.json";
  std::filesystem::create_directories(options.outputPath.parent_path());
  options.launcher = [&](const std::vector<std::string> &args,
                         std::chrono::milliseconds, std::string &error) {
    ++calls;
    argv = args;
    const auto output = std::filesystem::path(args.at(3));
    return writeCapabilityProfileRecordToFile(output.string(),
                                              validRecord(hardware()), &error);
  };
  auto callback = makeVulkanDeploymentProfileProbe(options);
  const auto persisted = temp.path / "persisted.json";
  const std::vector<StageWorkload> workloads = {
      {Stage::LEVEL_SET, Precision::FP32, 128U, false, RayMode::NONE, true}};
  const auto provisioned =
      provisionDeploymentProfile(hardware(), workloads, ManualSelectionConfig{},
                                 persisted.string(), callback);
  if (!provisioned.ok || !provisioned.provisioned || calls != 1U) {
    throw std::runtime_error("Strict probe provisioning failed: " +
                             provisioned.error);
  }
  const auto result = loadCapabilityProfileRecordFromFile(persisted.string());
  VC_TEST_ASSERT(result.ok && result.record.hardware.deviceUuid == "device-1");
  VC_TEST_ASSERT(argv.size() == 5U && argv[1] == "--strict-fp32-smoke" &&
                 argv[2] == "--write-deployment-profile" &&
                 argv[4] == "--validate-profile");
  VC_TEST_ASSERT(std::distance(std::filesystem::directory_iterator(
                                   options.outputPath.parent_path()),
                               std::filesystem::directory_iterator{}) == 0);
}

void TestFailuresFailClosedAndCleanOutput() {
  enum class FailureMode {
    NONZERO,
    TIMEOUT,
    INVALID_OUTPUT,
    MISMATCHED_HARDWARE,
    INVALID_EVIDENCE
  };
  for (const auto mode :
       {FailureMode::NONZERO, FailureMode::TIMEOUT, FailureMode::INVALID_OUTPUT,
        FailureMode::MISMATCHED_HARDWARE, FailureMode::INVALID_EVIDENCE}) {
    const auto temp = tempDir();
    VulkanDeploymentProbeOptions options;
    options.executable = "fake-viennaps-device-probe";
    options.outputPath = temp.path / "strict-profile.json";
    options.launcher = [mode](const std::vector<std::string> &args,
                              std::chrono::milliseconds, std::string &error) {
      const auto output = std::filesystem::path(args.at(3));
      if (mode == FailureMode::NONZERO) {
        error = "fake nonzero";
        return false;
      }
      if (mode == FailureMode::TIMEOUT) {
        error = "watchdog timeout";
        return false;
      }
      if (mode == FailureMode::INVALID_OUTPUT) {
        std::ofstream invalid(output);
        invalid << "{}";
        return true;
      }
      auto mismatched = hardware();
      if (mode == FailureMode::MISMATCHED_HARDWARE)
        mismatched.deviceUuid = "other-device";
      auto candidate = validRecord(mismatched);
      if (mode == FailureMode::INVALID_EVIDENCE)
        candidate.capabilityProfile.vulkanFp32NumericalSmoke.mismatchCount = 1U;
      std::string writeError;
      return writeCapabilityProfileRecordToFile(output.string(), candidate,
                                                &writeError);
    };
    auto callback = makeVulkanDeploymentProfileProbe(options);
    CapabilityProfileRecord result = validRecord(hardware());
    std::string error;
    VC_TEST_ASSERT(!callback(hardware(), result, error));
    VC_TEST_ASSERT(!error.empty());
    VC_TEST_ASSERT(result.hardware.deviceUuid.empty());
    VC_TEST_ASSERT(std::distance(std::filesystem::directory_iterator(temp.path),
                                 std::filesystem::directory_iterator{}) == 0);
  }
}

void TestExistingOutputIsNeverOverwritten() {
  const auto temp = tempDir();
  const auto output = temp.path / "strict-profile.json";
  const auto prior = validRecord(hardware());
  std::string writeError;
  VC_TEST_ASSERT(
      writeCapabilityProfileRecordToFile(output.string(), prior, &writeError));
  std::size_t calls = 0U;
  VulkanDeploymentProbeOptions options;
  options.executable = "fake-viennaps-device-probe";
  options.outputPath = output;
  options.launcher = [&](const std::vector<std::string> &,
                         std::chrono::milliseconds, std::string &) {
    ++calls;
    return true;
  };
  auto callback = makeVulkanDeploymentProfileProbe(options);
  CapabilityProfileRecord result;
  std::string error;
  VC_TEST_ASSERT(!callback(hardware(), result, error) && calls == 0U);
  const auto after = loadCapabilityProfileRecordFromFile(output.string());
  VC_TEST_ASSERT(after.ok && after.record.hardware.deviceUuid == "device-1");

  VulkanDeploymentProbeOptions traversalOptions;
  traversalOptions.executable = "fake-viennaps-device-probe";
  traversalOptions.outputPath = temp.path / ".." / "escape.json";
  traversalOptions.launcher = options.launcher;
  auto traversalCallback = makeVulkanDeploymentProfileProbe(traversalOptions);
  VC_TEST_ASSERT(!traversalCallback(hardware(), result, error) && calls == 0U);
}

} // namespace
} // namespace viennacore

int main() {
  try {
    viennacore::TestSuccessInvokesStrictProbeOnceAndCleansOutput();
    viennacore::TestFailuresFailClosedAndCleanOutput();
    viennacore::TestExistingOutputIsNeverOverwritten();
  } catch (const std::exception &exception) {
    std::cerr << exception.what() << '\n';
    return 1;
  }
  return 0;
}
