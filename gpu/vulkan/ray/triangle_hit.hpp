// Copyright 2026 ViennaPS
// SPDX-License-Identifier: MIT

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <string>
#include <string_view>

#include <vulkan/vulkan.h>

#include "../runtime/compute_session.hpp"
#include "../runtime/vulkan_compute_runtime.hpp"

namespace viennaps::vulkan::ray {

struct Ray {
  std::array<float, 3> origin{};
  std::array<float, 3> direction{};
  float tMin = 0.0F;
  float tMax = std::numeric_limits<float>::max();
};

struct Triangle {
  std::array<float, 3> a{};
  std::array<float, 3> b{};
  std::array<float, 3> c{};
};

struct TriangleHit {
  float t = std::numeric_limits<float>::max();
  std::uint32_t triangleIndex = std::numeric_limits<std::uint32_t>::max();
  float u = 0.0F;
  float v = 0.0F;

  [[nodiscard]] static constexpr TriangleHit miss() noexcept { return {}; }
  [[nodiscard]] constexpr bool isMiss() const noexcept {
    return triangleIndex == std::numeric_limits<std::uint32_t>::max();
  }
};
static_assert(sizeof(TriangleHit) == 16U,
              "TriangleHit must match the std430 shader hit record");

// The CPU oracle uses a fixed FP32 Moller-Trumbore calculation.  The output
// span must have at least one slot per ray.  On validation failure, output is
// left byte-for-byte unchanged.
[[nodiscard]] bool intersectCpu(std::span<const Ray> rays,
                                std::span<const Triangle> triangles,
                                std::span<TriangleHit> output,
                                std::string &error);

class TriangleHitPrimitive {
public:
  TriangleHitPrimitive() = default;
  ~TriangleHitPrimitive();
  TriangleHitPrimitive(const TriangleHitPrimitive &) = delete;
  TriangleHitPrimitive &operator=(const TriangleHitPrimitive &) = delete;
  TriangleHitPrimitive(TriangleHitPrimitive &&other) noexcept;
  TriangleHitPrimitive &operator=(TriangleHitPrimitive &&other) noexcept;

  [[nodiscard]] bool initialize(std::string_view spirvPath, std::string &error);
  [[nodiscard]] bool initialize(runtime::ComputeSession &session,
                                std::string_view spirvPath, std::string &error);
  void reset();
  [[nodiscard]] bool isInitialized() const;

  [[nodiscard]] bool createRayBuffer(std::size_t count,
                                     runtime::HostVisibleBuffer &origin,
                                     runtime::HostVisibleBuffer &direction,
                                     std::string &error);
  [[nodiscard]] bool createTriangleBuffer(std::size_t count,
                                          runtime::HostVisibleBuffer &buffer,
                                          std::string &error);
  [[nodiscard]] bool createHitBuffer(std::size_t count,
                                     runtime::HostVisibleBuffer &buffer,
                                     std::string &error);
  [[nodiscard]] bool intersect(runtime::HostVisibleBuffer &origin,
                               runtime::HostVisibleBuffer &direction,
                               runtime::HostVisibleBuffer &triangles,
                               std::size_t rayCount, std::size_t triangleCount,
                               runtime::HostVisibleBuffer &hits,
                               std::size_t outputCapacity, std::string &error);
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
  runtime::CommandContext commandContext_{};
  runtime::Fence fence_{};
  VkDescriptorSet descriptorSet_ = VK_NULL_HANDLE;
  VkCommandBuffer commandBuffer_ = VK_NULL_HANDLE;
};

} // namespace viennaps::vulkan::ray
