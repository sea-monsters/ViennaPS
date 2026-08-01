#pragma once

#include <cstdlib>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "backendPolicy.hpp"
#include "capabilityProfileIO.hpp"

namespace viennaps::compute {

constexpr std::string_view kDeploymentProfileDirEnvVar =
    "VIENNAPS_DEVICE_PROFILE_DIR";
constexpr std::string_view kDeploymentProfileDefaultDir =
    ".viennaps-device-profiles";

enum class DeploymentProfileState {
  MISSING,
  STALE,
  INVALID,
  VALID,
};

struct DeploymentProfileDecision {
  bool requiresProbe = false;
  bool hasProfile = false;
  DeploymentProfileState state = DeploymentProfileState::MISSING;
  CapabilityProfileIOResult profileReadResult{};
  CapabilityProfileRecord activeRecord{};
  std::string profilePath;
  SelectionPlan plan;
};

namespace detail {

inline std::string getEnvProfileDir() {
#ifdef _WIN32
  char *envValue = nullptr;
  std::size_t envLength = 0;
  if (_dupenv_s(&envValue, &envLength,
                std::string(kDeploymentProfileDirEnvVar).c_str()) != 0 ||
      envValue == nullptr) {
    return {};
  }
  const std::string result(envValue);
  std::free(envValue);
  return result;
#else
  const char *envValue =
      std::getenv(std::string(kDeploymentProfileDirEnvVar).c_str());
  if (envValue == nullptr) {
    return {};
  }
  return envValue;
#endif
}

inline CapabilityProfile failClosedCpuProfile() {
  CapabilityProfile fallback;
  fallback.cpuAvailable = true;
  return fallback;
}

} // namespace detail

[[nodiscard]] inline std::filesystem::path
resolveDeploymentProfileDirectory(std::string_view configuredPath) {
  if (!configuredPath.empty()) {
    return std::filesystem::path(configuredPath);
  }

  const auto envDir = detail::getEnvProfileDir();
  if (!envDir.empty()) {
    return std::filesystem::path(envDir);
  }

  return std::filesystem::path(std::string(kDeploymentProfileDefaultDir));
}

[[nodiscard]] inline std::filesystem::path
resolveDeploymentProfilePath(std::string_view configuredPath,
                             const HardwareFingerprint &hardwareFingerprint) {
  const auto basePath = resolveDeploymentProfileDirectory(configuredPath);
  if (basePath.extension() == ".json") {
    return basePath;
  }
  const std::string fileName = hardwareFingerprint.deviceUuid.empty()
                                   ? std::string("unknown-device")
                                   : hardwareFingerprint.deviceUuid;
  return basePath / (fileName + ".json");
}

[[nodiscard]] inline DeploymentProfileDecision
selectDeploymentProfile(const HardwareFingerprint &hardwareFingerprint,
                        const std::vector<StageWorkload> &workloads,
                        const ManualSelectionConfig &config,
                        std::string_view configuredPath = {}) {
  DeploymentProfileDecision decision;
  decision.profilePath =
      resolveDeploymentProfilePath(configuredPath, hardwareFingerprint)
          .string();
  decision.profileReadResult =
      loadCapabilityProfileRecordFromFile(decision.profilePath);

  CapabilityProfile capabilityProfile = detail::failClosedCpuProfile();
  if (decision.profileReadResult.ok) {
    if (isCapabilityProfileHardwareFingerprintStale(
            decision.profileReadResult.record, hardwareFingerprint)) {
      decision.requiresProbe = true;
      decision.state = DeploymentProfileState::STALE;
    } else {
      decision.hasProfile = true;
      decision.state = DeploymentProfileState::VALID;
      decision.activeRecord = decision.profileReadResult.record;
      capabilityProfile = decision.activeRecord.capabilityProfile;
    }
  } else {
    decision.requiresProbe = true;
    if (decision.profileReadResult.error ==
            CapabilityProfileIOError::SCHEMA_MISMATCH ||
        decision.profileReadResult.error ==
            CapabilityProfileIOError::TYPE_MISMATCH ||
        decision.profileReadResult.error ==
            CapabilityProfileIOError::JSON_SYNTAX_ERROR ||
        decision.profileReadResult.error ==
            CapabilityProfileIOError::MISSING_FIELD) {
      decision.state = DeploymentProfileState::INVALID;
    } else {
      decision.state = DeploymentProfileState::MISSING;
    }
  }

  decision.plan = buildSelectionPlan(capabilityProfile, workloads, config);
  return decision;
}

} // namespace viennaps::compute
