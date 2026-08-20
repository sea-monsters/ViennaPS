// Copyright 2026 ViennaPS
// SPDX-License-Identifier: MIT
//
// P5-S0 Vulkan surface-process acceptance smoke test.  The positive route
// requests COVERAGE, SURFACE_DIFFUSION, NEUTRAL_TRANSPORT_VELOCITY, and
// RAY_TRACING on one shared Vulkan session and is expected to be RED on
// current code: complete NeutralTransport surface physics (multi-bounce
// re-emission, coverage, desorption, surface diffusion) is not yet resident
// on the device route, so the device ray pipeline rejects the reflection
// request fail-closed.

// Include capabilityProfileIO.hpp first: on Windows it pulls in <Windows.h>,
// whose wingdi.h ERROR/WARNING/INFO/DEBUG macros break the Status::ERROR-style
// enumerators in the level-set deployment headers.  Windows.h is
// include-guarded, so undefining the macros right after this first inclusion
// keeps every later include chain clean (mirrors the guard in viennaps.hpp).
#include <compute/capabilityProfileIO.hpp>

#ifdef ERROR
#undef ERROR
#endif
#ifdef WARNING
#undef WARNING
#endif
#ifdef INFO
#undef INFO
#endif
#ifdef DEBUG
#undef DEBUG
#endif

#include "../../../tests/surfaceProcessAcceptance/surface_acceptance_fixture.hpp"

#include "levelset_surface_deployment_composition.hpp"

#include <compute/backendPolicy.hpp>
#include <compute/deploymentProfile.hpp>
#include <models/psNeutralTransport.hpp>
#include <process/psProcess.hpp>

#include "../runtime/compute_session.hpp"

#include <array>
#include <bit>
#include <cstdint>
#include <exception>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace {

using T = float;
constexpr int D = 2;
using Composition = viennaps::vulkan::levelset::LevelSetSurfaceDeploymentComposition<D>;
using SurfaceBinding = viennaps::vulkan::surface::ProcessDeploymentBinding<D>;
using namespace viennaps::compute;

std::string cpuRecordPath;

bool parseArguments(int argc, char *argv[]) {
  for (int i = 1; i < argc; ++i) {
    if (std::string(argv[i]) == "--cpu-record" && i + 1 < argc) {
      cpuRecordPath = argv[++i];
    }
  }
  return true;
}

bool require(const bool condition, const char *message) {
  if (!condition)
    std::cerr << message << '\n';
  return condition;
}

HardwareFingerprint probeFingerprint(viennaps::vulkan::runtime::ComputeSession &session) {
  HardwareFingerprint fingerprint{};
  if (!session.isValid())
    return fingerprint;

  VkPhysicalDeviceIDProperties ids{};
  ids.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES;
  VkPhysicalDeviceProperties2 properties{};
  properties.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
  properties.pNext = &ids;
  vkGetPhysicalDeviceProperties2(session.selection().handle, &properties);

  auto uuid = [](const std::uint8_t *bytes) {
    std::ostringstream stream;
    stream << std::hex << std::setfill('0');
    for (std::size_t i = 0U; i < VK_UUID_SIZE; ++i)
      stream << std::setw(2) << static_cast<unsigned int>(bytes[i]);
    return stream.str();
  };

  fingerprint.deviceUuid = uuid(ids.deviceUUID);
  fingerprint.driverUuid = uuid(ids.driverUUID);
  fingerprint.vendorId = properties.properties.vendorID;
  fingerprint.deviceId = properties.properties.deviceID;
  fingerprint.deviceName = properties.properties.deviceName;
  fingerprint.driverVersion =
      std::to_string(properties.properties.driverVersion);
  return fingerprint;
}

DeploymentProfileDecision makeNoVulkanDecision(std::span<const StageWorkload> workloads) {
  DeploymentProfileDecision decision{};
  decision.state = DeploymentProfileState::VALID;
  decision.hasProfile = true;
  decision.plan.ok = true;
  for (const auto &workload : workloads) {
    StageSelection stage{workload.stage};
    stage.selected = true;
    stage.selectedBackend = ComputeBackend::CPU;
    decision.plan.stages.push_back(stage);
  }
  return decision;
}

CapabilityProfile makeVulkanCapabilityProfile() {
  CapabilityProfile profile;
  profile.cpuAvailable = true;
  profile.vulkanAvailable = true;
  profile.vulkanPrimitiveSuitePass = true;
  profile.vulkanCompute = true;
  profile.vulkanRayQuery = true;
  profile.vulkanRayTracingPipeline = true;
  profile.safeVulkanWorkingSetBytes = 1024U * 1024U;
  return profile;
}

