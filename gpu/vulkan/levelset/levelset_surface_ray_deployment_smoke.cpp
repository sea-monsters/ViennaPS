// Copyright 2026 ViennaPS
// SPDX-License-Identifier: MIT
//
// P5-SURFACE-INTEGRATION shared deployment session smoke.
//
// Provisions one LevelSetSurfaceDeploymentComposition for level-set,
// surface-diffusion, coverage-delta, neutral-transport velocity, and ray
// tracing. Its executable ray differential uses the currently eligible
// single-particle route; NeutralTransport remains a CPU-only model.

#include "levelset_surface_deployment_composition.hpp"

#include <compute/capabilityProfileIO.hpp>
#include <geometries/psMakePlane.hpp>
#include <models/psSingleParticleProcess.hpp>
#include <process/psProcess.hpp>

#include <lsMakeGeometry.hpp>

#include <vcLogger.hpp>
#include <vcTestAsserts.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <memory>
#include <span>
#include <sstream>
#include <string>
#include <vector>

namespace {

using Composition =
    viennaps::vulkan::levelset::LevelSetSurfaceDeploymentComposition<2>;
using ComputeBackend = viennaps::compute::ComputeBackend;
using ComputeSession = viennaps::vulkan::runtime::ComputeSession;
using HardwareFingerprint = viennaps::compute::HardwareFingerprint;
using ManualSelectionConfig = viennaps::compute::ManualSelectionConfig;
using Precision = viennaps::compute::Precision;
using RayMode = viennaps::compute::RayMode;
using SelectionMode = viennaps::compute::SelectionMode;
using Stage = viennaps::compute::Stage;
using StageSelection = viennaps::compute::StageSelection;
using StageWorkload = viennaps::compute::StageWorkload;

bool require(const bool condition, const char *what) {
  if (!condition) {
    std::cerr << "[LevelSetSurfaceRayDeployment] FAIL: " << what << '\n';
    return false;
  }
  return true;
}

[[nodiscard]] std::string uuidToHex(const std::uint8_t *bytes) {
  std::ostringstream out;
  out << std::hex << std::setfill('0');
  for (std::size_t i = 0U; i < VK_UUID_SIZE; ++i)
    out << std::setw(2) << static_cast<unsigned int>(bytes[i]);
  return out.str();
}

[[nodiscard]] bool collectHardware(HardwareFingerprint &fingerprint,
                                   std::string &error) {
  ComputeSession probeSession;
  if (!probeSession.initialize(error))
    return false;

  VkPhysicalDeviceIDProperties ids{};
  ids.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES;
  VkPhysicalDeviceProperties2 properties{};
  properties.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
  properties.pNext = &ids;
  vkGetPhysicalDeviceProperties2(probeSession.selection().handle, &properties);
  fingerprint.deviceUuid = uuidToHex(ids.deviceUUID);
  fingerprint.driverUuid = uuidToHex(ids.driverUUID);
  fingerprint.vendorId = properties.properties.vendorID;
  fingerprint.deviceId = properties.properties.deviceID;
  fingerprint.deviceName = properties.properties.deviceName;
  fingerprint.driverVersion = std::to_string(properties.properties.driverVersion);
  fingerprint.driverDate = "unknown";
  return true;
}

[[nodiscard]] viennaps::compute::DeploymentProfileDecision
makeVulkanDecision(const HardwareFingerprint &hardware,
                   const std::span<const StageWorkload> workloads) {
  viennaps::compute::DeploymentProfileDecision decision;
  decision.state = viennaps::compute::DeploymentProfileState::VALID;
  decision.hasProfile = true;
  decision.activeRecord.hardware = hardware;
  decision.activeRecord.capabilityProfile.cpuAvailable = true;
  decision.activeRecord.capabilityProfile.vulkanAvailable = true;
  decision.activeRecord.capabilityProfile.vulkanPrimitiveSuitePass = true;
  decision.activeRecord.capabilityProfile.vulkanCompute = true;
  decision.activeRecord.capabilityProfile.vulkanFp32NumericalSmoke.status =
      viennaps::compute::VulkanNumericalSmokeStatus::PASS;
  decision.activeRecord.capabilityProfile.vulkanFp32NumericalSmoke.contractId =
      std::string(viennaps::compute::kVulkanFp32NumericalSmokeContract);
  decision.activeRecord.capabilityProfile.vulkanFp32NumericalSmoke.caseCount =
      18U;
  decision.activeRecord.capabilityProfile.vulkanFp32NumericalSmoke.watchdogMs =
      viennaps::compute::kVulkanFp32NumericalSmokeWatchdogMs;
  decision.activeRecord.capabilityProfile.safeVulkanWorkingSetBytes =
      128ULL * 1024ULL * 1024ULL;
  decision.plan.ok = true;
  for (const auto &workload : workloads) {
    StageSelection stage{workload.stage};
    stage.selected = true;
    stage.selectedBackend = ComputeBackend::VULKAN;
    decision.plan.stages.push_back(std::move(stage));
  }
  return decision;
}

[[nodiscard]] viennaps::RayTracingParameters singleBounceParams() {
  viennaps::RayTracingParameters params;
  params.rngSeed = 42;
  params.useRandomSeeds = false;
  params.maxReflections = 0; // currently eligible single-bounce route
  return params;
}

[[nodiscard]] auto makeEligibleRayModel() {
  return viennacore::SmartPointer<
      viennaps::SingleParticleProcess<float, 2>>::New(1.0F, 1.0F, 1.0F);
}

[[nodiscard]] auto makePlaneDomain() {
  auto domain = viennaps::Domain<float, 2>::New(0.5F, 10.0F, 10.0F);
  viennaps::MakePlane<float, 2>(domain, 0.0).apply();
  return domain;
}

[[nodiscard]] auto makePhysicsDomain() {
  constexpr int dimension = 2;
  constexpr viennahrle::CoordType extent = 4;
  constexpr viennahrle::CoordType gridDelta = 0.5;
  viennahrle::CoordType bounds[2 * dimension] = {-extent, extent, -extent,
                                                 extent};
  viennals::BoundaryConditionEnum boundary[dimension] = {
      viennals::BoundaryConditionEnum::REFLECTIVE_BOUNDARY,
      viennals::BoundaryConditionEnum::REFLECTIVE_BOUNDARY};
  auto levelSet = viennals::SmartPointer<viennals::Domain<float, dimension>>::New(
      bounds, boundary, gridDelta);
  float origin[dimension] = {0.0F, 0.0F};
  viennals::MakeGeometry<float, dimension>(
      levelSet,
      viennals::SmartPointer<viennals::Sphere<float, dimension>>::New(origin,
                                                                       1.5F))
      .apply();
  auto domain = viennaps::SmartPointer<viennaps::Domain<float, 2>>::New();
  domain->insertNextLevelSetAsMaterial(levelSet, viennaps::Material::Undefined,
                                       false);
  return domain;
}

[[nodiscard]] std::vector<float> readParticleFlux(
    const viennaps::SmartPointer<viennals::Mesh<float>> &mesh) {
  if (!mesh)
    return {};
  const auto data = mesh->getCellData().getScalarData("particleFlux", true);
  return data ? *data : std::vector<float>{};
}

[[nodiscard]] std::vector<float> snapshotSurfaceValues(
    const viennaps::SmartPointer<viennaps::Domain<float, 2>> &domain) {
  std::vector<float> values;
  const auto &surface = domain->getSurface()->getDomain();
  for (unsigned segment = 0U; segment < surface.getNumberOfSegments();
       ++segment) {
    const auto &defined = surface.getDomainSegment(segment).definedValues;
    values.insert(values.end(), defined.begin(), defined.end());
  }
  return values;
}

[[nodiscard]] std::uint32_t ulpDistance(const float lhs, const float rhs) {
  const auto lhsBits = std::bit_cast<std::uint32_t>(lhs);
  const auto rhsBits = std::bit_cast<std::uint32_t>(rhs);
  const auto ordered = [](const std::uint32_t bits) {
    return (bits & 0x80000000U) != 0U ? ~bits : bits ^ 0x80000000U;
  };
  const auto lhsOrdered = ordered(lhsBits);
  const auto rhsOrdered = ordered(rhsBits);
  return lhsOrdered > rhsOrdered ? lhsOrdered - rhsOrdered
                                  : rhsOrdered - lhsOrdered;
}

void assertNoVulkanCallbacks(const viennaps::Process<float, 2> &process) {
  VC_TEST_ASSERT(!static_cast<bool>(process.getCoverageDeltaExecutor()));
  VC_TEST_ASSERT(!static_cast<bool>(process.getSurfaceDiffusionExecutor()));
  VC_TEST_ASSERT(!static_cast<bool>(process.getLevelSetUpdateExecutor()));
  VC_TEST_ASSERT(!static_cast<bool>(process.getLevelSetRebuildExecutor()));
}

[[nodiscard]] Composition::SurfacePaths makeSurfacePaths() {
  Composition::SurfacePaths paths;
  paths.coverageDelta = VIENNAPS_VULKAN_COVERAGE_DELTA_METRIC_SPV_PATH;
  paths.surfaceDiffusion = VIENNAPS_VULKAN_GRAPH_DIFFUSION_SPV_PATH;
  paths.neutralTransportVelocity = VIENNAPS_VULKAN_NEUTRAL_TRANSPORT_SPV_PATH;
  paths.rayFlux.triangleHit = VIENNAPS_VULKAN_TRIANGLE_HIT_DEVICE_SPV_PATH;
  paths.rayFlux.recordCompaction =
      VIENNAPS_VULKAN_RAY_RECORD_COMPACTION_SPV_PATH;
  paths.rayFlux.reductionScan = VIENNAPS_VULKAN_REDUCTION_SCAN_SPV_PATH;
  paths.rayFlux.radixHistogram =
      VIENNAPS_VULKAN_RAY_RECORD_RADIX_HISTOGRAM_SPV_PATH;
  paths.rayFlux.radixPrefix =
      VIENNAPS_VULKAN_RAY_RECORD_RADIX_PREFIX_SPV_PATH;
  paths.rayFlux.radixScatter =
      VIENNAPS_VULKAN_RAY_RECORD_RADIX_SCATTER_SPV_PATH;
  paths.rayFlux.surfaceSegments =
      VIENNAPS_VULKAN_RAY_SURFACE_SEGMENTS_SPV_PATH;
  paths.rayFlux.surfaceReduce = VIENNAPS_VULKAN_RAY_SURFACE_REDUCE_SPV_PATH;
  paths.rayFlux.triangleBvh = VIENNAPS_VULKAN_TRIANGLE_BVH_HIT_SPV_PATH;
  return paths;
}

[[nodiscard]] Composition::RebuildPaths makeRebuildPaths() {
  return Composition::RebuildPaths{
      VIENNAPS_HRLE_CLASSIFICATION_SPV_PATH, VIENNAPS_REDUCTION_SCAN_SPV_PATH,
      VIENNAPS_HRLE_ACTION_FLAGS_SPV_PATH, VIENNAPS_HRLE_COMPACT_SPV_PATH};
}

[[nodiscard]] std::vector<StageWorkload> makeWorkloads() {
  return {
      StageWorkload{Stage::COVERAGE, Precision::FP32, 1U, false, RayMode::NONE,
                    true},
      StageWorkload{Stage::SURFACE_DIFFUSION, Precision::FP32, 2U, false,
                    RayMode::NONE, true},
      StageWorkload{Stage::RAY_TRACING, Precision::FP32, 3U, false,
                    RayMode::NONE, true},
      StageWorkload{Stage::LEVEL_SET, Precision::FP32, 4096U, false,
                    RayMode::NONE, true}};
}

[[nodiscard]] bool runManualCpuSmoke(const std::span<const StageWorkload> workloads,
                                     const StageWorkload &levelSetWorkload) {
  auto model = makeEligibleRayModel();
  auto retainedModel =
      viennacore::SmartPointer<viennaps::ProcessModelBase<float, 2>>(model);
  auto domain = makePlaneDomain();
  viennaps::Process<float, 2> process(domain, model);
  process.setFluxEngineType(viennaps::FluxEngineType::CPU_TRIANGLE);
  process.setProcessDuration(0.0);
  process.setParameters(singleBounceParams());

  Composition composition;
  ManualSelectionConfig selection;
  selection.selectionMode = SelectionMode::MANUAL;
  selection.globalBackend = ComputeBackend::CPU;

  const auto surfacePaths = makeSurfacePaths();
  const auto rebuildPaths = makeRebuildPaths();
  const viennaps::compute::DeploymentProfileDecision missingDecision{};
  std::cout << "[LevelSetSurfaceRayDeployment] manual CPU configure start"
            << std::endl;
  const auto result = composition.configure(
      process, missingDecision, {}, workloads, levelSetWorkload, selection, {},
      surfacePaths, retainedModel, VIENNAPS_LEVELSET_UPDATE_SPV_PATH,
      rebuildPaths);
  std::cout << "[LevelSetSurfaceRayDeployment] manual CPU configure returned"
            << std::endl;
  if (!require(result.ok, "manual CPU result.ok"))
    return false;
  if (!require(result.prepared, "manual CPU result.prepared"))
    return false;
  if (!require(!result.usingVulkan, "manual CPU does not use Vulkan"))
    return false;
  if (!require(!composition.sharedContext(), "manual CPU has no context"))
    return false;
  assertNoVulkanCallbacks(process);
  std::cout << "[LevelSetSurfaceRayDeployment] CPU surface oracle start"
            << std::endl;
  const auto cpuMesh = process.calculateFlux();
  if (!require(cpuMesh != nullptr, "CPU surface oracle returns mesh"))
    return false;
  return true;
}

[[nodiscard]] bool runManualVulkanFailClosedSmoke(
    const std::span<const StageWorkload> workloads,
    const StageWorkload &levelSetWorkload) {
  auto model = makeEligibleRayModel();
  auto domain = makePlaneDomain();
  viennaps::Process<float, 2> process(domain, model);
  Composition composition;
  ManualSelectionConfig selection;
  selection.selectionMode = SelectionMode::MANUAL;
  selection.globalBackend = ComputeBackend::VULKAN;
  const auto missingDecision = viennaps::compute::DeploymentProfileDecision{};
  const auto result = composition.configure(
      process, missingDecision, {}, workloads, levelSetWorkload, selection, {},
      makeSurfacePaths(), viennacore::SmartPointer<viennaps::ProcessModelBase<
          float, 2>>(model), VIENNAPS_LEVELSET_UPDATE_SPV_PATH,
      makeRebuildPaths());
  if (!require(!result.ok, "manual Vulkan missing profile fails closed"))
    return false;
  if (!require(!composition.sharedContext(),
               "manual Vulkan failure releases deployment context"))
    return false;
  assertNoVulkanCallbacks(process);
  return true;
}

} // namespace

