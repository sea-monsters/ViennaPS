#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace viennaps::compute {

enum class ComputeBackend { AUTO, CPU, CUDA, VULKAN };
enum class SelectionMode { AUTO, MANUAL };
enum class VulkanNumericalSmokeStatus { NOT_RUN, PASS, FAIL };

inline constexpr std::string_view kVulkanFp32NumericalSmokeContract =
    "fp32-bitwise-watchdog-v1";
inline constexpr std::uint64_t kVulkanFp32NumericalSmokeWatchdogMs = 60'000ULL;

struct VulkanFp32NumericalSmokeEvidence {
  VulkanNumericalSmokeStatus status = VulkanNumericalSmokeStatus::NOT_RUN;
  std::string contractId;
  std::uint32_t caseCount = 0U;
  std::uint32_t mismatchCount = 0U;
  std::uint32_t maxUlp = 0U;
  std::uint64_t watchdogMs = 0ULL;
  std::uint64_t elapsedMs = 0ULL;
  std::string failureDiagnostic;
};
enum class Stage {
  GEOMETRY_EXTRACTION,
  LEVEL_SET,
  RAY_TRACING,
  OXIDATION_LINEAR_SOLVE,
  SURFACE_DIFFUSION,
  CUSTOM,
  // Coverage convergence is an independent process stage. Keep it separate
  // from surface diffusion so per-stage deployment policy can select it on
  // its own; append after existing values to preserve their indices.
  COVERAGE,
  COUNT
};
enum class RayMode { NONE, COMPUTE_BVH, RAY_QUERY, RAY_TRACING_PIPELINE };
enum class Precision { FP32, FP64, MIXED };

namespace detail {
constexpr int toInt(const SelectionMode mode) { return static_cast<int>(mode); }
constexpr int toInt(const ComputeBackend backend) {
  return static_cast<int>(backend);
}
constexpr int toInt(const Stage stage) { return static_cast<int>(stage); }
constexpr int toInt(const RayMode mode) { return static_cast<int>(mode); }
constexpr int toInt(const Precision precision) {
  return static_cast<int>(precision);
}
} // namespace detail

static_assert(detail::toInt(SelectionMode::AUTO) == 0,
              "SelectionMode::AUTO must be zero.");
static_assert(detail::toInt(ComputeBackend::AUTO) == 0,
              "ComputeBackend::AUTO must be zero.");
static_assert(detail::toInt(Stage::GEOMETRY_EXTRACTION) == 0,
              "Stage indexing must be stable.");

static constexpr std::size_t stageCount =
    static_cast<std::size_t>(Stage::COUNT);

[[nodiscard]] inline constexpr std::string_view
toString(const ComputeBackend backend) {
  switch (backend) {
  case ComputeBackend::AUTO:
    return "AUTO";
  case ComputeBackend::CPU:
    return "CPU";
  case ComputeBackend::CUDA:
    return "CUDA";
  case ComputeBackend::VULKAN:
    return "VULKAN";
  }
  return "UNKNOWN";
}

[[nodiscard]] inline constexpr std::string_view
toString(const SelectionMode mode) {
  switch (mode) {
  case SelectionMode::AUTO:
    return "auto";
  case SelectionMode::MANUAL:
    return "manual";
  }
  return "unknown";
}

[[nodiscard]] inline constexpr std::string_view toString(const Stage stage) {
  switch (stage) {
  case Stage::GEOMETRY_EXTRACTION:
    return "geometryExtraction";
  case Stage::LEVEL_SET:
    return "levelSet";
  case Stage::RAY_TRACING:
    return "rayTracing";
  case Stage::OXIDATION_LINEAR_SOLVE:
    return "oxidationLinearSolve";
  case Stage::SURFACE_DIFFUSION:
    return "surfaceDiffusion";
  case Stage::CUSTOM:
    return "custom";
  case Stage::COVERAGE:
    return "coverage";
  case Stage::COUNT:
    return "count";
  }
  return "unknown";
}

