// Copyright 2026 ViennaPS
// SPDX-License-Identifier: MIT

#include "process_deployment_binding.hpp"

#include <models/psNeutralTransport.hpp>

#include <array>
#include <bit>
#include <cstdint>
#include <functional>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

int main() {
  viennaps::Process<float, 2> process;
  auto retainedNeutralModel =
      viennacore::SmartPointer<viennaps::ProcessModelBase<float, 2>>::New();
  process.setCoverageDeltaExecutor(
      [](viennaps::CoverageDeltaWork<float> &, std::string &) { return true; });
  process.setSurfaceDiffusionStatusExecutor(
      [](viennaps::SurfaceDiffusionWork<float> &,
         std::string &) { return viennaps::SurfaceDiffusionExecutionStatus::SUCCESS; });

  using namespace viennaps::compute;
  const std::array<StageWorkload, 2> workloads = {
      StageWorkload{Stage::COVERAGE, Precision::FP32, 1U, false, RayMode::NONE,
                    true},
      StageWorkload{Stage::SURFACE_DIFFUSION, Precision::FP32, 1U, false,
                    RayMode::NONE, true}};
  DeploymentProfileDecision decision{};
  decision.plan.ok = true;
  decision.plan.stages = {
      StageSelection{Stage::COVERAGE},
      StageSelection{Stage::SURFACE_DIFFUSION},
  };
  for (auto &stage : decision.plan.stages) {
    stage.selected = true;
    stage.selectedBackend = ComputeBackend::CPU;
  }
  ManualSelectionConfig selection{};
  selection.selectionMode = SelectionMode::MANUAL;
  selection.globalBackend = ComputeBackend::CPU;
  viennaps::vulkan::surface::ProcessDeploymentBinding<2> binding;
  // Regression coverage for the explicit retained-model overload (CPU path).
  const auto missingNeutralOverload = binding.configure(
      process, decision, {}, workloads, selection, {},
      viennaps::vulkan::surface::ProcessDeploymentBinding<2>::SpirvPaths{},
      retainedNeutralModel);
  static_cast<void>(missingNeutralOverload);
  const auto result = binding.configure(process, decision, {}, workloads,
                                        selection, {});
  if (!result.ok || process.getCoverageDeltaExecutor() ||
      process.getSurfaceDiffusionExecutor()) {
    std::cerr << "CPU deployment binding smoke failed\n";
    return 1;
  }
  binding.clear(process);
  binding.clear(process);

  DeploymentProfileDecision missingDecision = decision;
  missingDecision.state = DeploymentProfileState::MISSING;
  missingDecision.hasProfile = false;
  ManualSelectionConfig mixedManual = selection;
  mixedManual.perStageBackend.at(static_cast<std::size_t>(Stage::COVERAGE)) =
      ComputeBackend::VULKAN;
  const auto rejectedMixed = binding.configure(
      process, missingDecision, {}, workloads, mixedManual, {});
  if (rejectedMixed.ok || binding.context() != nullptr ||
      process.getCoverageDeltaExecutor() ||
      process.getSurfaceDiffusionExecutor()) {
    std::cerr << "manual mixed-profile rejection smoke failed\n";
    return 1;
  }

  // A valid resolved Vulkan decision with an empty shader path is a runtime
  // bridge failure, so automatic selection must degrade and release its
  // session instead of retaining an unusable context.
  viennaps::vulkan::runtime::ComputeSession probeSession;
  std::string error;
  if (!probeSession.initialize(error)) {
    std::cerr << error << '\n';
    return 1;
  }
  VkPhysicalDeviceIDProperties ids{};
  ids.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES;
  VkPhysicalDeviceProperties2 properties{};
  properties.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
  properties.pNext = &ids;
  vkGetPhysicalDeviceProperties2(probeSession.selection().handle, &properties);
  auto uuid = [](const std::uint8_t *bytes) {
    std::ostringstream stream;
    stream << std::hex << std::setfill('0');
    for (std::size_t index = 0U; index < VK_UUID_SIZE; ++index)
      stream << std::setw(2) << static_cast<unsigned int>(bytes[index]);
    return stream.str();
  };
  HardwareFingerprint fingerprint{};
  fingerprint.deviceUuid = uuid(ids.deviceUUID);
  fingerprint.driverUuid = uuid(ids.driverUUID);
  fingerprint.vendorId = properties.properties.vendorID;
  fingerprint.deviceId = properties.properties.deviceID;
  fingerprint.deviceName = properties.properties.deviceName;
  fingerprint.driverVersion =
      std::to_string(properties.properties.driverVersion);
  DeploymentProfileDecision vulkanDecision{};
  vulkanDecision.state = DeploymentProfileState::VALID;
  vulkanDecision.hasProfile = true;
  vulkanDecision.activeRecord.hardware = fingerprint;
  vulkanDecision.plan.ok = true;
  vulkanDecision.plan.stages.resize(2U);
  vulkanDecision.plan.stages[0].stage = Stage::COVERAGE;
  vulkanDecision.plan.stages[1].stage = Stage::SURFACE_DIFFUSION;
  for (auto &stage : vulkanDecision.plan.stages) {
    stage.selected = true;
    stage.selectedBackend = ComputeBackend::VULKAN;
  }
  ManualSelectionConfig automatic{};
  const auto degraded = binding.configure(process, vulkanDecision, fingerprint,
                                           workloads, automatic, {});
  if (!degraded.ok || !degraded.degraded || degraded.prepared ||
      binding.context() != nullptr || process.getCoverageDeltaExecutor() ||
      process.getSurfaceDiffusionExecutor()) {
    std::cerr << "automatic bridge-degrade smoke failed\n";
    return 1;
  }

  viennaps::CoverageDeltaExecutor<float> retainedCoverage;
  viennaps::Process<float, 2>::SurfaceDiffusionExecutor retainedDiffusion;
  {
    viennaps::vulkan::surface::ProcessDeploymentBinding<2> successfulBinding;
    const viennaps::vulkan::surface::ProcessDeploymentBinding<2>::SpirvPaths
        paths{VIENNAPS_VULKAN_COVERAGE_DELTA_METRIC_SPV_PATH,
              VIENNAPS_VULKAN_GRAPH_DIFFUSION_SPV_PATH};
    const auto configured = successfulBinding.configure(
        process, vulkanDecision, fingerprint, workloads, automatic, {}, paths);
    const auto *context = successfulBinding.context();
    const auto *session = context == nullptr ? nullptr : context->session();
    if (!configured.ok || configured.degraded || !configured.prepared ||
        !configured.coverageVulkan || !configured.surfaceDiffusionVulkan ||
        context == nullptr || session == nullptr ||
        configured.sessionGeneration != session->generation() ||
        configured.device != session->deviceHandle() ||
        !process.getCoverageDeltaExecutor() ||
        !process.getSurfaceDiffusionExecutor()) {
      std::cerr << "successful Vulkan binding smoke failed\n";
      return 1;
    }
  }

  // Fetch the callbacks only after the binding object has been destroyed.
  retainedCoverage = process.getCoverageDeltaExecutor();
  retainedDiffusion = process.getSurfaceDiffusionExecutor();

  std::vector<float> updated{1.0F, 2.0F};
  std::vector<float> previous{0.0F, 1.0F};
  std::vector<std::size_t> channelOffsets{0U, 2U};
  std::vector<float> coverageOutput(1U, -7.0F);
  viennaps::CoverageDeltaWork<float> coverageWork{
      updated, previous, channelOffsets, coverageOutput, 1U, 0U, false};
  if (!retainedCoverage || !retainedCoverage(coverageWork, error) ||
      !coverageWork.complete || coverageWork.writtenCount != 1U ||
      std::bit_cast<std::uint32_t>(coverageOutput[0]) !=
          std::bit_cast<std::uint32_t>(1.0F)) {
    std::cerr << "retained coverage callback smoke failed: " << error << '\n';
    return 1;
  }
  std::vector<std::uint32_t> rowOffsets{0U, 1U, 2U};
  std::vector<std::uint32_t> columns{0U, 1U};
  std::vector<float> weights{1.0F, 1.0F};
  std::vector<float> field{1.0F, 2.0F};
  std::vector<float> diffusionOutput(2U, -9.0F);
  viennaps::SurfaceDiffusionWork<float> diffusionWork{
      rowOffsets, columns, weights, field, diffusionOutput, 0.5F, 0U, false};
  if (!retainedDiffusion ||
      retainedDiffusion(diffusionWork, error) !=
          viennaps::ProcessResult::SUCCESS ||
      !diffusionWork.complete || diffusionWork.writtenCount != 2U ||
      std::bit_cast<std::uint32_t>(diffusionOutput[0]) !=
          std::bit_cast<std::uint32_t>(1.5F) ||
      std::bit_cast<std::uint32_t>(diffusionOutput[1]) !=
          std::bit_cast<std::uint32_t>(3.0F)) {
    std::cerr << "retained surface callback smoke failed: " << error << '\n';
    return 1;
  }

  // The neutral stage uses a caller-retained CPU model handle.  Coverage,
  // diffusion, and neutral all share the one deployment context/session.
  using NeutralModel = viennaps::NeutralTransport<float, 2>;
  using NeutralSurface =
      viennaps::impl::NeutralTransportSurfaceModel<float, 2>;
  viennaps::NeutralTransportParameters<float> neutralParameters;
  neutralParameters.kEtch = 2.0F;
  neutralParameters.surfaceSiteDensity = 3.0F;
  neutralParameters.siliconDensity = 6.0F;
  neutralParameters.etchFrontMaterial = viennaps::Material::Si;
  auto neutralModel = viennacore::SmartPointer<NeutralModel>::New(
      neutralParameters);
  auto neutralSurface =
      std::dynamic_pointer_cast<NeutralSurface>(neutralModel->getSurfaceModel());
  if (!neutralSurface) {
    std::cerr << "neutral model surface discovery failed\n";
    return 1;
  }
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

  viennaps::Process<float, 2> neutralProcess;
  neutralProcess.setProcessModel(neutralModel);
  const std::array<StageWorkload, 3> allSurfaceWorkloads = {
      StageWorkload{Stage::COVERAGE, Precision::FP32, 1U, false, RayMode::NONE,
                    true},
      StageWorkload{Stage::SURFACE_DIFFUSION, Precision::FP32, 1U, false,
                    RayMode::NONE, true},
      StageWorkload{Stage::NEUTRAL_TRANSPORT_VELOCITY, Precision::FP32, 3U,
                    false, RayMode::NONE, true}};
  auto allVulkanDecision = vulkanDecision;
  allVulkanDecision.plan.stages.push_back(
      StageSelection{Stage::NEUTRAL_TRANSPORT_VELOCITY});
  allVulkanDecision.plan.stages.back().selected = true;
  allVulkanDecision.plan.stages.back().selectedBackend =
      ComputeBackend::VULKAN;
  const auto retainedModel =
      viennacore::SmartPointer<viennaps::ProcessModelBase<float, 2>>(
          neutralModel);
  viennaps::vulkan::surface::ProcessDeploymentBinding<2>::SpirvPaths allPaths{
      VIENNAPS_VULKAN_COVERAGE_DELTA_METRIC_SPV_PATH,
      VIENNAPS_VULKAN_GRAPH_DIFFUSION_SPV_PATH,
      VIENNAPS_VULKAN_NEUTRAL_TRANSPORT_SPV_PATH};
  viennaps::vulkan::surface::ProcessDeploymentBinding<2> invalidNeutralBinding;
  const auto invalidNeutral = invalidNeutralBinding.configure(
      neutralProcess, allVulkanDecision, fingerprint, allSurfaceWorkloads,
      automatic, {}, allPaths, retainedNeutralModel);
  if (!invalidNeutral.ok || !invalidNeutral.degraded ||
      invalidNeutralBinding.context() != nullptr ||
      neutralProcess.getCoverageDeltaExecutor() ||
      neutralProcess.getSurfaceDiffusionExecutor()) {
    std::cerr << "invalid neutral model degradation smoke failed\n";
    return 1;
  }
  auto missingNeutralPath = allPaths;
  missingNeutralPath.neutralTransportVelocity.clear();
  viennaps::vulkan::surface::ProcessDeploymentBinding<2> missingNeutralBinding;
  const auto missingNeutral = missingNeutralBinding.configure(
      neutralProcess, allVulkanDecision, fingerprint, allSurfaceWorkloads,
      automatic, {}, missingNeutralPath, retainedModel);
  if (!missingNeutral.ok || !missingNeutral.degraded ||
      missingNeutralBinding.context() != nullptr ||
      neutralProcess.getCoverageDeltaExecutor() ||
      neutralProcess.getSurfaceDiffusionExecutor()) {
    std::cerr << "empty neutral shader degradation smoke failed\n";
    return 1;
  }
  viennaps::vulkan::surface::ProcessDeploymentBinding<2> neutralBinding;
  const auto neutralConfigured = neutralBinding.configure(
      neutralProcess, allVulkanDecision, fingerprint, allSurfaceWorkloads,
      automatic, {}, allPaths, retainedModel);
  const auto *neutralContext = neutralBinding.context();
  const auto *neutralSession =
      neutralContext == nullptr ? nullptr : neutralContext->session();
  if (!neutralConfigured.ok || neutralConfigured.degraded ||
      !neutralConfigured.coverageVulkan ||
      !neutralConfigured.surfaceDiffusionVulkan ||
      !neutralConfigured.neutralTransportVelocityVulkan ||
      neutralSession == nullptr ||
      neutralConfigured.sessionGeneration != neutralSession->generation() ||
      neutralConfigured.device != neutralSession->deviceHandle()) {
    std::cerr << "combined neutral Process binding smoke failed\n";
    return 1;
  }
  const auto neutralGpu = neutralSurface->calculateVelocities(
      neutralFluxes, neutralCoordinates, neutralMaterials);
  if (neutralGpu == nullptr || neutralGpu->size() != neutralCpu->size()) {
    std::cerr << "neutral callback did not produce output\n";
    return 1;
  }
  for (std::size_t index = 0U; index < neutralCpu->size(); ++index) {
    if (std::bit_cast<std::uint32_t>(neutralGpu->at(index)) !=
        std::bit_cast<std::uint32_t>(neutralCpu->at(index))) {
      std::cerr << "neutral callback differs from CPU oracle\n";
      return 1;
    }
  }
  neutralBinding.clear(neutralProcess);
  const auto neutralAfterClear = neutralSurface->calculateVelocities(
      neutralFluxes, neutralCoordinates, neutralMaterials);
  if (neutralAfterClear == nullptr || neutralAfterClear->size() != 3U) {
    std::cerr << "neutral clear lifecycle smoke failed\n";
    return 1;
  }
  process.clearCoverageDeltaExecutor();
  process.clearSurfaceDiffusionExecutor();
  probeSession.reset();
  return 0;
}
