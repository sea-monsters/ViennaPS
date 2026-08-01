// Copyright 2026 ViennaPS
// SPDX-License-Identifier: MIT
//
// Reusable Vulkan gather/scatter/histogram primitive wrapper for production usage.

#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>
#include <string>
#include <string_view>

#include <vulkan/vulkan.h>

#include "../runtime/vulkan_compute_runtime.hpp"

namespace viennaps::vulkan::primitives {

enum class GatherHistogramOperation : std::uint32_t {
  gather = 0u,
  scatter = 1u,
  histogram = 2u
};

struct ScatterOptions {
  // Reject duplicate indices by default. If set, duplicate-index inputs are
  // handled with a deterministic host-side dedupe policy before dispatch.
  bool enableDeterministicDuplicatePolicy = false;
};

class GatherHistogramPrimitives {
public:
  GatherHistogramPrimitives() = default;
  ~GatherHistogramPrimitives() = default;

  GatherHistogramPrimitives(const GatherHistogramPrimitives &) = delete;
  GatherHistogramPrimitives &operator=(const GatherHistogramPrimitives &) = delete;
  GatherHistogramPrimitives(GatherHistogramPrimitives &&) = default;
  GatherHistogramPrimitives &operator=(GatherHistogramPrimitives &&) = default;

  [[nodiscard]] bool initialize(std::string_view spirvPath, std::string &error);
  void reset();
  [[nodiscard]] bool isInitialized() const;

  [[nodiscard]] bool
  createFloatBuffer(std::size_t elementCount,
                    viennaps::vulkan::runtime::HostVisibleBuffer &buffer,
                    std::string &error);

  [[nodiscard]] bool
  createUInt32Buffer(std::size_t elementCount,
                     viennaps::vulkan::runtime::HostVisibleBuffer &buffer,
                     std::string &error);

  [[nodiscard]] bool gather(
      runtime::HostVisibleBuffer &output, std::size_t outputElementCount,
      runtime::HostVisibleBuffer &input, std::size_t inputElementCount,
      runtime::HostVisibleBuffer &indices, std::size_t indexElementCount,
      std::string &error);

  [[nodiscard]] bool scatter(
      runtime::HostVisibleBuffer &output, std::size_t outputElementCount,
      runtime::HostVisibleBuffer &input, std::size_t inputElementCount,
      runtime::HostVisibleBuffer &indices, std::size_t indexElementCount,
      std::string &error, ScatterOptions options = {});

  [[nodiscard]] bool histogram(
      runtime::HostVisibleBuffer &histogram, std::size_t binCount,
      runtime::HostVisibleBuffer &input, std::size_t inputElementCount,
      std::string &error);

  [[nodiscard]] const runtime::VulkanDevice &device() const;

private:
  static constexpr float kDefaultFillValue = 0.0f;
  static constexpr std::size_t kWorkgroupSize = 256u;
  static constexpr std::size_t kMaxHistogramBinCount = 65'535u;
  static constexpr VkBufferUsageFlags kFloatBufferUsage =
      VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
      VK_BUFFER_USAGE_TRANSFER_DST_BIT;
  static constexpr VkBufferUsageFlags kUInt32BufferUsage = kFloatBufferUsage;
  static constexpr VkMemoryPropertyFlags kHostOnlyMemoryFlags =
      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT;

  struct OperationContext {
    GatherHistogramOperation operation;
    runtime::HostVisibleBuffer *inputFloat;
    runtime::HostVisibleBuffer *indices;
    runtime::HostVisibleBuffer *outputFloat;
    runtime::HostVisibleBuffer *outputHistogram;
    std::size_t elementCount;
    std::size_t outputElementCount;
    std::size_t binCount;
  };

  struct PushConstants {
    std::uint32_t elementCount;
    std::uint32_t outputElementCount;
    std::uint32_t binCount;
  };