[[nodiscard]] inline constexpr std::string_view toString(const RayMode mode) {
  switch (mode) {
  case RayMode::NONE:
    return "none";
  case RayMode::COMPUTE_BVH:
    return "computeBvh";
  case RayMode::RAY_QUERY:
    return "rayQuery";
  case RayMode::RAY_TRACING_PIPELINE:
    return "rayTracingPipeline";
  }
  return "unknown";
}

[[nodiscard]] inline constexpr std::string_view
toString(const Precision precision) {
  switch (precision) {
  case Precision::FP32:
    return "fp32";
  case Precision::FP64:
    return "fp64";
  case Precision::MIXED:
    return "mixed";
  }
  return "unknown";
}

[[nodiscard]] inline constexpr bool requiresFp64(const Precision precision) {
  return precision == Precision::FP64 || precision == Precision::MIXED;
}

struct CapabilityProfile {
  bool cpuAvailable = true;
  bool cudaAvailable = false;

  bool vulkanAvailable = false;
  bool vulkanPrimitiveSuitePass = false;
  bool vulkanFp64SuitePass = false;
  bool vulkanCompute = false;
  bool vulkanRayQuery = false;
  bool vulkanRayTracingPipeline = false;
  bool shaderFloat64 = false;

  VulkanFp32NumericalSmokeEvidence vulkanFp32NumericalSmoke{};

  std::uint64_t safeVulkanWorkingSetBytes = 0;
};

struct StageWorkload {
  Stage stage = Stage::CUSTOM;
  Precision precision = Precision::FP32;
  std::uint64_t estimatedBytes = 0;
  bool requiresHostCallback = false;
  RayMode minimumRayMode = RayMode::NONE;
  bool allowFallback = true;
};

struct ManualSelectionConfig {
  SelectionMode selectionMode = SelectionMode::AUTO;
  ComputeBackend globalBackend = ComputeBackend::AUTO;
  std::array<std::optional<ComputeBackend>, stageCount> perStageBackend{};
  Precision precision = Precision::MIXED;
  RayMode rayMode = RayMode::NONE;
  bool allowStageFallback = false;
  // Auto always requires the validated strict-FP32 contract before it selects
  // Vulkan. A manual Vulkan selection can intentionally use a non-strict
  // route; set this only when the caller explicitly requires that guarantee.
  bool requireStrictFp32NumericalSmoke = false;
};

struct StageEligibility {
  ComputeBackend backend = ComputeBackend::CPU;
  Precision precision = Precision::FP32;
  RayMode resolvedRayMode = RayMode::NONE;
  bool eligible = false;
  std::vector<std::string> reasons;
};

struct StageSelection {
  Stage stage = Stage::CUSTOM;
  SelectionMode resolvedMode = SelectionMode::AUTO;
  ComputeBackend requestedBackend = ComputeBackend::AUTO;
  Precision requestedPrecision = Precision::MIXED;
  RayMode requestedRayMode = RayMode::NONE;
  bool isManualStage = false;
  bool ok = true;

  bool selected = false;
  ComputeBackend selectedBackend = ComputeBackend::CPU;
  Precision selectedPrecision = Precision::FP32;
  RayMode selectedRayMode = RayMode::NONE;

  std::vector<StageEligibility> candidates;
  std::vector<std::string> rejectionReasons;
  std::vector<std::string> selectedReasons;
};

struct SelectionPlan {
  bool ok = true;
  SelectionMode selectionMode = SelectionMode::AUTO;
  Precision precision = Precision::MIXED;
  RayMode rayMode = RayMode::NONE;
  std::vector<StageSelection> stages;
};

