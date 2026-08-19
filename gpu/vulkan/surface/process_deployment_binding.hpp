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

#include "../ray/vulkan_ray_flux_engine.hpp"
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

template <int D> struct ProcessDeploymentCallbackHolder {
  using NeutralSurfaceModel =
      viennaps::impl::NeutralTransportSurfaceModel<float, D>;

  explicit ProcessDeploymentCallbackHolder(
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
    viennaps::VulkanRayFluxSpirvPaths rayFlux;
  };
  using ShaderPaths = SpirvPaths;

  struct Result {
    bool ok = false;
    bool prepared = false;
    bool degraded = false;
    bool coverageVulkan = false;
    bool surfaceDiffusionVulkan = false;
    bool neutralTransportVelocityVulkan = false;
    bool rayTracingVulkan = false;
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
      const SpirvPaths &paths);

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
      const SpirvPaths &paths, const ProcessModelHandle &processModel);

  /// Equivalent explicit path for callers that already retain the CPU surface
  /// model rather than its containing process model.
  [[nodiscard]] Result configure(
      ProcessType &process,
      const compute::DeploymentProfileDecision &resolvedDecision,
      const compute::HardwareFingerprint &currentHardware,
      std::span<const compute::StageWorkload> workloads,
      const compute::ManualSelectionConfig &selection,
      const runtime::ComputeSessionOptions &manualDevice,
      const SpirvPaths &paths, const SurfaceModelHandle &surfaceModel);

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
      const bool allowNeutral);

public:

  [[nodiscard]] Result configure(
      ProcessType &process,
      const compute::DeploymentProfileDecision &resolvedDecision,
      const compute::HardwareFingerprint &currentHardware,
      std::span<const compute::StageWorkload> workloads,
      const compute::ManualSelectionConfig &selection,
      const SpirvPaths &paths);

  [[nodiscard]] Result configure(
      ProcessType &process,
      const compute::DeploymentProfileDecision &resolvedDecision,
      const compute::HardwareFingerprint &currentHardware,
      std::span<const compute::StageWorkload> workloads,
      const compute::ManualSelectionConfig &selection, const SpirvPaths &paths,
      const ProcessModelHandle &processModel);

  [[nodiscard]] Result configure(
      ProcessType &process,
      const compute::DeploymentProfileDecision &resolvedDecision,
      const compute::HardwareFingerprint &currentHardware,
      std::span<const compute::StageWorkload> workloads,
      const compute::ManualSelectionConfig &selection, const SpirvPaths &paths,
      const SurfaceModelHandle &surfaceModel);

  /// Clears both Process callbacks before releasing bridge/session state.
  void clear(ProcessType &process);

  /// Creates a Vulkan ray-flux engine bound to the prepared shared context.
  /// Returns nullptr when ray tracing is not Vulkan-selected or the context is
  /// no longer prepared. The caller typically passes the engine to
  /// Process::setFluxEngineOverride().
  [[nodiscard]] std::unique_ptr<viennaps::VulkanRayFluxEngine<float, D>>
  makeRayFluxEngine(bool allowCpuFallback = true) const;
  [[nodiscard]] bool isConfigured() const;
  [[nodiscard]] runtime::DeploymentComputeContext *context();
  [[nodiscard]] const runtime::DeploymentComputeContext *context() const;
  [[nodiscard]] std::shared_ptr<runtime::DeploymentComputeContext>
  sharedContext() const;

  [[nodiscard]] viennaps::NeutralTransportVelocityExecutor<float>
  neutralVelocityExecutor() const;

private:
  std::shared_ptr<runtime::DeploymentComputeContext> context_;
  std::shared_ptr<ProcessDeploymentCallbackHolder<D>> holder_;
  viennaps::VulkanRayFluxSpirvPaths rayFluxPaths_{};
};

} // namespace viennaps::vulkan::surface
