// Copyright 2026 ViennaPS
// SPDX-License-Identifier: MIT
//
// Vulkan pseudo-random number generation primitives for production usage.

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include <vulkan/vulkan.h>

#include "../runtime/vulkan_compute_runtime.hpp"

namespace viennaps::vulkan::primitives {

enum class RNGOperation : std::uint32_t {
  generateUint32 = 0u,
  generateFloat01 = 1u
};

class RNGPrimitives {
public:
  RNGPrimitives() = default;
  ~RNGPrimitives() = default;

  RNGPrimitives(const RNGPrimitives &) = delete;
  RNGPrimitives &operator=(const RNGPrimitives &) = delete;
  RNGPrimitives(RNGPrimitives &&) = default;
  RNGPrimitives &operator=(RNGPrimitives &&) = default;

  [[nodiscard]] bool initialize(std::string_view spirvPath, std::string &error);
  void reset();
  [[nodiscard]] bool isInitialized() const;
  [[nodiscard]] bool
  isDispatchLengthSupported(std::size_t elementCount, std::string &error,
                            std::uint32_t counterOffset = 0u) const;

  [[nodiscard]] bool
  createUint32Buffer(std::size_t elementCount,
                     viennaps::vulkan::runtime::HostVisibleBuffer &buffer,
                     std::string &error);
  [[nodiscard]] bool
  createFloatBuffer(std::size_t elementCount,
                    viennaps::vulkan::runtime::HostVisibleBuffer &buffer,
                    std::string &error);

  [[nodiscard]] bool generate(std::size_t elementCount, std::uint32_t seed,
                              runtime::HostVisibleBuffer &output,
                              std::string &error,
                              std::uint32_t counterOffset = 0u);
  [[nodiscard]] bool generateFloat01(std::size_t elementCount,
                                     std::uint32_t seed,
                                     runtime::HostVisibleBuffer &output,
                                     std::string &error,
                                     std::uint32_t counterOffset = 0u);

  [[nodiscard]] const runtime::VulkanDevice &device() const;

private:
  static constexpr std::size_t kWorkgroupSize = 256u;
  static constexpr std::size_t kUint32Size = sizeof(std::uint32_t);
  static constexpr std::size_t kFloatSize = sizeof(float);
  static constexpr VkBufferUsageFlags kBufferUsage =
      VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
      VK_BUFFER_USAGE_TRANSFER_DST_BIT;
  static constexpr VkMemoryPropertyFlags kMemoryPropertyFlags =
      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT;

  struct DispatchContext {
    RNGOperation operation;
    runtime::HostVisibleBuffer *output;
    std::size_t elementCount;
    std::uint32_t seed;
    std::uint32_t counterOffset;
  };

  [[nodiscard]] bool isReady(std::string &error) const;
  [[nodiscard]] bool createPipeline(runtime::ComputePipeline &pipeline,
                                    RNGOperation operation, std::string &error);
  [[nodiscard]] bool
  validateOutputLength(const std::string_view label,
                       const runtime::HostVisibleBuffer &buffer,
                       std::size_t elementCount, std::size_t elementSize,
                       std::string &error) const;
  [[nodiscard]] bool ensureMapped(runtime::HostVisibleBuffer &buffer,
                                  const std::string_view label,
                                  std::string &error) const;
  [[nodiscard]] bool dispatch(const DispatchContext &context,
                              std::string &error);
  [[nodiscard]] bool createBuffer(std::size_t elementCount,
                                  runtime::HostVisibleBuffer &buffer,
                                  std::size_t elementSize, std::string &error);

  runtime::VulkanInstance instance_{};
  runtime::VulkanDevice device_{};
  runtime::ShaderModule shaderModule_{};
  runtime::DescriptorSetLayout descriptorSetLayout_{};
  runtime::PipelineLayout pipelineLayout_{};
  runtime::ComputePipeline u32Pipeline_{};
  runtime::ComputePipeline f32Pipeline_{};
  runtime::DescriptorPool descriptorPool_{};
  runtime::CommandContext commandContext_{};
  runtime::Fence fence_{};

  mutable VkDescriptorSet descriptorSet_{VK_NULL_HANDLE};
  mutable VkCommandBuffer commandBuffer_{VK_NULL_HANDLE};
};

} // namespace viennaps::vulkan::primitives