namespace detail {

inline bool profileHasBaseVulkanSuite(const CapabilityProfile &profile) {
  return profile.vulkanAvailable && profile.vulkanCompute &&
         profile.vulkanPrimitiveSuitePass;
}

inline bool
profileHasStrictFp32NumericalSmoke(const CapabilityProfile &profile) {
  return profile.vulkanFp32NumericalSmoke.status ==
             VulkanNumericalSmokeStatus::PASS &&
         profile.vulkanFp32NumericalSmoke.contractId ==
             kVulkanFp32NumericalSmokeContract &&
         profile.vulkanFp32NumericalSmoke.caseCount != 0U &&
         profile.vulkanFp32NumericalSmoke.mismatchCount == 0U &&
         profile.vulkanFp32NumericalSmoke.maxUlp == 0U &&
         profile.vulkanFp32NumericalSmoke.watchdogMs ==
             kVulkanFp32NumericalSmokeWatchdogMs &&
         profile.vulkanFp32NumericalSmoke.elapsedMs <=
             profile.vulkanFp32NumericalSmoke.watchdogMs &&
         profile.vulkanFp32NumericalSmoke.failureDiagnostic.empty();
}

inline bool rayModeMeetsMinimum(const RayMode required,
                                const RayMode available) {
  return detail::toInt(available) >= detail::toInt(required);
}

inline RayMode resolveRayMode(const CapabilityProfile &profile,
                              const RayMode requiredMode) {
  if (requiredMode == RayMode::RAY_TRACING_PIPELINE) {
    if (profile.vulkanRayTracingPipeline)
      return RayMode::RAY_TRACING_PIPELINE;
    if (profile.vulkanRayQuery)
      return RayMode::RAY_QUERY;
    if (profile.vulkanCompute)
      return RayMode::COMPUTE_BVH;
    return RayMode::NONE;
  }
  if (requiredMode == RayMode::RAY_QUERY) {
    if (profile.vulkanRayQuery)
      return RayMode::RAY_QUERY;
    if (profile.vulkanCompute)
      return RayMode::COMPUTE_BVH;
    return RayMode::NONE;
  }
  if (requiredMode == RayMode::COMPUTE_BVH) {
    if (profile.vulkanCompute)
      return RayMode::COMPUTE_BVH;
    return RayMode::NONE;
  }
  return RayMode::NONE;
}

inline bool passesHardThresholds(const CapabilityProfile &profile,
                                 const StageWorkload &workload,
                                 const ComputeBackend backend,
                                 const RayMode requestedRayMode,
                                 const bool requireStrictFp32NumericalSmoke,
                                 StageEligibility &eligibility) {
  if (backend == ComputeBackend::AUTO)
    return false;

  if (backend == ComputeBackend::CPU) {
    if (!profile.cpuAvailable) {
      eligibility.reasons.emplace_back("CPU backend disabled by profile.");
      return false;
    }
    eligibility.eligible = true;
    eligibility.resolvedRayMode = RayMode::NONE;
    return true;
  }

  if (backend == ComputeBackend::CUDA) {
    if (!profile.cudaAvailable) {
      eligibility.reasons.emplace_back("CUDA backend not available.");
      return false;
    }
    eligibility.eligible = true;
    eligibility.resolvedRayMode = RayMode::NONE;
    return true;
  }

  if (!profileHasBaseVulkanSuite(profile)) {
    eligibility.reasons.emplace_back("Vulkan compute suite failed.");
    return false;
  }
  if (requireStrictFp32NumericalSmoke &&
      !profileHasStrictFp32NumericalSmoke(profile)) {
    if (profile.vulkanFp32NumericalSmoke.status !=
        VulkanNumericalSmokeStatus::PASS) {
      eligibility.reasons.emplace_back(
          "Vulkan strict FP32 numerical smoke did not pass.");
    } else {
      eligibility.reasons.emplace_back(
          "Vulkan strict FP32 numerical smoke evidence is incompatible.");
    }
    return false;
  }
  if (workload.estimatedBytes > 0 && profile.safeVulkanWorkingSetBytes > 0 &&
      workload.estimatedBytes > profile.safeVulkanWorkingSetBytes) {
    eligibility.reasons.emplace_back(
        "Estimated working set exceeds Vulkan safe budget.");
    return false;
  }
  if (workload.estimatedBytes > 0 && profile.safeVulkanWorkingSetBytes == 0) {
    eligibility.reasons.emplace_back(
        "Vulkan safe working-set budget is unavailable.");
    return false;
  }
  if (workload.requiresHostCallback) {
    eligibility.reasons.emplace_back(
        "Host callback stages are not yet GPU-accelerated.");
    return false;
  }

  const RayMode requiredRayMode = requestedRayMode != RayMode::NONE
                                      ? requestedRayMode
                                      : workload.minimumRayMode;
  if (requiredRayMode != RayMode::NONE) {
    const auto resolvedRayMode = resolveRayMode(profile, requiredRayMode);
    if (resolvedRayMode == RayMode::NONE) {
      eligibility.reasons.emplace_back("Required ray mode is unavailable.");
      return false;
    }
    if (!rayModeMeetsMinimum(requiredRayMode, resolvedRayMode)) {
      eligibility.reasons.emplace_back(
          "Resolved ray mode is below minimum requirement.");
      return false;
    }
    eligibility.resolvedRayMode = resolvedRayMode;
  }

  if (requiresFp64(workload.precision)) {
    if (!profile.shaderFloat64 || !profile.vulkanFp64SuitePass) {
      eligibility.reasons.emplace_back(
          "FP64 stage requires validated Vulkan fp64 capability.");
      return false;
    }
  }
  eligibility.eligible = true;
  return true;
}

inline StageSelection selectStage(const CapabilityProfile &profile,
                                  const ManualSelectionConfig &config,
                                  const StageWorkload &workload) {
  StageSelection selection;
  StageWorkload stageWorkload = workload;
  if (config.precision != Precision::MIXED) {
    stageWorkload.precision = config.precision;
  }

  selection.stage = stageWorkload.stage;
  selection.selectedPrecision = stageWorkload.precision;
  selection.resolvedMode = config.selectionMode;
  selection.requestedPrecision = stageWorkload.precision;
  selection.requestedRayMode = config.rayMode;

  const auto idx = static_cast<std::size_t>(stageWorkload.stage);
  ComputeBackend requestedBackend = config.globalBackend;
  if (config.selectionMode == SelectionMode::MANUAL) {
    requestedBackend =
        config.perStageBackend.at(idx).value_or(config.globalBackend);
    selection.isManualStage = true;
  }

  selection.requestedBackend = requestedBackend;
  selection.selected = false;

  if (selection.isManualStage) {
    StageEligibility manualEligibility;
    manualEligibility.backend = requestedBackend;
    manualEligibility.precision = stageWorkload.precision;
    if (requestedBackend == ComputeBackend::AUTO) {
      selection.ok = false;
      selection.rejectionReasons.emplace_back(
          "Manual mode requires an explicit backend.");
      return selection;
    }

    if (passesHardThresholds(
            profile, stageWorkload, requestedBackend, config.rayMode,
            config.requireStrictFp32NumericalSmoke, manualEligibility)) {
      selection.selected = true;
      selection.selectedBackend = requestedBackend;
      selection.selectedRayMode = manualEligibility.resolvedRayMode;
      selection.ok = true;
      selection.selectedReasons.emplace_back(
          std::string("Manual backend accepted: ") +
          std::string(toString(requestedBackend)));
      if (requestedBackend == ComputeBackend::VULKAN &&
          !config.requireStrictFp32NumericalSmoke &&
          !profileHasStrictFp32NumericalSmoke(profile)) {
        selection.selectedReasons.emplace_back(
            "Manual Vulkan selected without strict FP32 numerical guarantee.");
      }
      if (requestedBackend == ComputeBackend::VULKAN &&
          config.rayMode != RayMode::NONE) {
        selection.selectedReasons.emplace_back(
            std::string("Manual ray mode: ") +
            std::string(toString(config.rayMode)));
      }
      return selection;
    }

    selection.candidates.push_back(manualEligibility);
    selection.ok = false;
    if (!manualEligibility.reasons.empty()) {
      selection.rejectionReasons.emplace_back(
          std::string("Manual backend blocked: ") +
          manualEligibility.reasons.front());
    }
    if (stageWorkload.allowFallback && config.allowStageFallback) {
      ManualSelectionConfig fallbackConfig = config;
      fallbackConfig.selectionMode = SelectionMode::AUTO;
      selection = detail::selectStage(profile, fallbackConfig, stageWorkload);
      selection.rejectionReasons.push_back(
          "Manual selection failed; fallback to auto mode enabled.");
    }
    return selection;
  }

  std::vector<StageEligibility> candidates;
  const auto addCandidate = [&](const ComputeBackend candidateBackend,
                                const RayMode candidateRayMode) {
    StageEligibility candidate;
    candidate.backend = candidateBackend;
    candidate.precision = stageWorkload.precision;
    passesHardThresholds(profile, stageWorkload, candidateBackend,
                         candidateRayMode, true, candidate);
    candidates.push_back(candidate);
  };

  addCandidate(ComputeBackend::VULKAN, stageWorkload.minimumRayMode);
  addCandidate(ComputeBackend::CUDA, stageWorkload.minimumRayMode);
  addCandidate(ComputeBackend::CPU, RayMode::NONE);

  int bestScore = -1;
  StageEligibility bestCandidate;
  for (const auto &candidate : candidates) {
    if (!candidate.eligible)
      continue;
    int score = 0;
    if (candidate.backend == ComputeBackend::CPU)
      score = 1;
    else if (candidate.backend == ComputeBackend::CUDA)
      score = 2;
    else if (candidate.backend == ComputeBackend::VULKAN)
      score = 3;
    if (score > bestScore) {
      bestScore = score;
      bestCandidate = candidate;
    }
  }

  if (bestScore > 0) {
    selection.selected = true;
    selection.ok = true;
    selection.selectedBackend = bestCandidate.backend;
    selection.selectedRayMode = bestCandidate.resolvedRayMode;
    selection.selectedReasons.push_back(
        std::string("Selected by hard-threshold and preference score: ") +
        std::string(toString(bestCandidate.backend)));
    if (bestCandidate.backend == ComputeBackend::VULKAN &&
        stageWorkload.minimumRayMode != RayMode::NONE &&
        bestCandidate.resolvedRayMode != RayMode::NONE) {
      selection.selectedReasons.push_back(
          std::string("Resolved ray mode: ") +
          std::string(toString(bestCandidate.resolvedRayMode)));
    }
  } else {
    selection.ok = false;
    if (profile.cpuAvailable) {
      selection.selected = true;
      selection.selectedBackend = ComputeBackend::CPU;
      selection.selectedRayMode = RayMode::NONE;
      selection.ok = true;
      selection.selectedReasons.push_back("Fallback to CPU.");
    }
  }

  for (const auto &candidate : candidates) {
    for (const auto &reason : candidate.reasons) {
      selection.rejectionReasons.push_back(reason);
    }
  }
  selection.candidates = candidates;
  if (!selection.selected) {
    selection.rejectionReasons.emplace_back("No eligible backend found.");
  }

  return selection;
}

} // namespace detail

[[nodiscard]] inline SelectionPlan
buildSelectionPlan(const CapabilityProfile &profile,
                   const std::vector<StageWorkload> &workloads,
                   const ManualSelectionConfig &config = {}) {
  SelectionPlan plan;
  plan.selectionMode = config.selectionMode;
  plan.precision = config.precision;
  plan.rayMode = config.rayMode;

  for (const auto &stage : workloads) {
    auto stageSelection = detail::selectStage(profile, config, stage);
    if (!stageSelection.ok) {
      plan.ok = false;
    }
    plan.stages.push_back(std::move(stageSelection));
  }
  return plan;
}

} // namespace viennaps::compute