int main() try {
  viennacore::Logger::setLogLevel(viennacore::LogLevel::WARNING);

  const auto workloads = makeWorkloads();
  const auto surfaceWorkloads = std::span<const StageWorkload>(workloads);
  const auto &levelSetWorkload = workloads.back();

  std::cout << "[LevelSetSurfaceRayDeployment] start manual Vulkan fail-closed"
            << std::endl;
  if (!runManualVulkanFailClosedSmoke(surfaceWorkloads, levelSetWorkload))
    return EXIT_FAILURE;
  std::cout << "[LevelSetSurfaceRayDeployment] start manual CPU smoke" << std::endl;
  if (!runManualCpuSmoke(surfaceWorkloads, levelSetWorkload))
    return EXIT_FAILURE;
  std::cout << "[LevelSetSurfaceRayDeployment] manual CPU smoke OK" << std::endl;

  HardwareFingerprint hardware;
  std::string error;
  if (!collectHardware(hardware, error)) {
    std::cout << "[LevelSetSurfaceRayDeployment] SKIP: " << error << '\n';
    return EXIT_SUCCESS;
  }
  const auto decision = makeVulkanDecision(hardware, surfaceWorkloads);

  auto model = makeEligibleRayModel();
  auto retainedModel =
      viennacore::SmartPointer<viennaps::ProcessModelBase<float, 2>>(model);
  auto domain = makePhysicsDomain();
  viennaps::Process<float, 2> process(domain, model);
  process.setProcessDuration(0.0);
  process.setParameters(singleBounceParams());

  Composition composition;
  ManualSelectionConfig selection;
  selection.selectionMode = SelectionMode::MANUAL;
  selection.globalBackend = ComputeBackend::VULKAN;
  const auto surfacePaths = makeSurfacePaths();
  const auto rebuildPaths = makeRebuildPaths();

  std::cout << "[LevelSetSurfaceRayDeployment] start manual Vulkan configure"
            << std::endl;
  const auto configured = composition.configure(
      process, decision, hardware, surfaceWorkloads, levelSetWorkload, selection,
      {}, surfacePaths, retainedModel, VIENNAPS_LEVELSET_UPDATE_SPV_PATH,
      rebuildPaths);
  if (!require(configured.ok, "manual Vulkan result.ok")) {
    std::cerr << "[LevelSetSurfaceRayDeployment] message: " << configured.message
              << '\n';
    return EXIT_FAILURE;
  }
  std::cout << "[LevelSetSurfaceRayDeployment] manual Vulkan configure OK"
            << std::endl;
  if (!require(configured.prepared, "manual Vulkan result.prepared"))
    return EXIT_FAILURE;
  if (!require(!configured.degraded, "manual Vulkan not degraded"))
    return EXIT_FAILURE;
  if (!require(configured.usingVulkan, "manual Vulkan usingVulkan"))
    return EXIT_FAILURE;
  if (!require(configured.surface.rayTracingVulkan,
               "manual Vulkan rayTracingVulkan"))
    return EXIT_FAILURE;
  if (!require(configured.surface.coverageVulkan,
               "manual Vulkan coverageVulkan"))
    return EXIT_FAILURE;
  if (!require(configured.surface.surfaceDiffusionVulkan,
               "manual Vulkan surfaceDiffusionVulkan"))
    return EXIT_FAILURE;
  if (!require(!configured.surface.neutralTransportVelocityVulkan,
               "single-particle route has no neutral velocity executor"))
    return EXIT_FAILURE;
  if (!require(configured.levelSet.usingVulkan,
               "manual Vulkan levelSet.usingVulkan"))
    return EXIT_FAILURE;
  const auto context = composition.sharedContext();
  if (!require(context != nullptr, "one shared deployment context"))
    return EXIT_FAILURE;
  const auto *session = context->session();
  if (!require(session != nullptr && session->generation() != 0U,
               "shared deployment session generation"))
    return EXIT_FAILURE;
  if (!require(configured.surface.sessionGeneration == session->generation() &&
                   configured.surface.device == session->deviceHandle(),
               "surface uses shared deployment session"))
    return EXIT_FAILURE;
  if (!require(process.getCoverageDeltaExecutor() &&
                   process.getSurfaceDiffusionExecutor() &&
                   process.getLevelSetUpdateExecutor() &&
                   process.getLevelSetRebuildExecutor(),
               "shared-session callbacks installed"))
    return EXIT_FAILURE;

  // Freeze the CPU_TRIANGLE Process route immediately before exercising the
  // configured Vulkan route.  Both paths use the strict eligible model and
  // the same deterministic ray parameters.
  std::cout << "[LevelSetSurfaceRayDeployment] CPU flux baseline start\n";
  auto cpuModel = makeEligibleRayModel();
  auto cpuDomain = makePhysicsDomain();
  viennaps::Process<float, 2> cpuProcess(cpuDomain, cpuModel);
  cpuProcess.setFluxEngineType(viennaps::FluxEngineType::CPU_TRIANGLE);
  cpuProcess.setProcessDuration(0.0);
  cpuProcess.setParameters(singleBounceParams());
  const auto cpuFluxMesh = cpuProcess.calculateFlux();
  const auto cpuFlux = readParticleFlux(cpuFluxMesh);
  if (!require(!cpuFlux.empty(), "CPU_TRIANGLE flux baseline produced data"))
    return EXIT_FAILURE;

  // Process::calculateFlux consumes its override.  The composition adapter
  // recreates that engine from the existing shared session for the following
  // Process::apply() transaction without replacing callback state.
  std::cout << "[LevelSetSurfaceRayDeployment] Vulkan calculateFlux start\n";
  const auto vulkanFluxMesh = process.calculateFlux();
  const auto vulkanFlux = readParticleFlux(vulkanFluxMesh);
  if (!require(!vulkanFlux.empty(),
               "Vulkan Process::calculateFlux produced data"))
    return EXIT_FAILURE;
  if (!require(cpuFlux.size() == vulkanFlux.size(),
               "CPU/Vulkan flux array sizes match"))
    return EXIT_FAILURE;
  std::uint32_t maxFluxUlp = 0U;
  double cpuFluxTotal = 0.0;
  double vulkanFluxTotal = 0.0;
  for (std::size_t index = 0U; index < cpuFlux.size(); ++index) {
    maxFluxUlp = std::max(maxFluxUlp, ulpDistance(cpuFlux[index],
                                                   vulkanFlux[index]));
    cpuFluxTotal += static_cast<double>(cpuFlux[index]);
    vulkanFluxTotal += static_cast<double>(vulkanFlux[index]);
  }
  const double totalFluxRelDiff =
      std::abs(cpuFluxTotal - vulkanFluxTotal) /
      std::max(std::abs(cpuFluxTotal), 1.0e-12);
  if (!require(maxFluxUlp == 0U && totalFluxRelDiff <= 1.0e-6,
               "Vulkan calculateFlux matches CPU raw-bit/total oracle"))
    return EXIT_FAILURE;

  auto cpuApplyModel = makeEligibleRayModel();
  auto cpuApplyDomain = makePhysicsDomain();
  viennaps::Process<float, 2> cpuApply(cpuApplyDomain, cpuApplyModel);
  cpuApply.setFluxEngineType(viennaps::FluxEngineType::CPU_TRIANGLE);
  cpuApply.setProcessDuration(0.01);
  cpuApply.setParameters(singleBounceParams());
  viennaps::AdvectionParameters advection;
  advection.timeStepRatio = 0.05;
  cpuApply.setParameters(advection);
  std::cout << "[LevelSetSurfaceRayDeployment] CPU apply baseline start\n";
  cpuApply.apply();
  if (!require(cpuApply.getLastProcessResult() ==
                   viennaps::ProcessResult::SUCCESS,
               "CPU apply oracle succeeds"))
    return EXIT_FAILURE;
  const auto cpuApplyValues = snapshotSurfaceValues(cpuApplyDomain);

  if (!require(composition.installRayFluxEngine(process, false),
               "reinstall Vulkan ray engine for apply"))
    return EXIT_FAILURE;
  process.setProcessDuration(0.01);
  process.setParameters(advection);
  std::cout << "[LevelSetSurfaceRayDeployment] Vulkan apply start\n";
  process.apply();
  if (!require(process.getLastProcessResult() == viennaps::ProcessResult::SUCCESS,
               "Vulkan Process::apply succeeds"))
    return EXIT_FAILURE;
  const auto vulkanApplyValues = snapshotSurfaceValues(domain);
  if (!require(cpuApplyValues.size() == vulkanApplyValues.size(),
               "CPU/Vulkan apply geometry sizes match"))
    return EXIT_FAILURE;
  float maxGeometryDelta = 0.0F;
  for (std::size_t index = 0U; index < cpuApplyValues.size(); ++index)
    maxGeometryDelta = std::max(
        maxGeometryDelta, std::abs(cpuApplyValues[index] -
                                   vulkanApplyValues[index]));
  if (!require(maxGeometryDelta <= 1.0e-5F,
               "Vulkan apply geometry matches CPU oracle"))
    return EXIT_FAILURE;
  std::cout << "[LevelSetSurfaceRayDeployment] Vulkan Process "
               "calculateFlux/apply PASS (maxFluxUlp="
            << maxFluxUlp << ", totalFluxRelDiff=" << totalFluxRelDiff
            << ", maxGeometryDelta=" << maxGeometryDelta << ")\n";

  composition.clear(process);
  if (!require(!composition.sharedContext(), "context cleared"))
    return EXIT_FAILURE;
  assertNoVulkanCallbacks(process);

  // AUTO degradation: missing ray SPIR-V path must fall back to CPU and clear
  // all installed Process callbacks/override.
  auto autoDomain = makePhysicsDomain();
  viennaps::Process<float, 2> autoProcess(autoDomain, model);
  autoProcess.setProcessDuration(0.0);
  autoProcess.setParameters(singleBounceParams());
  Composition autoComposition;
  auto missingRayPaths = surfacePaths;
  missingRayPaths.rayFlux.triangleHit.clear();
  const auto autoResult = autoComposition.configure(
      autoProcess, decision, hardware, surfaceWorkloads, levelSetWorkload,
      ManualSelectionConfig{}, {}, missingRayPaths, retainedModel,
      VIENNAPS_LEVELSET_UPDATE_SPV_PATH, rebuildPaths);
  if (!require(autoResult.ok && autoResult.degraded,
               "AUTO missing ray path degraded"))
    return EXIT_FAILURE;
  if (!require(!autoComposition.sharedContext(),
               "AUTO degraded context released"))
    return EXIT_FAILURE;
  assertNoVulkanCallbacks(autoProcess);

  std::cout << "[LevelSetSurfaceRayDeployment] deployment session/fallback "
               "checks PASS; CPU/Vulkan Process physics oracle PASS\n";
  return EXIT_SUCCESS;
} catch (const std::exception &exception) {
  std::cerr << exception.what() << '\n';
  return EXIT_FAILURE;
}
