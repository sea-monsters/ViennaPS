#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>

#include <compute/deploymentProfile.hpp>
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

TempDir makeTempDir() {
  static std::uint64_t sequence = 0U;
  const auto tick = std::chrono::steady_clock::now().time_since_epoch().count();
  TempDir out{std::filesystem::temp_directory_path() /
              ("viennaps-deployment-profile-" + std::to_string(tick) + "-" +
               std::to_string(sequence++))};
  std::filesystem::create_directories(out.path);
  return out;
}

HardwareFingerprint hardware() {
  return {"device-1", "driver-1", 1U, 2U, "fixture", "1.0", "2026"};
}

CapabilityProfileRecord record(const HardwareFingerprint &fingerprint) {
  CapabilityProfileRecord out;
  out.recordedAt = "2026-08-02T00:00:00Z";
  out.hardware = fingerprint;
  out.capabilityProfile.cpuAvailable = true;
  out.capabilityProfile.vulkanAvailable = true;
  out.capabilityProfile.vulkanCompute = true;
  out.capabilityProfile.vulkanPrimitiveSuitePass = true;
  return out;
}

std::vector<StageWorkload> workloads() {
  return {
      {Stage::LEVEL_SET, Precision::FP32, 128U, false, RayMode::NONE, true}};
}

void write(const std::filesystem::path &path,
           const CapabilityProfileRecord &profile) {
  std::filesystem::create_directories(path.parent_path());
  std::string error;
  VC_TEST_ASSERT(
      writeCapabilityProfileRecordToFile(path.string(), profile, &error));
}

void TestValidReuseDoesNotProbe() {
  const auto temp = makeTempDir();
  const auto path = temp.path / "device-1.json";
  write(path, record(hardware()));
  int calls = 0;
  const auto result = provisionDeploymentProfile(
      hardware(), workloads(), ManualSelectionConfig{}, path.string(),
      [&](const HardwareFingerprint &, CapabilityProfileRecord &,
          std::string &) {
        ++calls;
        return true;
      });
  VC_TEST_ASSERT(result.ok && !result.provisioned && !result.callbackInvoked);
  VC_TEST_ASSERT(calls == 0);
}

void TestMissingProvisionRereadsAndCleansTemporary() {
  const auto temp = makeTempDir();
  const auto path = temp.path / "nested" / "device-1.json";
  int calls = 0;
  const auto result = provisionDeploymentProfile(
      hardware(), workloads(), ManualSelectionConfig{}, path.string(),
      [&](const HardwareFingerprint &fingerprint, CapabilityProfileRecord &out,
          std::string &) {
        ++calls;
        out = record(fingerprint);
        return true;
      });
  VC_TEST_ASSERT(result.ok && result.provisioned && result.callbackInvoked);
  VC_TEST_ASSERT(calls == 1 &&
                 result.decision.state == DeploymentProfileState::VALID);
  VC_TEST_ASSERT(loadCapabilityProfileRecordFromFile(path.string()).ok);
  VC_TEST_ASSERT(
      std::distance(std::filesystem::directory_iterator(path.parent_path()),
                    std::filesystem::directory_iterator{}) == 1);
}

void TestFailuresFailClosedAndMismatchDoesNotProbeTwice() {
  const auto temp = makeTempDir();
  const auto path = temp.path / "device-1.json";
  int calls = 0;
  const auto failed = provisionDeploymentProfile(
      hardware(), workloads(), ManualSelectionConfig{}, path.string(),
      [&](const HardwareFingerprint &, CapabilityProfileRecord &,
          std::string &error) {
        ++calls;
        error = "probe failure";
        return false;
      });
  VC_TEST_ASSERT(!failed.ok && failed.decision.plan.ok && calls == 1);
  const auto noCallback = provisionDeploymentProfile(
      hardware(), workloads(), ManualSelectionConfig{}, path.string());
  VC_TEST_ASSERT(!noCallback.ok && !noCallback.callbackInvoked);

  const auto mismatch = provisionDeploymentProfile(
      hardware(), workloads(), ManualSelectionConfig{}, path.string(),
      [&](const HardwareFingerprint &, CapabilityProfileRecord &out,
          std::string &) {
        ++calls;
        out = record(hardware());
        out.hardware.driverUuid = "other";
        return true;
      });
  VC_TEST_ASSERT(!mismatch.ok && mismatch.callbackInvoked && calls == 2);
}

