// Copyright 2026 ViennaPS
// SPDX-License-Identifier: MIT

#include "process_deployment_binding.hpp"

#include <utility>

namespace viennaps::vulkan::surface {

template <int D>
typename ProcessDeploymentBinding<D>::Result
ProcessDeploymentBinding<D>::configureImpl(
    typename ProcessDeploymentBinding<D>::ProcessType &process,
    const compute::DeploymentProfileDecision &resolvedDecision,
    const compute::HardwareFingerprint &currentHardware,
    std::span<const compute::StageWorkload> workloads,
    const compute::ManualSelectionConfig &selection,
    const runtime::ComputeSessionOptions &manualDevice,
    const typename ProcessDeploymentBinding<D>::SpirvPaths &paths,
    const typename ProcessDeploymentBinding<D>::ProcessModelHandle &processModel,
    std::shared_ptr<typename ProcessDeploymentBinding<D>::NeutralSurfaceModel>
        suppliedSurface,
    const bool allowNeutral) {
  clear(process);

  Result result;
  for (const auto &workload : workloads) {
    if (workload.stage != compute::Stage::COVERAGE &&
        workload.stage != compute::Stage::SURFACE_DIFFUSION &&
        workload.stage != compute::Stage::LEVEL_SET &&
        workload.stage != compute::Stage::RAY_TRACING &&
        (!allowNeutral ||
         workload.stage != compute::Stage::NEUTRAL_TRANSPORT_VELOCITY)) {
      result.message =
          "Process surface binding supports only COVERAGE, "
          "SURFACE_DIFFUSION, NEUTRAL_TRANSPORT_VELOCITY, RAY_TRACING, and "
          "composition-owned LEVEL_SET workloads";
      return result;
    }
  }

  auto context = std::make_shared<runtime::DeploymentComputeContext>();
  std::string error;
  if (!context->prepare(resolvedDecision, currentHardware, workloads, selection,
                        error, manualDevice)) {
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
  const bool rayTracingVulkan =
      context->backendFor(compute::Stage::RAY_TRACING) ==
      compute::ComputeBackend::VULKAN;
  result.coverageVulkan = coverageVulkan;
  result.surfaceDiffusionVulkan = surfaceVulkan;
  result.neutralTransportVelocityVulkan = neutralVulkan;
  result.rayTracingVulkan = rayTracingVulkan;

  auto *session = context->session();
  if (session != nullptr) {
    result.sessionGeneration = session->generation();
    result.device = session->deviceHandle();
  }
  if (!coverageVulkan && !surfaceVulkan && !neutralVulkan &&
      !rayTracingVulkan) {
    context_ = context;
    rayFluxPaths_ = paths.rayFlux;
    result.ok = true;
    return result;
  }
  if (session == nullptr) {
    result.message = "Vulkan stage selected without a deployment session";
    return result;
  }

  auto holder = std::make_shared<ProcessDeploymentCallbackHolder<D>>(context);
  auto bridgeFailure = [&](const std::string_view stage,
                           const std::string &bridgeError) {
    result.message =
        std::string(stage) + " Vulkan bridge initialization failed";
    if (!bridgeError.empty())
      result.message += ": " + bridgeError;
    if (selection.selectionMode == compute::SelectionMode::AUTO) {
      result.ok = true;
      result.degraded = true;
      result.prepared = false;
      result.coverageVulkan = false;
      result.surfaceDiffusionVulkan = false;
      result.neutralTransportVelocityVulkan = false;
      result.rayTracingVulkan = false;
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
    result.rayTracingVulkan = false;
    result.sessionGeneration = 0U;
    result.device = VK_NULL_HANDLE;
    return false;
  };

  std::shared_ptr<typename ProcessDeploymentBinding<D>::NeutralSurfaceModel>
      neutralSurface = std::move(suppliedSurface);
  if (neutralVulkan) {
    if (!neutralSurface && processModel) {
      const auto surfaceModel = processModel->getSurfaceModel();
      if (surfaceModel) {
        neutralSurface = std::dynamic_pointer_cast<
            typename ProcessDeploymentBinding<D>::NeutralSurfaceModel>(
            surfaceModel);
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
      bridgeFailure("surface diffusion",
                    "surface-diffusion SPIR-V path is empty");
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
  if (rayTracingVulkan) {
    const auto &ray = paths.rayFlux;
    if (ray.triangleHit.empty() || ray.recordCompaction.empty() ||
        ray.reductionScan.empty() || ray.radixHistogram.empty() ||
        ray.radixPrefix.empty() || ray.radixScatter.empty() ||
        ray.surfaceSegments.empty() || ray.surfaceReduce.empty()) {
      bridgeFailure("ray tracing",
                    "one or more required ray-tracing SPIR-V paths are empty");
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

  rayFluxPaths_ = paths.rayFlux;
  holder_ = std::move(holder);
  context_ = context;
  result.ok = true;
  return result;
}

template <int D>
typename ProcessDeploymentBinding<D>::Result ProcessDeploymentBinding<D>::
configure(ProcessType &process,
          const compute::DeploymentProfileDecision &resolvedDecision,
          const compute::HardwareFingerprint &currentHardware,
          std::span<const compute::StageWorkload> workloads,
          const compute::ManualSelectionConfig &selection,
          const runtime::ComputeSessionOptions &manualDevice,
          const SpirvPaths &paths) {
  return configureImpl(process, resolvedDecision, currentHardware, workloads,
                       selection, manualDevice, paths, nullptr, nullptr, false);
}

template <int D>
typename ProcessDeploymentBinding<D>::Result ProcessDeploymentBinding<D>::
configure(ProcessType &process,
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

template <int D>
typename ProcessDeploymentBinding<D>::Result ProcessDeploymentBinding<D>::
configure(ProcessType &process,
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

template <int D>
typename ProcessDeploymentBinding<D>::Result ProcessDeploymentBinding<D>::
configure(ProcessType &process,
          const compute::DeploymentProfileDecision &resolvedDecision,
          const compute::HardwareFingerprint &currentHardware,
          std::span<const compute::StageWorkload> workloads,
          const compute::ManualSelectionConfig &selection,
          const SpirvPaths &paths) {
  return configure(process, resolvedDecision, currentHardware, workloads,
                   selection, runtime::ComputeSessionOptions{}, paths);
}

template <int D>
typename ProcessDeploymentBinding<D>::Result ProcessDeploymentBinding<D>::
configure(ProcessType &process,
          const compute::DeploymentProfileDecision &resolvedDecision,
          const compute::HardwareFingerprint &currentHardware,
          std::span<const compute::StageWorkload> workloads,
          const compute::ManualSelectionConfig &selection,
          const SpirvPaths &paths, const ProcessModelHandle &processModel) {
  return configure(process, resolvedDecision, currentHardware, workloads,
                   selection, runtime::ComputeSessionOptions{}, paths,
                   processModel);
}

template <int D>
typename ProcessDeploymentBinding<D>::Result ProcessDeploymentBinding<D>::
configure(ProcessType &process,
          const compute::DeploymentProfileDecision &resolvedDecision,
          const compute::HardwareFingerprint &currentHardware,
          std::span<const compute::StageWorkload> workloads,
          const compute::ManualSelectionConfig &selection,
          const SpirvPaths &paths, const SurfaceModelHandle &surfaceModel) {
  return configure(process, resolvedDecision, currentHardware, workloads,
                   selection, runtime::ComputeSessionOptions{}, paths,
                   surfaceModel);
}

template <int D>
std::unique_ptr<viennaps::VulkanRayFluxEngine<float, D>>
ProcessDeploymentBinding<D>::makeRayFluxEngine(
    const bool allowCpuFallback) const {
#if !defined(VIENNAPS_VULKAN_RAY_FLUX_ENGINE_AVAILABLE)
  (void)allowCpuFallback;
  return nullptr;
#else
  if (context_ == nullptr ||
      context_->backendFor(compute::Stage::RAY_TRACING) !=
          compute::ComputeBackend::VULKAN ||
      rayFluxPaths_.triangleHit.empty()) {
    return nullptr;
  }
  return std::make_unique<viennaps::VulkanRayFluxEngine<float, D>>(
      context_, rayFluxPaths_, allowCpuFallback);
#endif
}

template <int D>
void ProcessDeploymentBinding<D>::clear(ProcessType &process) {
  process.clearCoverageDeltaExecutor();
  process.clearSurfaceDiffusionExecutor();
  if (holder_ != nullptr)
    holder_->clearNeutralCallback();
  holder_.reset();
  context_.reset();
  rayFluxPaths_ = {};
}

template <int D>
bool ProcessDeploymentBinding<D>::isConfigured() const {
  return holder_ != nullptr;
}

template <int D>
runtime::DeploymentComputeContext *ProcessDeploymentBinding<D>::context() {
  return context_.get();
}

template <int D>
const runtime::DeploymentComputeContext *
ProcessDeploymentBinding<D>::context() const {
  return context_.get();
}

template <int D>
std::shared_ptr<runtime::DeploymentComputeContext>
ProcessDeploymentBinding<D>::sharedContext() const {
  return context_;
}

template <int D>
viennaps::NeutralTransportVelocityExecutor<float>
ProcessDeploymentBinding<D>::neutralVelocityExecutor() const {
  const auto holder = holder_;
  if (holder == nullptr)
    return {};
  return [holder](viennaps::NeutralTransportVelocityWork<float> &work,
                  std::string &invokeError) {
    return holder->neutralTransportVelocity.makeExecutor()(work, invokeError);
  };
}

template class ProcessDeploymentBinding<2>;
template class ProcessDeploymentBinding<3>;

} // namespace viennaps::vulkan::surface
