// Copyright 2026 ViennaPS
// SPDX-License-Identifier: MIT

#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

#include "../runtime/compute_session.hpp"
#include "../runtime/vulkan_compute_runtime.hpp"
#include "triangle_hit.hpp"

namespace viennaps::vulkan::ray {

struct RayHitBatch {
  std::span<std::uint32_t> rayId;
  std::span<std::uint32_t> surfaceId;
  std::span<float> weight;
  std::size_t count = 0U;
};

using RayHitBatchSoA = RayHitBatch;
using RayHitBatchOutput = RayHitBatch;

// Compact non-miss triangle hits in their source-ray order.  The output is
// published only after all input, capacity, and numeric checks succeed.
[[nodiscard]] bool compactCpu(std::span<const TriangleHit> hits,
                              std::span<const float> rayWeights,
                              std::uint32_t surfaceDomain, RayHitBatch &output,
                              std::string &error);

class RayHitBatchPrimitive {
public:
  RayHitBatchPrimitive() = default;
  ~RayHitBatchPrimitive();
  RayHitBatchPrimitive(const RayHitBatchPrimitive &) = delete;
  RayHitBatchPrimitive &operator=(const RayHitBatchPrimitive &) = delete;
  RayHitBatchPrimitive(RayHitBatchPrimitive &&other) noexcept;
  RayHitBatchPrimitive &operator=(RayHitBatchPrimitive &&other) noexcept;

  [[nodiscard]] bool initialize(std::string_view spirvPath, std::string &error);
  [[nodiscard]] bool initialize(runtime::ComputeSession &session,
                                std::string_view spirvPath, std::string &error);
  void reset();
  [[nodiscard]] bool isInitialized() const;

  [[nodiscard]] bool createHitBuffer(std::size_t count,
                                     runtime::HostVisibleBuffer &buffer,
                                     std::string &error);
  [[nodiscard]] bool createWeightBuffer(std::size_t count,
                                        runtime::HostVisibleBuffer &buffer,
                                        std::string &error);
  [[nodiscard]] bool createRayIdBuffer(std::size_t count,
                                       runtime::HostVisibleBuffer &buffer,
                                       std::string &error);
  [[nodiscard]] bool createSurfaceIdBuffer(std::size_t count,
                                           runtime::HostVisibleBuffer &buffer,
                                           std::string &error);

  [[nodiscard]] bool compact(runtime::HostVisibleBuffer &hits,
                             runtime::HostVisibleBuffer &rayWeights,
                             std::size_t rayCount, std::uint32_t surfaceDomain,
                             runtime::HostVisibleBuffer &outputRayId,
                             runtime::HostVisibleBuffer &outputSurfaceId,
                             runtime::HostVisibleBuffer &outputWeight,
                             std::size_t outputCapacity,
                             std::size_t &outputCount, std::string &error);

  // Descriptive alias for callers that name the operation as a batch.
  [[nodiscard]] bool batch(runtime::HostVisibleBuffer &hits,
                           runtime::HostVisibleBuffer &rayWeights,
                           std::size_t rayCount, std::uint32_t surfaceDomain,
                           runtime::HostVisibleBuffer &outputRayId,
                           runtime::HostVisibleBuffer &outputSurfaceId,
                           runtime::HostVisibleBuffer &outputWeight,
                           std::size_t outputCapacity, std::size_t &outputCount,
                           std::string &error) {
    return compact(hits, rayWeights, rayCount, surfaceDomain, outputRayId,
                   outputSurfaceId, outputWeight, outputCapacity, outputCount,
                   error);
  }

  [[nodiscard]] const runtime::VulkanDevice &device() const;

private:
  static constexpr VkBufferUsageFlags kUsage =
      VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
      VK_BUFFER_USAGE_TRANSFER_DST_BIT;
  static constexpr VkMemoryPropertyFlags kMemory =
      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT;

  [[nodiscard]] bool setup(std::string_view spirvPath,
                           runtime::ComputeSession *externalSession,
                           std::string &error);
  [[nodiscard]] bool ready(std::string &error) const;
  [[nodiscard]] bool createBuffer(std::size_t count, std::size_t elementSize,
                                  runtime::HostVisibleBuffer &buffer,
                                  std::string &error);

  runtime::ComputeSession ownedSession_{};
  runtime::ComputeSession *session_ = nullptr;
  runtime::ShaderModule shaderModule_{};
  runtime::DescriptorSetLayout descriptorSetLayout_{};
  runtime::PipelineLayout pipelineLayout_{};
  runtime::ComputePipeline pipeline_{};
  runtime::DescriptorPool descriptorPool_{};
  runtime::Fence fence_{};
  VkDescriptorSet descriptorSet_ = VK_NULL_HANDLE;
  VkCommandBuffer commandBuffer_ = VK_NULL_HANDLE;
};

} // namespace viennaps::vulkan::ray
