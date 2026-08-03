// Copyright 2026 ViennaPS
// SPDX-License-Identifier: MIT

#include "levelset_surface_deployment_composition.hpp"

#include <compute/capabilityProfileIO.hpp>
#include <models/psNeutralTransport.hpp>

#include <lsMakeGeometry.hpp>
#include <lsToSurfaceMesh.hpp>
#include <lsVelocityField.hpp>

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
using SelectionMode = viennaps::compute::SelectionMode;
using Stage = viennaps::compute::Stage;
using StageSelection = viennaps::compute::StageSelection;
using StageWorkload = viennaps::compute::StageWorkload;

class ConstantVelocityField final : public viennals::VelocityField<float> {
public:
  float getScalarVelocity(const std::array<float, 3> &, int,
                          const std::array<float, 3> &,
                          unsigned long) override {
    return -0.1F;
  }
};

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

[[nodiscard]] viennaps::SmartPointer<viennaps::Domain<float, 2>>
makeProcessDomain() {
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
      levelSet, viennals::SmartPointer<viennals::Sphere<float, dimension>>::New(
                    origin, 1.5F))
      .apply();
  auto domain = viennaps::SmartPointer<viennaps::Domain<float, 2>>::New();
  domain->insertNextLevelSetAsMaterial(levelSet, viennaps::Material::Undefined,
                                       false);
  return domain;
}

[[nodiscard]] std::vector<std::array<float, 3>> collectSurfaceNodes(
    const viennals::SmartPointer<viennals::Domain<float, 2>> &domain) {
  auto mesh = viennals::SmartPointer<viennals::Mesh<float>>::New();
  viennals::ToSurfaceMesh<float, 2>(domain, mesh).apply();
  auto nodes = mesh->nodes;
  std::sort(nodes.begin(), nodes.end());
  return nodes;
}

void assertNear(const std::vector<std::array<float, 3>> &actual,
                const std::vector<std::array<float, 3>> &expected) {
  VC_TEST_ASSERT(actual.size() == expected.size());
  for (std::size_t i = 0U; i < actual.size(); ++i) {
    for (std::size_t axis = 0U; axis < 3U; ++axis)
      VC_TEST_ASSERT(std::fabs(actual[i][axis] - expected[i][axis]) <= 1.0e-6F);
  }
}

struct StepResult {
  double advectedTime = 0.0;
  std::vector<std::array<float, 3>> nodes;
};

[[nodiscard]] StepResult runOneStep(
    const viennals::SmartPointer<viennals::Domain<float, 2>> &domain,
    const viennaps::Process<float, 2>::LevelSetUpdateExecutor &update = {},
    const viennaps::Process<float, 2>::LevelSetRebuildExecutor &rebuild = {}) {
  viennals::Advect<float, 2> advect;
  advect.insertNextLevelSet(domain);
  advect.setVelocityField(
      viennals::SmartPointer<ConstantVelocityField>::New());
  advect.setSpatialScheme(
      viennals::SpatialSchemeEnum::ENGQUIST_OSHER_1ST_ORDER);
  advect.setTemporalScheme(viennals::TemporalSchemeEnum::FORWARD_EULER);
  advect.setAdvectionTime(0.05);
  advect.setTimeStepRatio(0.4999);
  advect.setDissipationAlpha(0.0);
  advect.setCheckDissipation(false);
  advect.setSingleStep(true);
  advect.setLevelSetUpdateExecutor(update);
  advect.setLevelSetRebuildExecutor(rebuild);
  advect.apply();
  return {advect.getAdvectedTime(), collectSurfaceNodes(domain)};
}

void assertNoVulkanCallbacks(const viennaps::Process<float, 2> &process) {
  VC_TEST_ASSERT(!static_cast<bool>(process.getCoverageDeltaExecutor()));
  VC_TEST_ASSERT(!static_cast<bool>(process.getSurfaceDiffusionExecutor()));
  VC_TEST_ASSERT(!static_cast<bool>(process.getLevelSetUpdateExecutor()));
  VC_TEST_ASSERT(!static_cast<bool>(process.getLevelSetRebuildExecutor()));
}

