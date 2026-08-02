// Copyright 2026 ViennaPS
// SPDX-License-Identifier: MIT
//
// Deployment-time composition for the FP32 Process coverage and
// surface-diffusion executor seams. This header is intentionally GPU-side;
// it must not be included by ViennaPS core headers.

#pragma once

#include "coverage_delta_executor.hpp"
#include "surface_diffusion_executor.hpp"

#include "../runtime/deployment_compute_context.hpp"

#include <compute/backendPolicy.hpp>
#include <compute/deploymentProfile.hpp>
#include <process/psProcess.hpp>

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>

namespace viennaps::vulkan::surface {

/// Owns cache-only deployment state for the two FP32 Process surface seams.
/// The binding never reads a profile or invokes a hardware probe.
template <int D> class ProcessDeploymentBinding {
public:
  using ProcessType = viennaps::Process<float, D>;

  struct SpirvPaths {
    std::string coverageDelta;
    std::string surfaceDiffusion;
  };
  using ShaderPaths = SpirvPaths;

  struct Result {
    bool ok = false;
    bool prepared = false;
    bool degraded = false;
    bool coverageVulkan = false;
    bool surfaceDiffusionVulkan = false;
    std::uint64_t sessionGeneration = 0U;
    VkDevice device = VK_NULL_HANDLE;
    std::string message;
  };

  ~ProcessDeploymentBinding() = default;
  ProcessDeploymentBinding() = default;
  ProcessDeploymentBinding(const ProcessDeploymentBinding &) = delete;
  ProcessDeploymentBinding &operator=(const ProcessDeploymentBinding &) = delete;

  [[nodiscard]] Result configure(
      ProcessType &process,
      const compute::DeploymentProfileDecision &resolvedDecision,
      const compute::HardwareFingerprint &currentHardware,
      std::span<const compute::StageWorkload> workloads,
      const compute::ManualSelectionConfig &selection,
      const runtime::ComputeSessionOptions &manualDevice,
      const SpirvPaths &paths) {
    clear(process);

    Result result;
    for (const auto &workload : workloads) {
      if (workload.stage != compute::Stage::COVERAGE &&
          workload.stage != compute::Stage::SURFACE_DIFFUSION) {
        result.message =
            "Process surface binding supports only COVERAGE and "
            "SURFACE_DIFFUSION workloads";
        return result;
      }
    }
    auto context = std::make_shared<runtime::DeploymentComputeContext>();
    std::string error;
    if (!context->prepare(resolvedDecision, currentHardware, workloads,
                          selection, error, manualDevice)) {
      result.message = error;
      return result;
    }
    result.prepared = true;

    if (selection.selectionMode == compute::SelectionMode::MANUAL) {
      for (const auto &workload : workloads) {
        const auto index = static_cast<std::size_t>(workload.stage);
        const auto requested = selection.perStageBackend.at(index).value_or(
            selection.globalBackend);
        if (requested != compute::ComputeBackend::AUTO &&
            requested != context->backendFor(workload.stage)) {
          result.message =
              "manual selection does not match the resolved stage plan";
          result.prepared = false;
          return result;
        }
      }
    }

    const bool coverageVulkan =
        context->backendFor(compute::Stage::COVERAGE) ==
        compute::ComputeBackend::VULKAN;
    const bool surfaceVulkan =
        context->backendFor(compute::Stage::SURFACE_DIFFUSION) ==
        compute::ComputeBackend::VULKAN;
    result.coverageVulkan = coverageVulkan;
    result.surfaceDiffusionVulkan = surfaceVulkan;

    auto *session = context->session();
    if (session != nullptr) {
      result.sessionGeneration = session->generation();
      result.device = session->deviceHandle();
    }
    if (!coverageVulkan && !surfaceVulkan) {
      context_ = context;
      result.ok = true;
      return result;
    }
    if (session == nullptr) {
      result.message = "Vulkan stage selected without a deployment session";
      return result;
    }

    auto holder = std::make_shared<CallbackHolder>(context);
    auto bridgeFailure = [&](const std::string_view stage,
                             const std::string &bridgeError) {
      result.message = std::string(stage) + " Vulkan bridge initialization failed";
      if (!bridgeError.empty())
        result.message += ": " + bridgeError;
      if (selection.selectionMode == compute::SelectionMode::AUTO) {
        result.ok = true;
        result.degraded = true;
        result.prepared = false;
        result.coverageVulkan = false;
        result.surfaceDiffusionVulkan = false;
        result.sessionGeneration = 0U;
        result.device = VK_NULL_HANDLE;
        result.message = "automatic Vulkan surface binding degraded to CPU: " +
                         result.message;
      }
      return false;
    };

    if (coverageVulkan) {
      if (paths.coverageDelta.empty()) {
        bridgeFailure("coverage", "coverage SPIR-V path is empty");
        return result;
      }
      if (!holder->coverage.initialize(*session, paths.coverageDelta, error)) {
        bridgeFailure("coverage", error);
        return result;
      }
    }
    if (surfaceVulkan) {
      if (paths.surfaceDiffusion.empty()) {
        bridgeFailure("surface diffusion", "surface-diffusion SPIR-V path is empty");
        return result;
      }
      if (!holder->surfaceDiffusion.initialize(*session, paths.surfaceDiffusion,
                                               error)) {
        bridgeFailure("surface diffusion", error);
        return result;
      }
    }

    // Capture the holder, rather than the bridge callbacks alone. This keeps
    // the borrowed session alive for copied Process callbacks and guarantees
    // bridge resources are destroyed before the session.
    if (coverageVulkan) {
      const auto holderCopy = holder;
      process.setCoverageDeltaExecutor(
          [holderCopy](viennaps::CoverageDeltaWork<float> &work,
                       std::string &invokeError) {
            return holderCopy->coverage.makeExecutor()(work, invokeError);
          });
    }
    if (surfaceVulkan) {
      const auto holderCopy = holder;
      process.setSurfaceDiffusionStatusExecutor(
          [holderCopy](viennaps::SurfaceDiffusionWork<float> &work,
                       std::string &invokeError) {
            return holderCopy->surfaceDiffusion.makeExecutor()(work,
                                                                invokeError);
          });
    }
    holder_ = std::move(holder);
    context_ = context;
    result.ok = true;
    return result;
  }

  [[nodiscard]] Result configure(
      ProcessType &process,
      const compute::DeploymentProfileDecision &resolvedDecision,
      const compute::HardwareFingerprint &currentHardware,
      std::span<const compute::StageWorkload> workloads,
      const compute::ManualSelectionConfig &selection,
      const SpirvPaths &paths) {
    return configure(process, resolvedDecision, currentHardware, workloads,
                     selection, {}, paths);
  }

  /// Clears both Process callbacks before releasing bridge/session state.
  void clear(ProcessType &process) {
    process.clearCoverageDeltaExecutor();
    process.clearSurfaceDiffusionExecutor();
    holder_.reset();
    context_.reset();
  }

  [[nodiscard]] bool isConfigured() const { return holder_ != nullptr; }
  [[nodiscard]] runtime::DeploymentComputeContext *context() {
    return context_.get();
  }
  [[nodiscard]] const runtime::DeploymentComputeContext *context() const {
    return context_.get();
  }

private:
  struct CallbackHolder {
    explicit CallbackHolder(
        std::shared_ptr<runtime::DeploymentComputeContext> deploymentContext)
        : context(std::move(deploymentContext)) {}

    // Declaration order is intentional: bridges release before context.
    std::shared_ptr<runtime::DeploymentComputeContext> context;
    VulkanCoverageDeltaExecutor coverage;
    VulkanSurfaceDiffusionExecutor surfaceDiffusion;
  };

  std::shared_ptr<runtime::DeploymentComputeContext> context_;
  std::shared_ptr<CallbackHolder> holder_;
};

} // namespace viennaps::vulkan::surface
