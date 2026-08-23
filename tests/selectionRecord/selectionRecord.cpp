// P7-R2 selection-record receipts:
//   1. Determinism - two emissions with identical inputs are byte-equal.
//   2. Replay - replaySelectionRecordMatches reproduces the record; any
//      workload/profile mutation flips it.
//   3. Purity - records carry no timestamps or environment data.
//   4. Local-Arc row - the FP64 LA stage resolves to CPU with an FP64-named
//      rejection reason inside the record itself.

#include <compute/backendPolicy.hpp>
#include <compute/selectionRecord.hpp>

#include <iostream>
#include <string>
#include <vector>

namespace compute = viennaps::compute;

namespace {

int failures = 0;

void require(const bool condition, const char *what) {
  if (!condition) {
    std::cerr << "selectionRecord FAIL: " << what << '\n';
    ++failures;
  }
}

compute::CapabilityProfile localArcProfile() {
  compute::CapabilityProfile p;
  p.cpuAvailable = true;
  p.vulkanAvailable = true;
  p.vulkanCompute = true;
  p.vulkanPrimitiveSuitePass = true;
  // Mirror the accepted K1/E0 strict-FP32 evidence so the strict gate passes
  // and the record isolates the FP64-specific rejection.
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

std::vector<compute::StageWorkload> mixedWorkloads() {
  compute::StageWorkload ray;
  ray.stage = compute::Stage::RAY_TRACING;
  ray.precision = compute::Precision::FP32;
  ray.minimumRayMode = compute::RayMode::COMPUTE_BVH;

  compute::StageWorkload la;
  la.stage = compute::Stage::OXIDATION_LINEAR_SOLVE;
  la.precision = compute::Precision::FP64;

  return {ray, la};
}

} // namespace

int main() {
  const auto profile = localArcProfile();
  const auto workloads = mixedWorkloads();
  const compute::ManualSelectionConfig config{};

  // 1. determinism
  const std::string first =
      compute::emitSelectionRecord(profile, config, workloads);
  const std::string second =
      compute::emitSelectionRecord(profile, config, workloads);
  require(first == second, "two emissions are byte-identical");
  require(!first.empty(), "record is non-empty");

  // 2. replay
  require(compute::replaySelectionRecordMatches(first, profile, config,
                                                workloads),
          "replay matches the original record");
  auto mutated = workloads;
  mutated[1].precision = compute::Precision::FP32; // flip LA to FP32
  require(!compute::replaySelectionRecordMatches(first, profile, config,
                                                 mutated),
          "mutated workload flips the replay");

  // 3. purity: no wall-clock / environment fields.
  require(first.find("recordedAt") == std::string::npos &&
              first.find("timestamp") == std::string::npos,
          "no timestamp fields in the record");
  require(first.rfind("selection-record v", 0) == 0,
          "record starts with its schema header");

  // 4. local-Arc FP64 row visible in the record.
  require(first.find("selected_backend=CPU") != std::string::npos,
          "CPU resolution recorded");
  bool fp64ReasonInRecord = false;
  std::istringstream stream(first);
  std::string line;
  while (std::getline(stream, line)) {
    if (line.rfind("reason ", 0) == 0 && line.find("FP64") != std::string::npos)
      fp64ReasonInRecord = true;
  }
  require(fp64ReasonInRecord, "FP64 rejection reason recorded");

  std::cout << "selectionRecord PASS (deterministic, replayable, pure, "
               "local-Arc rows recorded)\n";
  return failures == 0 ? 0 : 1;
}
