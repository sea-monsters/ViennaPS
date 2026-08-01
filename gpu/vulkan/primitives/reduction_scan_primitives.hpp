// Copyright 2026 ViennaPS
// SPDX-License-Identifier: MIT

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

#include <vulkan/vulkan.h>

#include "../runtime/vulkan_compute_runtime.hpp"

namespace viennaps::vulkan::primitives {

enum class ReductionScanOperation : std::uint32_t {
  reduceFloatBlocks = 0u,
  exclusiveScanIntBlocks = 1u,
  exclusiveScanIntAddOffsets = 2u,
};

struct ReductionScanStats {
  float sum{};
  float minValue{};
  float maxValue{};
};

struct ReductionScanOptions {
  bool allowInPlaceScan = false;
};

class ReductionScanPrimitives {
public:
  ReductionScanPrimitives() = default;
  ~ReductionScanPrimitives() = default;

  ReductionScanPrimitives(const ReductionScanPrimitives &) = delete;
  ReductionScanPrimitives &operator=(const ReductionScanPrimitives &) = delete;
  ReductionScanPrimitives(ReductionScanPrimitives &&) = default;
  ReductionScanPrimitives &operator=(ReductionScanPrimitives &&) = default;

  [[nodiscard]] bool initialize(std::string_view spirvPath, std::string &error);
  void reset();
  [[nodiscard]] bool isInitialized() const;

  [[nodiscard]] bool
  createFloatBuffer(std::size_t elementCount,
                    runtime::HostVisibleBuffer &buffer, std::string &error);
  [[nodiscard]] bool createIntBuffer(std::size_t elementCount,
                                     runtime::HostVisibleBuffer &buffer,
                                     std::string &error);

  [[nodiscard]] bool reduceSumMinMax(runtime::HostVisibleBuffer &input,
                                     std::size_t elementCount,
                                     ReductionScanStats &stats,
                                     std::string &error);
  [[nodiscard]] bool exclusiveScanInt(
      runtime::HostVisibleBuffer &input, std::size_t inputElementCount,
      runtime::HostVisibleBuffer &output, std::size_t outputElementCount,
      std::string &error, ReductionScanOptions options = {});

  [[nodiscard]] const runtime::VulkanDevice &device() const;

private:
  static constexpr std::size_t kWorkgroupSize = 256u;
  static constexpr VkBufferUsageFlags kBufferUsage =
      VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
      VK_BUFFER_USAGE_TRANSFER_DST_BIT;
  static constexpr VkMemoryPropertyFlags kMemoryFlags =
      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT;

  struct PushConstants {
    std::uint32_t elementCount;
    std::uint32_t inputIsTriples;
  };

  [[nodiscard]] bool isReady(std::string &error) const;
  [[nodiscard]] bool validateDeviceLimits(std::string &error) const;
  [[nodiscard]] bool createPipeline(runtime::ComputePipeline &pipeline,
                                    ReductionScanOperation operation,
                                    std::string &error);
  [[nodiscard]] bool createBuffer(std::size_t elementCount,
                                  std::size_t elementSize,
                                  runtime::HostVisibleBuffer &buffer,
                                  std::string &error);
  [[nodiscard]] bool validateFloatLength(
      std::string_view label, const runtime::HostVisibleBuffer &buffer,
      std::size_t elementCount, std::string &error) const;
  [[nodiscard]] bool validateIntLength(
      std::string_view label, const runtime::HostVisibleBuffer &buffer,
      std::size_t elementCount, std::string &error) const;
  [[nodiscard]] bool validateAlias(std::string_view label,
                                   const runtime::HostVisibleBuffer &input,
                                   const runtime::HostVisibleBuffer &output,
                                   bool allowInPlace,
                                   std::string &error) const;
  [[nodiscard]] bool ensureMapped(runtime::HostVisibleBuffer &buffer,
                                  std::string_view label,
                                  std::string &error) const;
  [[nodiscard]] bool ensureDummyBuffers(std::string &error);
  [[nodiscard]] bool updateDescriptors(
      runtime::HostVisibleBuffer &floatInput,
      runtime::HostVisibleBuffer &floatOutput,
      runtime::HostVisibleBuffer &intInput,
      runtime::HostVisibleBuffer &intOutput,
      runtime::HostVisibleBuffer &intAux, std::string &error);
  [[nodiscard]] bool dispatchKernel(
      runtime::ComputePipeline &pipeline, std::size_t dispatchX,
      const PushConstants &constants,
      std::span<const VkBufferMemoryBarrier> preBarriers,
      std::span<const VkBufferMemoryBarrier> postBarriers,
      std::string &error);
  [[nodiscard]] bool dispatchReduce(runtime::HostVisibleBuffer &input,
                                    runtime::HostVisibleBuffer &output,
                                    std::size_t elementCount,
                                    bool inputIsTriples,
                                    std::string &error);
  [[nodiscard]] bool dispatchScanBlocks(
      runtime::HostVisibleBuffer &input, runtime::HostVisibleBuffer &output,
      runtime::HostVisibleBuffer &blockSums, std::size_t elementCount,
      std::string &error);
  [[nodiscard]] bool dispatchScanAddOffsets(
      runtime::HostVisibleBuffer &output,
      runtime::HostVisibleBuffer &blockOffsets, std::size_t elementCount,
      std::string &error);
  [[nodiscard]] bool scanIntRecursive(runtime::HostVisibleBuffer &input,
                                      std::size_t elementCount,
                                      runtime::HostVisibleBuffer &output,
                                      std::string &error);

  runtime::VulkanInstance instance_{};
  runtime::VulkanDevice device_{};
  runtime::ShaderModule shaderModule_{};
  runtime::DescriptorSetLayout descriptorSetLayout_{};
  runtime::PipelineLayout pipelineLayout_{};
  runtime::ComputePipeline reducePipeline_{};
  runtime::ComputePipeline scanBlocksPipeline_{};
  runtime::ComputePipeline scanAddOffsetsPipeline_{};
  runtime::DescriptorPool descriptorPool_{};
  runtime::CommandContext commandContext_{};
  runtime::Fence fence_{};
  runtime::HostVisibleBuffer dummyFloat_{};
  runtime::HostVisibleBuffer dummyInt_{};
  VkDescriptorSet descriptorSet_{VK_NULL_HANDLE};
  VkCommandBuffer commandBuffer_{VK_NULL_HANDLE};
};

} // namespace viennaps::vulkan::primitives
