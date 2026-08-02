#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <functional>
#include <limits>
#include <string>
#include <vector>

#include "deploymentProfile.hpp"
#include "vulkanDeploymentProbe.hpp"

namespace viennaps::compute {

using VulkanDeploymentHardwareCollector =
    std::function<bool(HardwareFingerprint &, std::uint32_t &, std::string &)>;

struct VulkanDeploymentBootstrapOptions {
  std::string profilePath;
  std::string probeExecutable;
  std::filesystem::path transientOutputDirectory;
  std::chrono::milliseconds watchdog = std::chrono::milliseconds(60'000);
  VulkanDeploymentHardwareCollector collector;
  VulkanProbeProcessLauncher launcher;
};

struct VulkanDeploymentBootstrapResult {
  bool ok = false;
  bool collectorInvoked = false;
  bool probeInvoked = false;
  bool manualCpuBypass = false;
  HardwareFingerprint hardware{};
  DeploymentProfileProvisionResult provisioning{};
  std::string error;
};

namespace detail {

inline bool
isManualCpuConfiguration(const std::vector<StageWorkload> &workloads,
                         const ManualSelectionConfig &config) {
  if (config.selectionMode != SelectionMode::MANUAL) {
    return false;
  }
  for (const auto &workload : workloads) {
    const auto requested =
        config.perStageBackend.at(static_cast<std::size_t>(workload.stage))
            .value_or(config.globalBackend);
    if (requested != ComputeBackend::CPU) {
      return false;
    }
  }
  return true;
}

inline std::filesystem::path
uniqueBootstrapOutput(const std::filesystem::path &directory) {
  static std::atomic<std::uint64_t> sequence{0U};
  const auto tick = std::chrono::steady_clock::now().time_since_epoch().count();
  const auto id = sequence.fetch_add(1U, std::memory_order_relaxed);
  return directory / ("viennaps-probe-" + std::to_string(tick) + "-" +
                      std::to_string(id) + ".json");
}

inline DeploymentProfileProvisionResult
manualCpuProvision(const std::vector<StageWorkload> &workloads,
                   const ManualSelectionConfig &config) {
  DeploymentProfileProvisionResult result;
  CapabilityProfile cpu;
  cpu.cpuAvailable = true;
  result.decision.plan = buildSelectionPlan(cpu, workloads, config);
  result.decision.state = DeploymentProfileState::VALID;
  result.decision.hasProfile = true;
  result.ok = result.decision.plan.ok;
  if (!result.ok)
    result.error = "Manual CPU selection was rejected by policy.";
  return result;
}

} // namespace detail

[[nodiscard]] inline VulkanDeploymentBootstrapResult
bootstrapVulkanDeploymentProfile(const std::vector<StageWorkload> &workloads,
                                 const ManualSelectionConfig &config,
                                 VulkanDeploymentBootstrapOptions options) {
  VulkanDeploymentBootstrapResult result;
  if (detail::isManualCpuConfiguration(workloads, config)) {
    result.manualCpuBypass = true;
    result.provisioning = detail::manualCpuProvision(workloads, config);
    result.ok = result.provisioning.ok;
    result.error = result.provisioning.error;
    return result;
  }

  if (!options.collector) {
    result.error = "Vulkan deployment hardware collector is unavailable.";
    result.provisioning.decision.plan =
        buildSelectionPlan(detail::failClosedCpuProfile(), workloads, config);
    result.provisioning.error = result.error;
    return result;
  }
  result.collectorInvoked = true;
  std::uint32_t physicalDeviceIndex = std::numeric_limits<std::uint32_t>::max();
  std::string collectorError;
  try {
    if (!options.collector(result.hardware, physicalDeviceIndex,
                           collectorError)) {
      result.error = collectorError.empty()
                         ? "Vulkan hardware collection failed."
                         : collectorError;
      result.provisioning.decision.plan =
          buildSelectionPlan(detail::failClosedCpuProfile(), workloads, config);
      result.provisioning.error = result.error;
      return result;
    }
  } catch (const std::exception &exception) {
    result.error =
        std::string("Vulkan hardware collector threw: ") + exception.what();
    result.provisioning.decision.plan =
        buildSelectionPlan(detail::failClosedCpuProfile(), workloads, config);
    result.provisioning.error = result.error;
    return result;
  } catch (...) {
    result.error = "Vulkan hardware collector threw an unknown exception.";
    result.provisioning.decision.plan =
        buildSelectionPlan(detail::failClosedCpuProfile(), workloads, config);
    result.provisioning.error = result.error;
    return result;
  }
  std::error_code ec;
  if (options.transientOutputDirectory.empty() ||
      !options.transientOutputDirectory.is_absolute()) {
    result.error = "Vulkan transient output directory must be absolute.";
    result.provisioning.decision.plan =
        buildSelectionPlan(detail::failClosedCpuProfile(), workloads, config);
    result.provisioning.error = result.error;
    return result;
  }
  const auto transientDirectory =
      std::filesystem::weakly_canonical(options.transientOutputDirectory, ec);
  if (ec || transientDirectory.empty() ||
      !std::filesystem::is_directory(transientDirectory, ec) || ec) {
    result.error = "Vulkan transient output directory is unavailable.";
    result.provisioning.decision.plan =
        buildSelectionPlan(detail::failClosedCpuProfile(), workloads, config);
    result.provisioning.error = result.error;
    return result;
  }
  const auto outputPath = detail::uniqueBootstrapOutput(transientDirectory);
  if (outputPath.empty()) {
    result.error = "Vulkan transient output path could not be generated.";
    result.provisioning.decision.plan =
        buildSelectionPlan(detail::failClosedCpuProfile(), workloads, config);
    result.provisioning.error = result.error;
    return result;
  }

  VulkanDeploymentProbeOptions probeOptions;
  probeOptions.executable = std::move(options.probeExecutable);
  probeOptions.outputPath = outputPath;
  if (physicalDeviceIndex != std::numeric_limits<std::uint32_t>::max()) {
    probeOptions.strictFp32DeviceIndex = physicalDeviceIndex;
  }
  probeOptions.watchdog = options.watchdog;
  probeOptions.launcher = std::move(options.launcher);
  auto probe = makeVulkanDeploymentProfileProbe(std::move(probeOptions));
  result.provisioning = provisionDeploymentProfile(
      result.hardware, workloads, config, options.profilePath, probe);
  result.probeInvoked = result.provisioning.callbackInvoked;
  result.ok = result.provisioning.ok;
  result.error = result.provisioning.error;
  return result;
}

} // namespace viennaps::compute
