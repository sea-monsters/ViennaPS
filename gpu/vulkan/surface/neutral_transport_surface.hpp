// Copyright 2026 ViennaPS
// SPDX-License-Identifier: MIT
//
// Vulkan FP32 elementwise surface-model kernel for the neutral transport
// etch-front velocity formula.

#pragma once

#include <cstddef>
#include <string>
#include <string_view>

#include "../runtime/compute_session.hpp"
#include "../runtime/vulkan_compute_runtime.hpp"

namespace viennaps::vulkan::surface {

struct NeutralTransportSurfaceParamsFp32 {
  float kEtch = 0.0F;
  float surfaceSiteDensity = 1.66e-5F;
  float siliconDensity = 8.3e4F;
  float timeToSecond = 1.0F;
  float lengthToMeter = 1.0F;
  std::uint32_t etchFrontMaterialId = 10U;
};

class NeutralTransportSurfaceModelFp32 {
public:
  NeutralTransportSurfaceModelFp32() = default;
  ~NeutralTransportSurfaceModelFp32();

  NeutralTransportSurfaceModelFp32(const NeutralTransportSurfaceModelFp32 &) =
      delete;
  NeutralTransportSurfaceModelFp32 &
  operator=(const NeutralTransportSurfaceModelFp32 &) = delete;
  NeutralTransportSurfaceModelFp32(NeutralTransportSurfaceModelFp32 &&) =
      delete;
  NeutralTransportSurfaceModelFp32 &
  operator=(NeutralTransportSurfaceModelFp32 &&) = delete;

  [[nodiscard]] bool initialize(std::string_view spirvPath, std::string &error);
  void reset();
  [[nodiscard]] bool isInitialized() const;

  [[nodiscard]] bool
  createFloatBuffer(std::size_t elementCount,
                    viennaps::vulkan::runtime::HostVisibleBuffer &buffer,
                    std::string &error);

  [[nodiscard]] bool
  evaluate(viennaps::vulkan::runtime::HostVisibleBuffer &coverage,
           std::size_t coverageElementCount,
           viennaps::vulkan::runtime::HostVisibleBuffer &materialIds,
           std::size_t materialElementCount,
           viennaps::vulkan::runtime::HostVisibleBuffer &velocity,
           std::size_t velocityElementCount,
           const NeutralTransportSurfaceParamsFp32 &params,
           std::string &error) const;

  [[nodiscard]] const viennaps::vulkan::runtime::VulkanDevice &device() const;

  [[nodiscard]] static float
  cpuVelocity(float coverage, float materialId,
              const NeutralTransportSurfaceParamsFp32 &params);

private:
  static constexpr std::size_t kWorkgroupSize = 256U;
  static constexpr VkBufferUsageFlags kFloatBufferUsage =
      VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
      VK_BUFFER_USAGE_TRANSFER_DST_BIT;
  static constexpr VkMemoryPropertyFlags kFloatMemoryFlags =
      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT;

  [[nodiscard]] bool isReady(std::string &error) const;

  struct PushConstants {
    std::uint32_t elementCount;
    std::uint32_t etchFrontMaterialId;
    float kEtch;
    float surfaceSiteDensity;
    float siliconDensity;
    float timeToSecond;
    float lengthToMeter;
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
