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

  std::vector<StageWorkload> workloads = {{Stage::RAY_TRACING, Precision::FP32,
                                           1024 * 1024ULL, false, RayMode::NONE,
                                           true}};

  const auto plan =
      buildSelectionPlan(profile, workloads, ManualSelectionConfig{});
  VC_TEST_ASSERT(plan.ok);
  VC_TEST_ASSERT(plan.stages[0].selected);
  VC_TEST_ASSERT(plan.stages[0].selectedBackend == ComputeBackend::VULKAN);
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

  const std::vector<StageWorkload> workloads = {
      {Stage::LEVEL_SET, Precision::FP32, 4096, false, RayMode::NONE, true}};

  const auto plan = buildSelectionPlan(profile, workloads);
  VC_TEST_ASSERT(plan.ok);
  VC_TEST_ASSERT(plan.stages[0].selectedBackend == ComputeBackend::CPU);
}

void TestManualRayTierCannotSilentlyDowngrade() {
  CapabilityProfile profile{};
  profile.cpuAvailable = true;
  profile.vulkanAvailable = true;
  profile.vulkanPrimitiveSuitePass = true;
  profile.vulkanCompute = true;
  profile.safeVulkanWorkingSetBytes = 1024 * 1024;

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
  std::cerr << "TestAutoUsesTheLowestValidatedRayTier\n";
  viennacore::TestAutoUsesTheLowestValidatedRayTier();
  std::cerr << "TestVulkanRejectsFP64WithoutSupportThenFallsBackCPU\n";
  viennacore::TestVulkanRejectsFP64WithoutSupportThenFallsBackCPU();
  std::cerr << "TestAutoRejectsVulkanWhenMemoryBudgetIsUnknown\n";
  viennacore::TestAutoRejectsVulkanWhenMemoryBudgetIsUnknown();
  std::cerr << "TestManualRayTierCannotSilentlyDowngrade\n";
  viennacore::TestManualRayTierCannotSilentlyDowngrade();
  std::cerr << "TestManualOverrideAlwaysWinsOverAutoRanking\n";
  viennacore::TestManualOverrideAlwaysWinsOverAutoRanking();
  std::cerr << "TestCapabilityProfileFingerprintMismatchInvalidatesCache\n";
  viennacore::TestCapabilityProfileFingerprintMismatchInvalidatesCache();

  return 0;
}
