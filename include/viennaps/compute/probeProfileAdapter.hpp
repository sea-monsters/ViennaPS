#pragma once

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <ctime>
#include <iomanip>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "backendPolicy.hpp"
#include "capabilityProfileIO.hpp"

namespace viennaps::compute {

namespace detail {
constexpr std::uint64_t kVulkanSafeBudgetHeadroomBytes =
    64ULL * 1024ULL * 1024ULL;
constexpr std::uint64_t kVulkanSafeBudgetMinBytes = 128ULL * 1024ULL * 1024ULL;
constexpr std::uint64_t kVulkanSafeBudgetMaxBytes =
    4ULL * 1024ULL * 1024ULL * 1024ULL;
constexpr std::string_view kVulkanProbeUnknownDriverDate = "unknown";
} // namespace detail

enum class VulkanProbeSuiteStatus {
  NOT_RUN = 0,
  PASS,
  FAIL,
};

struct VulkanProbeValidationEvidence {
  VulkanProbeSuiteStatus primitiveSuite = VulkanProbeSuiteStatus::NOT_RUN;
  VulkanProbeSuiteStatus fp32Suite = VulkanProbeSuiteStatus::NOT_RUN;
  VulkanProbeSuiteStatus fp64Suite = VulkanProbeSuiteStatus::NOT_RUN;
  VulkanFp32NumericalSmokeEvidence fp32NumericalSmoke{};
};

struct VulkanProbeDeviceFacts {
  HardwareFingerprint hardware{};
  bool supportsVulkan = false;
  bool hasDedicatedComputeQueue = false;
  bool supportsComputeQueue = false;
  bool supportsRayQuery = false;
  bool supportsRayTracingPipeline = false;
  bool supportsShaderFloat64 = false;
  bool memoryBudgetExtensionAvailable = false;
  std::uint64_t deviceLocalBytes = 0;
  std::uint64_t hostVisibleBytes = 0;
  std::vector<std::uint64_t> memoryBudgetBytes{};
  VulkanProbeValidationEvidence validationEvidence{};
};

struct VulkanProfileAdapterResult {
  bool ok = false;
  CapabilityProfileRecord record{};
  std::string message;
};

[[nodiscard]] inline std::string probeUtcTimestamp() {
  const auto now = std::chrono::system_clock::now();
  const auto utc_time = std::chrono::floor<std::chrono::seconds>(now);
  const auto now_c = std::chrono::system_clock::to_time_t(utc_time);
  std::tm utc_tm{};
#if defined(_WIN32)
  gmtime_s(&utc_tm, &now_c);
#else
  gmtime_r(&now_c, &utc_tm);
#endif
  std::ostringstream out;
  out << std::put_time(&utc_tm, "%Y-%m-%dT%H:%M:%SZ");
  return out.str();
}

[[nodiscard]] inline std::uint64_t
deriveSafeVulkanWorkingSetBytes(std::span<const std::uint64_t> budgets,
                                const bool hasBudgetExtension,
                                std::string *message) {
  if (!hasBudgetExtension) {
    if (message) {
      *message = "Vulkan memory budget extension unavailable; safe budget is "
                 "unknown so Vulkan working-set limit is set to 0.";
    }
    return 0;
  }

  if (budgets.empty()) {
    if (message) {
      *message =
          "Vulkan memory budget array is empty; safe budget cannot be derived.";
    }
    return 0;
  }

  std::uint64_t largestAvailableBudget = 0;
  for (const auto budget : budgets) {
    largestAvailableBudget = std::max(largestAvailableBudget, budget);
  }

  const auto afterHeadroom =
      largestAvailableBudget > detail::kVulkanSafeBudgetHeadroomBytes
          ? largestAvailableBudget - detail::kVulkanSafeBudgetHeadroomBytes
          : 0ULL;
  const auto safeBudget = afterHeadroom - (afterHeadroom / 10ULL);

  if (safeBudget < detail::kVulkanSafeBudgetMinBytes) {
    if (message) {
      *message = "Conservative Vulkan budget is below minimum threshold; "
                 "set to 0 for fail-closed behavior.";
    }
    return 0;
  }

  if (safeBudget > detail::kVulkanSafeBudgetMaxBytes) {
    if (message) {
      *message = "Conservative Vulkan budget exceeded max cap; clamp applied.";
    }
    return detail::kVulkanSafeBudgetMaxBytes;
  }
  return safeBudget;
}

[[nodiscard]] inline VulkanProfileAdapterResult
adaptVulkanProbeFactsToCapabilityProfile(const VulkanProbeDeviceFacts &facts,
                                         const std::string &recordedAt = "") {
  VulkanProfileAdapterResult result;
  if (!facts.supportsVulkan) {
    result.message = "Vulkan is not enabled on this probe run.";
    return result;
  }

  if (facts.hardware.deviceUuid.empty() || facts.hardware.driverUuid.empty() ||
      facts.hardware.deviceName.empty() ||
      facts.hardware.driverVersion.empty()) {
    result.message = "Hardware fingerprint is incomplete.";
    return result;
  }

  result.record.recordedAt =
      recordedAt.empty() ? probeUtcTimestamp() : recordedAt;
  result.record.hardware.deviceUuid = facts.hardware.deviceUuid;
  result.record.hardware.driverUuid = facts.hardware.driverUuid;
  result.record.hardware.vendorId = facts.hardware.vendorId;
  result.record.hardware.deviceId = facts.hardware.deviceId;
  result.record.hardware.deviceName = facts.hardware.deviceName;
  result.record.hardware.driverVersion = facts.hardware.driverVersion;
  result.record.hardware.driverDate =
      facts.hardware.driverDate.empty()
          ? std::string(detail::kVulkanProbeUnknownDriverDate)
          : facts.hardware.driverDate;

  auto &profile = result.record.capabilityProfile;
  profile.cpuAvailable = true;
  profile.vulkanAvailable = facts.supportsComputeQueue;
  profile.vulkanPrimitiveSuitePass =
      facts.validationEvidence.primitiveSuite == VulkanProbeSuiteStatus::PASS &&
      facts.validationEvidence.fp32Suite == VulkanProbeSuiteStatus::PASS;
  profile.vulkanFp64SuitePass =
      facts.supportsShaderFloat64 &&
      facts.validationEvidence.fp64Suite == VulkanProbeSuiteStatus::PASS;
  profile.vulkanCompute = facts.supportsComputeQueue;
  profile.vulkanRayQuery = facts.supportsRayQuery;
  profile.vulkanRayTracingPipeline = facts.supportsRayTracingPipeline;
  profile.shaderFloat64 = facts.supportsShaderFloat64;
  profile.vulkanFp32NumericalSmoke =
      facts.validationEvidence.fp32NumericalSmoke;
  profile.safeVulkanWorkingSetBytes = deriveSafeVulkanWorkingSetBytes(
      facts.memoryBudgetBytes, facts.memoryBudgetExtensionAvailable,
      &result.message);

  result.ok = true;
  return result;
}

} // namespace viennaps::compute
