// Copyright 2026 ViennaPS
// SPDX-License-Identifier: MIT

#include "process_deployment_binding.hpp"

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
  process.clearCoverageDeltaExecutor();
  process.clearSurfaceDiffusionExecutor();
  probeSession.reset();
  return 0;
}