  [[nodiscard]] bool isReady(std::string &error) const;
  [[nodiscard]] bool createPipeline(runtime::ComputePipeline &pipeline,
                                   GatherHistogramOperation operation,
                                   std::string &error);
  [[nodiscard]] bool dispatch(const OperationContext &context,
                             std::string &error);
  [[nodiscard]] bool validateFloatLength(std::string_view label,
                                        const std::string_view operation,
                                        const runtime::HostVisibleBuffer &buffer,
                                        std::size_t elementCount,
                                        std::string &error) const;
  [[nodiscard]] bool validateUInt32Length(
      std::string_view label, const std::string_view operation,
      const runtime::HostVisibleBuffer &buffer, std::size_t elementCount,
      std::string &error) const;
  [[nodiscard]] bool validateAlias(std::string_view label,
                                  const runtime::HostVisibleBuffer &input,
                                  const runtime::HostVisibleBuffer &output,
                                  bool allowInPlace, std::string &error) const;
  [[nodiscard]] bool ensureMapped(runtime::HostVisibleBuffer &buffer,
                                  const std::string_view label,
                                  std::string &error) const;
  [[nodiscard]] bool readOutputIfNeeded(runtime::HostVisibleBuffer &buffer,
                                       std::string &error) const;
  [[nodiscard]] bool
  readFloatBuffer(runtime::HostVisibleBuffer &buffer, std::size_t elementCount,
                  std::vector<float> &values, std::string &error) const;
  [[nodiscard]] bool
  readUInt32Buffer(runtime::HostVisibleBuffer &buffer, std::size_t elementCount,
                   std::vector<std::uint32_t> &values, std::string &error) const;
  [[nodiscard]] bool writeUInt32Buffer(runtime::HostVisibleBuffer &buffer,
                                      const std::vector<std::uint32_t> &values,
                                      std::size_t elementCount,
                                      std::string &error) const;
  [[nodiscard]] bool writeFloatBuffer(runtime::HostVisibleBuffer &buffer,
                                     const std::vector<float> &values,
                                     std::size_t elementCount,
                                     std::string &error) const;
  [[nodiscard]] bool validateAndCheckIndices(
      const std::vector<std::uint32_t> &indices, std::size_t validElementCount,
      bool allowDuplicates, bool &hasDuplicate, std::string &error) const;
  [[nodiscard]] bool validateHistogramValues(
      const std::vector<std::uint32_t> &values, std::size_t binCount,
      std::string &error) const;
  [[nodiscard]] bool validateHistogramBinCount(std::size_t binCount,
                                              std::string &error) const;
  [[nodiscard]] bool buildDeterministicScatterInputs(
      const std::vector<std::uint32_t> &indices,
      const std::vector<float> &inputValues, std::size_t outputElementCount,
      std::vector<std::uint32_t> &deduplicatedIndices,
      std::vector<float> &deduplicatedValues, std::string &error) const;
  [[nodiscard]] bool zeroUInt32Buffer(runtime::HostVisibleBuffer &buffer,
                                     std::size_t elementCount,
                                     std::string &error) const;
  [[nodiscard]] bool ensureDummyBuffers(std::string &error);

  runtime::VulkanInstance instance_{};
  runtime::VulkanDevice device_{};
  runtime::ShaderModule shaderModule_{};
  runtime::DescriptorSetLayout descriptorSetLayout_{};
  runtime::PipelineLayout pipelineLayout_{};
  runtime::ComputePipeline gatherPipeline_{};
  runtime::ComputePipeline scatterPipeline_{};
  runtime::ComputePipeline histogramPipeline_{};
  runtime::DescriptorPool descriptorPool_{};
  runtime::CommandContext commandContext_{};
  runtime::Fence fence_{};
  runtime::HostVisibleBuffer dummyFloatBuffer_{};
  runtime::HostVisibleBuffer dummyUInt32Buffer_{};
  bool dummyBuffersReady_{false};

  VkDescriptorSet descriptorSet_{VK_NULL_HANDLE};
  VkCommandBuffer commandBuffer_{VK_NULL_HANDLE};
};

} // namespace viennaps::vulkan::primitives
