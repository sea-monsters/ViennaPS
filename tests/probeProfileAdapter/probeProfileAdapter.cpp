#include <cstdint>
#include <filesystem>
#include <iostream>
#include <limits>
#include <string>
#include <system_error>
#include <vector>

#include <compute/capabilityProfileIO.hpp>
#include <compute/deploymentProfile.hpp>
#include <compute/probeProfileAdapter.hpp>
#include <vcTestAsserts.hpp>

namespace viennacore {

using namespace viennaps::compute;

namespace {

[[nodiscard]] constexpr std::uint64_t miB(const std::uint64_t mebiBytes) {
  return mebiBytes * 1024ULL * 1024ULL;
}

[[nodiscard]] VulkanProbeDeviceFacts defaultFacts() {
  VulkanProbeDeviceFacts facts{};
  facts.hardware.deviceUuid = "DEV-FIXTURE-001";
  facts.hardware.driverUuid = "DRV-FIXTURE-001";
  facts.hardware.vendorId = 32902;
  facts.hardware.deviceId = 11;
  facts.hardware.deviceName = "fixture-device";
  facts.hardware.driverVersion = "1.2.3";
  facts.hardware.driverDate = "2026-01-01";
  facts.supportsVulkan = true;
  facts.supportsComputeQueue = true;
  facts.supportsRayQuery = true;
  facts.supportsRayTracingPipeline = false;
  facts.supportsShaderFloat64 = true;
  return facts;
}

void TestSafeBudgetDerivationUsesHeadroomAndSafetyMargin() {
  const std::vector<std::uint64_t> budgets = {miB(128), miB(256)};
  const auto safeBudget =
      deriveSafeVulkanWorkingSetBytes(budgets, true, nullptr);
  const auto available = miB(256) - miB(64);
  const auto expected = available - (available / 10ULL);
  VC_TEST_ASSERT(safeBudget == expected);
}

void TestSafeBudgetNotAvailableWhenExtensionMissing() {
  const std::vector<std::uint64_t> budgets = {miB(1024)};
  const auto safeBudget =
      deriveSafeVulkanWorkingSetBytes(budgets, false, nullptr);
  VC_TEST_ASSERT(safeBudget == 0ULL);
}

void TestSafeBudgetClampsToMinimum() {
  const std::vector<std::uint64_t> budgets = {miB(65)};
  const auto safeBudget =
      deriveSafeVulkanWorkingSetBytes(budgets, true, nullptr);
  VC_TEST_ASSERT(safeBudget == 0ULL);
}

void TestSafeBudgetCapsToMaxBytes() {
  const std::vector<std::uint64_t> budgets = {
      std::numeric_limits<std::uint64_t>::max()};
  const auto safeBudget =
      deriveSafeVulkanWorkingSetBytes(budgets, true, nullptr);
  VC_TEST_ASSERT(safeBudget == 4ULL * 1024ULL * 1024ULL * 1024ULL);
}

void TestSafeBudgetOverflowIsClamped() {
  const std::vector<std::uint64_t> budgets = {
      std::numeric_limits<std::uint64_t>::max() / 2ULL,
      std::numeric_limits<std::uint64_t>::max() / 2ULL};
  const auto safeBudget =
      deriveSafeVulkanWorkingSetBytes(budgets, true, nullptr);
  VC_TEST_ASSERT(safeBudget == 4ULL * 1024ULL * 1024ULL * 1024ULL);
}

void TestAdapterDefaultsKeepSuiteFlagsFalse() {
  auto facts = defaultFacts();
  facts.supportsComputeQueue = true;
  facts.memoryBudgetExtensionAvailable = true;
  facts.memoryBudgetBytes = {miB(2048)};
  const auto result =
      adaptVulkanProbeFactsToCapabilityProfile(facts, "2026-01-01T00:00:00Z");
  VC_TEST_ASSERT(result.ok);
  VC_TEST_ASSERT(!result.record.capabilityProfile.vulkanPrimitiveSuitePass);
  VC_TEST_ASSERT(!result.record.capabilityProfile.vulkanFp64SuitePass);
  VC_TEST_ASSERT(result.record.capabilityProfile.vulkanCompute);
}

void TestAdapterPropagatesNumericalSmokeEvidence() {
  auto facts = defaultFacts();
  facts.validationEvidence.fp32NumericalSmoke.status =
      VulkanNumericalSmokeStatus::PASS;
  facts.validationEvidence.fp32NumericalSmoke.contractId =
      std::string(kVulkanFp32NumericalSmokeContract);
  facts.validationEvidence.fp32NumericalSmoke.caseCount = 1U;
  facts.validationEvidence.fp32NumericalSmoke.maxUlp = 0U;
  facts.validationEvidence.fp32NumericalSmoke.watchdogMs =
      kVulkanFp32NumericalSmokeWatchdogMs;
  const auto result =
      adaptVulkanProbeFactsToCapabilityProfile(facts, "2026-01-01T00:00:00Z");
  VC_TEST_ASSERT(result.ok);
  VC_TEST_ASSERT(
      result.record.capabilityProfile.vulkanFp32NumericalSmoke.status ==
      VulkanNumericalSmokeStatus::PASS);
  VC_TEST_ASSERT(
      result.record.capabilityProfile.vulkanFp32NumericalSmoke.maxUlp == 0U);
}

void TestAdapterValidationEvidenceFailsClosed() {
  auto facts = defaultFacts();
  facts.validationEvidence.primitiveSuite = VulkanProbeSuiteStatus::NOT_RUN;
  facts.validationEvidence.fp32Suite = VulkanProbeSuiteStatus::FAIL;
  facts.validationEvidence.fp64Suite = VulkanProbeSuiteStatus::FAIL;
  const auto result =
      adaptVulkanProbeFactsToCapabilityProfile(facts, "2026-01-01T00:00:00Z");
  VC_TEST_ASSERT(result.ok);
  VC_TEST_ASSERT(!result.record.capabilityProfile.vulkanPrimitiveSuitePass);
  VC_TEST_ASSERT(!result.record.capabilityProfile.vulkanFp64SuitePass);
}

void TestAdapterExplicitPrimitiveSuitePassEnablesPrimitiveCapability() {
  auto facts = defaultFacts();
  facts.validationEvidence.primitiveSuite = VulkanProbeSuiteStatus::PASS;
  facts.validationEvidence.fp32Suite = VulkanProbeSuiteStatus::PASS;
  const auto result =
      adaptVulkanProbeFactsToCapabilityProfile(facts, "2026-01-01T00:00:00Z");
  VC_TEST_ASSERT(result.ok);
  VC_TEST_ASSERT(result.record.capabilityProfile.vulkanPrimitiveSuitePass);
  VC_TEST_ASSERT(!result.record.capabilityProfile.vulkanFp64SuitePass);
}

void TestAdapterFailedPrimitiveSuiteKeepsPrimitiveCapabilityDisabled() {
  auto facts = defaultFacts();
  facts.validationEvidence.primitiveSuite = VulkanProbeSuiteStatus::FAIL;
  const auto result =
      adaptVulkanProbeFactsToCapabilityProfile(facts, "2026-01-01T00:00:00Z");
  VC_TEST_ASSERT(result.ok);
  VC_TEST_ASSERT(!result.record.capabilityProfile.vulkanPrimitiveSuitePass);
}

void TestAdapterPrimitiveSuiteRequiresFp32SuitePass() {
  auto facts = defaultFacts();
  facts.validationEvidence.primitiveSuite = VulkanProbeSuiteStatus::PASS;
  facts.validationEvidence.fp32Suite = VulkanProbeSuiteStatus::FAIL;
  const auto result =
      adaptVulkanProbeFactsToCapabilityProfile(facts, "2026-01-01T00:00:00Z");
  VC_TEST_ASSERT(result.ok);
  VC_TEST_ASSERT(!result.record.capabilityProfile.vulkanPrimitiveSuitePass);
}

void TestAdapterInvalidValidationStatusFailsClosed() {
  auto facts = defaultFacts();
  facts.validationEvidence.primitiveSuite =
      static_cast<VulkanProbeSuiteStatus>(99);
  facts.validationEvidence.fp32Suite = static_cast<VulkanProbeSuiteStatus>(99);
  facts.validationEvidence.fp64Suite = static_cast<VulkanProbeSuiteStatus>(99);
  const auto result =
      adaptVulkanProbeFactsToCapabilityProfile(facts, "2026-01-01T00:00:00Z");
  VC_TEST_ASSERT(result.ok);
  VC_TEST_ASSERT(!result.record.capabilityProfile.vulkanPrimitiveSuitePass);
  VC_TEST_ASSERT(!result.record.capabilityProfile.vulkanFp64SuitePass);
}

void TestAdapterUnknownNumericalEvidenceFallsBackToCpu() {
  auto facts = defaultFacts();
  facts.memoryBudgetExtensionAvailable = true;
  facts.memoryBudgetBytes = {miB(2048)};
  facts.validationEvidence.primitiveSuite = VulkanProbeSuiteStatus::PASS;
  facts.validationEvidence.fp32Suite = VulkanProbeSuiteStatus::PASS;
  facts.validationEvidence.fp32NumericalSmoke.status =
      static_cast<VulkanNumericalSmokeStatus>(99);
  facts.validationEvidence.fp32NumericalSmoke.contractId =
      std::string(kVulkanFp32NumericalSmokeContract);
  facts.validationEvidence.fp32NumericalSmoke.caseCount = 1U;
  facts.validationEvidence.fp32NumericalSmoke.maxUlp = 0U;
  facts.validationEvidence.fp32NumericalSmoke.watchdogMs =
      kVulkanFp32NumericalSmokeWatchdogMs;

  const auto result =
      adaptVulkanProbeFactsToCapabilityProfile(facts, "2026-01-01T00:00:00Z");
  VC_TEST_ASSERT(result.ok);
  VC_TEST_ASSERT(result.record.capabilityProfile.vulkanPrimitiveSuitePass);
  VC_TEST_ASSERT(
      result.record.capabilityProfile.vulkanFp32NumericalSmoke.status ==
      static_cast<VulkanNumericalSmokeStatus>(99));

  const std::vector<StageWorkload> workloads = {
      {Stage::LEVEL_SET, Precision::FP32, miB(1), false, RayMode::NONE, true}};
  const auto plan = buildSelectionPlan(result.record.capabilityProfile, workloads);
  VC_TEST_ASSERT(plan.stages[0].selectedBackend == ComputeBackend::CPU);
}

void TestAdapterFp64SuiteRequiresShaderFloat64Feature() {
  auto facts = defaultFacts();
  facts.supportsShaderFloat64 = false;
  facts.validationEvidence.fp64Suite = VulkanProbeSuiteStatus::PASS;
  const auto result =
      adaptVulkanProbeFactsToCapabilityProfile(facts, "2026-01-01T00:00:00Z");
  VC_TEST_ASSERT(result.ok);
  VC_TEST_ASSERT(!result.record.capabilityProfile.vulkanFp64SuitePass);
}

void TestAdapterFp64SuitePassWithShaderFloat64EnablesCapability() {
  auto facts = defaultFacts();
  facts.supportsShaderFloat64 = true;
  facts.validationEvidence.fp64Suite = VulkanProbeSuiteStatus::PASS;
  const auto result =
      adaptVulkanProbeFactsToCapabilityProfile(facts, "2026-01-01T00:00:00Z");
  VC_TEST_ASSERT(result.ok);
  VC_TEST_ASSERT(result.record.capabilityProfile.vulkanFp64SuitePass);
}

void TestAdapterValidationEvidenceDoesNotChangeSafeBudget() {
  auto facts = defaultFacts();
  facts.memoryBudgetExtensionAvailable = true;
  facts.memoryBudgetBytes = {miB(2048)};
  const auto baseline =
      adaptVulkanProbeFactsToCapabilityProfile(facts, "2026-01-01T00:00:00Z");
  facts.validationEvidence.primitiveSuite = VulkanProbeSuiteStatus::PASS;
  facts.validationEvidence.fp32Suite = VulkanProbeSuiteStatus::PASS;
  facts.validationEvidence.fp64Suite = VulkanProbeSuiteStatus::PASS;
  const auto validated =
      adaptVulkanProbeFactsToCapabilityProfile(facts, "2026-01-01T00:00:00Z");
  VC_TEST_ASSERT(baseline.ok);
  VC_TEST_ASSERT(validated.ok);
  VC_TEST_ASSERT(validated.record.capabilityProfile.safeVulkanWorkingSetBytes ==
                 baseline.record.capabilityProfile.safeVulkanWorkingSetBytes);
}

void TestAdapterRejectsIncompleteHardwareFingerprint() {
  auto facts = defaultFacts();
  facts.hardware.driverVersion.clear();
  const auto result =
      adaptVulkanProbeFactsToCapabilityProfile(facts, "2026-01-01T00:00:00Z");
  VC_TEST_ASSERT(!result.ok);
}

void TestAdapterWithoutBudgetExtensionReportsZeroBytes() {
  auto facts = defaultFacts();
  facts.memoryBudgetExtensionAvailable = false;
  const auto result =
      adaptVulkanProbeFactsToCapabilityProfile(facts, "2026-01-01T00:00:00Z");
  VC_TEST_ASSERT(result.ok);
  VC_TEST_ASSERT(result.record.capabilityProfile.safeVulkanWorkingSetBytes ==
                 0ULL);
}

void TestAdapterWithoutComputeQueueCannotEnableVulkan() {
  auto facts = defaultFacts();
  facts.supportsComputeQueue = false;
  const auto result =
      adaptVulkanProbeFactsToCapabilityProfile(facts, "2026-01-01T00:00:00Z");
  VC_TEST_ASSERT(result.ok);
  VC_TEST_ASSERT(!result.record.capabilityProfile.vulkanAvailable);
  VC_TEST_ASSERT(!result.record.capabilityProfile.vulkanCompute);
}

void TestAdaptedProfileRoundTripsThroughAtomicWriter() {
  auto facts = defaultFacts();
  facts.memoryBudgetExtensionAvailable = true;
  facts.memoryBudgetBytes = {miB(2048)};
  const auto adapted =
      adaptVulkanProbeFactsToCapabilityProfile(facts, "2026-01-01T00:00:00Z");
  VC_TEST_ASSERT(adapted.ok);

  const auto profilePath = std::filesystem::temp_directory_path() /
                           "viennaps-probe-profile-roundtrip.json";
  std::error_code removeError;
  std::filesystem::remove(profilePath, removeError);
  VC_TEST_ASSERT(writeCapabilityProfileRecordToFile(profilePath.string(),
                                                    adapted.record, nullptr));
  const auto loaded = loadCapabilityProfileRecordFromFile(profilePath.string());
  VC_TEST_ASSERT(loaded.ok);
  VC_TEST_ASSERT(loaded.record.hardware.deviceUuid ==
                 facts.hardware.deviceUuid);
  VC_TEST_ASSERT(!loaded.record.capabilityProfile.vulkanPrimitiveSuitePass);
  VC_TEST_ASSERT(!loaded.record.capabilityProfile.vulkanFp64SuitePass);
  std::filesystem::remove(profilePath, removeError);
}

void TestDeploymentProfileSelectionRejectsStaleFingerprint() {
  const HardwareFingerprint runtimeFingerprint = {
      "DEV-FIXTURE-002", "DRV-FIXTURE-A", 333,         444,
      "adapter-device",  "2.0.0",         "2026-01-02"};
  CapabilityProfileRecord record;
  record.schemaVersion = kCapabilityProfileSchemaVersion;
  record.recordedAt = "2026-01-01T00:00:00Z";
  record.hardware = runtimeFingerprint;
  record.hardware.driverUuid = "DRV-FIXTURE-B";
  record.capabilityProfile.cpuAvailable = true;
  record.capabilityProfile.vulkanAvailable = true;
  record.capabilityProfile.vulkanPrimitiveSuitePass = true;
  record.capabilityProfile.vulkanFp64SuitePass = true;
  record.capabilityProfile.vulkanCompute = true;
  record.capabilityProfile.safeVulkanWorkingSetBytes = miB(128);

  const auto dir =
      std::filesystem::temp_directory_path() / "viennaps-probe-profile-adapter";
  std::filesystem::create_directories(dir);
  const auto profilePath = dir / (runtimeFingerprint.deviceUuid + ".json");
  VC_TEST_ASSERT(writeCapabilityProfileRecordToFile(profilePath.string(),
                                                    record, nullptr));

  const std::vector<StageWorkload> workloads = {
      {Stage::LEVEL_SET, Precision::FP32, 1024ULL, false, RayMode::NONE, true}};
  const auto decision = selectDeploymentProfile(
      runtimeFingerprint, workloads, ManualSelectionConfig{}, dir.string());
  VC_TEST_ASSERT(decision.requiresProbe);
  VC_TEST_ASSERT(decision.state == DeploymentProfileState::STALE);
  VC_TEST_ASSERT(!decision.hasProfile);
  VC_TEST_ASSERT(decision.plan.stages[0].selectedBackend ==
                 ComputeBackend::CPU);

  std::error_code removeError;
  std::filesystem::remove(profilePath, removeError);
}

} // namespace

} // namespace viennacore