DeploymentProfileDecision makeVulkanDecision(
    std::span<const StageWorkload> workloads,
    const HardwareFingerprint &fingerprint) {
  DeploymentProfileDecision decision{};
  decision.state = DeploymentProfileState::VALID;
  decision.hasProfile = true;
  decision.activeRecord.hardware = fingerprint;
  decision.activeRecord.capabilityProfile = makeVulkanCapabilityProfile();
  decision.plan.ok = true;
  for (const auto &workload : workloads) {
    StageSelection stage{workload.stage};
    stage.selected = true;
    stage.selectedBackend = ComputeBackend::VULKAN;
    decision.plan.stages.push_back(stage);
  }
  return decision;
}

std::array<StageWorkload, 5> makeSurfaceWorkloads() {
  return {
      StageWorkload{Stage::COVERAGE, Precision::FP32, 1U, false, RayMode::NONE,
                    true},
      StageWorkload{Stage::SURFACE_DIFFUSION, Precision::FP32, 1U, false,
                    RayMode::NONE, true},
      StageWorkload{Stage::NEUTRAL_TRANSPORT_VELOCITY, Precision::FP32, 1U,
                    false, RayMode::NONE, true},
      StageWorkload{Stage::RAY_TRACING, Precision::FP32, 1U, false,
                    RayMode::COMPUTE_BVH, true},
      // The shared deployment plan must carry the composition-owned LEVEL_SET
      // stage as its last entry; the controller looks it up in the resolved
      // decision instead of re-resolving it (levelset_surface_ray_deployment
      // smoke pattern).
      StageWorkload{Stage::LEVEL_SET, Precision::FP32, 1024U, false,
                    RayMode::NONE, true}};
}

StageWorkload makeLevelSetWorkload() {
  return StageWorkload{Stage::LEVEL_SET, Precision::FP32, 1024U, false,
                       RayMode::NONE, true};
}

SurfaceBinding::SpirvPaths makeSurfacePaths() {
  SurfaceBinding::SpirvPaths paths;
  paths.coverageDelta = VIENNAPS_VULKAN_COVERAGE_DELTA_METRIC_SPV_PATH;
  paths.surfaceDiffusion = VIENNAPS_VULKAN_GRAPH_DIFFUSION_SPV_PATH;
  paths.neutralTransportVelocity = VIENNAPS_VULKAN_NEUTRAL_TRANSPORT_SPV_PATH;
#if defined(VIENNAPS_VULKAN_TRIANGLE_HIT_DEVICE_SPV_PATH)
  // The ray SPIR-V paths are wired from the ray subdirectory, which is
  // configured after this one; they stay empty when the ray targets were not
  // built and main() reports HARDWARE_UNAVAILABLE.
  paths.rayFlux.triangleHit = VIENNAPS_VULKAN_TRIANGLE_HIT_DEVICE_SPV_PATH;
  paths.rayFlux.recordCompaction =
      VIENNAPS_VULKAN_RAY_RECORD_COMPACTION_SPV_PATH;
  paths.rayFlux.reductionScan = VIENNAPS_VULKAN_REDUCTION_SCAN_SPV_PATH;
  paths.rayFlux.radixHistogram =
      VIENNAPS_VULKAN_RAY_RECORD_RADIX_HISTOGRAM_SPV_PATH;
  paths.rayFlux.radixPrefix = VIENNAPS_VULKAN_RAY_RECORD_RADIX_PREFIX_SPV_PATH;
  paths.rayFlux.radixScatter = VIENNAPS_VULKAN_RAY_RECORD_RADIX_SCATTER_SPV_PATH;
  paths.rayFlux.surfaceSegments =
      VIENNAPS_VULKAN_RAY_SURFACE_SEGMENTS_SPV_PATH;
  paths.rayFlux.surfaceReduce = VIENNAPS_VULKAN_RAY_SURFACE_REDUCE_SPV_PATH;
  paths.rayFlux.triangleBvh = VIENNAPS_VULKAN_TRIANGLE_BVH_HIT_SPV_PATH;
  paths.rayFlux.multibounceFrontierQueue =
      VIENNAPS_VULKAN_MULTIBOUNCE_FRONTIER_QUEUE_SPV_PATH;
#endif
  return paths;
}

