// P6-A5 stage-gating matrix: proves the backend policy resolves every P6
// stage per the governing hardware facts, in BOTH build configurations (this
// fixture links no Vulkan types and runs in the CPU/no-SDK tree as well as
// the opt-in Vulkan tree).
//
// Matrix rows:
//   R1  Local Arc profile, OXIDATION_LINEAR_SOLVE FP64
//       AUTO -> CPU; MANUAL VULKAN fails closed with an FP64 reason.
//   R2  Hypothetical fp64-capable profile, same workload
//       MANUAL VULKAN -> eligible (execution remains DEVICE-PENDING here).
//   R3  Stale deployment fingerprint maps to a Vulkan-unavailable profile:
//       even an FP32 ray stage resolves to CPU.
//   R4  Mixed plan on fp64-capable vs local profile: ray stage rides VULKAN
//       while the LA stage splits per evidence - per-stage granularity.
//   R5  Strict-FP32 numerical-smoke gate rejects ray stages when required
//       evidence is NOT_RUN.

#include <compute/backendPolicy.hpp>
#include <compute/capabilityProfileIO.hpp>

#include <iostream>
#include <string>
#include <vector>

namespace compute = viennaps::compute;

namespace {

int failures = 0;

void require(const bool condition, const char *what) {
  if (!condition) {
    std::cerr << "stageGatingMatrix FAIL: " << what << '\n';
    ++failures;
  }
}

compute::CapabilityProfile localArcProfile() {
  compute::CapabilityProfile p;
  p.cpuAvailable = true;
  p.vulkanAvailable = true;
  // COMPUTE_BVH tier resolves from plain compute evidence (see
  // resolveRayMode), which the local Arc carries from its P5 receipts.
  p.vulkanCompute = true;
  p.vulkanPrimitiveSuitePass = true;
  // AUTO always demands the validated strict-FP32 numerical smoke
  // (passesHardThresholds is invoked with requireStrict=true on the AUTO
  // candidate path), so mirror the accepted K1/E0 evidence shape here.
  p.vulkanFp32NumericalSmoke.status =
      compute::VulkanNumericalSmokeStatus::PASS;
  p.vulkanFp32NumericalSmoke.contractId =
      std::string(compute::kVulkanFp32NumericalSmokeContract);
  p.vulkanFp32NumericalSmoke.caseCount = 16U;
  p.vulkanFp32NumericalSmoke.mismatchCount = 0U;
  p.vulkanFp32NumericalSmoke.maxUlp = 0U;
  p.vulkanFp32NumericalSmoke.watchdogMs =
      compute::kVulkanFp32NumericalSmokeWatchdogMs;
  p.vulkanFp32NumericalSmoke.elapsedMs = 1'000ULL;
  p.shaderFloat64 = false;
  p.vulkanFp64SuitePass = false;
  return p;
}

compute::CapabilityProfile fp64CapableProfile() {
  compute::CapabilityProfile p = localArcProfile();
  p.shaderFloat64 = true;
  p.vulkanFp64SuitePass = true;
  return p;
}

compute::CapabilityProfile staleAsUnavailableProfile() {
  // The deployment context zeroes Vulkan availability when the persisted
  // fingerprint is stale; model that mapping directly.
  auto p = localArcProfile();
  p.vulkanAvailable = false;
  p.vulkanCompute = false;
  p.vulkanPrimitiveSuitePass = false;
  return p;
}

compute::StageWorkload laFp64Workload() {
  compute::StageWorkload w;
  w.stage = compute::Stage::OXIDATION_LINEAR_SOLVE;
  w.precision = compute::Precision::FP64;
  return w;
}

compute::StageWorkload rayFp32Workload() {
  compute::StageWorkload w;
  w.stage = compute::Stage::RAY_TRACING;
  w.precision = compute::Precision::FP32;
  w.minimumRayMode = compute::RayMode::COMPUTE_BVH;
  return w;
}

bool hasReasonContaining(const compute::StageSelection &s,
                         const std::string &needle) {
  for (const auto &reason : s.rejectionReasons)
    if (reason.find(needle) != std::string::npos)
      return true;
  return false;
}

void requireStageBackend(const compute::StageSelection &stage,
                         const compute::ComputeBackend expected,
                         const char *what) {
  if (stage.selectedBackend != expected) {
    std::cerr << "  [" << what << "] backend="
              << static_cast<int>(stage.selectedBackend)
              << " rejection reasons:\n";
    for (const auto &r : stage.rejectionReasons)
      std::cerr << "    - " << r << '\n';
  }
  require(stage.selectedBackend == expected, what);
}

void runMatrix() {
  const auto localProfile = localArcProfile();
  const auto capableProfile = fp64CapableProfile();

  // R1: local profile - LA stage never reaches Vulkan.
  const std::vector<compute::StageWorkload> r1Workloads{laFp64Workload()};
  const auto r1Auto =
      compute::buildSelectionPlan(localProfile, r1Workloads,
                                  compute::ManualSelectionConfig{});
  require(r1Auto.stages.size() == 1U && r1Auto.stages.front().selectedBackend ==
                                             compute::ComputeBackend::CPU,
          "R1 AUTO: local profile resolves LA FP64 to CPU");

  compute::ManualSelectionConfig manualVulkan;
  manualVulkan.selectionMode = compute::SelectionMode::MANUAL;
  manualVulkan.globalBackend = compute::ComputeBackend::VULKAN;
  const auto r1Manual =
      compute::buildSelectionPlan(localProfile, {laFp64Workload()}, manualVulkan);
  require(hasReasonContaining(r1Manual.stages.front(), "FP64"),
          "R1 MANUAL: rejection reason names FP64");
  require(r1Manual.stages.front().selectedBackend !=
              compute::ComputeBackend::VULKAN,
          "R1 MANUAL: no silent Vulkan promotion");

  // R2: fp64-capable policy permits the manual route.
  const std::vector<compute::StageWorkload> r2Workloads{laFp64Workload()};
  const auto r2 =
      compute::buildSelectionPlan(capableProfile, r2Workloads, manualVulkan);
  require(r2.stages.front().selectedBackend == compute::ComputeBackend::VULKAN &&
              r2.stages.front().ok,
          "R2 MANUAL: capable profile admits VULKAN for LA FP64");

  // R3: stale fingerprint -> unavailable mapping forces CPU everywhere.
  const auto staleProfile = staleAsUnavailableProfile();
  compute::StageWorkload rayWorkload = rayFp32Workload();
  const std::vector<compute::StageWorkload> r3Workloads{rayWorkload};
  const auto r3 = compute::buildSelectionPlan(staleProfile, r3Workloads,
                                              compute::ManualSelectionConfig{});
  require(r3.stages.front().selectedBackend == compute::ComputeBackend::CPU,
          "R3 stale/unavailable resolves ray stage to CPU");

  // R4: mixed two-stage plans prove per-stage granularity.
  const std::vector<compute::StageWorkload> mixed = {rayFp32Workload(),
                                                     laFp64Workload()};
  const auto r4Capable =
      compute::buildSelectionPlan(capableProfile, mixed,
                                  compute::ManualSelectionConfig{});
  require(r4Capable.stages.size() == 2U, "R4 plan size");
  requireStageBackend(r4Capable.stages[0], compute::ComputeBackend::VULKAN,
                      "R4 capable profile: ray stage on VULKAN");
  requireStageBackend(r4Capable.stages[1], compute::ComputeBackend::VULKAN,
                      "R4 capable profile: LA stage on VULKAN");

  const auto r4Local =
      compute::buildSelectionPlan(localProfile, mixed,
                                  compute::ManualSelectionConfig{});
  requireStageBackend(r4Local.stages[0], compute::ComputeBackend::VULKAN,
                      "R4 local profile: ray stage still eligible");
  requireStageBackend(r4Local.stages[1], compute::ComputeBackend::CPU,
                      "R4 local profile: LA stage falls back to CPU");

  // R5: strict-FP32 gate.
  compute::CapabilityProfile strictGateProfile = localArcProfile();
  strictGateProfile.shaderFloat64 = true;
  strictGateProfile.vulkanFp64SuitePass = true;
  strictGateProfile.vulkanFp32NumericalSmoke.status =
      compute::VulkanNumericalSmokeStatus::NOT_RUN;

  compute::ManualSelectionConfig strictConfig;
  strictConfig.requireStrictFp32NumericalSmoke = true;
  const std::vector<compute::StageWorkload> r5Workloads{rayWorkload};
  const auto r5 = compute::buildSelectionPlan(strictGateProfile, r5Workloads,
                                              strictConfig);
  require(!r5.stages.front().ok ||
              r5.stages.front().selectedBackend ==
                  compute::ComputeBackend::CPU,
          "R5 strict-FP32 gate blocks unevidenced ray stage");
}

} // namespace

int main() {
  runMatrix();

  if (failures != 0) {
    std::cerr << "stageGatingMatrix FAILED with " << failures
              << " failure(s)\n";
    return 1;
  }
  std::cout << "stageGatingMatrix PASS (local-Arc fallback, capable-profile "
               "admission, staleness mapping, mixed-plan granularity, "
               "strict-FP32 gate)\n";
  return 0;
}
