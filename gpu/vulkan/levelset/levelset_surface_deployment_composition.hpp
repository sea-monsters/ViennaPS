// Copyright 2026 ViennaPS
// SPDX-License-Identifier: MIT
//
// Deployment-time owner for the one Process Vulkan context shared by the
// surface and Level Set callback seams.

#pragma once

#include "levelset_process_controller.hpp"
#include "../surface/process_deployment_binding.hpp"

#include <cstddef>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>

namespace viennaps::vulkan::levelset {

/// Composes the Process surface and Level Set deployment seams around one
/// context. A partial automatic Vulkan configuration is never retained: every
/// selected callback is removed and the caller receives a CPU degradation.
template <int D> class LevelSetSurfaceDeploymentComposition {
public:
  using ProcessType = viennaps::Process<float, D>;
  using SurfaceBinding = surface::ProcessDeploymentBinding<D>;
  using Controller = LevelSetProcessController<D>;
  using SurfacePaths = typename SurfaceBinding::SpirvPaths;
  using RebuildPaths = typename Controller::RebuildSpirvPaths;
  using ProcessModelHandle = typename SurfaceBinding::ProcessModelHandle;

  struct Result {
    bool ok = false;
    bool prepared = false;
    bool degraded = false;
    bool usingVulkan = false;
    typename SurfaceBinding::Result surface{};
    typename Controller::Result levelSet{};
    std::string message;
  };

  [[nodiscard]] Result configure(
      ProcessType &process,
      const compute::DeploymentProfileDecision &resolvedDecision,
      const compute::HardwareFingerprint &currentHardware,
      std::span<const compute::StageWorkload> workloads,
      const compute::StageWorkload &levelSetWorkload,
      const compute::ManualSelectionConfig &selection,
      const runtime::ComputeSessionOptions &manualDevice,
      const SurfacePaths &surfacePaths, const ProcessModelHandle &processModel,
      const std::string_view levelSetSpirvPath,
      const RebuildPaths &rebuildPaths = {}) {
    clear(process);

    Result result;
    if (selection.selectionMode == compute::SelectionMode::MANUAL &&
        requestedBackend(selection, compute::Stage::COVERAGE) ==
            compute::ComputeBackend::CPU &&
        requestedBackend(selection, compute::Stage::SURFACE_DIFFUSION) ==
            compute::ComputeBackend::CPU &&
        requestedBackend(selection, compute::Stage::NEUTRAL_TRANSPORT_VELOCITY) ==
            compute::ComputeBackend::CPU &&
        requestedBackend(selection, compute::Stage::LEVEL_SET) ==
            compute::ComputeBackend::CPU) {
      result.levelSet = levelSet_.configureResolved(
          process, selection, currentHardware, levelSetWorkload, resolvedDecision,
          manualDevice, levelSetSpirvPath, {}, rebuildPaths);
      result.ok = result.levelSet.ok;
      result.prepared = result.levelSet.prepared;
      result.message = result.levelSet.message;
      return result;
    }

    result.surface = surface_.configure(
        process, resolvedDecision, currentHardware, workloads, selection,
        manualDevice, surfacePaths, processModel);
    if (!result.surface.ok || result.surface.degraded || !result.surface.prepared) {
      const auto message = result.surface.message;
      return finishFailure(process, selection, std::move(result), message);
    }

    const auto context = surface_.sharedContext();
    result.levelSet = levelSet_.configureResolved(
        process, selection, currentHardware, levelSetWorkload, resolvedDecision,
        manualDevice, levelSetSpirvPath, {}, rebuildPaths, context);
    const bool manualLevelSetCpu =
        selection.selectionMode == compute::SelectionMode::MANUAL &&
        requestedBackend(selection, compute::Stage::LEVEL_SET) ==
            compute::ComputeBackend::CPU;
    if (!result.levelSet.ok || result.levelSet.degraded ||
        (!manualLevelSetCpu && !result.levelSet.usingVulkan)) {
      const auto message = result.levelSet.message;
      return finishFailure(process, selection, std::move(result), message);
    }

    result.ok = true;
    result.prepared = true;
    result.usingVulkan = true;
    return result;
  }

  void clear(ProcessType &process) {
    levelSet_.clear(process);
    surface_.clear(process);
  }

  [[nodiscard]] std::shared_ptr<runtime::DeploymentComputeContext>
  sharedContext() const {
    return surface_.sharedContext();
  }

  [[nodiscard]] typename SurfaceBinding::NeutralSurfaceModel::VelocityExecutor
  neutralVelocityExecutor() const {
    return surface_.neutralVelocityExecutor();
  }

private:
  [[nodiscard]] static compute::ComputeBackend
  requestedBackend(const compute::ManualSelectionConfig &selection,
                   const compute::Stage stage) {
    return selection.perStageBackend.at(static_cast<std::size_t>(stage))
        .value_or(selection.globalBackend);
  }

  [[nodiscard]] Result finishFailure(ProcessType &process,
                                     const compute::ManualSelectionConfig &selection,
                                     Result result, const std::string &message) {
    clear(process);
    result.prepared = false;
    result.usingVulkan = false;
    result.message = message;
    if (selection.selectionMode == compute::SelectionMode::AUTO) {
      result.ok = true;
      result.degraded = true;
      return result;
    }
    result.ok = false;
    return result;
  }

  SurfaceBinding surface_{};
  Controller levelSet_{};
};

} // namespace viennaps::vulkan::levelset