Composition::RebuildPaths makeRebuildPaths() {
  Composition::RebuildPaths paths;
  paths.classification = VIENNAPS_HRLE_CLASSIFICATION_SPV_PATH;
  paths.reductionScan = VIENNAPS_REDUCTION_SCAN_SPV_PATH;
  paths.actionFlags = VIENNAPS_HRLE_ACTION_FLAGS_SPV_PATH;
  paths.compact = VIENNAPS_HRLE_COMPACT_SPV_PATH;
  return paths;
}

// Negative battery: behavior that must hold even when no hardware is present.
bool runNegativeBattery(const SurfaceBinding::SpirvPaths &surfacePaths) {
  const auto workloads = makeSurfaceWorkloads();
  const auto levelSetWorkload = makeLevelSetWorkload();

  // (a) AUTO with no Vulkan capability must degrade cleanly and leave the
  // Process callbacks empty so a CPU run remains possible.
  {
    viennaps::Process<T, D> process;
    process.setCoverageDeltaExecutor(
        [](viennaps::CoverageDeltaWork<T> &, std::string &) { return true; });
    process.setSurfaceDiffusionStatusExecutor(
        [](viennaps::SurfaceDiffusionWork<T> &, std::string &) {
          return viennaps::SurfaceDiffusionExecutionStatus::SUCCESS;
        });

    Composition composition;
    ManualSelectionConfig autoSelection{};
    const auto noVulkanDecision = makeNoVulkanDecision(workloads);
    const auto result = composition.configure(
        process, noVulkanDecision, HardwareFingerprint{}, workloads,
        levelSetWorkload, autoSelection, {}, surfacePaths,
        viennaps::SmartPointer<viennaps::ProcessModelBase<T, D>>{},
        VIENNAPS_LEVELSET_UPDATE_SPV_PATH, makeRebuildPaths());
    if (!require(result.ok && result.degraded,
                 "NEGATIVE: AUTO no-Vulkan must degrade"))
      return false;
    if (!require(!process.getCoverageDeltaExecutor() &&
                     !process.getSurfaceDiffusionExecutor(),
                 "NEGATIVE: AUTO no-Vulkan must clear Process callbacks"))
      return false;
  }

  // (b) MANUAL Vulkan with an invalid device option must fail closed.
  {
    viennaps::Process<T, D> process;
    Composition composition;
    ManualSelectionConfig manualSelection{};
    manualSelection.selectionMode = SelectionMode::MANUAL;
    manualSelection.globalBackend = ComputeBackend::VULKAN;

    viennaps::vulkan::runtime::ComputeSessionOptions invalidDevice;
    invalidDevice.manualDeviceIndex = 999999U;
    const auto vulkanDecision = makeVulkanDecision(workloads, HardwareFingerprint{});
    const auto result = composition.configure(
        process, vulkanDecision, HardwareFingerprint{}, workloads,
        levelSetWorkload, manualSelection, invalidDevice, surfacePaths,
        viennaps::SmartPointer<viennaps::ProcessModelBase<T, D>>{},
        VIENNAPS_LEVELSET_UPDATE_SPV_PATH, makeRebuildPaths());
    if (!require(!result.ok, "NEGATIVE: manual Vulkan invalid device must fail"))
      return false;
    if (!require(!process.getCoverageDeltaExecutor() &&
                     !process.getSurfaceDiffusionExecutor(),
                 "NEGATIVE: manual Vulkan failure must clear callbacks"))
      return false;
  }

  // (c) Retained neutral velocity callback after the binding and its session
  // are destroyed must fail closed rather than crash or produce data.
  {
    viennaps::Process<T, D> process;
    const auto params = surface_process_acceptance::makeAcceptanceParameters<T>();
    auto model =
        viennaps::SmartPointer<viennaps::NeutralTransport<T, D>>::New(params);
    process.setProcessModel(model);
    const auto modelHandle =
        viennaps::SmartPointer<viennaps::ProcessModelBase<T, D>>(model);

    viennaps::vulkan::runtime::ComputeSession probeSession;
    std::string error;
    if (!probeSession.initialize(error))
      return require(false, "NEGATIVE: probe session unavailable");
    const auto fingerprint = probeFingerprint(probeSession);

    auto composition = std::make_unique<Composition>();
    ManualSelectionConfig manualSelection{};
    manualSelection.selectionMode = SelectionMode::MANUAL;
    manualSelection.globalBackend = ComputeBackend::VULKAN;
    manualSelection.precision = Precision::FP32;
    const auto vulkanDecision = makeVulkanDecision(workloads, fingerprint);
    const auto configured = composition->configure(
        process, vulkanDecision, fingerprint, workloads, levelSetWorkload,
        manualSelection, {}, surfacePaths, modelHandle,
        VIENNAPS_LEVELSET_UPDATE_SPV_PATH, makeRebuildPaths());

    viennaps::NeutralTransportVelocityExecutor<T> retained;
    if (configured.ok && configured.usingVulkan) {
      retained = composition->neutralVelocityExecutor();
    }

    // The composition is neither copyable nor movable; allocate it on the
    // heap so destroying it here leaves only the retained callback keeping
    // the callback holder (and its borrowed session) alive.
    composition.reset();

    // Force the borrowed session out from under the retained callback; this is
    // the only deterministic way to exercise fail-closed behavior without
    // racing device teardown.
    probeSession.reset();

    if (retained) {
      std::vector<T> candidate(1U, T(0));
      viennaps::NeutralTransportVelocityWork<T> work{
          std::span<const T>{}, std::span<const T>{},
          std::span<T>(candidate), {}};
      std::string invokeError;
      const bool invokeOk = retained(work, invokeError) && work.complete &&
                            work.writtenCount == candidate.size();
      if (!require(!invokeOk,
                   "NEGATIVE: retained neutral callback must fail closed"))
        return false;
    }
  }

  // (d) Session reset must invalidate the old generation.
  {
    viennaps::vulkan::runtime::ComputeSession session;
    std::string error;
    if (!session.initialize(error))
      return require(false, "NEGATIVE: session unavailable for generation test");
    const auto generation = session.generation();
    if (!require(viennaps::vulkan::runtime::isLiveComputeSessionGeneration(generation),
                 "NEGATIVE: current generation must be live"))
      return false;
    session.reset();
    if (!require(!viennaps::vulkan::runtime::isLiveComputeSessionGeneration(generation),
                 "NEGATIVE: old generation must be dead after reset"))
      return false;
  }

  return true;
}

