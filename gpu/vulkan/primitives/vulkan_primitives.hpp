// Copyright 2026 ViennaPS
// SPDX-License-Identifier: MIT
//
// Reusable Vulkan elementwise primitives for production usage.

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include <vulkan/vulkan.h>

#include "../runtime/vulkan_compute_runtime.hpp"

namespace viennaps::vulkan::primitives {

enum class ElementwiseOperation : std::uint32_t {
  fill = 0u,
  copy = 1u,
  affine = 2u,
};

struct ElementwisePrimitivesOptions {
  bool allowInPlaceCopy = false;
  bool allowInPlaceAffine = false;
};

class ElementwisePrimitives {
public:
  ElementwisePrimitives() = default;
  ~ElementwisePrimitives() = default;

  ElementwisePrimitives(const ElementwisePrimitives &) = delete;
  ElementwisePrimitives &operator=(const ElementwisePrimitives &) = delete;
  ElementwisePrimitives(ElementwisePrimitives &&) = default;
  ElementwisePrimitives &operator=(ElementwisePrimitives &&) = default;

  [[nodiscard]] bool initialize(std::string_view spirvPath, std::string &error);
  void reset();
  [[nodiscard]] bool isInitialized() const;

  [[nodiscard]] bool
  createFloatBuffer(std::size_t elementCount,
                    viennaps::vulkan::runtime::HostVisibleBuffer &buffer,
                    std::string &error);

  [[nodiscard]] bool fill(runtime::HostVisibleBuffer &output,
                          std::size_t elementCount, float value,
                          std::string &error) const;

  [[nodiscard]] bool copy(runtime::HostVisibleBuffer &output,
                          std::size_t outputElementCount,
                          runtime::HostVisibleBuffer &input,
                          std::size_t inputElementCount, std::string &error,
                          ElementwisePrimitivesOptions options = {}) const;

  [[nodiscard]] bool affineTransform(
      runtime::HostVisibleBuffer &output, std::size_t outputElementCount,
      runtime::HostVisibleBuffer &input, std::size_t inputElementCount,
      float scale, float offset, std::string &error,
      ElementwisePrimitivesOptions options = {}) const;

  [[nodiscard]] const runtime::VulkanDevice &device() const;

private:
  static constexpr float kDefaultFillValue = 0.0f;
  static constexpr std::size_t kWorkgroupSize = 256u;
  static constexpr VkBufferUsageFlags kFloatBufferUsage =
      VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
      VK_BUFFER_USAGE_TRANSFER_DST_BIT;
  static constexpr VkMemoryPropertyFlags kFloatMemoryFlags =
      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT;

  struct OperationContext {
    ElementwiseOperation operation;
    runtime::HostVisibleBuffer *input;
    runtime::HostVisibleBuffer *output;
    std::size_t elementCount;
    float scalarA;
    float scalarB;
    bool allowInPlace;
  };

  [[nodiscard]] bool isReady(std::string &error) const;
  [[nodiscard]] bool createPipeline(runtime::ComputePipeline &pipeline,
                                    ElementwiseOperation operation,
                                    std::string &error);
  [[nodiscard]] bool dispatch(const OperationContext &context,
                              std::string &error) const;
  [[nodiscard]] bool validateLength(const std::string_view label,
                                    const runtime::HostVisibleBuffer &buffer,
                                    const std::size_t elementCount,
                                    std::string &error) const;
  [[nodiscard]] bool validateAlias(const std::string_view label,
                                   const runtime::HostVisibleBuffer &input,
                                   const runtime::HostVisibleBuffer &output,
                                   bool allowInPlace, std::string &error) const;
  [[nodiscard]] bool ensureMapped(runtime::HostVisibleBuffer &buffer,
                                  const std::string_view label,
                                  std::string &error) const;
  [[nodiscard]] bool readOutputIfNeeded(runtime::HostVisibleBuffer &buffer,
                                        std::string &error) const;

  runtime::VulkanInstance instance_{};
  runtime::VulkanDevice device_{};
  runtime::ShaderModule shaderModule_{};
  runtime::DescriptorSetLayout descriptorSetLayout_{};
  runtime::PipelineLayout pipelineLayout_{};
  runtime::ComputePipeline fillPipeline_{};
  runtime::ComputePipeline copyPipeline_{};
  runtime::ComputePipeline affinePipeline_{};
  runtime::DescriptorPool descriptorPool_{};
  runtime::CommandContext commandContext_{};
  runtime::Fence fence_{};

  mutable VkDescriptorSet descriptorSet_{VK_NULL_HANDLE};
  mutable VkCommandBuffer commandBuffer_{VK_NULL_HANDLE};
};

} // namespace viennaps::vulkan::primitives
