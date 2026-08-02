// Copyright 2026 ViennaPS
// SPDX-License-Identifier: MIT
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include "../runtime/compute_session.hpp"
#include "../runtime/vulkan_compute_runtime.hpp"
#include "ray_record_compaction.hpp"

namespace viennaps::vulkan::ray {

// Device-resident stable 4-bit LSD radix sorter. The two-level prefix stage
// rejects inputs that exceed its device workgroup/tile bound. The sort consumes
// the device count word and never downloads records or counts to the host.
class DeviceRayRecordRadixSort {
public:
  DeviceRayRecordRadixSort() = default;
  ~DeviceRayRecordRadixSort();
  DeviceRayRecordRadixSort(const DeviceRayRecordRadixSort &) = delete;
  DeviceRayRecordRadixSort &
  operator=(const DeviceRayRecordRadixSort &) = delete;
  DeviceRayRecordRadixSort(DeviceRayRecordRadixSort &&) = delete;
  DeviceRayRecordRadixSort &operator=(DeviceRayRecordRadixSort &&) = delete;

  [[nodiscard]] bool initialize(std::string_view histogramSpirv,
                                std::string_view prefixSpirv,
                                std::string_view scatterSpirv,
                                std::string &error);
  [[nodiscard]] bool initialize(runtime::ComputeSession &session,
                                std::string_view histogramSpirv,
                                std::string_view prefixSpirv,
                                std::string_view scatterSpirv,
                                std::string &error);
  void reset();
  [[nodiscard]] bool isInitialized() const;
  [[nodiscard]] const runtime::VulkanDevice &device() const;

  [[nodiscard]] bool sort(const runtime::DeviceBuffer &inputRecords,
                          const runtime::DeviceBuffer &inputCount,
                          std::size_t inputCapacity,
                          runtime::DeviceBuffer &outputRecords,
                          std::size_t outputCapacity, std::string &error);

private:
  [[nodiscard]] bool setup(std::string_view histogramSpirv,
                           std::string_view prefixSpirv,
                           std::string_view scatterSpirv,
                           runtime::ComputeSession *external,
                           std::string &error);
  [[nodiscard]] bool ready(std::string &error) const;
  [[nodiscard]] bool ensureScratch(std::size_t capacity, std::uint32_t groups,
                                   std::string &error);

  runtime::ComputeSession ownedSession_{};
  runtime::ComputeSession *session_{nullptr};
  std::array<runtime::ShaderModule, 3U> shaderModules_{};
  runtime::DescriptorSetLayout descriptorSetLayout_{};
  runtime::PipelineLayout pipelineLayout_{};
  std::array<runtime::ComputePipeline, 3U> pipelines_{};
  runtime::DescriptorPool descriptorPool_{};
  runtime::Fence fence_{};
  runtime::DeviceBuffer scratchA_{};
  runtime::DeviceBuffer scratchB_{};
  runtime::DeviceBuffer histogram_{};
  runtime::DeviceBuffer offsets_{};
  runtime::DeviceBuffer blockSums_{};
  runtime::DeviceBuffer blockOffsets_{};
  runtime::DeviceBuffer digitBases_{};
  std::array<VkDescriptorSet, 16U> descriptorSets_{};
  VkCommandBuffer commandBuffer_{VK_NULL_HANDLE};
  std::size_t scratchCapacity_{0U};
  std::uint32_t scratchGroups_{0U};
};

using DeviceRayRecordRadixSorter = DeviceRayRecordRadixSort;

} // namespace viennaps::vulkan::ray