bool recordsBitwiseEqual(
    const surface_process_acceptance::AcceptanceRecord<T, D> &cpu,
    const surface_process_acceptance::AcceptanceRecord<T, D> &vulkan) {
  bool equal = true;
  auto check = [&](const char *name, bool condition) {
    if (!condition) {
      std::cerr << "RED_BOUNDARY=" << name << '\n';
      equal = false;
    }
  };

  check("schema", cpu.kSchemaVersion == vulkan.kSchemaVersion);
  check("gridDelta",
        std::bit_cast<std::uint32_t>(cpu.gridDelta) ==
            std::bit_cast<std::uint32_t>(vulkan.gridDelta));
  check("xExtent",
        std::bit_cast<std::uint32_t>(cpu.xExtent) ==
            std::bit_cast<std::uint32_t>(vulkan.xExtent));
  check("yExtent",
        std::bit_cast<std::uint32_t>(cpu.yExtent) ==
            std::bit_cast<std::uint32_t>(vulkan.yExtent));
  check("processDuration",
        std::bit_cast<std::uint32_t>(cpu.processDuration) ==
            std::bit_cast<std::uint32_t>(vulkan.processDuration));
  check("rngSeed", cpu.rngSeed == vulkan.rngSeed);
  check("raysPerPoint", cpu.raysPerPoint == vulkan.raysPerPoint);
  check("maxReflections", cpu.maxReflections == vulkan.maxReflections);
  check("initialCoverageValue",
        std::bit_cast<std::uint32_t>(cpu.initialCoverageValue) ==
            std::bit_cast<std::uint32_t>(vulkan.initialCoverageValue));

  check("fluxCellData.count", cpu.fluxCellData.size() == vulkan.fluxCellData.size());
  if (cpu.fluxCellData.size() == vulkan.fluxCellData.size()) {
    for (std::size_t i = 0; i < cpu.fluxCellData.size(); ++i) {
      check("fluxCellData.value",
            std::bit_cast<std::uint32_t>(cpu.fluxCellData[i]) ==
                std::bit_cast<std::uint32_t>(vulkan.fluxCellData[i]));
    }
  }
  check("fluxCellDataSum",
        std::bit_cast<std::uint32_t>(cpu.fluxCellDataSum) ==
            std::bit_cast<std::uint32_t>(vulkan.fluxCellDataSum));

  check("processResult", cpu.processResult == vulkan.processResult);

  check("initialCoverages.count",
        cpu.initialCoverages.size() == vulkan.initialCoverages.size());
  if (cpu.initialCoverages.size() == vulkan.initialCoverages.size()) {
    for (std::size_t i = 0; i < cpu.initialCoverages.size(); ++i) {
      check("initialCoverages.value",
            std::bit_cast<std::uint32_t>(cpu.initialCoverages[i]) ==
                std::bit_cast<std::uint32_t>(vulkan.initialCoverages[i]));
    }
  }

  check("finalSurfacePoints.count",
        cpu.finalSurfacePoints.size() == vulkan.finalSurfacePoints.size());
  if (cpu.finalSurfacePoints.size() == vulkan.finalSurfacePoints.size()) {
    for (std::size_t i = 0; i < cpu.finalSurfacePoints.size(); ++i) {
      for (int j = 0; j < 3; ++j) {
        check("finalSurfacePoints.value",
              std::bit_cast<std::uint32_t>(cpu.finalSurfacePoints[i][j]) ==
                  std::bit_cast<std::uint32_t>(vulkan.finalSurfacePoints[i][j]));
      }
    }
  }

  check("finalSurfaceMaterialIds.count",
        cpu.finalSurfaceMaterialIds.size() == vulkan.finalSurfaceMaterialIds.size());
  if (cpu.finalSurfaceMaterialIds.size() == vulkan.finalSurfaceMaterialIds.size()) {
    for (std::size_t i = 0; i < cpu.finalSurfaceMaterialIds.size(); ++i) {
      check("finalSurfaceMaterialIds.value",
            std::bit_cast<std::uint32_t>(cpu.finalSurfaceMaterialIds[i]) ==
                std::bit_cast<std::uint32_t>(vulkan.finalSurfaceMaterialIds[i]));
    }
  }

  check("finalCoverages.count",
        cpu.finalCoverages.size() == vulkan.finalCoverages.size());
  if (cpu.finalCoverages.size() == vulkan.finalCoverages.size()) {
    for (std::size_t i = 0; i < cpu.finalCoverages.size(); ++i) {
      check("finalCoverages.value",
            std::bit_cast<std::uint32_t>(cpu.finalCoverages[i]) ==
                std::bit_cast<std::uint32_t>(vulkan.finalCoverages[i]));
    }
  }

  check("topLevelSetValid", cpu.topLevelSetValid == vulkan.topLevelSetValid);
  check("processTime",
        std::bit_cast<std::uint32_t>(cpu.processTime) ==
            std::bit_cast<std::uint32_t>(vulkan.processTime));
  check("advectionIterationCount",
        cpu.advectionIterationCount == vulkan.advectionIterationCount);

  return equal;
}

