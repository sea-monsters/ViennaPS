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
#include <string_view>

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
  // Optional bounded multi-bounce frontier shader.  An empty path keeps the
  // existing single-bounce route unchanged and forces CPU fallback for any
  // reflection-enabled request.  A non-empty path additionally admits the
  // bounded one-reflection SingleParticleProcess<float, 2> slice and the
  // NeutralTransport<float, 2> slice (single particle, no source,
  // maxReflections <= 2).
  std::string multibounceFrontierQueue;
};

/// Vulkan ray-flux engine for the Process/FluxProcessStrategy route.
/// When the injected deployment context selects Vulkan for the RAY_TRACING
/// stage, the engine dispatches rays through the accepted
/// DeviceRayFluxPipeline.  Otherwise it degrades to the CPU triangle engine
/// (AUTO mode) or fails closed (manual Vulkan without a valid profile).
///
/// The default route remains the strict single-bounce slice:
///   - SingleParticleProcess<float, 2> with one particle, one data label,
///     no source, and maxReflections == 0.
/// An explicitly supplied frontier shader additionally admits:
///   - SingleParticleProcess<float, 2> with maxReflections == 1, no coverages
///     or surface desorption, and a non-empty multibounceFrontierQueue;
///   - NeutralTransport<float, 2> with one particle, one data label, no source,
///     maxReflections <= 2, and a non-empty multibounceFrontierQueue.
/// Generic model/multi-bounce semantics remain CPU-owned.
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

  /// Generic device-resident multi-bounce remains unavailable.  The only
  /// admitted routes are:
  ///   - SingleParticleProcess<float, 2> with one particle, one data label,
  ///     no source, and maxReflections == 0;
  ///   - the bounded one-reflection SingleParticleProcess<float, 2> extension
  ///     (maxReflections == 1, no coverages/desorption, frontier shader);
  ///   - NeutralTransport<float, 2> with one particle, one data label, no
  ///     source, maxReflections <= 2, and the frontier shader.
  [[nodiscard]] static constexpr std::string_view devicePhysicsGap() {
    return "generic device multi-bounce physics is unavailable; only the bounded CPU-decision frontier route is admitted";
  }

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
