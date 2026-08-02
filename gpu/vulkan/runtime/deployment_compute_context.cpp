// Copyright 2026 ViennaPS
// SPDX-License-Identifier: MIT

#include "deployment_compute_context.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <iomanip>
#include <limits>
#include <sstream>
#include <string_view>
#include <vector>

namespace {

[[nodiscard]] bool fail(std::string &error, const std::string_view message) {
  error = "deployment compute context: " + std::string(message);
  return false;
}

[[nodiscard]] std::string normalizeIdentifier(const std::string_view value) {
  std::string normalized;
  normalized.reserve(value.size());
  for (const char ch : value) {
    if (ch == '-' || ch == '{' || ch == '}') {
      continue;
    }
    normalized.push_back(
        static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
  }
  return normalized;
}

[[nodiscard]] std::string uuidToHex(const std::uint8_t *bytes) {
  std::ostringstream out;
  out << std::hex << std::setfill('0');
  for (std::size_t i = 0; i < VK_UUID_SIZE; ++i) {
    out << std::setw(2) << static_cast<unsigned int>(bytes[i]);
  }
  return out.str();
}

[[nodiscard]] bool
sessionMatchesProfile(const viennaps::vulkan::runtime::ComputeSession &session,
                      const viennaps::compute::HardwareFingerprint &profile,
                      std::string &error) {
  VkPhysicalDeviceIDProperties idProperties{};
  idProperties.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES;
  VkPhysicalDeviceProperties2 properties{};
  properties.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
  properties.pNext = &idProperties;
  vkGetPhysicalDeviceProperties2(session.selection().handle, &properties);
  const bool identityMatches =
      normalizeIdentifier(uuidToHex(idProperties.deviceUUID)) ==
          normalizeIdentifier(profile.deviceUuid) &&
      normalizeIdentifier(uuidToHex(idProperties.driverUUID)) ==
          normalizeIdentifier(profile.driverUuid) &&
      properties.properties.vendorID == profile.vendorId &&
      properties.properties.deviceID == profile.deviceId &&
      properties.properties.deviceName == profile.deviceName &&
      std::to_string(properties.properties.driverVersion) ==
          profile.driverVersion;
  return identityMatches ||
         fail(error,
              "selected Vulkan device does not match the active profile");
}

} // namespace

namespace viennaps::vulkan::runtime {

bool DeploymentComputeContext::prepare(
    const compute::HardwareFingerprint &currentHardware,
    const std::span<const compute::StageWorkload> workloads,
    const compute::ManualSelectionConfig &selectionConfig,
    const std::string_view configuredProfilePath, std::string &error,
    const ComputeSessionOptions &manualDevice) {
  error.clear();
  if (prepared_) {
    return true;
  }
  reset();

  const std::vector<compute::StageWorkload> workloadVector(workloads.begin(),
                                                           workloads.end());
  const auto resolvedDecision = compute::selectDeploymentProfile(
      currentHardware, workloadVector, selectionConfig, configuredProfilePath);
  return prepare(resolvedDecision, currentHardware, workloads, selectionConfig,
                 error, manualDevice);
}

bool DeploymentComputeContext::prepare(
    const compute::DeploymentProfileDecision &resolvedDecision,
    const compute::HardwareFingerprint &currentHardware,
    const std::span<const compute::StageWorkload> workloads,
    const compute::ManualSelectionConfig &selectionConfig, std::string &error,
    const ComputeSessionOptions &manualDevice) {
  error.clear();
  if (prepared_) {
    return true;
  }
  reset();

  decision_ = resolvedDecision;
  const std::vector<compute::StageWorkload> workloadVector(workloads.begin(),
                                                           workloads.end());
  if (decision_.plan.stages.size() != workloadVector.size()) {
    error = "deployment compute context: resolved selection plan does not "
            "match the supplied workload count";
    return false;
  }
  for (std::size_t index = 0; index < workloadVector.size(); ++index) {
    if (decision_.plan.stages[index].stage != workloadVector[index].stage) {
      error = "deployment compute context: resolved selection plan does not "
              "match the supplied workload stage";
      return false;
    }
  }

  // A supplied decision is authoritative for automatic routing, but a caller's
  // explicit manual override must still be evaluated against the persisted
  // capability record. This recomputation is in-memory and performs no I/O.
  if (selectionConfig.selectionMode == compute::SelectionMode::MANUAL) {
    const auto requestedBackend =
        workloadVector.size() == 1U
            ? selectionConfig.perStageBackend
                  .at(static_cast<std::size_t>(workloadVector.front().stage))
                  .value_or(selectionConfig.globalBackend)
            : selectionConfig.globalBackend;
    if (requestedBackend == compute::ComputeBackend::CPU) {
      decision_.plan =
          compute::buildSelectionPlan(compute::detail::failClosedCpuProfile(),
                                      workloadVector, selectionConfig);
    } else if (decision_.state == compute::DeploymentProfileState::VALID &&
               decision_.hasProfile) {
      decision_.plan =
          compute::buildSelectionPlan(decision_.activeRecord.capabilityProfile,
                                      workloadVector, selectionConfig);
    }
  }

  if (!decision_.plan.ok) {
    session_.reset();
    prepared_ = false;
    error = "deployment compute context: backend selection plan is not "
            "executable";
    for (const auto &stage : decision_.plan.stages) {
      for (const auto &reason : stage.rejectionReasons) {
        error += " [" + std::string(compute::toString(stage.stage)) + ": " +
                 reason + "]";
      }
    }
    return false;
  }

  const bool requiresVulkan =
      std::ranges::any_of(decision_.plan.stages, [](const auto &stage) {
        return stage.selected &&
               stage.selectedBackend == compute::ComputeBackend::VULKAN;
      });
  if (requiresVulkan) {
    if (decision_.state != compute::DeploymentProfileState::VALID ||
        !decision_.hasProfile ||
        compute::isCapabilityProfileHardwareFingerprintStale(
            decision_.activeRecord, currentHardware)) {
      session_.reset();
      prepared_ = false;
      return fail(error,
                  "Vulkan was selected without a valid matching deployment "
                  "profile");
    }

    ComputeSessionOptions deviceOptions = manualDevice;
    const bool hasManualDevice =
        deviceOptions.manualDeviceIndex !=
            std::numeric_limits<std::uint32_t>::max() ||
        !deviceOptions.manualDeviceName.empty() ||
        !deviceOptions.manualDeviceUuid.empty();
    if (!hasManualDevice) {
      deviceOptions.manualDeviceUuid =
          decision_.activeRecord.hardware.deviceUuid;
    }
    if (!session_.initialize(error, deviceOptions)) {
      session_.reset();
      prepared_ = false;
      return false;
    }
    if (!sessionMatchesProfile(session_, decision_.activeRecord.hardware,
                               error)) {
      session_.reset();
      prepared_ = false;
      return false;
    }
  }

  prepared_ = true;
  return true;
}

void DeploymentComputeContext::reset() {
  session_.reset();
  decision_ = {};
  prepared_ = false;
}

bool DeploymentComputeContext::isPrepared() const { return prepared_; }

bool DeploymentComputeContext::hasVulkanSession() const {
  return prepared_ && session_.isValid();
}

compute::ComputeBackend
DeploymentComputeContext::backendFor(const compute::Stage stage) const {
  if (!prepared_) {
    return compute::ComputeBackend::CPU;
  }
  const auto selected =
      std::ranges::find_if(decision_.plan.stages, [stage](const auto &entry) {
        return entry.stage == stage && entry.ok && entry.selected;
      });
  return selected == decision_.plan.stages.end() ? compute::ComputeBackend::CPU
                                                 : selected->selectedBackend;
}

const compute::DeploymentProfileDecision &
DeploymentComputeContext::decision() const {
  return decision_;
}

ComputeSession *DeploymentComputeContext::session() {
  return hasVulkanSession() ? &session_ : nullptr;
}

const ComputeSession *DeploymentComputeContext::session() const {
  return hasVulkanSession() ? &session_ : nullptr;
}

} // namespace viennaps::vulkan::runtime
