#pragma once

#include <cstdlib>
#include <exception>
#include <filesystem>
#include <functional>
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

// A probe is deliberately synchronous: callers invoke provisioning on their
// deployment/configuration thread before worker threads are started.
using DeploymentProfileProbe = std::function<bool(
    const HardwareFingerprint &, CapabilityProfileRecord &, std::string &)>;

struct DeploymentProfileProvisionResult {
  bool ok = false;
  bool provisioned = false;
  bool callbackInvoked = false;
  DeploymentProfileDecision decision{};
  std::string error;
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

inline bool sameHardwareFingerprint(const HardwareFingerprint &left,
                                    const HardwareFingerprint &right) {
  return left.deviceUuid == right.deviceUuid &&
         left.driverUuid == right.driverUuid &&
         left.vendorId == right.vendorId && left.deviceId == right.deviceId &&
         left.deviceName == right.deviceName &&
         left.driverVersion == right.driverVersion &&
         left.driverDate == right.driverDate;
}

inline bool
hasCompleteHardwareFingerprint(const HardwareFingerprint &fingerprint) {
  return !fingerprint.deviceUuid.empty() && !fingerprint.driverUuid.empty() &&
         fingerprint.vendorId != 0U && fingerprint.deviceId != 0U &&
         !fingerprint.deviceName.empty() &&
         !fingerprint.driverVersion.empty() && !fingerprint.driverDate.empty();
}

inline void failClosed(DeploymentProfileDecision &decision,
                       const std::vector<StageWorkload> &workloads,
                       const ManualSelectionConfig &config) {
  decision.hasProfile = false;
  decision.requiresProbe = true;
  decision.plan = buildSelectionPlan(failClosedCpuProfile(), workloads, config);
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

  if (!detail::hasCompleteHardwareFingerprint(hardwareFingerprint)) {
    decision.requiresProbe = true;
    decision.state = DeploymentProfileState::INVALID;
    decision.profileReadResult.error = CapabilityProfileIOError::MISSING_FIELD;
    decision.plan =
        buildSelectionPlan(detail::failClosedCpuProfile(), workloads, config);
    return decision;
  }

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

// Resolve and, when needed, provision one deployment profile. The callback is
// evaluated at most once and only for MISSING, STALE, or INVALID state. Any
// failure leaves automatic routing fail-closed to CPU.
[[nodiscard]] inline DeploymentProfileProvisionResult
provisionDeploymentProfile(const HardwareFingerprint &hardwareFingerprint,
                           const std::vector<StageWorkload> &workloads,
                           const ManualSelectionConfig &config,
                           std::string_view configuredPath,
                           const DeploymentProfileProbe &probe) {
  DeploymentProfileProvisionResult result;
  result.decision = selectDeploymentProfile(hardwareFingerprint, workloads,
                                            config, configuredPath);
  if (!detail::hasCompleteHardwareFingerprint(hardwareFingerprint)) {
    result.error =
        "Deployment profile provisioning requires a complete hardware "
        "fingerprint.";
    detail::failClosed(result.decision, workloads, config);
    return result;
  }

  if (result.decision.state == DeploymentProfileState::VALID) {
    // Preserve the existing manual/automatic selection gate even when no
    // provisioning work is needed.
    result.ok = result.decision.plan.ok;
    if (!result.ok) {
      result.error =
          "Deployment profile is valid but selection policy rejected it.";
    }
    return result;
  }

  if (!probe) {
    result.error = "Deployment profile requires a probe callback.";
    detail::failClosed(result.decision, workloads, config);
    return result;
  }

  result.callbackInvoked = true;
  CapabilityProfileRecord candidate{};
  std::string probeError;
  bool probeOk = false;
  try {
    probeOk = probe(hardwareFingerprint, candidate, probeError);
  } catch (const std::exception &exception) {
    probeError =
        std::string("Deployment profile probe threw: ") + exception.what();
  } catch (...) {
    probeError = "Deployment profile probe threw an unknown exception.";
  }
  if (!probeOk) {
    result.error =
        probeError.empty() ? "Deployment profile probe failed." : probeError;
    detail::failClosed(result.decision, workloads, config);
    return result;
  }

  if (candidate.schemaVersion == 0U ||
      candidate.schemaVersion > kCapabilityProfileSchemaVersion) {
    result.error = "Deployment profile probe returned an unsupported schema.";
    detail::failClosed(result.decision, workloads, config);
    return result;
  }
  if (!detail::sameHardwareFingerprint(candidate.hardware,
                                       hardwareFingerprint)) {
    result.error =
        "Deployment profile probe returned a mismatched hardware fingerprint.";
    detail::failClosed(result.decision, workloads, config);
    return result;
  }

  const auto profilePath = std::filesystem::path(result.decision.profilePath);
  std::error_code directoryError;
  if (!profilePath.parent_path().empty() &&
      !std::filesystem::create_directories(profilePath.parent_path(),
                                           directoryError) &&
      directoryError) {
    result.error = "Failed to create deployment profile directory: " +
                   directoryError.message();
    detail::failClosed(result.decision, workloads, config);
    return result;
  }

  std::string writeError;
  if (!writeCapabilityProfileRecordToFile(result.decision.profilePath,
                                          candidate, &writeError)) {
    result.error = writeError.empty() ? "Failed to persist deployment profile."
                                      : writeError;
    detail::failClosed(result.decision, workloads, config);
    return result;
  }

  const auto reread =
      loadCapabilityProfileRecordFromFile(result.decision.profilePath);
  if (!reread.ok || isCapabilityProfileHardwareFingerprintStale(
                        reread.record, hardwareFingerprint)) {
    result.error = "Persisted deployment profile failed validation.";
    detail::failClosed(result.decision, workloads, config);
    return result;
  }

  result.decision.profileReadResult = reread;
  result.decision.activeRecord = reread.record;
  result.decision.hasProfile = true;
  result.decision.requiresProbe = false;
  result.decision.state = DeploymentProfileState::VALID;
  result.decision.plan =
      buildSelectionPlan(reread.record.capabilityProfile, workloads, config);
  result.ok = result.decision.plan.ok;
  if (!result.ok) {
    result.error =
        "Deployment profile was persisted but selection policy rejected it.";
  }
  result.provisioned = true;
  return result;
}

[[nodiscard]] inline DeploymentProfileProvisionResult
provisionDeploymentProfile(const HardwareFingerprint &hardwareFingerprint,
                           const std::vector<StageWorkload> &workloads,
                           const ManualSelectionConfig &config,
                           const DeploymentProfileProbe &probe) {
  return provisionDeploymentProfile(hardwareFingerprint, workloads, config, {},
                                    probe);
}

[[nodiscard]] inline DeploymentProfileProvisionResult
provisionDeploymentProfile(const HardwareFingerprint &hardwareFingerprint,
                           const std::vector<StageWorkload> &workloads,
                           const ManualSelectionConfig &config,
                           std::string_view configuredPath = {}) {
  return provisionDeploymentProfile(hardwareFingerprint, workloads, config,
                                    configuredPath, DeploymentProfileProbe{});
}

} // namespace viennaps::compute
