// Copyright 2026 ViennaPS
// SPDX-License-Identifier: MIT
//
// Deployment-time composition for the Vulkan level-set controller. A session
// provisions the selected hardware once, then configures simulation processes
// exclusively from the cached deployment decision.

#pragma once

#include "../runtime/deployment_hardware_fingerprint.hpp"
#include "levelset_process_controller.hpp"

#include <cstddef>
#include <cstdlib>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace viennaps::vulkan::levelset {

template <int D> class LevelSetDeploymentSession {
public:
  using Controller = LevelSetProcessController<D>;
  using ProcessType = typename Controller::ProcessType;

  struct Request {
    compute::ManualSelectionConfig selection{};
    compute::StageWorkload workload{};
    runtime::ComputeSessionOptions manualDevice{};
    std::string levelSetSpirvPath;
    std::string configuredProfilePath;
    typename Controller::RebuildSpirvPaths rebuildSpirvPaths{};
  };

  struct ProvisionResult {
    bool ok = false;
    bool cached = false;
    bool reused = false;
    bool manualCpuBypass = false;
    bool collectorInvoked = false;
    bool probeInvoked = false;
    compute::HardwareFingerprint hardware{};
    compute::DeploymentProfileDecision decision{};
    std::string error;
  };

  // Provisions deployment state once. The caller must explicitly reset this
  // object before changing selection, workload, device, or probe settings.
  [[nodiscard]] ProvisionResult provision(const Request &request,
                                          std::string &error) {
    return provision(request, {}, {}, error);
  }

  [[nodiscard]] ProvisionResult
  provision(const Request &request,
            compute::VulkanDeploymentBootstrapOptions bootstrapOptions,
            runtime::VulkanDeploymentHardwareCollectorOptions hardwareOptions,
            std::string &error) {
    error.clear();
    if (state_.has_value()) {
      auto result = state_->provision;
      result.reused = true;
      error = result.error;
      return result;
    }

    if (bootstrapOptions.profilePath.empty()) {
      bootstrapOptions.profilePath = request.configuredProfilePath;
    }
    bootstrapOptions.probeExecutable = resolveProbeExecutable(
        std::move(bootstrapOptions.probeExecutable));
    const auto resolvedProbeExecutable = bootstrapOptions.probeExecutable;

    const std::vector<compute::StageWorkload> workloads{request.workload};
    auto bootstrap = runtime::bootstrapVulkanDeploymentForSelectedDevice(
        workloads, request.selection, std::move(bootstrapOptions),
        std::move(hardwareOptions));

    ProvisionResult result;
    result.ok = bootstrap.ok;
    result.cached = true;
    result.manualCpuBypass = bootstrap.manualCpuBypass;
    result.collectorInvoked = bootstrap.collectorInvoked;
    result.probeInvoked = bootstrap.probeInvoked;
    result.hardware = bootstrap.hardware;
    result.decision = bootstrap.provisioning.decision;
    result.error = bootstrap.error;
    state_ = State{request, result, resolvedProbeExecutable};
    error = result.error;
    return result;
  }

  // This operation is cache-only: it never re-reads a profile or invokes a
  // hardware collector/probe. Re-provision explicitly after configuration
  // changes.
  [[nodiscard]] typename Controller::Result configure(ProcessType &process) {
    if (!state_.has_value()) {
      typename Controller::Result result;
      result.message =
          "Vulkan level-set deployment must be provisioned before configure.";
      return result;
    }
    const auto &state = *state_;
    return controller_.configureResolved(
        process, state.request.selection, state.provision.hardware,
        state.request.workload, state.provision.decision,
        state.request.manualDevice, state.request.levelSetSpirvPath,
        state.request.configuredProfilePath, state.request.rebuildSpirvPaths);
  }

  void resetDeployment() { state_.reset(); }

  [[nodiscard]] bool isProvisioned() const { return state_.has_value(); }

  [[nodiscard]] std::string probeExecutable() const {
    return state_.has_value() ? state_->probeExecutable : std::string{};
  }

private:
  struct State {
    Request request;
    ProvisionResult provision;
    std::string probeExecutable;
  };

  [[nodiscard]] static std::string
  resolveProbeExecutable(std::string configuredPath) {
    if (!configuredPath.empty()) {
      return configuredPath;
    }
#ifdef _WIN32
    char *fromEnvironment = nullptr;
    std::size_t length = 0U;
    if (_dupenv_s(&fromEnvironment, &length, "VIENNAPS_DEVICE_PROBE_PATH") !=
            0 ||
        fromEnvironment == nullptr) {
      return {};
    }
    std::string resolved(fromEnvironment);
    std::free(fromEnvironment);
    return resolved;
#else
    const char *fromEnvironment = std::getenv("VIENNAPS_DEVICE_PROBE_PATH");
    if (fromEnvironment != nullptr && fromEnvironment[0] != '\0') {
      return fromEnvironment;
    }
    return {};
#endif
  }

  Controller controller_{};
  std::optional<State> state_{};
};

} // namespace viennaps::vulkan::levelset
