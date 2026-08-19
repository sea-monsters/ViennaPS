// Copyright 2026 ViennaPS
// SPDX-License-Identifier: MIT
//
// P5-MODEL-MATRIX-ROW-01-SURFACE-CONSERVATION global position: this smoke is
// the narrow strict SingleParticleProcess<float,2>, one-label/default-source,
// zero-reflection acceptance slice after the existing route contract. It owns
// only this test fixture and its CMake/CTest registration; Process/model/public
// headers, Vulkan runtime, eligibility, and reference CPU code remain
// unchanged. The ordered exits are CPU oracle, strict Vulkan route, raw/ULP
// flux and conservation checks, apply geometry, zero-flux, then invalid-status
// fail-closed checks. Acceptance is fixed-seed, same-geometry Release execution
// on a real adapter with explicit normalization and no publication on failure.
// The invariant is that every CPU formula, source seed, label, normalization,
// surface ordering, rollback boundary, and geometry update remains authoritative
// while Vulkan only executes the already-eligible ray operation. Validation is
// the top-level Release target, the real Intel Arc executable, focused CTest,
// and a scoped diff check. Handoff is strict row-01 local evidence only; it
// must not be aggregated into the broader model matrix, surface-physics claim,
// promotion gate, or release status. If a required route/status observation is
// unavailable, stop and report that instrumentation gap rather than weakening
// this oracle or changing production behavior.

#include "vulkan_ray_flux_engine.hpp"
#include "device_ray_flux_pipeline.hpp"

#include "../runtime/deployment_compute_context.hpp"

#include <compute/deploymentProfile.hpp>
#include <geometries/psMakePlane.hpp>
#include <models/psSingleParticleProcess.hpp>
#include <process/psProcess.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <sstream>
#include <span>
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

