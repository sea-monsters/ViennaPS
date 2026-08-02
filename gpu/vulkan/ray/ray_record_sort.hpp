// Copyright 2026 ViennaPS
// SPDX-License-Identifier: MIT
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include "../runtime/compute_session.hpp"
#include "../runtime/vulkan_compute_runtime.hpp"
#include "ray_record_compaction.hpp"

namespace viennaps::vulkan::ray {

// Device-only stable sorter.  The current correctness baseline computes each
// output rank independently on the GPU (O(N^2)); it deliberately performs no
// host readback and preserves the byte representation of every record. P5-JB2
// can replace the shader with 16 device-resident 4-bit LSD passes without
// changing this session/count/capacity contract.
class DeviceRayRecordSort {
public:
  DeviceRayRecordSort() = default;
  ~DeviceRayRecordSort();
  DeviceRayRecordSort(const DeviceRayRecordSort &) = delete;
  DeviceRayRecordSort &operator=(const DeviceRayRecordSort &) = delete;
  DeviceRayRecordSort(DeviceRayRecordSort &&) = delete;
  DeviceRayRecordSort &operator=(DeviceRayRecordSort &&) = delete;

  [[nodiscard]] bool initialize(std::string_view spirvPath, std::string &error);
  [[nodiscard]] bool initialize(runtime::ComputeSession &session,
                                std::string_view spirvPath, std::string &error);
  void reset();
  [[nodiscard]] bool isInitialized() const;
  [[nodiscard]] const runtime::VulkanDevice &device() const;

  // inputCount is a one-word device buffer produced by the compactor.  The
  // shader clamps it to inputCapacity; no count is downloaded by this call.
  [[nodiscard]] bool sort(const runtime::DeviceBuffer &inputRecords,
                          const runtime::DeviceBuffer &inputCount,
                          std::size_t inputCapacity,
                          runtime::DeviceBuffer &outputRecords,
                          std::size_t outputCapacity, std::string &error);

private:
  [[nodiscard]] bool setup(std::string_view spirvPath,
                           runtime::ComputeSession *external,
                           std::string &error);
  [[nodiscard]] bool ready(std::string &error) const;

  runtime::ComputeSession ownedSession_{};
  runtime::ComputeSession *session_{nullptr};
  runtime::ShaderModule shaderModule_{};
  runtime::DescriptorSetLayout descriptorSetLayout_{};
  runtime::PipelineLayout pipelineLayout_{};
  runtime::ComputePipeline pipeline_{};
  runtime::DescriptorPool descriptorPool_{};
  runtime::Fence fence_{};
  VkDescriptorSet descriptorSet_{VK_NULL_HANDLE};
  VkCommandBuffer commandBuffer_{VK_NULL_HANDLE};
};

using DeviceRayRecordSorter = DeviceRayRecordSort;

} // namespace viennaps::vulkan::ray
