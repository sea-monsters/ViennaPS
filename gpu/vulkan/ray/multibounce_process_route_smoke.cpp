// Copyright 2026 ViennaPS
// SPDX-License-Identifier: MIT
//
// P5-RAY-MULTIBOUNCE-SLICE-05 acceptance smoke.
//
// This is deliberately a narrow Process route: float/2D,
// SingleParticleProcess, one particle/label, default source, no coverages or
// desorption, and maxReflections == 1.  ViennaPS Process/FluxProcessStrategy,
// the particle callbacks, RNG, reflection and roulette decisions remain host
// authoritative.  Vulkan owns only the triangle-hit and bounded frontier
// application/reduction stages.  The smoke compares the complete Process
// result with the CPU_TRIANGLE route and verifies Manual fail-closed output.

#include "vulkan_ray_flux_engine.hpp"

#include "../runtime/deployment_compute_context.hpp"

#include <compute/deploymentProfile.hpp>

#include <geometries/psMakePlane.hpp>
#include <models/psSingleParticleProcess.hpp>
#include <process/psAdvectionCallback.hpp>
#include <process/psProcess.hpp>

#include <array>
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#ifndef VIENNAPS_VULKAN_TRIANGLE_HIT_DEVICE_SPV_PATH
#define VIENNAPS_VULKAN_TRIANGLE_HIT_DEVICE_SPV_PATH ""
#endif
#ifndef VIENNAPS_VULKAN_RAY_RECORD_COMPACTION_SPV_PATH
#define VIENNAPS_VULKAN_RAY_RECORD_COMPACTION_SPV_PATH ""
#endif
#ifndef VIENNAPS_VULKAN_REDUCTION_SCAN_SPV_PATH
#define VIENNAPS_VULKAN_REDUCTION_SCAN_SPV_PATH ""
#endif
#ifndef VIENNAPS_VULKAN_RAY_RECORD_RADIX_HISTOGRAM_SPV_PATH
#define VIENNAPS_VULKAN_RAY_RECORD_RADIX_HISTOGRAM_SPV_PATH ""
#endif
#ifndef VIENNAPS_VULKAN_RAY_RECORD_RADIX_PREFIX_SPV_PATH
#define VIENNAPS_VULKAN_RAY_RECORD_RADIX_PREFIX_SPV_PATH ""
#endif
#ifndef VIENNAPS_VULKAN_RAY_RECORD_RADIX_SCATTER_SPV_PATH
#define VIENNAPS_VULKAN_RAY_RECORD_RADIX_SCATTER_SPV_PATH ""
#endif
#ifndef VIENNAPS_VULKAN_RAY_SURFACE_SEGMENTS_SPV_PATH
#define VIENNAPS_VULKAN_RAY_SURFACE_SEGMENTS_SPV_PATH ""
#endif
#ifndef VIENNAPS_VULKAN_RAY_SURFACE_REDUCE_SPV_PATH
#define VIENNAPS_VULKAN_RAY_SURFACE_REDUCE_SPV_PATH ""
#endif
#ifndef VIENNAPS_VULKAN_TRIANGLE_BVH_HIT_SPV_PATH
#define VIENNAPS_VULKAN_TRIANGLE_BVH_HIT_SPV_PATH ""
#endif
#ifndef VIENNAPS_VULKAN_MULTIBOUNCE_FRONTIER_QUEUE_SPV_PATH
#define VIENNAPS_VULKAN_MULTIBOUNCE_FRONTIER_QUEUE_SPV_PATH ""
#endif

