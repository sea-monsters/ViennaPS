// Copyright 2026 ViennaPS
// SPDX-License-Identifier: MIT
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include "../primitives/reduction_scan_primitives.hpp"
#include "../runtime/compute_session.hpp"
#include "../runtime/vulkan_compute_runtime.hpp"
#include "ray_record_compaction.hpp"

namespace viennaps::vulkan::ray {

// Device-resident segment metadata for a record stream already ordered by
// (surfaceId, rayId). A nonzero flag starts a surface segment; offsets map
// every active record to its dense segment index and count is the segment
// count.
struct DeviceRaySurfaceReductionOutput {
  runtime::DeviceBuffer flags{};
  runtime::DeviceBuffer offsets{};
  runtime::DeviceBuffer count{};
  // Device-local sticky failure bit: bit 0 means strict FP32 reduction left
  // the normal-or-exact-zero domain.
  runtime::DeviceBuffer status{};
  std::uint32_t inputCapacity{0U};
  std::uint64_t sessionGeneration{0U};
};

// Builds surface segments and reduces each segment in sorted record order.
// Output buffers remain device-local. Each segment's first weightBits word is
// the FP32 accumulator seed, preserving a singleton negative-zero bit pattern.
class DeviceRaySurfaceReducer {
public:
  DeviceRaySurfaceReducer() = default;
  ~DeviceRaySurfaceReducer();
  DeviceRaySurfaceReducer(const DeviceRaySurfaceReducer &) = delete;
  DeviceRaySurfaceReducer &operator=(const DeviceRaySurfaceReducer &) = delete;
  DeviceRaySurfaceReducer(DeviceRaySurfaceReducer &&) = delete;
  DeviceRaySurfaceReducer &operator=(DeviceRaySurfaceReducer &&) = delete;

  [[nodiscard]] bool initialize(std::string_view segmentSpirv,
                                std::string_view reduceSpirv,
                                std::string_view scanSpirv, std::string &error);
  [[nodiscard]] bool initialize(runtime::ComputeSession &session,
                                std::string_view segmentSpirv,
                                std::string_view reduceSpirv,
                                std::string_view scanSpirv, std::string &error);
  void reset();
  [[nodiscard]] bool isInitialized() const;
  [[nodiscard]] const runtime::VulkanDevice &device() const;

  // inputRecords must already be stably ordered by (surfaceId, rayId).
  // outputCapacity must conservatively hold inputCapacity records because the
  // active device count is intentionally not read back on the host.
  [[nodiscard]] bool
  reduce(const runtime::DeviceBuffer &inputRecords,
         const runtime::DeviceBuffer &inputCount, std::size_t inputCapacity,
         runtime::DeviceBuffer &outputSurfaceId,
         runtime::DeviceBuffer &outputWeight, std::size_t outputCapacity,
         DeviceRaySurfaceReductionOutput &output, std::string &error);

  // Record segment construction, scan/count, and ordered reduction into a
  // caller-owned command buffer using preallocated output buffers. The command
  // buffer must already be recording; no reset/begin/end/submit/wait/download
  // occurs here. `scanScratch` and every buffer must live through completion.
  [[nodiscard]] bool recordReduce(
      VkCommandBuffer commandBuffer, const runtime::DeviceBuffer &inputRecords,
      const runtime::DeviceBuffer &inputCount, std::size_t inputCapacity,
      runtime::DeviceBuffer &outputSurfaceId,
      runtime::DeviceBuffer &outputWeight, std::size_t outputCapacity,
      DeviceRaySurfaceReductionOutput &output,
      primitives::ReductionScanPrimitives::DeviceScanScratch &scanScratch,
      std::string &error);

private:
  [[nodiscard]] bool setup(std::string_view segmentSpirv,
                           std::string_view reduceSpirv,
                           std::string_view scanSpirv,
                           runtime::ComputeSession *external,
                           std::string &error);
  [[nodiscard]] bool ready(std::string &error) const;

  runtime::ComputeSession ownedSession_{};
  runtime::ComputeSession *session_{nullptr};
  primitives::ReductionScanPrimitives scan_{};
  std::array<runtime::ShaderModule, 2U> shaderModules_{};
  runtime::DescriptorSetLayout descriptorSetLayout_{};
  runtime::PipelineLayout pipelineLayout_{};
  std::array<runtime::ComputePipeline, 2U> pipelines_{};
  runtime::DescriptorPool descriptorPool_{};
  runtime::Fence fence_{};
  VkDescriptorSet descriptorSet_{VK_NULL_HANDLE};
  VkCommandBuffer commandBuffer_{VK_NULL_HANDLE};
};

using DeviceRaySegmentReducer = DeviceRaySurfaceReducer;

} // namespace viennaps::vulkan::ray
