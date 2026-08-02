// Copyright 2026 ViennaPS
// SPDX-License-Identifier: MIT
#pragma once

#include <cstddef>
#include <span>
#include <string>
#include <string_view>

#include "../runtime/compute_session.hpp"
#include "triangle_hit.hpp"

namespace viennaps::vulkan::ray {

// Device-resident triangle intersection.  Inputs and output are caller-owned
// DeviceBuffers so subsequent device stages can consume the hit records
// without a host round trip.
class DeviceTriangleHitPrimitive {
public:
  DeviceTriangleHitPrimitive() = default;
  ~DeviceTriangleHitPrimitive();
  DeviceTriangleHitPrimitive(const DeviceTriangleHitPrimitive &) = delete;
  DeviceTriangleHitPrimitive &
  operator=(const DeviceTriangleHitPrimitive &) = delete;
  DeviceTriangleHitPrimitive(DeviceTriangleHitPrimitive &&) = delete;
  DeviceTriangleHitPrimitive &operator=(DeviceTriangleHitPrimitive &&) = delete;

  [[nodiscard]] bool initialize(std::string_view spirvPath, std::string &error);
  [[nodiscard]] bool initialize(runtime::ComputeSession &session,
                                std::string_view spirvPath, std::string &error);
  void reset();
  [[nodiscard]] bool isInitialized() const;
  [[nodiscard]] const runtime::VulkanDevice &device() const;

  [[nodiscard]] bool createRayBuffers(std::size_t count,
                                      runtime::DeviceBuffer &origins,
                                      runtime::DeviceBuffer &directions,
                                      std::string &error) const;
  [[nodiscard]] bool createTriangleBuffer(std::size_t count,
                                          runtime::DeviceBuffer &triangles,
                                          std::string &error) const;
  [[nodiscard]] bool createHitBuffer(std::size_t count,
                                     runtime::DeviceBuffer &hits,
                                     std::string &error) const;
  [[nodiscard]] bool uploadRays(std::span<const Ray> rays,
                                runtime::DeviceBuffer &origins,
                                runtime::DeviceBuffer &directions,
                                std::string &error) const;
  [[nodiscard]] bool uploadTriangles(std::span<const Triangle> triangles,
                                     runtime::DeviceBuffer &buffer,
                                     std::string &error) const;
  [[nodiscard]] bool downloadHits(std::size_t count,
                                  const runtime::DeviceBuffer &buffer,
                                  std::span<TriangleHit> output,
                                  std::string &error) const;

  [[nodiscard]] bool dispatch(runtime::DeviceBuffer &origins,
                              runtime::DeviceBuffer &directions,
                              runtime::DeviceBuffer &triangles,
                              std::size_t rayCount, std::size_t triangleCount,
                              runtime::DeviceBuffer &hits,
                              std::size_t outputCapacity, std::string &error);

  // Record the triangle-hit dispatch into a command buffer owned by the
  // caller. The command buffer must already be recording; this method never
  // resets, begins, ends, submits, waits, or performs host transfers.
  [[nodiscard]] bool
  recordDispatch(VkCommandBuffer commandBuffer, runtime::DeviceBuffer &origins,
                 runtime::DeviceBuffer &directions,
                 runtime::DeviceBuffer &triangles, std::size_t rayCount,
                 std::size_t triangleCount, runtime::DeviceBuffer &hits,
                 std::size_t outputCapacity, std::string &error);

private:
  [[nodiscard]] bool setup(std::string_view path,
                           runtime::ComputeSession *external,
                           std::string &error);
  [[nodiscard]] bool ready(std::string &error) const;
  [[nodiscard]] bool createBytes(std::size_t bytes,
                                 runtime::DeviceBuffer &buffer,
                                 std::string &error) const;
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

using DeviceTriangleHit = DeviceTriangleHitPrimitive;
} // namespace viennaps::vulkan::ray
