// Copyright 2026 ViennaPS
// SPDX-License-Identifier: MIT
//
// Reusable Vulkan radix-sort primitive wrapper for production usage.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

#include <vulkan/vulkan.h>

#include "../runtime/vulkan_compute_runtime.hpp"

namespace viennaps::vulkan::primitives {

enum class RadixSortOperation : std::uint32_t { histogram = 0u, scatter = 1u };

struct RadixSortOptions {
  bool allowInPlaceSort = false;
};

class RadixSortPrimitives {
public:
  RadixSortPrimitives() = default;
  ~RadixSortPrimitives() = default;

  RadixSortPrimitives(const RadixSortPrimitives &) = delete;
  RadixSortPrimitives &operator=(const RadixSortPrimitives &) = delete;
  RadixSortPrimitives(RadixSortPrimitives &&) = default;
  RadixSortPrimitives &operator=(RadixSortPrimitives &&) = default;

  [[nodiscard]] bool initialize(std::string_view spirvPath, std::string &error);
  void reset();
  [[nodiscard]] bool isInitialized() const;

  [[nodiscard]] bool createKeyBuffer(std::size_t elementCount,
                                     runtime::HostVisibleBuffer &buffer,
                                     std::string &error);
  [[nodiscard]] bool createValueBuffer(std::size_t elementCount,
                                       runtime::HostVisibleBuffer &buffer,
                                       std::string &error);

  [[nodiscard]] bool sortByKey(
      runtime::HostVisibleBuffer &inputKeys, std::size_t inputElementCount,
      runtime::HostVisibleBuffer &inputValues, std::size_t inputValueCount,
      runtime::HostVisibleBuffer &sortedKeys, std::size_t sortedKeyCapacity,
      runtime::HostVisibleBuffer &sortedValues, std::size_t sortedValueCapacity,
      std::string &error, RadixSortOptions options = {});

  [[nodiscard]] const runtime::VulkanDevice &device() const;

private:
  static constexpr std::size_t kWorkgroupSize = 256u;
  static constexpr std::size_t kBins = 16u;
  static constexpr std::size_t kRequiredSharedMemoryBytes =
      3u * kWorkgroupSize * sizeof(std::uint32_t);
  static constexpr std::size_t kRadixBits = 4u;
  static constexpr std::size_t kPasses = 32u / kRadixBits;
  static constexpr std::array<std::uint32_t, kPasses> kBitShifts = {
      0u, 4u, 8u, 12u, 16u, 20u, 24u, 28u};
  static constexpr VkBufferUsageFlags kBufferUsage =
      VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
      VK_BUFFER_USAGE_TRANSFER_DST_BIT;
  static constexpr VkMemoryPropertyFlags kMemoryFlags =
      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT;

  struct PushConstants {
    std::uint32_t elementCount;
    std::uint32_t bitShift;
  };

  struct DispatchContext {
    runtime::HostVisibleBuffer *inputKeys;
    runtime::HostVisibleBuffer *inputValues;
    runtime::HostVisibleBuffer *outputKeys;
    runtime::HostVisibleBuffer *outputValues;
    runtime::HostVisibleBuffer *histogram;
    std::size_t elementCount;
    std::uint32_t bitShift;
  };

  [[nodiscard]] bool isReady(std::string &error) const;
  [[nodiscard]] bool
  validateInputLengths(std::string &error, std::size_t keyCount,
                       std::size_t valueCount, std::size_t sortedKeyCapacity,
                       std::size_t sortedValueCapacity) const;
  [[nodiscard]] bool validateSameDevice(std::string &error,
                                        const runtime::HostVisibleBuffer &left,
                                        const runtime::HostVisibleBuffer &right,
                                        const std::string_view label) const;
  [[nodiscard]] bool validateLength(std::string_view label,
                                    const runtime::HostVisibleBuffer &buffer,
                                    std::size_t elementCount,
                                    std::size_t elementSize,
                                    std::string &error) const;
  [[nodiscard]] bool validateAlias(std::string_view label,
                                   const runtime::HostVisibleBuffer &input,
                                   const runtime::HostVisibleBuffer &output,
                                   bool allowAlias, std::string &error) const;
  [[nodiscard]] bool validateDeviceLimits(std::string &error,
                                          std::size_t elementCount) const;
  [[nodiscard]] bool createPipeline(runtime::ComputePipeline &pipeline,
                                    RadixSortOperation operation,
                                    std::string &error);
  [[nodiscard]] bool ensureMapped(runtime::HostVisibleBuffer &buffer,
                                  std::string_view label,
                                  std::string &error) const;
  [[nodiscard]] bool dispatch(const DispatchContext &context,
                              RadixSortOperation operation,
                              std::string &error) const;
  [[nodiscard]] bool copyBuffer(runtime::HostVisibleBuffer &source,
                                std::size_t elementCount,
                                runtime::HostVisibleBuffer &destination,
                                std::string &error) const;
  [[nodiscard]] bool ensureBufferSize(std::string_view label,
                                      runtime::HostVisibleBuffer &buffer,
                                      std::size_t elementCount,
                                      std::size_t elementSize,
                                      std::string &error) const;
  [[nodiscard]] bool clearBuffer(runtime::HostVisibleBuffer &buffer,
                                 std::size_t elementCount,
                                 std::string &error) const;
  [[nodiscard]] bool createBuffer(std::size_t elementCount,
                                  std::size_t elementSize,
                                  runtime::HostVisibleBuffer &buffer,
                                  std::string &error);
  [[nodiscard]] bool readHistogram(runtime::HostVisibleBuffer &buffer,
                                   std::size_t elementCount,
                                   std::vector<std::uint32_t> &counts,
                                   std::string &error) const;
  [[nodiscard]] bool writeHistogram(runtime::HostVisibleBuffer &buffer,
                                    const std::vector<std::uint32_t> &counts,
                                    std::string &error) const;
  [[nodiscard]] bool materializePrefix(std::vector<std::uint32_t> &counts,
                                       std::size_t workgroupCount,
                                       std::size_t elementCount,
                                       std::string &error) const;

  runtime::VulkanInstance instance_{};
  runtime::VulkanDevice device_{};
  runtime::ShaderModule shaderModule_{};
  runtime::DescriptorSetLayout descriptorSetLayout_{};
  runtime::PipelineLayout pipelineLayout_{};
  runtime::ComputePipeline histogramPipeline_{};
  runtime::ComputePipeline scatterPipeline_{};
  runtime::DescriptorPool descriptorPool_{};
  runtime::CommandContext commandContext_{};
  runtime::Fence fence_{};
  VkDescriptorSet descriptorSet_{VK_NULL_HANDLE};
  VkCommandBuffer commandBuffer_{VK_NULL_HANDLE};
};

} // namespace viennaps::vulkan::primitives
