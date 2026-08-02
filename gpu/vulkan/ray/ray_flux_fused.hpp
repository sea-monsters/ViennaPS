// Copyright 2026 ViennaPS
// SPDX-License-Identifier: MIT

#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

#include "../runtime/compute_session.hpp"
#include "ray_flux_pipeline.hpp"

namespace viennaps::vulkan::ray {

struct RayFluxFusedResult {
  std::span<std::uint32_t> surfaceId;
  std::span<float> weight;
  std::size_t count = 0U;
};

using RayFluxFusedOutput = RayFluxFusedResult;

// One-dispatch, device-resident D->F->C baseline.  The implementation is
// intentionally O(rays*triangles*rays): it is a correctness baseline, not a
// scalable production transport primitive.
class FusedRayFluxPrimitive {
public:
  FusedRayFluxPrimitive() = default;
  ~FusedRayFluxPrimitive();
  FusedRayFluxPrimitive(const FusedRayFluxPrimitive &) = delete;
  FusedRayFluxPrimitive &operator=(const FusedRayFluxPrimitive &) = delete;
  FusedRayFluxPrimitive(FusedRayFluxPrimitive &&) = delete;
  FusedRayFluxPrimitive &operator=(FusedRayFluxPrimitive &&) = delete;

  [[nodiscard]] bool initialize(std::string_view spirvPath, std::string &error);
  [[nodiscard]] bool initialize(runtime::ComputeSession &session,
                                std::string_view spirvPath, std::string &error);
  void reset();
  [[nodiscard]] bool isInitialized() const;
  [[nodiscard]] const runtime::VulkanDevice &device() const;

  [[nodiscard]] bool runCpu(std::span<const Ray> rays,
                            std::span<const Triangle> triangles,
                            std::span<const float> weights,
                            RayFluxFusedResult &output,
                            std::string &error) const;
  [[nodiscard]] bool runGpu(std::span<const Ray> rays,
                            std::span<const Triangle> triangles,
                            std::span<const float> weights,
                            RayFluxFusedResult &output, std::string &error);

private:
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

using RayFluxFused = FusedRayFluxPrimitive;

} // namespace viennaps::vulkan::ray
