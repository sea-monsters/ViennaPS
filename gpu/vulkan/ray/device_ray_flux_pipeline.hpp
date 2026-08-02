// Copyright 2026 ViennaPS
// SPDX-License-Identifier: MIT
#pragma once

#include <cstddef>
#include <span>
#include <string>
#include <string_view>

#include "ray_flux_pipeline.hpp"
#include "ray_record_radix_sort.hpp"
#include "ray_surface_reduction.hpp"
#include "triangle_hit_device.hpp"

namespace viennaps::vulkan::ray {

// Shader paths needed by the device-resident ray-flux composition. The paths
// are consumed during initialize() and do not need to outlive that call.
struct DeviceRayFluxSpirv {
  std::string_view triangleHit;
  std::string_view recordCompaction;
  std::string_view reductionScan;
  std::string_view radixHistogram;
  std::string_view radixPrefix;
  std::string_view radixScatter;
  std::string_view surfaceSegments;
  std::string_view surfaceReduce;
};

// Composes device triangle intersection, record compaction, stable radix
// ordering, and ordered surface reduction. All intermediate hits, records,
// flags, offsets, and counts remain device-local. The current primitive APIs
// submit their own ordered command buffers, so this class is device-resident
// composition rather than the later one-command fusion path.
class DeviceRayFluxPipeline {
public:
  DeviceRayFluxPipeline() = default;
  ~DeviceRayFluxPipeline();
  DeviceRayFluxPipeline(const DeviceRayFluxPipeline &) = delete;
  DeviceRayFluxPipeline &operator=(const DeviceRayFluxPipeline &) = delete;
  DeviceRayFluxPipeline(DeviceRayFluxPipeline &&) = delete;
  DeviceRayFluxPipeline &operator=(DeviceRayFluxPipeline &&) = delete;

  [[nodiscard]] bool initialize(const DeviceRayFluxSpirv &spirv,
                                std::string &error);
  [[nodiscard]] bool initialize(runtime::ComputeSession &session,
                                const DeviceRayFluxSpirv &spirv,
                                std::string &error);
  void reset();
  [[nodiscard]] bool isInitialized() const;
  [[nodiscard]] const runtime::VulkanDevice &device() const;

  // Shares the existing deterministic CPU pipeline contract. It is deliberately
  // explicit so validation callers can choose CPU as the numerical oracle.
  [[nodiscard]] bool runCpu(std::span<const Ray> rays,
                            std::span<const Triangle> triangles,
                            std::span<const float> weights,
                            RayFluxResult &output, std::string &error) const;

  // Requires output capacity for every input ray because the active device
  // count is not read until the terminal result transfer. Caller storage is
  // changed only after all terminal downloads and validation succeed.
  [[nodiscard]] bool runGpu(std::span<const Ray> rays,
                            std::span<const Triangle> triangles,
                            std::span<const float> weights,
                            RayFluxResult &output, std::string &error);

private:
  [[nodiscard]] bool setup(runtime::ComputeSession *external,
                           const DeviceRayFluxSpirv &spirv, std::string &error);
  [[nodiscard]] bool ready(std::string &error) const;

  runtime::ComputeSession ownedSession_{};
  runtime::ComputeSession *session_{nullptr};
  DeviceTriangleHitPrimitive triangleHit_{};
  DeviceRayRecordCompactor compactor_{};
  DeviceRayRecordRadixSort sorter_{};
  DeviceRaySurfaceReducer reducer_{};
};

} // namespace viennaps::vulkan::ray
