// Copyright 2026 ViennaPS
// SPDX-License-Identifier: MIT
//
// Deployment-time composition for the FP32 Process coverage,
// surface-diffusion, and neutral-transport executor seams. This header is
// intentionally GPU-side;
// it must not be included by ViennaPS core headers.

#pragma once

#include "coverage_delta_executor.hpp"
#include "neutral_transport_velocity_executor.hpp"
#include "surface_diffusion_executor.hpp"

#include "../runtime/deployment_compute_context.hpp"

#include <compute/backendPolicy.hpp>
#include <compute/deploymentProfile.hpp>
#include <process/psProcess.hpp>
#include <models/psNeutralTransport.hpp>

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>

namespace viennaps::vulkan::surface {

/// Owns cache-only deployment state for the three FP32 Process surface seams.
/// The binding never reads a profile or invokes a hardware probe.
template <int D> class ProcessDeploymentBinding {
public:
  using ProcessType = viennaps::Process<float, D>;
  using ProcessModelHandle =
      viennacore::SmartPointer<viennaps::ProcessModelBase<float, D>>;
  using SurfaceModelHandle =
      viennacore::SmartPointer<viennaps::SurfaceModel<float>>;
  using NeutralSurfaceModel =
      viennaps::impl::NeutralTransportSurfaceModel<float, D>;

  struct SpirvPaths {
    std::string coverageDelta;
    std::string surfaceDiffusion;
    std::string neutralTransportVelocity;
  };
  using ShaderPaths = SpirvPaths;

  struct Result {
    bool ok = false;
    bool prepared = false;
    bool degraded = false;
    bool coverageVulkan = false;
    bool surfaceDiffusionVulkan = false;
    bool neutralTransportVelocityVulkan = false;
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
    return configureImpl(process, resolvedDecision, currentHardware, workloads,
                          selection, manualDevice, paths, nullptr, nullptr,
                          false);
  }

  /// Configures the three supported surface seams while retaining the caller's
  /// CPU process model long enough to clear a neutral velocity callback.
  /// The caller retains ownership of the Process and model; this binding keeps
  /// a shared handle to the discovered concrete neutral surface model.
  [[nodiscard]] Result configure(
      ProcessType &process,
      const compute::DeploymentProfileDecision &resolvedDecision,
      const compute::HardwareFingerprint &currentHardware,
      std::span<const compute::StageWorkload> workloads,
      const compute::ManualSelectionConfig &selection,
      const runtime::ComputeSessionOptions &manualDevice,
      const SpirvPaths &paths, const ProcessModelHandle &processModel) {
    return configureImpl(process, resolvedDecision, currentHardware, workloads,
                          selection, manualDevice, paths, processModel, nullptr,
                          true);
  }

  /// Equivalent explicit path for callers that already retain the CPU surface
  /// model rather than its containing process model.
  [[nodiscard]] Result configure(
      ProcessType &process,
      const compute::DeploymentProfileDecision &resolvedDecision,
      const compute::HardwareFingerprint &currentHardware,
      std::span<const compute::StageWorkload> workloads,
      const compute::ManualSelectionConfig &selection,
      const runtime::ComputeSessionOptions &manualDevice,
      const SpirvPaths &paths, const SurfaceModelHandle &surfaceModel) {
    std::shared_ptr<NeutralSurfaceModel> neutralSurface;
    if (surfaceModel)
      neutralSurface =
          std::dynamic_pointer_cast<NeutralSurfaceModel>(surfaceModel);
    return configureImpl(process, resolvedDecision, currentHardware, workloads,
                          selection, manualDevice, paths, nullptr,
                          std::move(neutralSurface), true);
  }

private:
  [[nodiscard]] Result configureImpl(
      ProcessType &process,
      const compute::DeploymentProfileDecision &resolvedDecision,
      const compute::HardwareFingerprint &currentHardware,
      std::span<const compute::StageWorkload> workloads,
      const compute::ManualSelectionConfig &selection,
      const runtime::ComputeSessionOptions &manualDevice,
      const SpirvPaths &paths, const ProcessModelHandle &processModel,
      std::shared_ptr<NeutralSurfaceModel> suppliedSurface,
      const bool allowNeutral) {
    clear(process);

    Result result;
    for (const auto &workload : workloads) {
      if (workload.stage != compute::Stage::COVERAGE &&
          workload.stage != compute::Stage::SURFACE_DIFFUSION &&
          workload.stage != compute::Stage::LEVEL_SET &&
          (!allowNeutral ||
           workload.stage != compute::Stage::NEUTRAL_TRANSPORT_VELOCITY)) {
        result.message =
            "Process surface binding supports only COVERAGE, "
            "SURFACE_DIFFUSION, NEUTRAL_TRANSPORT_VELOCITY, and "
            "composition-owned LEVEL_SET workloads";
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
    const bool neutralVulkan =
        allowNeutral &&
        context->backendFor(compute::Stage::NEUTRAL_TRANSPORT_VELOCITY) ==
            compute::ComputeBackend::VULKAN;
    result.coverageVulkan = coverageVulkan;
    result.surfaceDiffusionVulkan = surfaceVulkan;
    result.neutralTransportVelocityVulkan = neutralVulkan;

    auto *session = context->session();
    if (session != nullptr) {
      result.sessionGeneration = session->generation();
      result.device = session->deviceHandle();
    }
    if (!coverageVulkan && !surfaceVulkan && !neutralVulkan) {
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
        result.neutralTransportVelocityVulkan = false;
        result.sessionGeneration = 0U;
        result.device = VK_NULL_HANDLE;
        result.message = "automatic Vulkan surface binding degraded to CPU: " +
                         result.message;
        return false;
      }
      result.ok = false;
      result.prepared = false;
      result.coverageVulkan = false;
      result.surfaceDiffusionVulkan = false;
      result.neutralTransportVelocityVulkan = false;
      result.sessionGeneration = 0U;
      result.device = VK_NULL_HANDLE;
      return false;
    };

    std::shared_ptr<NeutralSurfaceModel> neutralSurface;
    if (neutralVulkan) {
      neutralSurface = std::move(suppliedSurface);
      if (!neutralSurface && processModel) {
        const auto surfaceModel = processModel->getSurfaceModel();
        if (surfaceModel) {
          neutralSurface =
              std::dynamic_pointer_cast<NeutralSurfaceModel>(surfaceModel);
        }
      }
      if (!neutralSurface) {
        bridgeFailure("neutral transport velocity",
                      "a retained neutral CPU process model is required");
        return result;
      }
    }

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
    if (neutralVulkan) {
      if (paths.neutralTransportVelocity.empty()) {
        bridgeFailure("neutral transport velocity",
                      "neutral transport velocity SPIR-V path is empty");
        return result;
      }
      if (!holder->neutralTransportVelocity.initialize(
              *session, paths.neutralTransportVelocity, error)) {
        bridgeFailure("neutral transport velocity", error);
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
    if (neutralVulkan) {
      const auto holderCopy = holder;
      neutralSurface->setVelocityExecutor(
          [holderCopy](viennaps::NeutralTransportVelocityWork<float> &work,
                       std::string &invokeError) {
            return holderCopy->neutralTransportVelocity.makeExecutor()(
                work, invokeError);
          });
      holder->neutralSurface = std::move(neutralSurface);
    }
    holder_ = std::move(holder);
    context_ = context;
    result.ok = true;
    return result;
  }

public:

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

  [[nodiscard]] Result configure(
      ProcessType &process,
      const compute::DeploymentProfileDecision &resolvedDecision,
      const compute::HardwareFingerprint &currentHardware,
      std::span<const compute::StageWorkload> workloads,
      const compute::ManualSelectionConfig &selection, const SpirvPaths &paths,
      const ProcessModelHandle &processModel) {
    return configure(process, resolvedDecision, currentHardware, workloads,
                     selection, {}, paths, processModel);
  }

  [[nodiscard]] Result configure(
      ProcessType &process,
      const compute::DeploymentProfileDecision &resolvedDecision,
      const compute::HardwareFingerprint &currentHardware,
      std::span<const compute::StageWorkload> workloads,
      const compute::ManualSelectionConfig &selection, const SpirvPaths &paths,
      const SurfaceModelHandle &surfaceModel) {
    return configure(process, resolvedDecision, currentHardware, workloads,
                     selection, {}, paths, surfaceModel);
  }

  /// Clears both Process callbacks before releasing bridge/session state.
  void clear(ProcessType &process) {
    process.clearCoverageDeltaExecutor();
    process.clearSurfaceDiffusionExecutor();
    if (holder_ != nullptr)
      holder_->clearNeutralCallback();
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
  [[nodiscard]] std::shared_ptr<runtime::DeploymentComputeContext>
  sharedContext() const {
    return context_;
  }

  [[nodiscard]] typename NeutralSurfaceModel::VelocityExecutor
  neutralVelocityExecutor() const {
    const auto holder = holder_;
    if (holder == nullptr)
      return {};
    return [holder](viennaps::NeutralTransportVelocityWork<float> &work,
                    std::string &invokeError) {
      return holder->neutralTransportVelocity.makeExecutor()(work, invokeError);
    };
  }

private:
  struct CallbackHolder {
    explicit CallbackHolder(
    std::shared_ptr<runtime::DeploymentComputeContext> deploymentContext)
        : context(std::move(deploymentContext)) {}

    void clearNeutralCallback() {
      if (neutralSurface != nullptr)
        neutralSurface->clearVelocityExecutor();
    }

    // Declaration order is intentional: bridges release before context.
    std::shared_ptr<runtime::DeploymentComputeContext> context;
    VulkanCoverageDeltaExecutor coverage;
    VulkanSurfaceDiffusionExecutor surfaceDiffusion;
    VulkanNeutralTransportVelocityExecutor neutralTransportVelocity;
    std::shared_ptr<NeutralSurfaceModel> neutralSurface;
  };

  std::shared_ptr<runtime::DeploymentComputeContext> context_;
  std::shared_ptr<CallbackHolder> holder_;
};

} // namespace viennaps::vulkan::surface