namespace {

using HardwareFingerprint = viennaps::compute::HardwareFingerprint;
using ComputeBackend = viennaps::compute::ComputeBackend;
using ComputeSession = viennaps::vulkan::runtime::ComputeSession;
using ManualSelectionConfig = viennaps::compute::ManualSelectionConfig;
using SelectionMode = viennaps::compute::SelectionMode;
using Stage = viennaps::compute::Stage;
using StageSelection = viennaps::compute::StageSelection;
using StageWorkload = viennaps::compute::StageWorkload;
using Precision = viennaps::compute::Precision;
using RayMode = viennaps::compute::RayMode;

bool require(const bool condition, const char *what) {
  if (!condition)
    std::cerr << "model-matrix row01 conservation FAIL: " << what << '\n';
  return condition;
}

std::string uuidToHex(const std::uint8_t *bytes) {
  std::ostringstream output;
  output << std::hex << std::setfill('0');
  for (std::size_t index = 0U; index < VK_UUID_SIZE; ++index)
    output << std::setw(2) << static_cast<unsigned int>(bytes[index]);
  return output.str();
}

bool collectHardware(HardwareFingerprint &hardware, std::string &error) {
  ComputeSession probe;
  if (!probe.initialize(error))
    return false;
  VkPhysicalDeviceIDProperties ids{};
  ids.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES;
  VkPhysicalDeviceProperties2 properties{};
  properties.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
  properties.pNext = &ids;
  vkGetPhysicalDeviceProperties2(probe.selection().handle, &properties);
  hardware.deviceUuid = uuidToHex(ids.deviceUUID);
  hardware.driverUuid = uuidToHex(ids.driverUUID);
  hardware.vendorId = properties.properties.vendorID;
  hardware.deviceId = properties.properties.deviceID;
  hardware.deviceName = properties.properties.deviceName;
  hardware.driverVersion = std::to_string(properties.properties.driverVersion);
  hardware.driverDate = "unknown";
  return true;
}

viennaps::compute::DeploymentProfileDecision makeDecision(
    const HardwareFingerprint &hardware,
    const std::span<const StageWorkload> workloads) {
  viennaps::compute::DeploymentProfileDecision decision;
  decision.state = viennaps::compute::DeploymentProfileState::VALID;
  decision.hasProfile = true;
  decision.activeRecord.hardware = hardware;
  auto &profile = decision.activeRecord.capabilityProfile;
  profile.cpuAvailable = true;
  profile.vulkanAvailable = true;
  profile.vulkanPrimitiveSuitePass = true;
  profile.vulkanCompute = true;
  profile.safeVulkanWorkingSetBytes = 128ULL * 1024ULL * 1024ULL;
  profile.vulkanFp32NumericalSmoke.status =
      viennaps::compute::VulkanNumericalSmokeStatus::PASS;
  profile.vulkanFp32NumericalSmoke.contractId =
      std::string(viennaps::compute::kVulkanFp32NumericalSmokeContract);
  profile.vulkanFp32NumericalSmoke.caseCount = 18U;
  profile.vulkanFp32NumericalSmoke.watchdogMs =
      viennaps::compute::kVulkanFp32NumericalSmokeWatchdogMs;
  decision.plan.ok = true;
  for (const auto &workload : workloads) {
    StageSelection stage{workload.stage};
    stage.selected = true;
    stage.selectedBackend = ComputeBackend::VULKAN;
    decision.plan.stages.push_back(stage);
  }
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
  return paths;
}

viennaps::RayTracingParameters strictParams() {
  viennaps::RayTracingParameters params;
  params.rngSeed = 42U;
  params.useRandomSeeds = false;
  params.maxReflections = 0U;
  params.normalizationType = viennaray::NormalizationType::SOURCE;
  return params;
}

auto makeDomain() {
  auto domain = viennaps::Domain<float, 2>::New(0.5F, 10.0F, 10.0F);
  viennaps::MakePlane<float, 2>(domain, 0.0F).apply();
  return domain;
}

std::vector<float> readFlux(
    const viennaps::SmartPointer<viennals::Mesh<float>> &mesh) {
  if (!mesh)
    return {};
  const auto values = mesh->getCellData().getScalarData("particleFlux", true);
  return values ? *values : std::vector<float>{};
}

std::vector<float> snapshotGeometry(
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

std::uint32_t ulpDistance(const float lhs, const float rhs) {
  const auto ordered = [](const std::uint32_t bits) {
    return (bits & 0x80000000U) != 0U ? ~bits : bits ^ 0x80000000U;
  };
  const auto left = ordered(std::bit_cast<std::uint32_t>(lhs));
  const auto right = ordered(std::bit_cast<std::uint32_t>(rhs));
  return left > right ? left - right : right - left;
}

} // namespace

int main() try {
  viennacore::Logger::setLogLevel(viennacore::LogLevel::WARNING);
  const auto paths = makePaths();
  if (!require(!paths.triangleHit.empty() && !paths.surfaceReduce.empty() &&
                   !paths.triangleBvh.empty(),
               "strict SPIR-V paths configured"))
    return EXIT_FAILURE;

  HardwareFingerprint hardware;
  std::string error;
  if (!collectHardware(hardware, error)) {
    std::cout << "model-matrix row01 conservation SKIP: " << error << '\n';
    return EXIT_SUCCESS;
  }
  const std::array<StageWorkload, 1> workloads = {
      StageWorkload{Stage::RAY_TRACING, Precision::FP32, 1U, false,
                    RayMode::NONE, true}};
  ManualSelectionConfig selection;
  selection.selectionMode = SelectionMode::MANUAL;
  selection.globalBackend = ComputeBackend::VULKAN;
  auto context = std::make_shared<
      viennaps::vulkan::runtime::DeploymentComputeContext>();
  if (!require(context->prepare(makeDecision(hardware, workloads), hardware,
                                workloads, selection, error),
               "strict Vulkan deployment context prepared"))
    return EXIT_FAILURE;

  // CPU is the reference definition; Vulkan receives the same model, seed,
  // normalization, and plane geometry through the real Process route.
  const auto params = strictParams();
  auto cpuModel =
      viennaps::SmartPointer<viennaps::SingleParticleProcess<float, 2>>::New(
          1.0F, 1.0F, 1.0F);
  auto cpuDomain = makeDomain();
  viennaps::Process<float, 2> cpuProcess(cpuDomain, cpuModel);
  cpuProcess.setFluxEngineType(viennaps::FluxEngineType::CPU_TRIANGLE);
  cpuProcess.setParameters(params);
  const auto cpuFlux = readFlux(cpuProcess.calculateFlux());
  if (!require(!cpuFlux.empty(), "CPU Process flux oracle produced data"))
    return EXIT_FAILURE;

  auto vkModel =
      viennaps::SmartPointer<viennaps::SingleParticleProcess<float, 2>>::New(
          1.0F, 1.0F, 1.0F);
  auto vkDomain = makeDomain();
  viennaps::Process<float, 2> vkProcess(vkDomain, vkModel);
  vkProcess.setFluxEngineOverride(
      std::make_unique<viennaps::VulkanRayFluxEngine<float, 2>>(
          context, paths, false));
  vkProcess.setParameters(params);
  const auto vkFlux = readFlux(vkProcess.calculateFlux());
  if (!require(!vkFlux.empty(), "Vulkan Process flux route produced data"))
    return EXIT_FAILURE;
  if (!require(cpuFlux.size() == vkFlux.size(),
               "CPU/Vulkan surface flux sizes match"))
    return EXIT_FAILURE;

  std::uint32_t maxFluxUlp = 0U;
  double cpuTotal = 0.0;
  double vkTotal = 0.0;
  for (std::size_t index = 0U; index < cpuFlux.size(); ++index) {
    if (!require(std::isfinite(cpuFlux[index]) && std::isfinite(vkFlux[index]),
                 "normalized surface flux values are finite"))
      return EXIT_FAILURE;
    maxFluxUlp = std::max(maxFluxUlp, ulpDistance(cpuFlux[index], vkFlux[index]));
    cpuTotal += static_cast<double>(cpuFlux[index]);
    vkTotal += static_cast<double>(vkFlux[index]);
  }
  const double totalRelDiff =
      std::abs(cpuTotal - vkTotal) / std::max(std::abs(cpuTotal), 1.0e-12);
  if (!require(cpuTotal > 0.0 && vkTotal > 0.0,
               "surface flux conservation has positive totals"))
    return EXIT_FAILURE;
  if (!require(maxFluxUlp == 0U && totalRelDiff <= 1.0e-6,
               "CPU/Vulkan normalized surface flux is raw-bit conserved"))
    return EXIT_FAILURE;

  auto cpuApplyModel =
      viennaps::SmartPointer<viennaps::SingleParticleProcess<float, 2>>::New(
          1.0F, 1.0F, 1.0F);
  auto cpuApplyDomain = makeDomain();
  viennaps::Process<float, 2> cpuApply(cpuApplyDomain, cpuApplyModel);
  cpuApply.setFluxEngineType(viennaps::FluxEngineType::CPU_TRIANGLE);
  cpuApply.setProcessDuration(0.01);
  cpuApply.setParameters(params);
  viennaps::AdvectionParameters advection;
  advection.timeStepRatio = 0.05;
  cpuApply.setParameters(advection);
  cpuApply.apply();
  if (!require(cpuApply.getLastProcessResult() == viennaps::ProcessResult::SUCCESS,
               "CPU Process apply oracle succeeds"))
    return EXIT_FAILURE;
  const auto cpuGeometry = snapshotGeometry(cpuApplyDomain);

  auto vkApplyModel =
      viennaps::SmartPointer<viennaps::SingleParticleProcess<float, 2>>::New(
          1.0F, 1.0F, 1.0F);
  auto vkApplyDomain = makeDomain();
  viennaps::Process<float, 2> vkApply(vkApplyDomain, vkApplyModel);
  vkApply.setFluxEngineOverride(
      std::make_unique<viennaps::VulkanRayFluxEngine<float, 2>>(
          context, paths, false));
  vkApply.setProcessDuration(0.01);
  vkApply.setParameters(params);
  vkApply.setParameters(advection);
  vkApply.apply();
  if (!require(vkApply.getLastProcessResult() == viennaps::ProcessResult::SUCCESS,
               "Vulkan Process apply succeeds"))
    return EXIT_FAILURE;
  const auto vkGeometry = snapshotGeometry(vkApplyDomain);
  if (!require(cpuGeometry.size() == vkGeometry.size(),
               "CPU/Vulkan geometry sizes match"))
    return EXIT_FAILURE;
  float maxGeometryDelta = 0.0F;
  for (std::size_t index = 0U; index < cpuGeometry.size(); ++index)
    maxGeometryDelta = std::max(
        maxGeometryDelta, std::abs(cpuGeometry[index] - vkGeometry[index]));
  if (!require(maxGeometryDelta <= 1.0e-5F,
               "CPU/Vulkan apply geometry delta is bounded"))
    return EXIT_FAILURE;

  // Zero-weight and invalid-status device transactions are checked directly
  // at the accepted pipeline boundary; the Process route above remains the
  // semantic fixture and no CPU algorithm is reimplemented here.
  viennaps::vulkan::ray::DeviceRayFluxPipeline pipeline;
  viennaps::vulkan::ray::DeviceRayFluxSpirv devicePaths{
      paths.triangleHit, paths.recordCompaction, paths.reductionScan,
      paths.radixHistogram, paths.radixPrefix, paths.radixScatter,
      paths.surfaceSegments, paths.surfaceReduce, paths.triangleBvh};
  if (!require(pipeline.initialize(*context->session(), devicePaths, error),
               "device pipeline initialized for transaction checks"))
    return EXIT_FAILURE;
  std::array<viennaps::vulkan::ray::Ray, 1> zeroRays{};
  zeroRays[0].origin = {0.0F, 0.0F, 1.0F};
  zeroRays[0].direction = {0.0F, 0.0F, -1.0F};
  zeroRays[0].tMax = 10.0F;
  std::array<viennaps::vulkan::ray::Triangle, 1> zeroTriangles{};
  zeroTriangles[0].a = {-1.0F, -1.0F, 0.0F};
  zeroTriangles[0].b = {1.0F, -1.0F, 0.0F};
  zeroTriangles[0].c = {0.0F, 1.0F, 0.0F};
  const std::array<float, 1> zeroWeights = {0.0F};
  std::array<std::uint32_t, 1> zeroSurface = {0xA5A5A5A5U};
  std::array<float, 1> zeroOutput = {0.0F};
  viennaps::vulkan::ray::RayFluxResult zeroResult{zeroSurface, zeroOutput, 0U};
  if (!require(pipeline.runGpu(zeroRays, zeroTriangles, zeroWeights,
                               zeroResult, error),
               "zero-weight device transaction succeeds"))
    return EXIT_FAILURE;
  if (!require(zeroResult.count == 1U && zeroOutput[0] == 0.0F,
               "zero-weight device transaction remains zero flux"))
    return EXIT_FAILURE;
  const auto invalidSurfaceBefore = zeroSurface;
  const auto invalidOutputBefore = zeroOutput;
  const std::array<float, 1> invalidWeights = {
      std::numeric_limits<float>::quiet_NaN()};
  viennaps::vulkan::ray::RayFluxResult invalidResult{zeroSurface, zeroOutput,
                                                     zeroResult.count};
  error.clear();
  if (!require(!pipeline.runGpu(zeroRays, zeroTriangles, invalidWeights,
                                invalidResult, error) && !error.empty(),
               "invalid FP32 status fails closed"))
    return EXIT_FAILURE;
  if (!require(zeroSurface == invalidSurfaceBefore &&
                   zeroOutput == invalidOutputBefore &&
                   invalidResult.count == zeroResult.count,
               "invalid status preserves output sentinels"))
    return EXIT_FAILURE;

  // An invalid/unprepared deployment status is fail-closed: no flux may be
  // published even though Process::calculateFlux returns its mesh container.
  auto invalidContext = std::make_shared<
      viennaps::vulkan::runtime::DeploymentComputeContext>();
  auto invalidDomain = makeDomain();
  auto invalidModel =
      viennaps::SmartPointer<viennaps::SingleParticleProcess<float, 2>>::New();
  viennaps::Process<float, 2> invalid(invalidDomain, invalidModel);
  invalid.setFluxEngineOverride(
      std::make_unique<viennaps::VulkanRayFluxEngine<float, 2>>(
          invalidContext, paths, false));
  invalid.setParameters(params);
  if (!require(readFlux(invalid.calculateFlux()).empty(),
               "invalid Vulkan status fails closed without flux publication"))
    return EXIT_FAILURE;

  std::cout << "model-matrix row01 conservation PASS (maxFluxUlp="
            << maxFluxUlp << ", totalFluxRelDiff=" << totalRelDiff
            << ", maxGeometryDelta=" << maxGeometryDelta << ")\n";
  return EXIT_SUCCESS;
} catch (const std::exception &exception) {
  std::cerr << "model-matrix row01 conservation FAIL: " << exception.what()
            << '\n';
  return EXIT_FAILURE;
}
