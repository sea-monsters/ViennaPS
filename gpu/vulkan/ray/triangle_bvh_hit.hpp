// Copyright 2026 ViennaPS
// SPDX-License-Identifier: MIT
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "../runtime/compute_session.hpp"
#include "triangle_hit.hpp"

namespace viennaps::vulkan::ray {

struct TriangleBvhNode {
  float minX{}, minY{}, minZ{};
  std::uint32_t leftFirst{};
  float maxX{}, maxY{}, maxZ{};
  std::uint32_t count{};
};
static_assert(sizeof(TriangleBvhNode) == 32U);

class TriangleBvhHitPrimitive {
public:
  TriangleBvhHitPrimitive() = default;
  ~TriangleBvhHitPrimitive();
  TriangleBvhHitPrimitive(const TriangleBvhHitPrimitive &) = delete;
  TriangleBvhHitPrimitive &operator=(const TriangleBvhHitPrimitive &) = delete;

  [[nodiscard]] bool initialize(std::string_view spirvPath, std::string &error);
  [[nodiscard]] bool initialize(runtime::ComputeSession &session,
                                std::string_view spirvPath, std::string &error);
  void reset();
  [[nodiscard]] bool isInitialized() const;
  [[nodiscard]] bool build(std::span<const Triangle> triangles,
                           std::string &error);
  [[nodiscard]] bool intersect(std::span<const Ray> rays,
                               std::span<TriangleHit> output,
                               std::string &error);
  // Records traversal into a caller-owned command buffer. Geometry must first
  // be built; this method neither changes command-buffer lifecycle nor moves
  // data between host and device.
  [[nodiscard]] bool
  recordDispatch(VkCommandBuffer commandBuffer, runtime::DeviceBuffer &origins,
                 runtime::DeviceBuffer &directions, std::size_t rayCount,
                 runtime::DeviceBuffer &hits, std::size_t outputCapacity,
                 std::string &error);
  [[nodiscard]] const runtime::VulkanDevice &device() const;

private:
  [[nodiscard]] bool setup(std::string_view path,
                           runtime::ComputeSession *external,
                           std::string &error);
  [[nodiscard]] bool ready(std::string &error) const;
  [[nodiscard]] bool createBytes(std::size_t bytes, runtime::DeviceBuffer &b,
                                 std::string &error) const;

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
  runtime::DeviceBuffer nodes_{}, triangles_{}, indices_{};
  std::size_t triangleCount_ = 0U;
  bool built_ = false;
};

using DeviceTriangleBvhHit = TriangleBvhHitPrimitive;

} // namespace viennaps::vulkan::ray
