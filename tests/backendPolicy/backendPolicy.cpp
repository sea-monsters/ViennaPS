#include <cstddef>
#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

#include <compute/backendPolicy.hpp>
#include <compute/capabilityProfileIO.hpp>
#include <vcTestAsserts.hpp>

namespace viennacore {

using namespace viennaps::compute;

void enableValidatedFp32Smoke(CapabilityProfile &profile) {
  profile.vulkanFp32NumericalSmoke.status = VulkanNumericalSmokeStatus::PASS;
  profile.vulkanFp32NumericalSmoke.contractId =
      std::string(kVulkanFp32NumericalSmokeContract);
  profile.vulkanFp32NumericalSmoke.caseCount = 1U;
  profile.vulkanFp32NumericalSmoke.maxUlp = 0U;
  profile.vulkanFp32NumericalSmoke.watchdogMs =
      kVulkanFp32NumericalSmokeWatchdogMs;
}

void TestManualOverrideCanForceCPU() {
  CapabilityProfile profile{};
  profile.cpuAvailable = true;
  profile.cudaAvailable = false;
  profile.vulkanAvailable = false;

  ManualSelectionConfig config{};
  config.selectionMode = SelectionMode::MANUAL;
  config.globalBackend = ComputeBackend::CPU;
  config.precision = Precision::FP32;

  std::vector<StageWorkload> workloads = {{Stage::GEOMETRY_EXTRACTION,
                                           Precision::FP64, 2048, false,
                                           RayMode::NONE, true}};

  const auto plan = buildSelectionPlan(profile, workloads, config);
  VC_TEST_ASSERT(plan.ok);
  VC_TEST_ASSERT(plan.stages.size() == 1);
  VC_TEST_ASSERT(plan.stages[0].requestedBackend == ComputeBackend::CPU);
  VC_TEST_ASSERT(plan.stages[0].selected);
  VC_TEST_ASSERT(plan.stages[0].selectedBackend == ComputeBackend::CPU);
  VC_TEST_ASSERT(plan.stages[0].ok);
  VC_TEST_ASSERT(plan.stages[0].selectedRayMode == RayMode::NONE);
}

void TestManualPerStageOverrideIsHonored() {
  CapabilityProfile profile{};
  profile.cpuAvailable = true;
  profile.cudaAvailable = true;
  profile.vulkanAvailable = false;

  ManualSelectionConfig config{};
  config.selectionMode = SelectionMode::MANUAL;
  config.globalBackend = ComputeBackend::CUDA;
  config.perStageBackend[static_cast<std::size_t>(Stage::LEVEL_SET)] =
      ComputeBackend::CPU;

  std::vector<StageWorkload> workloads = {
      {Stage::LEVEL_SET, Precision::FP32, 1024, false, RayMode::NONE, true}};

  const auto plan = buildSelectionPlan(profile, workloads, config);
  VC_TEST_ASSERT(plan.ok);
  VC_TEST_ASSERT(plan.stages[0].selectedBackend == ComputeBackend::CPU);
  VC_TEST_ASSERT(plan.stages[0].requestedBackend == ComputeBackend::CPU);
}

void TestManualOverrideFailsWhenBackendUnavailable() {
  CapabilityProfile profile{};
  profile.cpuAvailable = true;
  profile.cudaAvailable = false;
  profile.vulkanAvailable = false;

  ManualSelectionConfig config{};
  config.selectionMode = SelectionMode::MANUAL;
  config.globalBackend = ComputeBackend::CUDA;
  config.precision = Precision::FP32;

  std::vector<StageWorkload> workloads = {
      {Stage::LEVEL_SET, Precision::FP32, 1024, false, RayMode::NONE, true}};

  const auto plan = buildSelectionPlan(profile, workloads, config);
  VC_TEST_ASSERT(!plan.ok);
  VC_TEST_ASSERT(plan.stages[0].requestedBackend == ComputeBackend::CUDA);
  VC_TEST_ASSERT(!plan.stages[0].selected);
  VC_TEST_ASSERT(!plan.stages[0].ok);
  VC_TEST_ASSERT(!plan.stages[0].rejectionReasons.empty());
  VC_TEST_ASSERT(plan.stages[0].rejectionReasons.front() ==
                 "Manual backend blocked: CUDA backend not available.");
}

void TestAutoFallsBackToCPUWhenOnlyCPUAvailable() {
  CapabilityProfile profile{};
  profile.cpuAvailable = true;
  profile.cudaAvailable = false;
  profile.vulkanAvailable = false;

  std::vector<StageWorkload> workloads = {{Stage::GEOMETRY_EXTRACTION,
                                           Precision::FP32, 1024, false,
                                           RayMode::NONE, true}};

  const auto plan =
      buildSelectionPlan(profile, workloads, ManualSelectionConfig{});
  VC_TEST_ASSERT(plan.ok);
  VC_TEST_ASSERT(plan.stages[0].selected);
  VC_TEST_ASSERT(plan.stages[0].selectedBackend == ComputeBackend::CPU);
  VC_TEST_ASSERT(plan.stages[0].selectedReasons[0] ==
                 "Selected by hard-threshold and preference score: CPU");
}

void TestAutoPrefersVulkanWhenCapabilitiesPass() {
  CapabilityProfile profile{};
  profile.cpuAvailable = true;
  profile.cudaAvailable = true;
  profile.vulkanAvailable = true;
  profile.vulkanPrimitiveSuitePass = true;
  profile.vulkanFp64SuitePass = true;
  profile.vulkanCompute = true;
  profile.vulkanRayQuery = true;
  profile.vulkanRayTracingPipeline = true;
  profile.shaderFloat64 = true;
  profile.safeVulkanWorkingSetBytes = 1024 * 1024 * 1024ULL;
  enableValidatedFp32Smoke(profile);

  std::vector<StageWorkload> workloads = {{Stage::RAY_TRACING, Precision::FP32,
                                           1024 * 1024ULL, false, RayMode::NONE,
                                           true}};

  const auto plan =
      buildSelectionPlan(profile, workloads, ManualSelectionConfig{});
  VC_TEST_ASSERT(plan.ok);
  VC_TEST_ASSERT(plan.stages[0].selected);
  VC_TEST_ASSERT(plan.stages[0].selectedBackend == ComputeBackend::VULKAN);
}

void TestCoverageStageCanBeSelectedIndependently() {
  CapabilityProfile profile{};
  profile.cpuAvailable = true;
  profile.vulkanAvailable = true;
  profile.vulkanPrimitiveSuitePass = true;
  profile.vulkanCompute = true;
  profile.safeVulkanWorkingSetBytes = 1024ULL * 1024ULL;
  enableValidatedFp32Smoke(profile);

  const std::vector<StageWorkload> workloads = {
      {Stage::COVERAGE, Precision::FP32, 1024U, false, RayMode::NONE, true}};
  const auto plan = buildSelectionPlan(profile, workloads);
  VC_TEST_ASSERT(plan.ok);
  VC_TEST_ASSERT(plan.stages.size() == 1U);
  VC_TEST_ASSERT(plan.stages[0].stage == Stage::COVERAGE);
  VC_TEST_ASSERT(plan.stages[0].selectedBackend == ComputeBackend::VULKAN);
  VC_TEST_ASSERT(toString(Stage::COVERAGE) == "coverage");
}

void TestNeutralTransportVelocityStagePolicy() {
  CapabilityProfile profile{};
  profile.cpuAvailable = true;
  profile.vulkanAvailable = true;
  profile.vulkanPrimitiveSuitePass = true;
  profile.vulkanCompute = true;
  profile.safeVulkanWorkingSetBytes = 1024ULL * 1024ULL;
  enableValidatedFp32Smoke(profile);

  const std::vector<StageWorkload> workloads = {
      {Stage::NEUTRAL_TRANSPORT_VELOCITY, Precision::FP32, 1024U, false,
       RayMode::NONE, true}};
  const auto automaticPlan = buildSelectionPlan(profile, workloads);
  VC_TEST_ASSERT(automaticPlan.ok);
  VC_TEST_ASSERT(automaticPlan.stages[0].selectedBackend ==
                 ComputeBackend::VULKAN);
  VC_TEST_ASSERT(toString(Stage::NEUTRAL_TRANSPORT_VELOCITY) ==
                 "neutralTransportVelocity");

  ManualSelectionConfig manualConfig{};
  manualConfig.selectionMode = SelectionMode::MANUAL;
  manualConfig.globalBackend = ComputeBackend::CPU;
  manualConfig.perStageBackend[static_cast<std::size_t>(
      Stage::NEUTRAL_TRANSPORT_VELOCITY)] = ComputeBackend::VULKAN;
  const auto manualVulkanPlan =
      buildSelectionPlan(profile, workloads, manualConfig);
  VC_TEST_ASSERT(manualVulkanPlan.ok);
  VC_TEST_ASSERT(manualVulkanPlan.stages[0].requestedBackend ==
                 ComputeBackend::VULKAN);
  VC_TEST_ASSERT(manualVulkanPlan.stages[0].selectedBackend ==
                 ComputeBackend::VULKAN);

  manualConfig.globalBackend = ComputeBackend::VULKAN;
  manualConfig.perStageBackend[static_cast<std::size_t>(
      Stage::NEUTRAL_TRANSPORT_VELOCITY)] = ComputeBackend::CPU;
  const auto manualCpuPlan =
      buildSelectionPlan(profile, workloads, manualConfig);
  VC_TEST_ASSERT(manualCpuPlan.ok);
  VC_TEST_ASSERT(manualCpuPlan.stages[0].requestedBackend ==
                 ComputeBackend::CPU);
  VC_TEST_ASSERT(manualCpuPlan.stages[0].selectedBackend == ComputeBackend::CPU);
}

void TestAutoUsesTheLowestValidatedRayTier() {
  CapabilityProfile profile{};
  profile.cpuAvailable = true;
  profile.cudaAvailable = false;
  profile.vulkanAvailable = true;
  profile.vulkanPrimitiveSuitePass = true;
  profile.vulkanFp64SuitePass = true;
  profile.vulkanCompute = true;
  profile.vulkanRayQuery = false;
  profile.vulkanRayTracingPipeline = false;
  profile.shaderFloat64 = true;
  profile.safeVulkanWorkingSetBytes = 1024 * 1024 * 1024ULL;
  enableValidatedFp32Smoke(profile);

  ManualSelectionConfig config{};
  std::vector<StageWorkload> workloads = {{Stage::RAY_TRACING, Precision::FP32,
                                           1024 * 1024ULL, false,
                                           RayMode::COMPUTE_BVH, true}};

  const auto plan = buildSelectionPlan(profile, workloads, config);
  VC_TEST_ASSERT(plan.ok);
  VC_TEST_ASSERT(plan.stages[0].selectedBackend == ComputeBackend::VULKAN);
  VC_TEST_ASSERT(plan.stages[0].selectedRayMode == RayMode::COMPUTE_BVH);
}

void TestVulkanRejectsFP64WithoutSupportThenFallsBackCPU() {
  CapabilityProfile profile{};
  profile.cpuAvailable = true;
  profile.cudaAvailable = false;
  profile.vulkanAvailable = true;
  profile.vulkanPrimitiveSuitePass = true;
  profile.vulkanFp64SuitePass = false;
  profile.vulkanCompute = true;
  profile.shaderFloat64 = false;
  profile.safeVulkanWorkingSetBytes = 1024ULL * 1024ULL * 1024ULL;
  enableValidatedFp32Smoke(profile);

  std::vector<StageWorkload> workloads = {{Stage::OXIDATION_LINEAR_SOLVE,
                                           Precision::FP64, 1ULL << 12, false,
                                           RayMode::NONE, true}};

  const auto plan =
      buildSelectionPlan(profile, workloads, ManualSelectionConfig{});
  VC_TEST_ASSERT(plan.ok);
  VC_TEST_ASSERT(plan.stages[0].selected);
  VC_TEST_ASSERT(plan.stages[0].selectedBackend == ComputeBackend::CPU);
  VC_TEST_ASSERT(plan.stages[0].selectedReasons.size() == 1);
  VC_TEST_ASSERT(plan.stages[0].selectedReasons[0] ==
                 "Selected by hard-threshold and preference score: CPU");
}

void TestAutoRejectsVulkanWhenMemoryBudgetIsUnknown() {
  CapabilityProfile profile{};
  profile.cpuAvailable = true;
  profile.vulkanAvailable = true;
  profile.vulkanPrimitiveSuitePass = true;
  profile.vulkanCompute = true;
  profile.safeVulkanWorkingSetBytes = 0;
  enableValidatedFp32Smoke(profile);

  const std::vector<StageWorkload> workloads = {
      {Stage::LEVEL_SET, Precision::FP32, 4096, false, RayMode::NONE, true}};

  const auto plan = buildSelectionPlan(profile, workloads);
  VC_TEST_ASSERT(plan.ok);
  VC_TEST_ASSERT(plan.stages[0].selectedBackend == ComputeBackend::CPU);
}

void TestVulkanRejectsWorkloadAboveSafeMemoryBudget() {
  CapabilityProfile profile{};
  profile.cpuAvailable = true;
  profile.vulkanAvailable = true;
  profile.vulkanPrimitiveSuitePass = true;
  profile.vulkanCompute = true;
  profile.safeVulkanWorkingSetBytes = 4096;
  enableValidatedFp32Smoke(profile);

  const std::vector<StageWorkload> workloads = {
      {Stage::LEVEL_SET, Precision::FP32, 4097, false, RayMode::NONE, false}};

  const auto automaticPlan = buildSelectionPlan(profile, workloads);
  VC_TEST_ASSERT(automaticPlan.ok);
  VC_TEST_ASSERT(automaticPlan.stages[0].selectedBackend ==
                 ComputeBackend::CPU);
  VC_TEST_ASSERT(!automaticPlan.stages[0].rejectionReasons.empty());
  VC_TEST_ASSERT(automaticPlan.stages[0].rejectionReasons.front() ==
                 "Estimated working set exceeds Vulkan safe budget.");

  ManualSelectionConfig manualConfig{};
  manualConfig.selectionMode = SelectionMode::MANUAL;
  manualConfig.globalBackend = ComputeBackend::VULKAN;
  const auto manualPlan = buildSelectionPlan(profile, workloads, manualConfig);
  VC_TEST_ASSERT(!manualPlan.ok);
  VC_TEST_ASSERT(!manualPlan.stages[0].selected);
  VC_TEST_ASSERT(!manualPlan.stages[0].rejectionReasons.empty());
  VC_TEST_ASSERT(manualPlan.stages[0].rejectionReasons.front() ==
                 "Manual backend blocked: Estimated working set exceeds "
                 "Vulkan safe budget.");
}

void TestManualRayTierCannotSilentlyDowngrade() {
  CapabilityProfile profile{};
  profile.cpuAvailable = true;
  profile.vulkanAvailable = true;
  profile.vulkanPrimitiveSuitePass = true;
  profile.vulkanCompute = true;
  profile.safeVulkanWorkingSetBytes = 1024 * 1024;
  enableValidatedFp32Smoke(profile);

  ManualSelectionConfig config{};
  config.selectionMode = SelectionMode::MANUAL;
  config.globalBackend = ComputeBackend::VULKAN;
  config.rayMode = RayMode::RAY_TRACING_PIPELINE;

  const std::vector<StageWorkload> workloads = {{Stage::RAY_TRACING,
                                                 Precision::FP32, 4096, false,
                                                 RayMode::COMPUTE_BVH, false}};

  const auto plan = buildSelectionPlan(profile, workloads, config);
  VC_TEST_ASSERT(!plan.ok);
  VC_TEST_ASSERT(!plan.stages[0].selected);
  VC_TEST_ASSERT(!plan.stages[0].rejectionReasons.empty());
}

void TestManualOverrideAlwaysWinsOverAutoRanking() {
  CapabilityProfile profile{};
  profile.cpuAvailable = true;
  profile.cudaAvailable = true;
  profile.vulkanAvailable = true;
  profile.vulkanPrimitiveSuitePass = true;
  profile.vulkanFp64SuitePass = true;
  profile.vulkanCompute = true;
  profile.vulkanRayQuery = true;
  profile.vulkanRayTracingPipeline = true;
  profile.shaderFloat64 = true;
  profile.safeVulkanWorkingSetBytes = 1024ULL * 1024ULL * 1024ULL;
  enableValidatedFp32Smoke(profile);

  ManualSelectionConfig config{};
  config.selectionMode = SelectionMode::MANUAL;
  config.globalBackend = ComputeBackend::CPU;
  config.precision = Precision::FP32;

  const std::vector<StageWorkload> workloads = {
      {Stage::RAY_TRACING, Precision::FP32, 1024ULL * 1024ULL, false,
       RayMode::NONE, true}};

  const auto plan = buildSelectionPlan(profile, workloads, config);
  VC_TEST_ASSERT(plan.ok);
  VC_TEST_ASSERT(plan.stages[0].selected);
  VC_TEST_ASSERT(plan.stages[0].requestedBackend == ComputeBackend::CPU);
  VC_TEST_ASSERT(plan.stages[0].selectedBackend == ComputeBackend::CPU);
  VC_TEST_ASSERT(!plan.stages[0].selectedReasons.empty());
  VC_TEST_ASSERT(plan.stages[0].selectedReasons[0] ==
                 "Manual backend accepted: CPU");
}

void TestCapabilityProfileFingerprintMismatchInvalidatesCache() {
  CapabilityProfileRecord cachedProfile;
  cachedProfile.hardware.deviceUuid = "DEV-001";
  cachedProfile.hardware.driverUuid = "DRV-001";
  cachedProfile.hardware.vendorId = 1000;
  cachedProfile.hardware.deviceId = 2000;
  cachedProfile.hardware.deviceName = "cached-device";
  cachedProfile.hardware.driverVersion = "1.0.0";
  cachedProfile.hardware.driverDate = "2026-01-01";

  HardwareFingerprint runtimeFingerprint = cachedProfile.hardware;
  VC_TEST_ASSERT(!isCapabilityProfileHardwareFingerprintStale(
      cachedProfile, runtimeFingerprint));

  runtimeFingerprint.deviceUuid = "DEV-002";
  VC_TEST_ASSERT(isCapabilityProfileHardwareFingerprintStale(
      cachedProfile, runtimeFingerprint));

  runtimeFingerprint.deviceUuid = cachedProfile.hardware.deviceUuid;
  runtimeFingerprint.driverUuid = "DRV-002";
  VC_TEST_ASSERT(isCapabilityProfileHardwareFingerprintStale(
      cachedProfile, runtimeFingerprint));
}

void TestAutoRequiresStrictFp32NumericalSmoke() {
  CapabilityProfile profile{};
  profile.cpuAvailable = true;
  profile.vulkanAvailable = true;
  profile.vulkanCompute = true;
  profile.vulkanPrimitiveSuitePass = true;
  profile.safeVulkanWorkingSetBytes = 1024ULL * 1024ULL * 1024ULL;

  const std::vector<StageWorkload> workloads = {
      {Stage::LEVEL_SET, Precision::FP32, 1024ULL, false, RayMode::NONE, true}};
  auto plan = buildSelectionPlan(profile, workloads);
  VC_TEST_ASSERT(plan.ok);
  VC_TEST_ASSERT(plan.stages[0].selectedBackend == ComputeBackend::CPU);

  profile.vulkanFp32NumericalSmoke.status = VulkanNumericalSmokeStatus::PASS;
  profile.vulkanFp32NumericalSmoke.contractId =
      std::string(kVulkanFp32NumericalSmokeContract);
  profile.vulkanFp32NumericalSmoke.caseCount = 1U;
  profile.vulkanFp32NumericalSmoke.maxUlp = 0U;
  profile.vulkanFp32NumericalSmoke.watchdogMs =
      kVulkanFp32NumericalSmokeWatchdogMs;
  plan = buildSelectionPlan(profile, workloads);
  VC_TEST_ASSERT(plan.stages[0].selectedBackend == ComputeBackend::VULKAN);
}

void TestManualVulkanCanSelectNonStrictRoute() {
  CapabilityProfile profile{};
  profile.cpuAvailable = true;
  profile.vulkanAvailable = true;
  profile.vulkanCompute = true;
  profile.vulkanPrimitiveSuitePass = true;
  profile.safeVulkanWorkingSetBytes = 1024ULL * 1024ULL * 1024ULL;

  ManualSelectionConfig config{};
  config.selectionMode = SelectionMode::MANUAL;
  config.globalBackend = ComputeBackend::VULKAN;
  config.allowStageFallback = false;
  const std::vector<StageWorkload> workloads = {
      {Stage::LEVEL_SET, Precision::FP32, 1024ULL, false, RayMode::NONE, true}};
  const auto plan = buildSelectionPlan(profile, workloads, config);
  VC_TEST_ASSERT(plan.ok);
  VC_TEST_ASSERT(plan.stages[0].selected);
  VC_TEST_ASSERT(plan.stages[0].selectedBackend == ComputeBackend::VULKAN);
  VC_TEST_ASSERT(plan.stages[0].selectedReasons.back() ==
                 "Manual Vulkan selected without strict FP32 numerical "
                 "guarantee.");

  config.requireStrictFp32NumericalSmoke = true;
  const auto strictPlan = buildSelectionPlan(profile, workloads, config);
  VC_TEST_ASSERT(!strictPlan.ok);
  VC_TEST_ASSERT(!strictPlan.stages[0].selected);
  VC_TEST_ASSERT(strictPlan.stages[0].rejectionReasons.front() ==
                 "Manual backend blocked: Vulkan strict FP32 numerical smoke "
                 "did not pass.");
}

} // namespace viennacore