void runManualCpuSmoke(const std::span<const StageWorkload> workloads,
                       const StageWorkload &levelSetWorkload) {
  viennaps::Process<float, 2> process;
  Composition composition;
  ManualSelectionConfig selection;
  selection.selectionMode = SelectionMode::MANUAL;
  selection.globalBackend = ComputeBackend::CPU;
  const viennaps::compute::DeploymentProfileDecision missingDecision{};
  const auto result = composition.configure(
      process, missingDecision, {}, workloads, levelSetWorkload, selection, {},
      {}, {}, {}, {});
  VC_TEST_ASSERT(result.ok);
  VC_TEST_ASSERT(result.prepared);
  VC_TEST_ASSERT(!result.usingVulkan);
  VC_TEST_ASSERT(!composition.sharedContext());
  assertNoVulkanCallbacks(process);
}

} // namespace

int main() try {
  viennacore::Logger::setLogLevel(viennacore::LogLevel::WARNING);

  const std::array workloads{
      StageWorkload{Stage::COVERAGE, Precision::FP32, 1U, false,
                    viennaps::compute::RayMode::NONE, true},
      StageWorkload{Stage::SURFACE_DIFFUSION, Precision::FP32, 2U, false,
                    viennaps::compute::RayMode::NONE, true},
      StageWorkload{Stage::NEUTRAL_TRANSPORT_VELOCITY, Precision::FP32, 3U,
                    false, viennaps::compute::RayMode::NONE, true},
      StageWorkload{Stage::LEVEL_SET, Precision::FP32, 4096U, false,
                    viennaps::compute::RayMode::NONE, true}};
  const auto surfaceWorkloads = std::span<const StageWorkload>(workloads);
  const auto &levelSetWorkload = workloads.back();
  runManualCpuSmoke(surfaceWorkloads, levelSetWorkload);

  HardwareFingerprint hardware;
  std::string error;
  if (!collectHardware(hardware, error)) {
    std::cout << "[LevelSetSurfaceCompositionExecution] SKIP: " << error
              << '\n';
    return EXIT_SUCCESS;
  }
  const auto decision = makeVulkanDecision(hardware, surfaceWorkloads);

  using NeutralModel = viennaps::NeutralTransport<float, 2>;
  using NeutralSurface = viennaps::impl::NeutralTransportSurfaceModel<float, 2>;
  viennaps::NeutralTransportParameters<float> neutralParameters;
  neutralParameters.kEtch = 2.0F;
  neutralParameters.surfaceSiteDensity = 3.0F;
  neutralParameters.siliconDensity = 6.0F;
  neutralParameters.etchFrontMaterial = viennaps::Material::Si;
  auto neutralModel = viennacore::SmartPointer<NeutralModel>::New(neutralParameters);
  auto neutralSurface =
      std::dynamic_pointer_cast<NeutralSurface>(neutralModel->getSurfaceModel());
  VC_TEST_ASSERT(neutralSurface != nullptr);
  neutralSurface->initializeCoverages(3U);
  auto neutralCoverage =
      neutralSurface->getCoverages()->getScalarData(neutralParameters.coverageLabel);
  *neutralCoverage = {0.25F, 0.5F, 0.75F};
  auto neutralFluxes = viennacore::PointData<float>::New();
  neutralFluxes->insertNextScalarData(std::vector<float>(3U, 1.0F),
                                      neutralParameters.fluxLabel);
  const std::vector<float> neutralMaterials{
      static_cast<float>(neutralParameters.etchFrontMaterial.legacyId()), 1.0F,
      static_cast<float>(neutralParameters.etchFrontMaterial.legacyId())};
  const std::vector<viennacore::Vec3D<float>> neutralCoordinates(3U);
  const auto neutralCpu = neutralSurface->calculateVelocities(
      neutralFluxes, neutralCoordinates, neutralMaterials);
  VC_TEST_ASSERT(neutralCpu != nullptr);

  viennaps::Process<float, 2> process;
  process.setProcessModel(neutralModel);
  const auto retainedModel =
      viennacore::SmartPointer<viennaps::ProcessModelBase<float, 2>>(neutralModel);
  Composition composition;
  Composition::SurfacePaths surfacePaths{
      VIENNAPS_VULKAN_COVERAGE_DELTA_METRIC_SPV_PATH,
      VIENNAPS_VULKAN_GRAPH_DIFFUSION_SPV_PATH,
      VIENNAPS_VULKAN_NEUTRAL_TRANSPORT_SPV_PATH};
  Composition::RebuildPaths rebuildPaths{
      VIENNAPS_HRLE_CLASSIFICATION_SPV_PATH, VIENNAPS_REDUCTION_SCAN_SPV_PATH,
      VIENNAPS_HRLE_ACTION_FLAGS_SPV_PATH, VIENNAPS_HRLE_COMPACT_SPV_PATH};
  const auto configured = composition.configure(
      process, decision, hardware, surfaceWorkloads, levelSetWorkload,
      ManualSelectionConfig{}, {}, surfacePaths, retainedModel,
      VIENNAPS_LEVELSET_UPDATE_SPV_PATH, rebuildPaths);
  VC_TEST_ASSERT(configured.ok);
  VC_TEST_ASSERT(configured.prepared);
  VC_TEST_ASSERT(!configured.degraded);
  VC_TEST_ASSERT(configured.usingVulkan);
  VC_TEST_ASSERT(configured.surface.coverageVulkan);
  VC_TEST_ASSERT(configured.surface.surfaceDiffusionVulkan);
  VC_TEST_ASSERT(configured.surface.neutralTransportVelocityVulkan);
  VC_TEST_ASSERT(configured.levelSet.usingVulkan);

  viennaps::Process<float, 2> mixedManualProcess;
  mixedManualProcess.setProcessModel(neutralModel);
  ManualSelectionConfig mixedManual;
  mixedManual.selectionMode = SelectionMode::MANUAL;
  mixedManual.globalBackend = ComputeBackend::VULKAN;
  mixedManual.perStageBackend.at(static_cast<std::size_t>(Stage::LEVEL_SET)) =
      ComputeBackend::CPU;
  Composition mixedManualComposition;
  const auto mixedManualResult = mixedManualComposition.configure(
      mixedManualProcess, decision, hardware, surfaceWorkloads, levelSetWorkload,
      mixedManual, {}, surfacePaths, retainedModel, VIENNAPS_LEVELSET_UPDATE_SPV_PATH,
      rebuildPaths);
  VC_TEST_ASSERT(mixedManualResult.ok && mixedManualResult.prepared);
  VC_TEST_ASSERT(mixedManualResult.usingVulkan);
  VC_TEST_ASSERT(mixedManualResult.surface.coverageVulkan);
  VC_TEST_ASSERT(mixedManualResult.surface.surfaceDiffusionVulkan);
  VC_TEST_ASSERT(mixedManualResult.surface.neutralTransportVelocityVulkan);
  VC_TEST_ASSERT(!mixedManualResult.levelSet.usingVulkan);
  VC_TEST_ASSERT(mixedManualComposition.sharedContext() != nullptr);
  VC_TEST_ASSERT(!static_cast<bool>(mixedManualProcess.getLevelSetUpdateExecutor()));
  VC_TEST_ASSERT(!static_cast<bool>(mixedManualProcess.getLevelSetRebuildExecutor()));
  mixedManualComposition.clear(mixedManualProcess);
  VC_TEST_ASSERT(!mixedManualComposition.sharedContext());
  assertNoVulkanCallbacks(mixedManualProcess);

  const auto context = composition.sharedContext();
  VC_TEST_ASSERT(context != nullptr);
  const auto *session = context->session();
  VC_TEST_ASSERT(session != nullptr);
  const auto generation = session->generation();
  const auto deviceName = session->selection().properties.deviceName;
  VC_TEST_ASSERT(generation != 0U);
  VC_TEST_ASSERT(session->selection().properties.deviceName[0] != '\0');
  VC_TEST_ASSERT(configured.surface.sessionGeneration == generation);
  VC_TEST_ASSERT(configured.levelSet.updateSessionGeneration == generation);
  VC_TEST_ASSERT(configured.levelSet.rebuildSessionGeneration == generation);
  VC_TEST_ASSERT(configured.levelSet.updateSessionDeviceName == deviceName);
  VC_TEST_ASSERT(configured.levelSet.rebuildSessionDeviceName == deviceName);

  auto coverage = process.getCoverageDeltaExecutor();
  auto diffusion = process.getSurfaceDiffusionExecutor();
  auto neutral = composition.neutralVelocityExecutor();
  auto update = process.getLevelSetUpdateExecutor();
  auto rebuild = process.getLevelSetRebuildExecutor();
  VC_TEST_ASSERT(coverage && diffusion && neutral && update && rebuild);

  std::vector<float> updated{1.0F, 2.0F};
  std::vector<float> previous{0.0F, 1.0F};
  std::vector<std::size_t> offsets{0U, 2U};
  std::vector<float> coverageOutput(1U, -1.0F);
  viennaps::CoverageDeltaWork<float> coverageWork{
      updated, previous, offsets, coverageOutput, 1U, 0U, false};
  VC_TEST_ASSERT(coverage(coverageWork, error));
  VC_TEST_ASSERT(coverageWork.complete && coverageWork.writtenCount == 1U);
  VC_TEST_ASSERT(std::bit_cast<std::uint32_t>(coverageOutput[0]) ==
                 std::bit_cast<std::uint32_t>(1.0F));
  VC_TEST_ASSERT(session->generation() == generation);

  std::vector<std::uint32_t> rowOffsets{0U, 1U, 2U};
  std::vector<std::uint32_t> columns{0U, 1U};
  std::vector<float> weights{1.0F, 1.0F};
  std::vector<float> field{1.0F, 2.0F};
  std::vector<float> diffusionOutput(2U, -1.0F);
  viennaps::SurfaceDiffusionWork<float> diffusionWork{
      rowOffsets, columns, weights, field, diffusionOutput, 0.5F, 0U, false};
  VC_TEST_ASSERT(diffusion(diffusionWork, error) == viennaps::ProcessResult::SUCCESS);
  VC_TEST_ASSERT(diffusionWork.complete && diffusionWork.writtenCount == 2U);
  VC_TEST_ASSERT(std::bit_cast<std::uint32_t>(diffusionOutput[0]) ==
                 std::bit_cast<std::uint32_t>(1.5F));
  VC_TEST_ASSERT(std::bit_cast<std::uint32_t>(diffusionOutput[1]) ==
                 std::bit_cast<std::uint32_t>(3.0F));
  VC_TEST_ASSERT(session->generation() == generation);

  const auto neutralGpu = neutralSurface->calculateVelocities(
      neutralFluxes, neutralCoordinates, neutralMaterials);
  VC_TEST_ASSERT(neutralGpu != nullptr && neutralGpu->size() == neutralCpu->size());
  for (std::size_t index = 0U; index < neutralCpu->size(); ++index)
    VC_TEST_ASSERT(std::bit_cast<std::uint32_t>(neutralGpu->at(index)) ==
                   std::bit_cast<std::uint32_t>(neutralCpu->at(index)));
  VC_TEST_ASSERT(session->generation() == generation);

  const auto cpuDomain = makeProcessDomain();
  const auto cpu = runOneStep(cpuDomain->getSurface());
  const auto vulkanDomain = makeProcessDomain();
  const auto vulkan = runOneStep(vulkanDomain->getSurface(), update, rebuild);
  VC_TEST_ASSERT(vulkan.advectedTime == cpu.advectedTime);
  assertNear(vulkan.nodes, cpu.nodes);
  VC_TEST_ASSERT(session->generation() == generation);

  composition.clear(process);
  VC_TEST_ASSERT(!composition.sharedContext());
  assertNoVulkanCallbacks(process);

  // Copied callbacks retain their original one-session holders even after the
  // composition owner has explicitly released its Process wiring.
  VC_TEST_ASSERT(coverage(coverageWork, error));
  VC_TEST_ASSERT(diffusion(diffusionWork, error) == viennaps::ProcessResult::SUCCESS);
  std::vector<float> retainedNeutralVelocity(neutralCpu->size(), 0.0F);
  viennaps::NeutralTransportVelocityWork<float> retainedNeutralWork{
      *neutralCoverage, neutralMaterials, retainedNeutralVelocity,
      {neutralParameters.kEtch, neutralParameters.surfaceSiteDensity,
       neutralParameters.siliconDensity,
       static_cast<float>(viennaps::units::Time::convertSecond()),
       static_cast<float>(viennaps::units::Length::convertMeter()),
       static_cast<int>(neutralParameters.etchFrontMaterial.legacyId())}};
  VC_TEST_ASSERT(neutral(retainedNeutralWork, error));
  VC_TEST_ASSERT(retainedNeutralWork.complete &&
                 retainedNeutralWork.writtenCount == retainedNeutralVelocity.size());
  for (std::size_t index = 0U; index < neutralCpu->size(); ++index)
    VC_TEST_ASSERT(std::bit_cast<std::uint32_t>(retainedNeutralVelocity[index]) ==
                   std::bit_cast<std::uint32_t>(neutralCpu->at(index)));
  const auto retainedDomain = makeProcessDomain();
  const auto retained = runOneStep(retainedDomain->getSurface(), update, rebuild);
  VC_TEST_ASSERT(retained.advectedTime == cpu.advectedTime);
  assertNear(retained.nodes, cpu.nodes);

  viennaps::Process<float, 2> automaticProcess;
  automaticProcess.setProcessModel(neutralModel);
  Composition automaticFailure;
  auto missingSurfacePaths = surfacePaths;
  missingSurfacePaths.surfaceDiffusion.clear();
  const auto automaticResult = automaticFailure.configure(
      automaticProcess, decision, hardware, surfaceWorkloads, levelSetWorkload,
      ManualSelectionConfig{}, {}, missingSurfacePaths, retainedModel,
      VIENNAPS_LEVELSET_UPDATE_SPV_PATH, rebuildPaths);
  VC_TEST_ASSERT(automaticResult.ok && automaticResult.degraded);
  VC_TEST_ASSERT(!automaticFailure.sharedContext());
  assertNoVulkanCallbacks(automaticProcess);

  viennaps::Process<float, 2> manualProcess;
  manualProcess.setProcessModel(neutralModel);
  ManualSelectionConfig manualVulkan;
  manualVulkan.selectionMode = SelectionMode::MANUAL;
  manualVulkan.globalBackend = ComputeBackend::VULKAN;
  Composition manualFailure;
  const auto manualResult = manualFailure.configure(
      manualProcess, decision, hardware, surfaceWorkloads, levelSetWorkload,
      manualVulkan, {}, missingSurfacePaths, retainedModel,
      VIENNAPS_LEVELSET_UPDATE_SPV_PATH, rebuildPaths);
  VC_TEST_ASSERT(!manualResult.ok);
  VC_TEST_ASSERT(!manualFailure.sharedContext());
  assertNoVulkanCallbacks(manualProcess);

  std::cout << "[LevelSetSurfaceCompositionExecution] five callbacks share "
               "one generation/device with CPU oracle PASS\n";
  return EXIT_SUCCESS;
} catch (const std::exception &exception) {
  std::cerr << exception.what() << '\n';
  return EXIT_FAILURE;
}
