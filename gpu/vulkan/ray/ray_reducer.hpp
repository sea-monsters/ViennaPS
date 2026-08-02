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

namespace viennaps::vulkan::ray {

// The optional normal span is three FP32 values per ray.  A zero vector is
// accepted for rays without a surface normal; otherwise the vector must be a
// finite, approximately unit normal.
struct RayRecordSoA {
  std::span<const std::uint32_t> rayId;
  std::span<const std::uint32_t> surfaceId;
  std::span<const float> weight;
  std::span<const float> normal;
};

struct RayReduction {
  std::span<std::uint32_t> surfaceId;
  std::span<float> weight;
  std::size_t count = 0U;
};

// Canonical CPU oracle.  Records are totally ordered by (surfaceId, rayId,
// input index), then each surface is accumulated in that order with FP32 +=.
[[nodiscard]] bool reduceCpu(const RayRecordSoA &input,
                             std::uint32_t surfaceDomain, RayReduction &output,
                             std::string &error);

class DeterministicRayReducer {
public:
  DeterministicRayReducer() = default;
  ~DeterministicRayReducer();
  DeterministicRayReducer(const DeterministicRayReducer &) = delete;
  DeterministicRayReducer &operator=(const DeterministicRayReducer &) = delete;
  DeterministicRayReducer(DeterministicRayReducer &&) noexcept;
  DeterministicRayReducer &operator=(DeterministicRayReducer &&) noexcept;

  [[nodiscard]] bool initialize(std::string_view spirvPath, std::string &error);
  [[nodiscard]] bool initialize(runtime::ComputeSession &session,
                                std::string_view spirvPath, std::string &error);
  void reset();
  [[nodiscard]] bool isInitialized() const;

  [[nodiscard]] bool createRayIdBuffer(std::size_t count,
                                       runtime::HostVisibleBuffer &buffer,
                                       std::string &error);
  [[nodiscard]] bool createSurfaceIdBuffer(std::size_t count,
                                           runtime::HostVisibleBuffer &buffer,
                                           std::string &error);
  [[nodiscard]] bool createWeightBuffer(std::size_t count,
                                        runtime::HostVisibleBuffer &buffer,
                                        std::string &error);

  // All buffers must be host-visible, owned by this reducer's device, and
  // disjoint.  outputCount receives the number of unique surfaces only after
  // successful dispatch; output buffers are not modified on validation/failure.
  [[nodiscard]] bool reduce(runtime::HostVisibleBuffer &rayId,
                            runtime::HostVisibleBuffer &surfaceId,
                            runtime::HostVisibleBuffer &weight,
                            std::size_t rayCount, std::uint32_t surfaceDomain,
                            runtime::HostVisibleBuffer &outputSurfaceId,
                            runtime::HostVisibleBuffer &outputWeight,
                            std::size_t outputCapacity,
                            std::size_t &outputCount, std::string &error);

  [[nodiscard]] const runtime::VulkanDevice &device() const;

private:
  static constexpr std::size_t kWorkgroupSize = 1U;
  static constexpr VkBufferUsageFlags kUsage =
      VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
      VK_BUFFER_USAGE_TRANSFER_DST_BIT;
  static constexpr VkMemoryPropertyFlags kMemory =
      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT;

  [[nodiscard]] bool createBuffer(std::size_t count, std::size_t elementSize,
                                  runtime::HostVisibleBuffer &buffer,
                                  std::string &error);
  [[nodiscard]] bool ready(std::string &error) const;
  [[nodiscard]] bool setup(std::string_view spirvPath,
                           runtime::ComputeSession *externalSession,
                           std::string &error);

  runtime::ComputeSession ownedSession_{};
  runtime::ComputeSession *session_ = nullptr;
  runtime::ShaderModule shaderModule_{};
  runtime::DescriptorSetLayout descriptorSetLayout_{};
  runtime::PipelineLayout pipelineLayout_{};
  runtime::ComputePipeline pipeline_{};
  runtime::DescriptorPool descriptorPool_{};
  runtime::CommandContext commandContext_{};
  runtime::Fence fence_{};
  VkDescriptorSet descriptorSet_ = VK_NULL_HANDLE;
  VkCommandBuffer commandBuffer_ = VK_NULL_HANDLE;
};

} // namespace viennaps::vulkan::ray
