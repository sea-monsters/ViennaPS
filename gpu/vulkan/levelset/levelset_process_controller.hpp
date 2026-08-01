// Copyright 2026 ViennaPS
// SPDX-License-Identifier: MIT

#pragma once

#include "deployment_compute_context.hpp"
#include "levelset_update.hpp"
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
    std::string message;
  };

  [[nodiscard]] Result
  configure(ProcessType &process,
            const compute::ManualSelectionConfig &selection,
            const compute::HardwareFingerprint &currentHardware,
            const compute::StageWorkload &workload,
            const runtime::ComputeSessionOptions &manualDevice = {},
            const std::string_view configuredSpirvPath = {},
            const std::string_view configuredProfilePath = {}) {
    process.setLevelSetUpdateFailurePolicy(
        viennaps::LevelSetUpdateFailurePolicy::FALLBACK);
    process.clearLevelSetUpdateExecutor();
    Result result;
    result.manualMode =
        selection.selectionMode == compute::SelectionMode::MANUAL;

    const auto state = std::make_shared<RuntimeState>();
    const auto requestedBackend = resolveRequestedBackend(selection);
    const bool wantVulkan = requestedBackend == compute::ComputeBackend::VULKAN;

    if (selection.selectionMode == compute::SelectionMode::MANUAL &&
        requestedBackend == compute::ComputeBackend::AUTO) {
      result.ok = false;
      result.usingVulkan = false;
      result.message = "Manual selection requires an explicit backend for the "
                       "level-set stage.";
      return result;
    }
    if (workload.stage != compute::Stage::LEVEL_SET ||
        workload.precision != compute::Precision::FP32) {
      result.message =
          "Level-set Vulkan controller requires an FP32 LEVEL_SET workload.";
      return result;
    }
    if (workload.estimatedBytes == 0) {
      result.message =
          "Level-set Vulkan controller requires a non-zero estimated workload.";
      return result;
    }
    if (selection.precision != compute::Precision::MIXED &&
        selection.precision != compute::Precision::FP32) {
      result.message =
          "Level-set Vulkan controller supports FP32 selection only.";
      return result;
    }
    if (result.manualMode && requestedBackend != compute::ComputeBackend::CPU &&
        requestedBackend != compute::ComputeBackend::VULKAN) {
      result.message =
          "Level-set Vulkan controller supports manual CPU or Vulkan only.";
      return result;
    }

    const std::array workloads{workload};

    std::string prepareError;
    result.prepared = state->computeContext.prepare(
        currentHardware, workloads, selection, configuredProfilePath,
        prepareError, manualDevice);
    result.selectedBackend =
        result.prepared
            ? state->computeContext.backendFor(compute::Stage::LEVEL_SET)
            : compute::ComputeBackend::CPU;
    result.message = prepareError;

    if (!result.prepared) {
      process.clearLevelSetUpdateExecutor();
      if (result.manualMode) {
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

    if (selection.selectionMode == compute::SelectionMode::MANUAL &&
        requestedBackend == compute::ComputeBackend::CPU) {
      result.ok = true;
      result.usingVulkan = false;
      result.degraded = false;
      process.clearLevelSetUpdateExecutor();
      return result;
    }

    const bool levelSetVulkan =
        result.selectedBackend == compute::ComputeBackend::VULKAN;
    if (levelSetVulkan) {
      std::string spvPath;
      if (!resolveSpirvPath(configuredSpirvPath, spvPath, prepareError)) {
        if (result.manualMode) {
          result.ok = false;
          result.message = "Manual Vulkan selected, but no level-set SPIR-V "
                           "path is available.";
          return result;
        }
        result.degraded = true;
        result.selectedBackend = compute::ComputeBackend::CPU;
        process.clearLevelSetUpdateExecutor();
        result.ok = true;
        result.usingVulkan = false;
        return result;
      }

      state->program = std::make_shared<runtime::SpirvProgram>();
      std::string spirvError;
      if (!runtime::readSpirv(spvPath, *state->program, spirvError)) {
        if (result.manualMode) {
          result.ok = false;
          result.message =
              std::string("Manual Vulkan SPIR-V load failed: ") + spirvError;
          return result;
        }
        result.degraded = true;
        result.message = spirvError;
        result.selectedBackend = compute::ComputeBackend::CPU;
        process.clearLevelSetUpdateExecutor();
        result.ok = true;
        result.usingVulkan = false;
        return result;
      }

      const auto *session = state->computeContext.session();
      if (session == nullptr) {
        if (result.manualMode) {
          result.ok = false;
          result.message =
              "Manual Vulkan selected, but compute session is absent.";
          return result;
        }
        result.degraded = true;
        result.selectedBackend = compute::ComputeBackend::CPU;
        process.clearLevelSetUpdateExecutor();
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
                state ? state->computeContext.session() : nullptr;
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
      result.message =
          "Manual Vulkan requested, but the deployment plan selected another "
          "backend.";
      return result;
    }

    process.clearLevelSetUpdateExecutor();
    result.ok = true;
    result.usingVulkan = false;
    result.degraded =
        !result.manualMode && state->computeContext.decision().requiresProbe;
    return result;
  }

  void clear(ProcessType &process) const {
    process.clearLevelSetUpdateExecutor();
    process.setLevelSetUpdateFailurePolicy(
        viennaps::LevelSetUpdateFailurePolicy::FALLBACK);
  }

private:
  struct RuntimeState {
    runtime::DeploymentComputeContext computeContext{};
    std::shared_ptr<runtime::SpirvProgram> program{};
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
};

} // namespace viennaps::vulkan::levelset