bool runPositiveRoute(const SurfaceBinding::SpirvPaths &surfacePaths) {
  viennaps::vulkan::runtime::ComputeSession probeSession;
  std::string error;
  if (!probeSession.initialize(error)) {
    std::cout << "HARDWARE_UNAVAILABLE\n";
    return true;
  }
  const auto fingerprint = probeFingerprint(probeSession);
  probeSession.reset();

  const auto workloads = makeSurfaceWorkloads();
  const auto levelSetWorkload = makeLevelSetWorkload();

  surface_process_acceptance::AcceptanceConfig<T> config;
  auto [domain, model] =
      surface_process_acceptance::makeAcceptanceDomainAndModel<T, D>(config);

  viennaps::Process<T, D> process(domain, model);
  const auto modelHandle =
      viennaps::SmartPointer<viennaps::ProcessModelBase<T, D>>(model);

  Composition composition;
  ManualSelectionConfig manualSelection{};
  manualSelection.selectionMode = SelectionMode::MANUAL;
  manualSelection.globalBackend = ComputeBackend::VULKAN;
  manualSelection.precision = Precision::FP32;
  const auto vulkanDecision = makeVulkanDecision(workloads, fingerprint);

  const auto compositionResult = composition.configure(
      process, vulkanDecision, fingerprint, workloads, levelSetWorkload,
      manualSelection, {}, surfacePaths, modelHandle,
      VIENNAPS_LEVELSET_UPDATE_SPV_PATH, makeRebuildPaths());

  bool ok = true;
  auto assertSurface = [&](const char *name, bool condition) {
    if (!condition) {
      std::cerr << "RED_BOUNDARY=" << name << '\n';
      ok = false;
    }
  };

  assertSurface("composition.ok",
                compositionResult.ok && compositionResult.usingVulkan);
  assertSurface("composition.notDegraded", !compositionResult.degraded);
  assertSurface("surface.coverageVulkan",
                compositionResult.surface.coverageVulkan);
  assertSurface("surface.surfaceDiffusionVulkan",
                compositionResult.surface.surfaceDiffusionVulkan);
  assertSurface("surface.neutralTransportVelocityVulkan",
                compositionResult.surface.neutralTransportVelocityVulkan);
  assertSurface("surface.rayTracingVulkan",
                compositionResult.surface.rayTracingVulkan);
  assertSurface("surface.sessionGeneration",
                compositionResult.surface.sessionGeneration != 0U);
  assertSurface("levelSet.sessionGeneration",
                compositionResult.levelSet.updateSessionGeneration != 0U);
  assertSurface("sessionGenerationMatch",
                compositionResult.surface.sessionGeneration ==
                    compositionResult.levelSet.updateSessionGeneration);

  const auto context = composition.sharedContext();
  assertSurface("sharedContext",
                context != nullptr && context->session() != nullptr);
  if (context != nullptr && context->session() != nullptr) {
    assertSurface("sharedContext.generation",
                  compositionResult.surface.sessionGeneration ==
                      context->session()->generation());
  }

  if (!ok) {
    if (!compositionResult.message.empty())
      std::cerr << "composition.message=" << compositionResult.message << '\n';
    if (!compositionResult.surface.message.empty())
      std::cerr << "surface.message=" << compositionResult.surface.message
                << '\n';
    if (!compositionResult.levelSet.message.empty())
      std::cerr << "levelSet.message=" << compositionResult.levelSet.message
                << '\n';
    return false;
  }

  surface_process_acceptance::AcceptanceRecord<T, D> vulkanRecord;
  try {
    vulkanRecord = surface_process_acceptance::runAcceptance<T, D>(
        "vulkan", domain, process, model, config);
  } catch (const std::exception &exception) {
    std::cerr << "RED_BOUNDARY=vulkanRoute.exception " << exception.what()
              << '\n';
    return false;
  } catch (...) {
    std::cerr << "RED_BOUNDARY=vulkanRoute.exception unknown\n";
    return false;
  }
  std::cerr << "MARKER runAcceptance returned processResult="
            << static_cast<int>(vulkanRecord.processResult) << '\n';

  surface_process_acceptance::AcceptanceRecord<T, D> cpuRecord;
  if (!cpuRecordPath.empty()) {
    std::ifstream cpuIn(cpuRecordPath);
    if (!cpuIn || !surface_process_acceptance::deserializeRecord(cpuRecord,
                                                                 cpuIn)) {
      std::cerr << "failed to read CPU record: " << cpuRecordPath << '\n';
      return false;
    }
  } else {
    // Re-run the CPU route with a fresh domain/model so the Vulkan velocity
    // executor installed on the shared surface model does not contaminate it.
    auto [cpuDomain, cpuModel] =
        surface_process_acceptance::makeAcceptanceDomainAndModel<T, D>(config);
    viennaps::Process<T, D> cpuProcess(cpuDomain, cpuModel);
    cpuProcess.setFluxEngineType(viennaps::FluxEngineType::CPU_TRIANGLE);
    cpuRecord = surface_process_acceptance::runAcceptance<T, D>(
        "cpu", cpuDomain, cpuProcess, cpuModel, config);
  }

  return recordsBitwiseEqual(cpuRecord, vulkanRecord);
}

} // namespace

int main(int argc, char *argv[]) {
  if (!parseArguments(argc, argv))
    return 1;

  const auto surfacePaths = makeSurfacePaths();
  if (!require(!surfacePaths.coverageDelta.empty(),
               "coverage delta SPIR-V path unavailable") ||
      !require(!surfacePaths.surfaceDiffusion.empty(),
               "surface diffusion SPIR-V path unavailable") ||
      !require(!surfacePaths.neutralTransportVelocity.empty(),
               "neutral transport velocity SPIR-V path unavailable") ||
      !require(!surfacePaths.rayFlux.triangleHit.empty() &&
                   !surfacePaths.rayFlux.multibounceFrontierQueue.empty(),
               "ray tracing SPIR-V paths unavailable")) {
    std::cout << "HARDWARE_UNAVAILABLE\n";
    return 0;
  }

  if (!runNegativeBattery(surfacePaths))
    return 1;

  if (!runPositiveRoute(surfacePaths))
    return 1;

  return 0;
}
