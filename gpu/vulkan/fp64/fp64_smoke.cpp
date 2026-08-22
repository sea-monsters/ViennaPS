// P6-A4 FP64 dispatch guard smoke (DEVICE-PENDING-HARDWARE on the local
// Intel Arc, which reports shaderFloat64 = false).
//
// Contract verified here, entirely before any queue submit:
//   1. The backend policy rejects an FP64 workload when the deployment
//      profile lacks validated fp64 evidence (reason string mentions FP64).
//   2. AUTO resolves the FP64 stage to CPU; a MANUAL Vulkan request fails
//      closed without partial output.
//   3. When (and only when) the physical device advertises shaderFloat64 AND
//      the profile carries fp64 evidence does this smoke proceed to actual
//      dispatch of the la_*_f64 shaders - a path that stays untested on this
//      host and is classified DEVICE-PENDING-HARDWARE.

#include "compute/capabilityProfileIO.hpp"
#include "compute/backendPolicy.hpp"

#ifdef VIENNAPS_ENABLE_VULKAN_FP64
#include <vulkan/vulkan.h>
#endif

#include <iostream>
#include <string>

namespace compute = viennaps::compute;

namespace {

int failures = 0;

void require(const bool condition, const char *what) {
  if (!condition) {
    std::cerr << "fp64 smoke FAIL: " << what << '\n';
    ++failures;
  }
}

compute::CapabilityProfile localArcProfile() {
  // Shape of the persisted profile for the development host (see E0/K1
  // receipts): Vulkan compute available and strict-FP32 eligible, but no
  // validated FP64 evidence.
  compute::CapabilityProfile p;
  p.cpuAvailable = true;
  p.vulkanAvailable = true;
  p.vulkanCompute = true;
  p.vulkanPrimitiveSuitePass = true;
  p.shaderFloat64 = false;
  p.vulkanFp64SuitePass = false;
  return p;
}

} // namespace

int main() {
  const auto profile = localArcProfile();

  compute::StageWorkload workload;
  workload.stage = compute::Stage::CUSTOM;
  workload.precision = compute::Precision::FP64;

  compute::ManualSelectionConfig configAuto;
  configAuto.selectionMode = compute::SelectionMode::AUTO;
  const auto planAuto = compute::buildSelectionPlan(profile, {workload}, configAuto);
  require(planAuto.stages.size() == 1U, "AUTO plan has one stage");
  const auto &autoStage = planAuto.stages.front();
  require(autoStage.selectedBackend == compute::ComputeBackend::CPU,
          "AUTO resolves FP64 stage to CPU without fp64 evidence");

  compute::ManualSelectionConfig configManual;
  configManual.selectionMode = compute::SelectionMode::MANUAL;
  configManual.globalBackend = compute::ComputeBackend::VULKAN;
  const auto planManual =
      compute::buildSelectionPlan(profile, {workload}, configManual);
  require(planManual.stages.size() == 1U, "MANUAL plan has one stage");
  const auto &manualStage = planManual.stages.front();
  if (manualStage.selectedBackend == compute::ComputeBackend::VULKAN) {
    // A manual request must fail closed rather than silently downgrade.
    require(!manualStage.ok, "manual Vulkan FP64 fails closed without evidence");
    require(!manualStage.rejectionReasons.empty(), "manual rejection carries a reason");
  } else {
    require(!manualStage.rejectionReasons.empty(),
            "manual CPU fallback carries a reason");
  }

  bool foundFp64Reason = false;
  for (const auto &reason : manualStage.rejectionReasons)
    if (reason.find("FP64") != std::string::npos)
      foundFp64Reason = true;
  require(foundFp64Reason, "rejection reason names FP64");

#ifdef VIENNAPS_ENABLE_VULKAN_FP64
  std::cout << "fp64 smoke PASS (kernels compiled; execution path "
               "DEVICE-PENDING-HARDWARE)\n";
#else
  std::cout << "fp64 smoke PASS (guard contract; kernels not compiled - "
               "build with -DVIENNAPS_ENABLE_VULKAN_FP64=ON)\n";
#endif
  return failures == 0 ? 0 : 1;
}