void TestStaleAndInvalidProfilesProbeExactlyOnce() {
  const auto temp = makeTempDir();
  const auto stalePath = temp.path / "stale.json";
  auto stale = hardware();
  stale.driverVersion = "old";
  write(stalePath, record(stale));

  int staleCalls = 0;
  const auto staleResult = provisionDeploymentProfile(
      hardware(), workloads(), ManualSelectionConfig{}, stalePath.string(),
      [&](const HardwareFingerprint &fingerprint, CapabilityProfileRecord &out,
          std::string &) {
        ++staleCalls;
        out = record(fingerprint);
        return true;
      });
  VC_TEST_ASSERT(staleResult.ok && staleResult.provisioned &&
                 staleResult.callbackInvoked && staleCalls == 1);

  const auto invalidPath = temp.path / "invalid.json";
  std::ofstream invalidFile(invalidPath);
  invalidFile << "{";
  invalidFile.close();
  int invalidCalls = 0;
  const auto invalidResult = provisionDeploymentProfile(
      hardware(), workloads(), ManualSelectionConfig{}, invalidPath.string(),
      [&](const HardwareFingerprint &fingerprint, CapabilityProfileRecord &out,
          std::string &) {
        ++invalidCalls;
        out = record(fingerprint);
        return true;
      });
  VC_TEST_ASSERT(invalidResult.ok && invalidResult.provisioned &&
                 invalidResult.callbackInvoked && invalidCalls == 1);
}

void TestIncompleteHardwareFailsClosedBeforeUnknownProfileReuse() {
  const auto temp = makeTempDir();
  auto incomplete = hardware();
  incomplete.deviceUuid.clear();
  const auto unknownProfilePath = temp.path / "unknown-device.json";
  write(unknownProfilePath, record(incomplete));

  int calls = 0;
  const auto result = provisionDeploymentProfile(
      incomplete, workloads(), ManualSelectionConfig{}, temp.path.string(),
      [&](const HardwareFingerprint &fingerprint, CapabilityProfileRecord &out,
          std::string &) {
        ++calls;
        out = record(fingerprint);
        return true;
      });
  const auto after =
      loadCapabilityProfileRecordFromFile(unknownProfilePath.string());
  VC_TEST_ASSERT(!result.ok && !result.callbackInvoked && calls == 0);
  VC_TEST_ASSERT(result.decision.state == DeploymentProfileState::INVALID &&
                 !result.decision.hasProfile && result.decision.requiresProbe);
  VC_TEST_ASSERT(after.ok);
}

void TestThrowAndPriorTargetPreservedOnWriteFailure() {
  const auto temp = makeTempDir();
  const auto path = temp.path / "device-1.json";
  write(path, record(hardware()));
  HardwareFingerprint stale = hardware();
  stale.driverVersion = "old";
  write(path, record(stale));
  const auto before = loadCapabilityProfileRecordFromFile(path.string());
  const auto thrown = provisionDeploymentProfile(
      hardware(), workloads(), ManualSelectionConfig{}, path.string(),
      [&](const HardwareFingerprint &, CapabilityProfileRecord &,
          std::string &) -> bool { throw std::runtime_error("boom"); });
  const auto after = loadCapabilityProfileRecordFromFile(path.string());
  VC_TEST_ASSERT(!thrown.ok && after.ok &&
                 after.record.hardware.driverVersion ==
                     before.record.hardware.driverVersion);
  VC_TEST_ASSERT(before.ok);

  const auto directoryTarget = temp.path / "target.json";
  std::filesystem::create_directory(directoryTarget);
  const auto failedWrite = provisionDeploymentProfile(
      hardware(), workloads(), ManualSelectionConfig{},
      directoryTarget.string(),
      [&](const HardwareFingerprint &fingerprint, CapabilityProfileRecord &out,
          std::string &) {
        out = record(fingerprint);
        return true;
      });
  VC_TEST_ASSERT(!failedWrite.ok &&
                 std::filesystem::is_directory(directoryTarget));
  VC_TEST_ASSERT(std::distance(std::filesystem::directory_iterator(temp.path),
                               std::filesystem::directory_iterator{}) == 2);
}

} // namespace
} // namespace viennacore

int main() {
  viennacore::TestValidReuseDoesNotProbe();
  viennacore::TestMissingProvisionRereadsAndCleansTemporary();
  viennacore::TestFailuresFailClosedAndMismatchDoesNotProbeTwice();
  viennacore::TestStaleAndInvalidProfilesProbeExactlyOnce();
  viennacore::TestIncompleteHardwareFailsClosedBeforeUnknownProfileReuse();
  viennacore::TestThrowAndPriorTargetPreservedOnWriteFailure();
  return 0;
}
