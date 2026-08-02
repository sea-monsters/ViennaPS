// Copyright 2026 ViennaPS
// SPDX-License-Identifier: MIT

#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

#include "../runtime/compute_session.hpp"
#include "ray_hit_batch.hpp"
#include "ray_reducer.hpp"
#include "triangle_hit.hpp"

namespace viennaps::vulkan::ray {

struct RayFluxResult {
  std::span<std::uint32_t> surfaceId;
  std::span<float> weight;
  std::size_t count = 0U;
};

using RayFluxOutput = RayFluxResult;

// Staged ray-flux composition: intersect -> compact -> reduce.  The GPU path
// uses one shared ComputeSession, but each accepted primitive may stage through
// host-visible buffers internally; this type does not claim residency.
class RayFluxPipeline {
public:
  RayFluxPipeline() = default;
  ~RayFluxPipeline();
  RayFluxPipeline(const RayFluxPipeline &) = delete;
  RayFluxPipeline &operator=(const RayFluxPipeline &) = delete;
  // The three primitives retain pointers to this pipeline's shared session;
  // moving an initialized aggregate would invalidate those external-session
  // bindings.
  RayFluxPipeline(RayFluxPipeline &&) = delete;
  RayFluxPipeline &operator=(RayFluxPipeline &&) = delete;

  [[nodiscard]] bool initialize(std::string_view triangleHitSpirv,
                                std::string_view rayHitBatchSpirv,
                                std::string_view reducerSpirv,
                                std::string &error);
  [[nodiscard]] bool initialize(runtime::ComputeSession &session,
                                std::string_view triangleHitSpirv,
                                std::string_view rayHitBatchSpirv,
                                std::string_view reducerSpirv,
                                std::string &error);
  void reset();
  [[nodiscard]] bool isInitialized() const;
  [[nodiscard]] const runtime::VulkanDevice &device() const;

  [[nodiscard]] bool runCpu(std::span<const Ray> rays,
                            std::span<const Triangle> triangles,
                            std::span<const float> weights,
                            RayFluxResult &output, std::string &error) const;

  [[nodiscard]] bool runGpu(std::span<const Ray> rays,
                            std::span<const Triangle> triangles,
                            std::span<const float> weights,
                            RayFluxResult &output, std::string &error);

private:
  runtime::ComputeSession ownedSession_{};
  runtime::ComputeSession *session_ = nullptr;
  TriangleHitPrimitive triangleHit_{};
  RayHitBatchPrimitive rayHitBatch_{};
  DeterministicRayReducer reducer_{};
};

} // namespace viennaps::vulkan::ray
