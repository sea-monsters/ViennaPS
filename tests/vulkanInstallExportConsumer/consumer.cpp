#include <chrono>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

#include <compute/capabilityProfileIO.hpp>
#include <compute/deploymentProfile.hpp>
#include <compute_session.hpp>

int main() {
  viennaps::vulkan::runtime::ComputeSession session;
  if (session.isValid() || session.generation() != 0U)
    return 1;

  std::string error;
  if (!error.empty())
    return 2;

  using namespace viennaps::compute;
  const auto tempRoot = std::filesystem::temp_directory_path() /
                        ("viennaps-installed-profile-" +
                         std::to_string(std::chrono::steady_clock::now()
                                            .time_since_epoch()
                                            .count()));
  std::error_code cleanupError;
  struct Cleanup {
    std::filesystem::path path;
    ~Cleanup() {
      std::error_code ignored;
      std::filesystem::remove_all(path, ignored);
    }
  } cleanup{tempRoot};
  std::filesystem::create_directories(tempRoot, cleanupError);
  if (cleanupError)
    return 3;

  HardwareFingerprint fingerprint{};
  fingerprint.deviceUuid = "installed-consumer-device";
  fingerprint.driverUuid = "installed-consumer-driver";
  fingerprint.vendorId = 0x8086U;
  fingerprint.deviceId = 0x1234U;
  fingerprint.deviceName = "installed-consumer-adapter";
  fingerprint.driverVersion = "1.0";
  fingerprint.driverDate = "2026-08-18";

  const std::vector<StageWorkload> workloads = {
      StageWorkload{Stage::LEVEL_SET, Precision::FP32, 4096U, false,
                    RayMode::NONE, true}};
  const auto profilePath = tempRoot / "profile.json";
  CapabilityProfileRecord record{};
  record.recordedAt = "2026-08-18T00:00:00Z";
  record.hardware = fingerprint;
  record.capabilityProfile.cpuAvailable = true;
  if (!writeCapabilityProfileRecordToFile(profilePath.string(), record,
                                          &error))
    return 4;

  const auto loaded = loadCapabilityProfileRecordFromFile(profilePath.string());
  if (!loaded.ok ||
      isCapabilityProfileHardwareFingerprintStale(loaded.record, fingerprint))
    return 5;

  const auto selected =
      selectDeploymentProfile(fingerprint, workloads, ManualSelectionConfig{},
                              profilePath.string());
  if (!selected.hasProfile || selected.state != DeploymentProfileState::VALID ||
      !selected.plan.ok)
    return 6;

  const auto provisionPath = tempRoot / "provisioned";
  const auto provisioned = provisionDeploymentProfile(
      fingerprint, workloads, ManualSelectionConfig{}, provisionPath.string(),
      [](const HardwareFingerprint &expected, CapabilityProfileRecord &out,
         std::string &) {
        out = CapabilityProfileRecord{};
        out.recordedAt = "2026-08-18T00:00:01Z";
        out.hardware = expected;
        out.capabilityProfile.cpuAvailable = true;
        return true;
      });
  if (!provisioned.ok || !provisioned.provisioned ||
      !provisioned.callbackInvoked ||
      provisioned.decision.state != DeploymentProfileState::VALID ||
      !std::filesystem::exists(provisioned.decision.profilePath))
    return 7;

  auto stale = fingerprint;
  stale.driverVersion += "-stale";
  const auto staleDecision = selectDeploymentProfile(
      stale, workloads, ManualSelectionConfig{}, provisionPath.string());
  if (staleDecision.state != DeploymentProfileState::STALE ||
      !staleDecision.requiresProbe)
    return 8;

  std::cout << "PD5 Vulkan install consumer profile persistence PASS\n";
  return 0;
}
