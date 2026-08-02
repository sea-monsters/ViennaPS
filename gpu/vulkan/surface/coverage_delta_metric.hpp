// Copyright 2026 ViennaPS
// SPDX-License-Identifier: MIT
//
// Independent strict-FP32 coverage convergence metric.

#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

#include "../runtime/compute_session.hpp"
#include "../runtime/vulkan_compute_runtime.hpp"

namespace viennaps::vulkan::surface {

class CoverageDeltaMetricFp32 {
public:
  CoverageDeltaMetricFp32() = default;
  ~CoverageDeltaMetricFp32();

  CoverageDeltaMetricFp32(const CoverageDeltaMetricFp32 &) = delete;
  CoverageDeltaMetricFp32 &operator=(const CoverageDeltaMetricFp32 &) = delete;
  CoverageDeltaMetricFp32(CoverageDeltaMetricFp32 &&) = delete;
  CoverageDeltaMetricFp32 &operator=(CoverageDeltaMetricFp32 &&) = delete;

  [[nodiscard]] bool initialize(std::string_view spirvPath, std::string &error);
  [[nodiscard]] bool initialize(runtime::ComputeSession &session,
                                std::string_view spirvPath,
                                std::string &error);
  void reset();
  [[nodiscard]] bool isInitialized() const;

  [[nodiscard]] bool createFloatBuffer(std::size_t elementCount,
                                       runtime::HostVisibleBuffer &buffer,
                                       std::string &error);

  // Values are channel-major: values[channel * pointCount + point]. One GPU
  // invocation owns one complete channel and writes one FP32 result.
  [[nodiscard]] bool evaluate(runtime::HostVisibleBuffer &updated,
                               std::size_t updatedCount,
                               runtime::HostVisibleBuffer &previous,
                               std::size_t previousCount,
                               runtime::HostVisibleBuffer &output,
                               std::size_t channelCount,
                               std::size_t pointCount,
                               std::size_t outputCapacity,
                               std::string &error) const;

  // Device-resident variant. Host spans are validation mirrors for the
  // already-uploaded channel-major device buffers.
  [[nodiscard]] bool evaluateDevice(
      runtime::DeviceBuffer &updated, std::size_t updatedCount,
      runtime::DeviceBuffer &previous, std::size_t previousCount,
      runtime::DeviceBuffer &output, std::size_t channelCount,
      std::size_t pointCount, std::size_t outputCapacity,
      std::span<const float> updatedValues,
      std::span<const float> previousValues, std::string &error) const;

  [[nodiscard]] const runtime::VulkanDevice &device() const;

private:
  static constexpr std::size_t kWorkgroupSize = 256U;
  static constexpr VkBufferUsageFlags kFloatBufferUsage =
      VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
      VK_BUFFER_USAGE_TRANSFER_DST_BIT;
  static constexpr VkMemoryPropertyFlags kHostMemoryFlags =
      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT;

  [[nodiscard]] bool isReady(std::string &error) const;
  [[nodiscard]] bool setup(std::string_view spirvPath,
                           runtime::ComputeSession *externalSession,
                           std::string &error);

  struct PushConstants {
    std::uint32_t channelCount;
    std::uint32_t pointCount;
  };

  runtime::ComputeSession ownedSession_{};
  runtime::ComputeSession *session_ = nullptr;
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
