// Copyright 2026 ViennaPS
// SPDX-License-Identifier: MIT
//
// Process-side injection point for the device-resident Vulkan ray-flux
// pipeline.  This header intentionally does not include Vulkan headers or
// expose DeviceRayFluxPipeline; the implementation is hidden behind a PIMPL
// so that the public API remains backend-neutral.

#pragma once

#include <process/psFluxEngine.hpp>

#include <memory>
#include <string>

namespace viennaps::vulkan::runtime {
class DeploymentComputeContext;
} // namespace viennaps::vulkan::runtime

namespace viennaps {

struct VulkanRayFluxSpirvPaths {
  std::string triangleHit;
  std::string recordCompaction;
  std::string reductionScan;
  std::string radixHistogram;
  std::string radixPrefix;
  std::string radixScatter;
  std::string surfaceSegments;
  std::string surfaceReduce;
  std::string triangleBvh; // may be empty -> brute-force triangle hit
};

/// Vulkan ray-flux engine for the Process/FluxProcessStrategy route.
/// When the injected deployment context selects Vulkan for the RAY_TRACING
/// stage, the engine dispatches rays through the accepted
/// DeviceRayFluxPipeline.  Otherwise it degrades to the CPU triangle engine
/// (AUTO mode) or fails closed (manual Vulkan without a valid profile).
///
/// This slice supports single-bounce, no-reflection transport only.  Models
/// that require reflection, roulette, or multi-bounce physics must not select
/// this engine yet; those contracts belong to the later P5-RAY-PHYSICS card.
template <typename NumericType, int D>
class VulkanRayFluxEngine final : public FluxEngine<NumericType, D> {
public:
  /// allowCpuFallback = true  -> degrade to CPUTriangleEngine when Vulkan is
  ///                            not selected (AUTO semantics).
  /// allowCpuFallback = false -> fail closed when the manual Vulkan request
  ///                            cannot be satisfied.
  VulkanRayFluxEngine(
      std::shared_ptr<vulkan::runtime::DeploymentComputeContext> context,
      const VulkanRayFluxSpirvPaths &paths, bool allowCpuFallback = true);

  ~VulkanRayFluxEngine() override;

  VulkanRayFluxEngine(const VulkanRayFluxEngine &) = delete;
  VulkanRayFluxEngine &operator=(const VulkanRayFluxEngine &) = delete;

  ProcessResult checkInput(ProcessContext<NumericType, D> &context) override;
  ProcessResult initialize(ProcessContext<NumericType, D> &context) override;
  ProcessResult updateSurface(ProcessContext<NumericType, D> &context) override;
  ProcessResult calculateSourceFluxes(
      ProcessContext<NumericType, D> &context,
      viennacore::SmartPointer<PointData<NumericType>> &fluxes) override;
  ProcessResult calculateSurfaceFluxes(
      ProcessContext<NumericType, D> &context,
      viennacore::SmartPointer<PointData<NumericType>> &fluxes) override;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace viennaps
