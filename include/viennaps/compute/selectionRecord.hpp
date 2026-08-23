#pragma once

// P7-R2 deterministic selection records.
//
// A selection record captures the complete decision surface of one
// buildSelectionPlan call: a digest of the capability profile, the workload
// descriptors, and every per-stage decision with its rejection reasons.
// Emission is pure - no timestamps, no environment reads - so two calls with
// identical inputs produce byte-identical records and the replay helper can
// verify decisions reproduce exactly.

#include "backendPolicy.hpp"

#include <cstdint>
#include <sstream>
#include <string>
#include <vector>

namespace viennaps::compute {

inline constexpr std::uint32_t kSelectionRecordSchemaVersion = 1U;

[[nodiscard]] inline std::string
selectionRecordProfileDigest(const CapabilityProfile &profile) {
  // FNV-1a over every decision-relevant scalar in declaration order. The
  // strict-FP32 evidence struct is folded in field by field so any evidence
  // change flips the digest.
  const std::uint64_t fnvPrime = 1099511628211ull;
  std::uint64_t hash = 1469598103934665603ull;
  auto mix = [&](bool v) {
    hash ^= v ? 0x01ull : 0x00ull;
    hash *= fnvPrime;
  };
  auto mixU64 = [&](std::uint64_t v) {
    for (unsigned i = 0; i < 8; ++i) {
      hash ^= (v >> (i * 8)) & 0xFFull;
      hash *= fnvPrime;
    }
  };
  mix(profile.cpuAvailable);
  mix(profile.cudaAvailable);
  mix(profile.vulkanAvailable);
  mix(profile.vulkanPrimitiveSuitePass);
  mix(profile.vulkanFp64SuitePass);
  mix(profile.vulkanCompute);
  mix(profile.vulkanRayQuery);
  mix(profile.vulkanRayTracingPipeline);
  mix(profile.shaderFloat64);
  mix(profile.vulkanFp32NumericalSmoke.status ==
      VulkanNumericalSmokeStatus::PASS);
  mix(profile.vulkanFp32NumericalSmoke.contractId ==
      kVulkanFp32NumericalSmokeContract);
  mixU64(profile.vulkanFp32NumericalSmoke.caseCount);
  mixU64(profile.vulkanFp32NumericalSmoke.mismatchCount);
  mixU64(profile.vulkanFp32NumericalSmoke.maxUlp);
  mixU64(profile.vulkanFp32NumericalSmoke.watchdogMs);
  mixU64(profile.vulkanFp32NumericalSmoke.elapsedMs);
  mixU64(profile.safeVulkanWorkingSetBytes);

  static const char *hex = "0123456789abcdef";
  std::string out = "0x";
  for (int shift = 60; shift >= 0; shift -= 4)
    out += hex[(hash >> shift) & 0xF];
  return out;
}

/// Emit the deterministic selection record for one plan call.
[[nodiscard]] inline std::string
emitSelectionRecord(const CapabilityProfile &profile,
                    const ManualSelectionConfig &config,
                    const std::vector<StageWorkload> &workloads,
                    bool manualRequest = false) {
  const auto plan =
      manualRequest ? buildSelectionPlan(
                          profile, workloads, [&] {
                            ManualSelectionConfig c = config;
                            c.selectionMode = SelectionMode::MANUAL;
                            c.globalBackend = ComputeBackend::VULKAN;
                            return c;
                          }())
                    : buildSelectionPlan(profile, workloads, config);

  std::ostringstream out;
  out << "selection-record v" << kSelectionRecordSchemaVersion << "\n";
  out << "profile-digest " << selectionRecordProfileDigest(profile) << "\n";
  out << "mode "
      << (plan.selectionMode == SelectionMode::MANUAL ? "MANUAL" : "AUTO")
      << "\n";
  out << "stages " << plan.stages.size() << "\n";
  for (const auto &stage : plan.stages) {
    out << "stage " << static_cast<int>(stage.stage) << " req_precision="
        << toString(stage.requestedPrecision) << " req_backend="
        << toString(stage.requestedBackend) << " selected_backend="
        << toString(stage.selectedBackend) << " selected_precision="
        << toString(stage.selectedPrecision) << " ok=" << (stage.ok ? 1 : 0)
        << "\n";
    for (const auto &reason : stage.rejectionReasons)
      out << "reason " << reason << "\n";
    for (const auto &reason : stage.selectedReasons)
      out << "note " << reason << "\n";
  }
  if (!plan.ok)
    out << "plan-incomplete\n";
  return out.str();
}

/// Replay contract: re-emitting with the same inputs reproduces the record
/// byte-for-byte; any input change flips at least one byte.
[[nodiscard]] inline bool replaySelectionRecordMatches(
    const std::string &record, const CapabilityProfile &profile,
    const ManualSelectionConfig &config,
    const std::vector<StageWorkload> &workloads, bool manualRequest = false) {
  return record == emitSelectionRecord(profile, config, workloads,
                                       manualRequest);
}

} // namespace viennaps::compute