int main() {
  std::cerr << "TestManualOverrideCanForceCPU\n";
  viennacore::TestManualOverrideCanForceCPU();
  std::cerr << "TestManualPerStageOverrideIsHonored\n";
  viennacore::TestManualPerStageOverrideIsHonored();
  std::cerr << "TestManualOverrideFailsWhenBackendUnavailable\n";
  viennacore::TestManualOverrideFailsWhenBackendUnavailable();
  std::cerr << "TestAutoFallsBackToCPUWhenOnlyCPUAvailable\n";
  viennacore::TestAutoFallsBackToCPUWhenOnlyCPUAvailable();
  std::cerr << "TestAutoPrefersVulkanWhenCapabilitiesPass\n";
  viennacore::TestAutoPrefersVulkanWhenCapabilitiesPass();
  std::cerr << "TestCoverageStageCanBeSelectedIndependently\n";
  viennacore::TestCoverageStageCanBeSelectedIndependently();
  std::cerr << "TestNeutralTransportVelocityStagePolicy\n";
  viennacore::TestNeutralTransportVelocityStagePolicy();
  std::cerr << "TestAutoUsesTheLowestValidatedRayTier\n";
  viennacore::TestAutoUsesTheLowestValidatedRayTier();
  std::cerr << "TestVulkanRejectsFP64WithoutSupportThenFallsBackCPU\n";
  viennacore::TestVulkanRejectsFP64WithoutSupportThenFallsBackCPU();
  std::cerr << "TestAutoRejectsVulkanWhenMemoryBudgetIsUnknown\n";
  viennacore::TestAutoRejectsVulkanWhenMemoryBudgetIsUnknown();
  std::cerr << "TestVulkanRejectsWorkloadAboveSafeMemoryBudget\n";
  viennacore::TestVulkanRejectsWorkloadAboveSafeMemoryBudget();
  std::cerr << "TestManualRayTierCannotSilentlyDowngrade\n";
  viennacore::TestManualRayTierCannotSilentlyDowngrade();
  std::cerr << "TestManualOverrideAlwaysWinsOverAutoRanking\n";
  viennacore::TestManualOverrideAlwaysWinsOverAutoRanking();
  std::cerr << "TestCapabilityProfileFingerprintMismatchInvalidatesCache\n";
  viennacore::TestCapabilityProfileFingerprintMismatchInvalidatesCache();
  std::cerr << "TestAutoRequiresStrictFp32NumericalSmoke\n";
  viennacore::TestAutoRequiresStrictFp32NumericalSmoke();
  std::cerr << "TestManualVulkanCanSelectNonStrictRoute\n";
  viennacore::TestManualVulkanCanSelectNonStrictRoute();

  return 0;
}