namespace {

using T = float;
constexpr int D = 2;
constexpr const char *kFluxLabel = "particleFlux";

class ResetSessionAfterFirstAdvection final
    : public viennaps::AdvectionCallback<T, D> {
public:
  explicit ResetSessionAfterFirstAdvection(
      std::shared_ptr<viennaps::vulkan::runtime::DeploymentComputeContext>
          context)
      : context_(std::move(context)) {}

  bool applyPostAdvect(const T) override {
    if (!fired_) {
      fired_ = true;
      context_->reset();
    }
    return true;
  }

  [[nodiscard]] bool fired() const { return fired_; }

private:
  std::shared_ptr<viennaps::vulkan::runtime::DeploymentComputeContext>
      context_;
  bool fired_ = false;
};

bool require(const bool condition, const char *message) {
  if (!condition)
    std::cerr << "multibounce Process route FAIL: " << message << '\n';
  return condition;
}

std::string uuid(const std::uint8_t *bytes) {
  std::ostringstream stream;
  stream << std::hex << std::setfill('0');
  for (std::size_t i = 0; i < VK_UUID_SIZE; ++i)
    stream << std::setw(2) << static_cast<unsigned>(bytes[i]);
  return stream.str();
}

bool getHardware(viennaps::compute::HardwareFingerprint &fingerprint,
                 std::string &error) {
  viennaps::vulkan::runtime::ComputeSession session;
  if (!session.initialize(error))
    return false;
  VkPhysicalDeviceIDProperties ids{
      VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES};
  VkPhysicalDeviceProperties2 properties{
      VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
  properties.pNext = &ids;
  vkGetPhysicalDeviceProperties2(session.selection().handle, &properties);
  fingerprint.deviceUuid = uuid(ids.deviceUUID);
  fingerprint.driverUuid = uuid(ids.driverUUID);
  fingerprint.vendorId = properties.properties.vendorID;
  fingerprint.deviceId = properties.properties.deviceID;
  fingerprint.deviceName = properties.properties.deviceName;
  fingerprint.driverVersion =
      std::to_string(properties.properties.driverVersion);
  fingerprint.driverDate = "unknown";
  return true;
}

viennaps::compute::DeploymentProfileDecision makeDecision(
    const viennaps::compute::HardwareFingerprint &hardware) {
  using namespace viennaps::compute;
  DeploymentProfileDecision decision;
  decision.state = DeploymentProfileState::VALID;
  decision.hasProfile = true;
  decision.activeRecord.hardware = hardware;
  auto &profile = decision.activeRecord.capabilityProfile;
  profile.cpuAvailable = true;
  profile.vulkanAvailable = true;
  profile.vulkanPrimitiveSuitePass = true;
  profile.vulkanCompute = true;
  profile.safeVulkanWorkingSetBytes = 128ULL * 1024ULL * 1024ULL;
  profile.vulkanFp32NumericalSmoke.status = VulkanNumericalSmokeStatus::PASS;
  profile.vulkanFp32NumericalSmoke.contractId =
      std::string(kVulkanFp32NumericalSmokeContract);
  profile.vulkanFp32NumericalSmoke.caseCount = 1U;
  profile.vulkanFp32NumericalSmoke.watchdogMs =
      kVulkanFp32NumericalSmokeWatchdogMs;
  decision.plan.ok = true;
  StageSelection stage{Stage::RAY_TRACING};
  stage.selected = true;
  stage.selectedBackend = ComputeBackend::VULKAN;
  decision.plan.stages.push_back(stage);
  return decision;
}

viennaps::VulkanRayFluxSpirvPaths makePaths() {
  viennaps::VulkanRayFluxSpirvPaths paths;
  paths.triangleHit = VIENNAPS_VULKAN_TRIANGLE_HIT_DEVICE_SPV_PATH;
  paths.recordCompaction = VIENNAPS_VULKAN_RAY_RECORD_COMPACTION_SPV_PATH;
  paths.reductionScan = VIENNAPS_VULKAN_REDUCTION_SCAN_SPV_PATH;
  paths.radixHistogram = VIENNAPS_VULKAN_RAY_RECORD_RADIX_HISTOGRAM_SPV_PATH;
  paths.radixPrefix = VIENNAPS_VULKAN_RAY_RECORD_RADIX_PREFIX_SPV_PATH;
  paths.radixScatter = VIENNAPS_VULKAN_RAY_RECORD_RADIX_SCATTER_SPV_PATH;
  paths.surfaceSegments = VIENNAPS_VULKAN_RAY_SURFACE_SEGMENTS_SPV_PATH;
  paths.surfaceReduce = VIENNAPS_VULKAN_RAY_SURFACE_REDUCE_SPV_PATH;
  paths.triangleBvh = VIENNAPS_VULKAN_TRIANGLE_BVH_HIT_SPV_PATH;
  paths.multibounceFrontierQueue =
      VIENNAPS_VULKAN_MULTIBOUNCE_FRONTIER_QUEUE_SPV_PATH;
  return paths;
}

viennaps::RayTracingParameters routeParameters() {
  viennaps::RayTracingParameters parameters;
  parameters.rngSeed = 42U;
  parameters.useRandomSeeds = false;
  parameters.raysPerPoint = 64U;
  parameters.maxReflections = 1U;
  parameters.normalizationType = viennaray::NormalizationType::SOURCE;
  return parameters;
}

std::vector<T> flux(const viennacore::SmartPointer<viennals::Mesh<T>> &mesh) {
  if (!mesh)
    return {};
  const auto values = mesh->getCellData().getScalarData(kFluxLabel, true);
  return values ? *values : std::vector<T>{};
}

bool sameDomain(const viennaps::SmartPointer<viennaps::Domain<T, D>> &domain,
                const std::vector<std::vector<T>> &before) {
  const auto &surface = domain->getSurface()->getDomain();
  if (surface.getNumberOfSegments() != before.size())
    return false;
  for (unsigned i = 0; i < surface.getNumberOfSegments(); ++i) {
    if (surface.getDomainSegment(i).definedValues != before[i])
      return false;
  }
  return true;
}

std::vector<std::vector<T>> snapshotDomain(
    const viennaps::SmartPointer<viennaps::Domain<T, D>> &domain) {
  const auto &surface = domain->getSurface()->getDomain();
  std::vector<std::vector<T>> values(surface.getNumberOfSegments());
  for (unsigned i = 0; i < surface.getNumberOfSegments(); ++i)
    values[i] = surface.getDomainSegment(i).definedValues;
  return values;
}

} // namespace

int main() {
  viennacore::Logger::setLogLevel(viennacore::LogLevel::WARNING);
  const auto paths = makePaths();
  if (!require(!paths.triangleHit.empty() && !paths.triangleBvh.empty() &&
                   !paths.multibounceFrontierQueue.empty(),
               "all bounded-route SPIR-V paths configured"))
    return 1;

  viennaps::compute::HardwareFingerprint hardware;
  std::string error;
  if (!getHardware(hardware, error)) {
    std::cerr << "multibounce Process route SKIP: " << error << '\n';
    return 2;
  }
  const std::array<viennaps::compute::StageWorkload, 1> workloads = {
      viennaps::compute::StageWorkload{
          viennaps::compute::Stage::RAY_TRACING,
          viennaps::compute::Precision::FP32, 1U, false,
          viennaps::compute::RayMode::NONE, true}};
  viennaps::compute::ManualSelectionConfig selection;
  selection.selectionMode = viennaps::compute::SelectionMode::MANUAL;
  selection.globalBackend = viennaps::compute::ComputeBackend::VULKAN;
  auto context = std::make_shared<
      viennaps::vulkan::runtime::DeploymentComputeContext>();
  if (!context->prepare(makeDecision(hardware), hardware, workloads, selection,
                        error)) {
    std::cerr << "multibounce Process route FAIL: context prepare: " << error
              << '\n';
    return 1;
  }

  const auto parameters = routeParameters();
  auto cpuDomain = viennaps::Domain<T, D>::New(0.5F, 10.0F, 10.0F);
  viennaps::MakePlane<T, D>(cpuDomain, 0.0F).apply();
  auto cpuModel =
      viennaps::SmartPointer<viennaps::SingleParticleProcess<T, D>>::New(
          1.0F, 1.0F, 1.0F);
  viennaps::Process<T, D> cpuProcess(cpuDomain, cpuModel);
  cpuProcess.setFluxEngineType(viennaps::FluxEngineType::CPU_TRIANGLE);
  cpuProcess.setProcessDuration(0.0);
  cpuProcess.setParameters(parameters);
  const auto cpuFlux = flux(cpuProcess.calculateFlux());
  if (!require(!cpuFlux.empty(), "CPU_TRIANGLE oracle produced a mesh"))
    return 1;

  auto vulkanDomain = viennaps::Domain<T, D>::New(0.5F, 10.0F, 10.0F);
  viennaps::MakePlane<T, D>(vulkanDomain, 0.0F).apply();
  auto vulkanModel =
      viennaps::SmartPointer<viennaps::SingleParticleProcess<T, D>>::New(
          1.0F, 1.0F, 1.0F);
  viennaps::Process<T, D> vulkanProcess(vulkanDomain, vulkanModel);
  vulkanProcess.setFluxEngineOverride(
      std::make_unique<viennaps::VulkanRayFluxEngine<T, D>>(
          context, paths, false));
  vulkanProcess.setProcessDuration(0.0);
  vulkanProcess.setParameters(parameters);
  const auto vulkanFlux = flux(vulkanProcess.calculateFlux());
  if (!require(vulkanFlux.size() == cpuFlux.size(),
               "bounded Vulkan route preserved flux shape"))
    return 1;

  double cpuTotal = 0.0;
  double vulkanTotal = 0.0;
  double maxRelative = 0.0;
  for (std::size_t i = 0; i < cpuFlux.size(); ++i) {
    cpuTotal += cpuFlux[i];
    vulkanTotal += vulkanFlux[i];
    const double denominator =
        std::max(std::abs(static_cast<double>(cpuFlux[i])), 1e-6);
    maxRelative = std::max(
        maxRelative,
        std::abs(static_cast<double>(cpuFlux[i]) - vulkanFlux[i]) /
            denominator);
  }
  const double totalRelative =
      std::abs(cpuTotal - vulkanTotal) /
      std::max(std::abs(cpuTotal), 1e-12);
  if (!require(totalRelative < 0.02 && maxRelative < 0.25,
               "bounded Vulkan Process matches CPU_TRIANGLE oracle"))
    return 1;

  // Manual mode with no prepared context must not publish flux or geometry.
  auto failDomain = viennaps::Domain<T, D>::New(0.5F, 10.0F, 10.0F);
  viennaps::MakePlane<T, D>(failDomain, 0.0F).apply();
  const auto before = snapshotDomain(failDomain);
  auto failModel =
      viennaps::SmartPointer<viennaps::SingleParticleProcess<T, D>>::New(
          1.0F, 1.0F, 1.0F);
  auto unprepared = std::make_shared<
      viennaps::vulkan::runtime::DeploymentComputeContext>();
  viennaps::Process<T, D> failProcess(failDomain, failModel);
  failProcess.setFluxEngineOverride(
      std::make_unique<viennaps::VulkanRayFluxEngine<T, D>>(
          unprepared, paths, false));
  failProcess.setProcessDuration(0.0);
  failProcess.setParameters(parameters);
  const auto failedFlux = flux(failProcess.calculateFlux());
  if (!require(failedFlux.empty() && sameDomain(failDomain, before),
               "Manual unprepared route preserves output sentinel"))
    return 1;

  // AUTO runtime-loss contract: the first bounded Vulkan dispatch succeeds,
  // then the callback invalidates the deployment session after that step.
  // The next identical flux request must retry through the already-initialized
  // CPU triangle engine and complete without publishing a partial failure.
  viennaps::compute::HardwareFingerprint autoHardware;
  if (!getHardware(autoHardware, error)) {
    std::cerr << "multibounce Process route SKIP: " << error << '\n';
    return 2;
  }
  auto autoContext = std::make_shared<
      viennaps::vulkan::runtime::DeploymentComputeContext>();
  if (!autoContext->prepare(makeDecision(autoHardware), autoHardware, workloads,
                            selection, error)) {
    std::cerr << "multibounce Process route FAIL: auto context prepare: "
              << error << '\n';
    return 1;
  }
  auto autoDomain = viennaps::Domain<T, D>::New(0.5F, 10.0F, 10.0F);
  viennaps::MakePlane<T, D>(autoDomain, 0.0F).apply();
  auto autoModel =
      viennaps::SmartPointer<viennaps::SingleParticleProcess<T, D>>::New(
          1.0F, 1.0F, 1.0F);
  auto resetCallback =
      viennaps::SmartPointer<ResetSessionAfterFirstAdvection>::New(
          autoContext);
  autoModel->setAdvectionCallback(resetCallback);
  viennaps::Process<T, D> autoProcess(autoDomain, autoModel);
  autoProcess.setFluxEngineOverride(
      std::make_unique<viennaps::VulkanRayFluxEngine<T, D>>(
          autoContext, paths, true));
  autoProcess.setProcessDuration(1.0);
  autoProcess.setParameters(parameters);
  autoProcess.apply();
  if (!require(resetCallback->fired() &&
                   autoProcess.getLastProcessResult() ==
                       viennaps::ProcessResult::SUCCESS,
               "AUTO runtime session loss retries through CPU"))
    return 1;

  std::cout << "multibounce Process route Vulkan dispatch PASS"
            << " (totalRelDiff=" << totalRelative
            << ", maxRelDiff=" << maxRelative << ")\n";
  return 0;
}
