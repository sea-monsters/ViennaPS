// Copyright 2026 ViennaPS
// SPDX-License-Identifier: MIT

#pragma once

#include "deployment_compute_context.hpp"
#include "levelset_update.hpp"
#include "viennals_rebuild_executor.hpp"
#include "viennals_update_executor.hpp"

#include <compute/backendPolicy.hpp>

#include <array>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <span>
#include <string>
#include <string_view>

#include <process/psProcess.hpp>

namespace viennaps::vulkan::levelset {

template <int D> class LevelSetProcessController {
public:
  using ProcessType = viennaps::Process<float, D>;
  struct Result {
    bool ok = false;
    bool manualMode = false;
    bool usingVulkan = false;
    bool degraded = false;
    bool prepared = false;
    compute::ComputeBackend selectedBackend = compute::ComputeBackend::CPU;
    // Narrow observability for focused update/rebuild session-identity tests.
    std::uint64_t updateSessionGeneration = 0U;
    std::uint64_t rebuildSessionGeneration = 0U;
    std::string updateSessionDeviceName;
    std::string rebuildSessionDeviceName;
    std::string message;
  };

  struct RebuildSpirvPaths {
    std::string classification;
    std::string reductionScan;
    std::string actionFlags;
    std::string compact;
  };

  [[nodiscard]] Result configure(
      ProcessType &process, const compute::ManualSelectionConfig &selection,
      const compute::HardwareFingerprint &currentHardware,
      const compute::StageWorkload &workload,
      const runtime::ComputeSessionOptions &manualDevice = {},
      const std::string_view configuredSpirvPath = {},
      const std::string_view configuredProfilePath = {},
      const RebuildSpirvPaths &configuredRebuildSpirvPaths = {},
      const compute::DeploymentProfileDecision *resolvedDecision = nullptr,
      std::shared_ptr<runtime::DeploymentComputeContext> borrowedContext = {}) {
    const auto previousUpdateExecutor = process.getLevelSetUpdateExecutor();
    const auto previousRebuildExecutor = process.getLevelSetRebuildExecutor();
    const auto previousFailurePolicy = process.getLevelSetUpdateFailurePolicy();
    const auto restoreManualState = [&]() {
      process.setLevelSetUpdateExecutor(previousUpdateExecutor);
      process.setLevelSetRebuildExecutor(previousRebuildExecutor);
      process.setLevelSetUpdateFailurePolicy(previousFailurePolicy);
    };
    process.setLevelSetUpdateFailurePolicy(
        viennaps::LevelSetUpdateFailurePolicy::FALLBACK);
    process.clearLevelSetUpdateExecutor();
    process.clearLevelSetRebuildExecutor();
    Result result;
    result.manualMode =
        selection.selectionMode == compute::SelectionMode::MANUAL;

    const auto state = std::make_shared<RuntimeState>();
    state->borrowedComputeContext = std::move(borrowedContext);
    const auto requestedBackend = resolveRequestedBackend(selection);
    const bool wantVulkan = requestedBackend == compute::ComputeBackend::VULKAN;

    if (selection.selectionMode == compute::SelectionMode::MANUAL &&
        requestedBackend == compute::ComputeBackend::AUTO) {
      restoreManualState();
      result.ok = false;
      result.usingVulkan = false;
      result.message = "Manual selection requires an explicit backend for the "
                       "level-set stage.";
      return result;
    }
    if (workload.stage != compute::Stage::LEVEL_SET ||
        workload.precision != compute::Precision::FP32) {
      if (result.manualMode)
        restoreManualState();
      result.message =
          "Level-set Vulkan controller requires an FP32 LEVEL_SET workload.";
      return result;
    }
    if (workload.estimatedBytes == 0) {
      if (result.manualMode)
        restoreManualState();
      result.message =
          "Level-set Vulkan controller requires a non-zero estimated workload.";
      return result;
    }
    if (selection.precision != compute::Precision::MIXED &&
        selection.precision != compute::Precision::FP32) {
      if (result.manualMode)
        restoreManualState();
      result.message =
          "Level-set Vulkan controller supports FP32 selection only.";
      return result;
    }
    if (result.manualMode && requestedBackend != compute::ComputeBackend::CPU &&
        requestedBackend != compute::ComputeBackend::VULKAN) {
      restoreManualState();
      result.message =
          "Level-set Vulkan controller supports manual CPU or Vulkan only.";
      return result;
    }

    // Manual CPU is an explicit executable route. It must not consult a
    // deployment profile (or require a complete hardware fingerprint).
    if (result.manualMode && requestedBackend == compute::ComputeBackend::CPU) {
      result.ok = true;
      result.prepared = true;
      result.usingVulkan = false;
      result.degraded = false;
      result.selectedBackend = compute::ComputeBackend::CPU;
      process.clearLevelSetUpdateExecutor();
      process.clearLevelSetRebuildExecutor();
      return result;
    }

    const bool forwardEuler = process.getAdvectionParameters().temporalScheme ==
                              viennals::TemporalSchemeEnum::FORWARD_EULER;
    if (!forwardEuler) {
      const std::string unsupportedTemporal =
          "Level-set Vulkan execution is validated only for Forward Euler; "
          "Runge-Kutta temporal schemes remain on the CPU.";
      if (result.manualMode && wantVulkan) {
        restoreManualState();
        result.ok = false;
        result.usingVulkan = false;
        result.message = "Manual Vulkan requested, but " + unsupportedTemporal;
        return result;
      }
      result.ok = true;
      result.usingVulkan = false;
      result.selectedBackend = compute::ComputeBackend::CPU;
      result.degraded = !result.manualMode;
      result.message = unsupportedTemporal;
      return result;
    }

    const std::array workloads{workload};

    std::string prepareError;
    auto &computeContext = state->activeContext();
    if (state->borrowedComputeContext) {
      result.prepared = computeContext.isPrepared() &&
                        computeContext.session() != nullptr;
      if (!result.prepared)
        prepareError = "Borrowed Vulkan deployment context is not prepared.";
    } else {
      result.prepared =
          resolvedDecision != nullptr
              ? computeContext.prepare(*resolvedDecision, currentHardware,
                                       workloads, selection, prepareError,
                                       manualDevice)
              : computeContext.prepare(currentHardware, workloads, selection,
                                       configuredProfilePath, prepareError,
                                       manualDevice);
    }
    result.selectedBackend = compute::ComputeBackend::CPU;
    if (result.prepared && resolvedDecision != nullptr) {
      for (const auto &stage : resolvedDecision->plan.stages) {
        if (stage.stage == compute::Stage::LEVEL_SET) {
          result.selectedBackend = stage.selectedBackend;
          break;
        }
      }
    } else if (result.prepared) {
      result.selectedBackend =
          computeContext.backendFor(compute::Stage::LEVEL_SET);
    }
    result.message = prepareError;

    if (!result.prepared) {
      process.clearLevelSetUpdateExecutor();
      process.clearLevelSetRebuildExecutor();
      if (result.manualMode) {
        restoreManualState();
        result.ok = false;
        if (wantVulkan && !result.message.empty() &&
            result.message.find("Manual Vulkan") == std::string::npos) {
          result.message =
              std::string(
                  "Manual Vulkan requested, but backend selection failed: ") +
              result.message;
        }
        return result;
      }
      result.ok = true;
      result.degraded = true;
      result.usingVulkan = false;
      return result;
    }

    const bool levelSetVulkan =
        result.selectedBackend == compute::ComputeBackend::VULKAN;
    if (levelSetVulkan) {
      std::string spvPath;
      if (!resolveSpirvPath(configuredSpirvPath, spvPath, prepareError)) {
        if (result.manualMode) {
          restoreManualState();
          result.ok = false;
          result.message = "Manual Vulkan selected, but no level-set SPIR-V "
                           "path is available.";
          return result;
        }
        result.degraded = true;
        result.selectedBackend = compute::ComputeBackend::CPU;
        process.clearLevelSetUpdateExecutor();
        process.clearLevelSetRebuildExecutor();
        result.ok = true;
        result.usingVulkan = false;
        return result;
      }

      state->program = std::make_shared<runtime::SpirvProgram>();
      std::string spirvError;
      if (!runtime::readSpirv(spvPath, *state->program, spirvError)) {
        if (result.manualMode) {
          restoreManualState();
          result.ok = false;
          result.message =
              std::string("Manual Vulkan SPIR-V load failed: ") + spirvError;
          return result;
        }
        result.degraded = true;
        result.message = spirvError;
        result.selectedBackend = compute::ComputeBackend::CPU;
        process.clearLevelSetUpdateExecutor();
        process.clearLevelSetRebuildExecutor();
        result.ok = true;
        result.usingVulkan = false;
        return result;
      }

      RebuildSpirvPaths rebuildPaths = configuredRebuildSpirvPaths;
      std::string rebuildPathError;
      if (!resolveRebuildSpirvPaths(rebuildPaths, rebuildPathError)) {
        if (result.manualMode) {
          restoreManualState();
          result.ok = false;
          result.message =
              "Manual Vulkan rebuild SPIR-V paths are incomplete: " +
              rebuildPathError;
          return result;
        }
        result.degraded = true;
        result.message = rebuildPathError;
        result.selectedBackend = compute::ComputeBackend::CPU;
        process.clearLevelSetUpdateExecutor();
        process.clearLevelSetRebuildExecutor();
        result.ok = true;
        result.usingVulkan = false;
        return result;
      }

      auto *session = state->activeContext().session();
      if (session == nullptr) {
        if (result.manualMode) {
          restoreManualState();
          result.ok = false;
          result.message =
              "Manual Vulkan selected, but compute session is absent.";
          return result;
        }
        result.degraded = true;
        result.selectedBackend = compute::ComputeBackend::CPU;
        process.clearLevelSetUpdateExecutor();
        process.clearLevelSetRebuildExecutor();
        result.ok = true;
        result.usingVulkan = false;
        return result;
      }
      result.updateSessionGeneration = session->generation();
      result.updateSessionDeviceName =
          session->selection().properties.deviceName;
      state->rebuildPrimitives =
          std::make_shared<primitives::ReductionScanPrimitives>();
      if (!state->rebuildPrimitives->initialize(
              *session, rebuildPaths.reductionScan, spirvError)) {
        if (result.manualMode) {
          restoreManualState();
          result.ok = false;
          result.message =
              "Manual Vulkan rebuild primitive initialization failed: " +
              spirvError;
          return result;
        }
        result.degraded = true;
        result.message = spirvError;
        result.selectedBackend = compute::ComputeBackend::CPU;
        process.clearLevelSetUpdateExecutor();
        process.clearLevelSetRebuildExecutor();
        result.ok = true;
        result.usingVulkan = false;
        return result;
      }
      state->rebuildClassificationProgram =
          std::make_shared<runtime::SpirvProgram>();
      state->rebuildActionFlagsProgram =
          std::make_shared<runtime::SpirvProgram>();
      state->rebuildCompactProgram = std::make_shared<runtime::SpirvProgram>();
      if (!runtime::readSpirv(rebuildPaths.classification,
                              *state->rebuildClassificationProgram,
                              spirvError) ||
          !runtime::readSpirv(rebuildPaths.actionFlags,
                              *state->rebuildActionFlagsProgram, spirvError) ||
          !runtime::readSpirv(rebuildPaths.compact,
                              *state->rebuildCompactProgram, spirvError)) {
        if (result.manualMode) {
          restoreManualState();
          result.ok = false;
          result.message =
              "Manual Vulkan rebuild SPIR-V load failed: " + spirvError;
          return result;
        }
        result.degraded = true;
        result.message = spirvError;
        result.selectedBackend = compute::ComputeBackend::CPU;
        process.clearLevelSetUpdateExecutor();
        process.clearLevelSetRebuildExecutor();
        result.ok = true;
        result.usingVulkan = false;
        return result;
      }

      process.setLevelSetUpdateExecutor(
          [state](
              const viennals::Advect<float, D>::LevelSetUpdateContext &context,
              viennals::Advect<float, D>::LevelSetUpdateOutput &output,
              std::string &error) {
            auto *activeSession =
                state ? state->activeContext().session() : nullptr;
            if (!state || !state->program || !activeSession ||
                !activeSession->isValid()) {
              error =
                  "Vulkan level-set callback has an invalid execution state.";
              return viennals::Advect<float, D>::LevelSetUpdateStatus::ERROR;
            }
            const auto executor = makeViennaLsUpdateExecutorFp32<D>(
                *activeSession, state->program);
            return executor(context, output, error);
          });
      auto rebuildState = std::make_shared<ViennaLsRebuildExecutorStateFp32>();
      // The aliasing shared_ptr retains RuntimeState while borrowing the one
      // ComputeSession owned by DeploymentComputeContext. It creates no
      // second Vulkan session and no ownership cycle.
      rebuildState->session = std::shared_ptr<runtime::ComputeSession>(
          state, session);
      result.rebuildSessionGeneration = rebuildState->session->generation();
      result.rebuildSessionDeviceName =
          rebuildState->session->selection().properties.deviceName;
      rebuildState->primitives = state->rebuildPrimitives;
      rebuildState->classificationProgram = state->rebuildClassificationProgram;
      rebuildState->actionFlagsProgram = state->rebuildActionFlagsProgram;
      rebuildState->compactProgram = state->rebuildCompactProgram;
      const auto rebuildExecutor =
          makeViennaLsRebuildExecutorFp32<D>(std::move(rebuildState));
      process.setLevelSetRebuildExecutor(
          [rebuildExecutor](
              const viennals::Advect<float, D>::LevelSetRebuildContext &context,
              viennals::Advect<float, D>::LevelSetRebuildOutput &output,
              std::string &error) mutable {
            return rebuildExecutor(context, output, error);
          });
      if (result.manualMode) {
        process.setLevelSetUpdateFailurePolicy(
            viennaps::LevelSetUpdateFailurePolicy::FAIL);
      }

      result.ok = true;
      result.usingVulkan = true;
      result.degraded = false;
      return result;
    }

    if (result.manualMode && wantVulkan) {
      restoreManualState();
      result.message =
          "Manual Vulkan requested, but the deployment plan selected another "
          "backend.";
      return result;
    }

    process.clearLevelSetUpdateExecutor();
    process.clearLevelSetRebuildExecutor();
    result.ok = true;
    result.usingVulkan = false;
    result.degraded =
        !result.manualMode && state->activeContext().decision().requiresProbe;
    return result;
  }

  // Configure from a decision resolved/provisioned by the deployment thread.
  // The decision is consumed cache-only by the controller.
  [[nodiscard]] Result
  configureResolved(ProcessType &process,
                    const compute::ManualSelectionConfig &selection,
                    const compute::HardwareFingerprint &currentHardware,
                    const compute::StageWorkload &workload,
                    const compute::DeploymentProfileDecision &resolvedDecision,
                    const runtime::ComputeSessionOptions &manualDevice = {},
                    const std::string_view configuredSpirvPath = {},
                    const std::string_view configuredProfilePath = {},
                    const RebuildSpirvPaths &configuredRebuildSpirvPaths = {},
                    std::shared_ptr<runtime::DeploymentComputeContext>
                        borrowedContext = {}) {
    return configure(process, selection, currentHardware, workload,
                     manualDevice, configuredSpirvPath, configuredProfilePath,
                     configuredRebuildSpirvPaths, &resolvedDecision,
                     std::move(borrowedContext));
  }

  void clear(ProcessType &process) const {
    process.clearLevelSetUpdateExecutor();
    process.clearLevelSetRebuildExecutor();
    process.setLevelSetUpdateFailurePolicy(
        viennaps::LevelSetUpdateFailurePolicy::FALLBACK);
  }

private:
  struct RuntimeState {
    runtime::DeploymentComputeContext computeContext{};
    std::shared_ptr<runtime::DeploymentComputeContext> borrowedComputeContext{};
    std::shared_ptr<runtime::SpirvProgram> program{};
    std::shared_ptr<primitives::ReductionScanPrimitives> rebuildPrimitives{};
    std::shared_ptr<runtime::SpirvProgram> rebuildClassificationProgram{};
    std::shared_ptr<runtime::SpirvProgram> rebuildActionFlagsProgram{};
    std::shared_ptr<runtime::SpirvProgram> rebuildCompactProgram{};

    [[nodiscard]] runtime::DeploymentComputeContext &activeContext() {
      return borrowedComputeContext ? *borrowedComputeContext : computeContext;
    }
  };

  [[nodiscard]] static compute::ComputeBackend
  resolveRequestedBackend(const compute::ManualSelectionConfig &selection) {
    if (selection.selectionMode != compute::SelectionMode::MANUAL) {
      return compute::ComputeBackend::AUTO;
    }
    const auto idx = static_cast<std::size_t>(compute::Stage::LEVEL_SET);
    return selection.perStageBackend.at(idx).value_or(selection.globalBackend);
  }

  [[nodiscard]] static bool readEnvironmentPath(const std::string_view name,
                                                std::string &value) {
#ifdef _WIN32
    char *raw = nullptr;
    std::size_t len = 0;
    if (_dupenv_s(&raw, &len, std::string(name).c_str()) != 0 ||
        raw == nullptr) {
      return false;
    }
    value.assign(raw);
    std::free(raw);
    return true;
#else
    const char *raw = std::getenv(std::string(name).c_str());
    if (raw == nullptr) {
      return false;
    }
    value = raw;
    return true;
#endif
  }

  [[nodiscard]] static bool
  resolveSpirvPath(const std::string_view configuredPath,
                   std::string &resolvedPath, std::string &error) {
    if (!configuredPath.empty()) {
      resolvedPath = configuredPath;
      return true;
    }
#ifdef VIENNAPS_LEVELSET_UPDATE_SPV_PATH
    resolvedPath = std::string(VIENNAPS_LEVELSET_UPDATE_SPV_PATH);
    if (!resolvedPath.empty()) {
      return true;
    }
#endif
    if (readEnvironmentPath("VIENNAPS_LEVELSET_UPDATE_SPV_PATH",
                            resolvedPath) &&
        !resolvedPath.empty()) {
      return true;
    }
    error = "No level-set SPIR-V path configured.";
    return false;
  }

  [[nodiscard]] static bool resolveRebuildSpirvPaths(RebuildSpirvPaths &paths,
                                                     std::string &error) {
#ifdef VIENNAPS_HRLE_CLASSIFICATION_SPV_PATH
    if (paths.classification.empty())
      paths.classification = VIENNAPS_HRLE_CLASSIFICATION_SPV_PATH;
#endif
#ifdef VIENNAPS_REDUCTION_SCAN_SPV_PATH
    if (paths.reductionScan.empty())
      paths.reductionScan = VIENNAPS_REDUCTION_SCAN_SPV_PATH;
#endif
#ifdef VIENNAPS_HRLE_ACTION_FLAGS_SPV_PATH
    if (paths.actionFlags.empty())
      paths.actionFlags = VIENNAPS_HRLE_ACTION_FLAGS_SPV_PATH;
#endif
#ifdef VIENNAPS_HRLE_COMPACT_SPV_PATH
    if (paths.compact.empty())
      paths.compact = VIENNAPS_HRLE_COMPACT_SPV_PATH;
#endif
    if (paths.classification.empty())
      readEnvironmentPath("VIENNAPS_HRLE_CLASSIFICATION_SPV_PATH",
                          paths.classification);
    if (paths.reductionScan.empty())
      readEnvironmentPath("VIENNAPS_REDUCTION_SCAN_SPV_PATH",
                          paths.reductionScan);
    if (paths.actionFlags.empty())
      readEnvironmentPath("VIENNAPS_HRLE_ACTION_FLAGS_SPV_PATH",
                          paths.actionFlags);
    if (paths.compact.empty())
      readEnvironmentPath("VIENNAPS_HRLE_COMPACT_SPV_PATH", paths.compact);
    if (paths.classification.empty() || paths.reductionScan.empty() ||
        paths.actionFlags.empty() || paths.compact.empty()) {
      error = "one or more rebuild SPIR-V paths are unavailable";
      return false;
    }
    return true;
  }
};

} // namespace viennaps::vulkan::levelset
