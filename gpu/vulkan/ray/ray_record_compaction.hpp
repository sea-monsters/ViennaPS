// Copyright 2026 ViennaPS
// SPDX-License-Identifier: MIT
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

#include "../primitives/reduction_scan_primitives.hpp"
#include "../runtime/compute_session.hpp"
#include "../runtime/vulkan_compute_runtime.hpp"
#include "triangle_hit.hpp"

namespace viennaps::vulkan::ray {

struct RayRecord {
  std::uint32_t rayId{};
  std::uint32_t surfaceId{};
  std::uint32_t weightBits{};
  std::uint32_t reserved{};
};
static_assert(sizeof(RayRecord) == 16U, "RayRecord must match std430 ABI");

struct RayRecordCompactionDeviceOutput {
  runtime::DeviceBuffer flags{};
  runtime::DeviceBuffer offsets{};
  runtime::DeviceBuffer count{};
  runtime::DeviceBuffer records{};
  std::uint32_t inputCount{0U};
  std::uint64_t sessionGeneration{0U};
};

class DeviceRayRecordCompactor {
public:
  DeviceRayRecordCompactor() = default;
  ~DeviceRayRecordCompactor();
  DeviceRayRecordCompactor(const DeviceRayRecordCompactor &) = delete;
  DeviceRayRecordCompactor &
  operator=(const DeviceRayRecordCompactor &) = delete;
  DeviceRayRecordCompactor(DeviceRayRecordCompactor &&) = delete;
  DeviceRayRecordCompactor &operator=(DeviceRayRecordCompactor &&) = delete;

  [[nodiscard]] bool initialize(std::string_view compactionSpirv,
                                std::string_view reductionScanSpirv,
                                std::string &error);
  [[nodiscard]] bool initialize(runtime::ComputeSession &session,
                                std::string_view compactionSpirv,
                                std::string_view reductionScanSpirv,
                                std::string &error);
  void reset();
  [[nodiscard]] bool isInitialized() const;
  [[nodiscard]] const runtime::VulkanDevice &device() const;
  [[nodiscard]] bool createRecordBuffer(std::size_t capacity,
                                        runtime::DeviceBuffer &records,
                                        std::string &error) const;

  // All intermediates and outputs remain device-local. `output.records` must
  // be caller-owned storage; it is modified only after count/capacity checks.
  [[nodiscard]] bool compact(const runtime::DeviceBuffer &hits,
                             const runtime::DeviceBuffer &weights,
                             std::size_t rayCount, std::uint32_t surfaceDomain,
                             std::size_t outputCapacity,
                             RayRecordCompactionDeviceOutput &output,
                             std::string &error);

  // Record both compaction dispatches and their device-resident scan/count
  // steps into a caller-owned command buffer. All output buffers (including
  // flags, offsets, count, and records) must be preallocated and remain alive
  // together with scanScratch until command completion. This method never
  // resets, begins, ends, submits, waits, or performs host transfers.
  [[nodiscard]] bool recordCompact(
      VkCommandBuffer commandBuffer, const runtime::DeviceBuffer &hits,
      const runtime::DeviceBuffer &weights, std::size_t rayCount,
      std::uint32_t surfaceDomain, std::size_t outputCapacity,
      RayRecordCompactionDeviceOutput &output,
      primitives::ReductionScanPrimitives::DeviceScanScratch &scanScratch,
      std::string &error);

private:
  [[nodiscard]] bool setup(std::string_view compactionSpirv,
                           std::string_view reductionScanSpirv,
                           runtime::ComputeSession *external,
                           std::string &error);
  [[nodiscard]] bool ready(std::string &error) const;
  runtime::ComputeSession ownedSession_{};
  runtime::ComputeSession *session_{nullptr};
  primitives::ReductionScanPrimitives scan_{};
  runtime::ShaderModule shaderModule_{};
  runtime::DescriptorSetLayout descriptorSetLayout_{};
  runtime::PipelineLayout pipelineLayout_{};
  runtime::ComputePipeline pipeline_{};
  runtime::DescriptorPool descriptorPool_{};
  runtime::Fence fence_{};
  VkDescriptorSet descriptorSet_{VK_NULL_HANDLE};
  VkDescriptorSet recordDescriptorSet_{VK_NULL_HANDLE};
  VkCommandBuffer commandBuffer_{VK_NULL_HANDLE};
};

} // namespace viennaps::vulkan::ray
