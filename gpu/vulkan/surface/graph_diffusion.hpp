// Copyright 2026 ViennaPS
// SPDX-License-Identifier: MIT
//
// Vulkan FP32 CSR graph-diffusion step for the surface diffusion stencil.

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include "../runtime/compute_session.hpp"
#include "../runtime/vulkan_compute_runtime.hpp"

namespace viennaps::vulkan::surface {

class SurfaceGraphDiffusionFp32 {
public:
  SurfaceGraphDiffusionFp32() = default;
  ~SurfaceGraphDiffusionFp32();

  SurfaceGraphDiffusionFp32(const SurfaceGraphDiffusionFp32 &) = delete;
  SurfaceGraphDiffusionFp32 &
  operator=(const SurfaceGraphDiffusionFp32 &) = delete;
  SurfaceGraphDiffusionFp32(SurfaceGraphDiffusionFp32 &&) = delete;
  SurfaceGraphDiffusionFp32 &operator=(SurfaceGraphDiffusionFp32 &&) = delete;

  [[nodiscard]] bool initialize(std::string_view spirvPath, std::string &error);
  void reset();
  [[nodiscard]] bool isInitialized() const;

  [[nodiscard]] bool
  createFloatBuffer(std::size_t elementCount,
                    viennaps::vulkan::runtime::HostVisibleBuffer &buffer,
                    std::string &error);

  [[nodiscard]] bool
  createIndexBuffer(std::size_t elementCount,
                    viennaps::vulkan::runtime::HostVisibleBuffer &buffer,
                    std::string &error);

  // Evaluates one explicit CPU-equivalent step. `fieldCount` is N; the CSR
  // row-offset count must therefore be N + 1. `outputCapacity` may exceed N
  // so callers can place a tail sentinel after the written range.
  [[nodiscard]] bool
  evaluate(viennaps::vulkan::runtime::HostVisibleBuffer &rowOffsets,
           std::size_t rowOffsetCount,
           viennaps::vulkan::runtime::HostVisibleBuffer &columnIndices,
           std::size_t nonzeroCount,
           viennaps::vulkan::runtime::HostVisibleBuffer &weights,
           std::size_t weightCount,
           viennaps::vulkan::runtime::HostVisibleBuffer &field,
           std::size_t fieldCount,
           viennaps::vulkan::runtime::HostVisibleBuffer &output,
           std::size_t outputCapacity, float diffusionStep,
           std::string &error) const;

  [[nodiscard]] const viennaps::vulkan::runtime::VulkanDevice &device() const;

private:
  static constexpr std::size_t kWorkgroupSize = 256U;
  static constexpr VkBufferUsageFlags kFloatBufferUsage =
      VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
      VK_BUFFER_USAGE_TRANSFER_DST_BIT;
  static constexpr VkBufferUsageFlags kIndexBufferUsage =
      VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
      VK_BUFFER_USAGE_TRANSFER_DST_BIT;
  static constexpr VkMemoryPropertyFlags kHostMemoryFlags =
      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT;

  [[nodiscard]] bool isReady(std::string &error) const;

  struct PushConstants {
    std::uint32_t elementCount;
    std::uint32_t nonzeroCount;
    float diffusionStep;
  };

  runtime::ComputeSession session_{};
  runtime::ShaderModule shaderModule_{};
  runtime::DescriptorSetLayout descriptorSetLayout_{};
  runtime::PipelineLayout pipelineLayout_{};
  runtime::ComputePipeline pipeline_{};
  runtime::DescriptorPool descriptorPool_{};
  runtime::Fence fence_{};

  mutable VkDescriptorSet descriptorSet_{VK_NULL_HANDLE};
  mutable VkCommandBuffer commandBuffer_{VK_NULL_HANDLE};
};

} // namespace viennaps::vulkan::surface