int main() {
  try {
    viennacore::TestSafeBudgetDerivationUsesHeadroomAndSafetyMargin();
    viennacore::TestSafeBudgetNotAvailableWhenExtensionMissing();
    viennacore::TestSafeBudgetClampsToMinimum();
    viennacore::TestSafeBudgetCapsToMaxBytes();
    viennacore::TestSafeBudgetOverflowIsClamped();
    viennacore::TestAdapterDefaultsKeepSuiteFlagsFalse();
    viennacore::TestAdapterPropagatesNumericalSmokeEvidence();
    viennacore::TestAdapterValidationEvidenceFailsClosed();
    viennacore::
        TestAdapterExplicitPrimitiveSuitePassEnablesPrimitiveCapability();
    viennacore::
        TestAdapterFailedPrimitiveSuiteKeepsPrimitiveCapabilityDisabled();
    viennacore::TestAdapterPrimitiveSuiteRequiresFp32SuitePass();
    viennacore::TestAdapterInvalidValidationStatusFailsClosed();
    viennacore::TestAdapterFp64SuiteRequiresShaderFloat64Feature();
    viennacore::TestAdapterFp64SuitePassWithShaderFloat64EnablesCapability();
    viennacore::TestAdapterValidationEvidenceDoesNotChangeSafeBudget();
    viennacore::TestAdapterRejectsIncompleteHardwareFingerprint();
    viennacore::TestAdapterWithoutBudgetExtensionReportsZeroBytes();
    viennacore::TestAdapterWithoutComputeQueueCannotEnableVulkan();
    viennacore::TestAdaptedProfileRoundTripsThroughAtomicWriter();
    viennacore::TestDeploymentProfileSelectionRejectsStaleFingerprint();
    return 0;
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
