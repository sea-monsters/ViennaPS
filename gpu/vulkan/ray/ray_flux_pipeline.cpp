// Copyright 2026 ViennaPS
// SPDX-License-Identifier: MIT

#include "ray_flux_pipeline.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <utility>
#include <vector>

namespace viennaps::vulkan::ray {
namespace {

bool fail(std::string &error, const char *message) {
  error = message;
  return false;
}

bool strictFp32(const float value) {
  return std::isfinite(value) && (value == 0.0F || std::isnormal(value));
}

bool validOutput(const RayFluxResult &output) {
  return output.surfaceId.size() == output.weight.size() &&
         (output.surfaceId.empty() || (output.surfaceId.data() != nullptr &&
                                       output.weight.data() != nullptr));
}

bool validateInputs(const std::span<const Ray> rays,
                    const std::span<const Triangle> triangles,
                    const std::span<const float> weights, std::string &error) {
  if (rays.size() != weights.size()) {
    return fail(error, "rays and weights have different lengths");
  }
  if (rays.size() > std::numeric_limits<std::uint32_t>::max() ||
      triangles.size() > std::numeric_limits<std::uint32_t>::max()) {
    return fail(error, "ray or triangle count exceeds uint32 index range");
  }
  if ((!rays.empty() && rays.data() == nullptr) ||
      (!triangles.empty() && triangles.data() == nullptr) ||
      (!weights.empty() && weights.data() == nullptr)) {
    return fail(error, "ray, triangle, or weight span has a null pointer");
  }
  for (const auto &ray : rays) {
    for (const float value : ray.origin) {
      if (!strictFp32(value)) {
        return fail(error, "ray origin contains a non-normal FP32 value");
      }
    }
    for (const float value : ray.direction) {
      if (!strictFp32(value)) {
        return fail(error, "ray direction contains a non-normal FP32 value");
      }
    }
    if (!strictFp32(ray.tMin) || !strictFp32(ray.tMax) || ray.tMin < 0.0F ||
        ray.tMax < ray.tMin) {
      return fail(error, "ray near/far limits are outside the FP32 domain");
    }
  }
  for (const auto &triangle : triangles) {
    for (const auto vertex : {triangle.a, triangle.b, triangle.c}) {
      for (const float value : vertex) {
        if (!strictFp32(value)) {
          return fail(error,
                      "triangle vertex contains a non-normal FP32 value");
        }
      }
    }
  }
  for (const float value : weights) {
    if (!strictFp32(value)) {
      return fail(error,
                  "ray weights must be finite normal FP32 values or zero");
    }
  }
  return true;
}

} // namespace

RayFluxPipeline::~RayFluxPipeline() { reset(); }

bool RayFluxPipeline::initialize(const std::string_view triangleHitSpirv,
                                 const std::string_view rayHitBatchSpirv,
                                 const std::string_view reducerSpirv,
                                 std::string &error) {
  error.clear();
  reset();
  if (!ownedSession_.initialize(error)) {
    return false;
  }
  session_ = &ownedSession_;
  if (!triangleHit_.initialize(*session_, triangleHitSpirv, error) ||
      !rayHitBatch_.initialize(*session_, rayHitBatchSpirv, error) ||
      !reducer_.initialize(*session_, reducerSpirv, error)) {
    reset();
    return false;
  }
  return true;
}

bool RayFluxPipeline::initialize(runtime::ComputeSession &session,
                                 const std::string_view triangleHitSpirv,
                                 const std::string_view rayHitBatchSpirv,
                                 const std::string_view reducerSpirv,
                                 std::string &error) {
  error.clear();
  reset();
  if (!session.isValid()) {
    return fail(error, "external compute session is not initialized");
  }
  session_ = &session;
  if (!triangleHit_.initialize(session, triangleHitSpirv, error) ||
      !rayHitBatch_.initialize(session, rayHitBatchSpirv, error) ||
      !reducer_.initialize(session, reducerSpirv, error)) {
    reset();
    return false;
  }
  return true;
}

void RayFluxPipeline::reset() {
  const bool ownsSession = session_ == &ownedSession_;
  reducer_.reset();
  rayHitBatch_.reset();
  triangleHit_.reset();
  session_ = nullptr;
  if (ownsSession) {
    ownedSession_.reset();
  }
}

bool RayFluxPipeline::isInitialized() const {
  return session_ != nullptr && session_->isValid() &&
         triangleHit_.isInitialized() && rayHitBatch_.isInitialized() &&
         reducer_.isInitialized();
}

const runtime::VulkanDevice &RayFluxPipeline::device() const {
  static const runtime::VulkanDevice empty{};
  return session_ == nullptr ? empty : session_->device();
}

bool RayFluxPipeline::runCpu(const std::span<const Ray> rays,
                             const std::span<const Triangle> triangles,
                             const std::span<const float> weights,
                             RayFluxResult &output, std::string &error) const {
  error.clear();
  if (!validOutput(output) ||
      !validateInputs(rays, triangles, weights, error)) {
    if (error.empty()) {
      error = "ray-flux output columns have different lengths";
    }
    return false;
  }
  if (rays.empty()) {
    return true;
  }

  std::vector<TriangleHit> hits(rays.size(), TriangleHit::miss());
  if (!intersectCpu(rays, triangles, hits, error)) {
    return false;
  }
  std::vector<std::uint32_t> rayIds(rays.size());
  std::vector<std::uint32_t> surfaceIds(rays.size());
  std::vector<float> compactWeights(rays.size());
  RayHitBatch batch{rayIds, surfaceIds, compactWeights, 0U};
  if (!compactCpu(hits, weights, static_cast<std::uint32_t>(triangles.size()),
                  batch, error)) {
    return false;
  }
  RayRecordSoA records{
      std::span<const std::uint32_t>(rayIds.data(), batch.count),
      std::span<const std::uint32_t>(surfaceIds.data(), batch.count),
      std::span<const float>(compactWeights.data(), batch.count),
      {}};
  std::vector<std::uint32_t> stagedSurface(output.surfaceId.size());
  std::vector<float> stagedWeight(output.weight.size());
  RayReduction reduction{stagedSurface, stagedWeight, output.count};
  if (!reduceCpu(records, static_cast<std::uint32_t>(triangles.size()),
                 reduction, error)) {
    return false;
  }
  std::copy_n(stagedSurface.data(), reduction.count, output.surfaceId.data());
  std::copy_n(stagedWeight.data(), reduction.count, output.weight.data());
  output.count = reduction.count;
  return true;
}

bool RayFluxPipeline::runGpu(const std::span<const Ray> rays,
                             const std::span<const Triangle> triangles,
                             const std::span<const float> weights,
                             RayFluxResult &output, std::string &error) {
  error.clear();
  if (!isInitialized()) {
    return fail(error, "ray-flux pipeline is not initialized");
  }
  if (!validOutput(output) ||
      !validateInputs(rays, triangles, weights, error)) {
    if (error.empty()) {
      error = "ray-flux output columns have different lengths";
    }
    return false;
  }
  if (rays.empty()) {
    return true;
  }
  if (output.surfaceId.size() >= std::numeric_limits<std::uint32_t>::max()) {
    return fail(error, "ray-flux output capacity exceeds uint32 range");
  }

  runtime::HostVisibleBuffer origins;
  runtime::HostVisibleBuffer directions;
  runtime::HostVisibleBuffer trianglesBuffer;
  runtime::HostVisibleBuffer hits;
  runtime::HostVisibleBuffer weightsBuffer;
  runtime::HostVisibleBuffer rayIds;
  runtime::HostVisibleBuffer surfaceIds;
  runtime::HostVisibleBuffer compactWeights;
  runtime::HostVisibleBuffer finalSurface;
  runtime::HostVisibleBuffer finalWeight;
  if (!triangleHit_.createRayBuffer(rays.size(), origins, directions, error) ||
      !triangleHit_.createTriangleBuffer(triangles.size(), trianglesBuffer,
                                         error) ||
      !triangleHit_.createHitBuffer(rays.size(), hits, error) ||
      !rayHitBatch_.createWeightBuffer(weights.size(), weightsBuffer, error) ||
      !rayHitBatch_.createRayIdBuffer(rays.size(), rayIds, error) ||
      !rayHitBatch_.createSurfaceIdBuffer(rays.size(), surfaceIds, error) ||
      !rayHitBatch_.createWeightBuffer(rays.size(), compactWeights, error) ||
      !reducer_.createSurfaceIdBuffer(output.surfaceId.size(), finalSurface,
                                      error) ||
      !reducer_.createWeightBuffer(output.weight.size(), finalWeight, error)) {
    return false;
  }

  std::vector<std::array<float, 4>> packedOrigins;
  std::vector<std::array<float, 4>> packedDirections;
  std::vector<std::array<float, 4>> packedTriangles;
  packedOrigins.reserve(rays.size());
  packedDirections.reserve(rays.size());
  packedTriangles.reserve(triangles.size() * 3U);
  for (const auto &ray : rays) {
    packedOrigins.push_back(
        {ray.origin[0], ray.origin[1], ray.origin[2], ray.tMin});
    packedDirections.push_back(
        {ray.direction[0], ray.direction[1], ray.direction[2], ray.tMax});
  }
  for (const auto &triangle : triangles) {
    packedTriangles.push_back(
        {triangle.a[0], triangle.a[1], triangle.a[2], 0.0F});
    packedTriangles.push_back(
        {triangle.b[0], triangle.b[1], triangle.b[2], 0.0F});
    packedTriangles.push_back(
        {triangle.c[0], triangle.c[1], triangle.c[2], 0.0F});
  }
  if (!origins.write(packedOrigins.data(),
                     packedOrigins.size() * sizeof(packedOrigins[0]), 0U,
                     error) ||
      !directions.write(packedDirections.data(),
                        packedDirections.size() * sizeof(packedDirections[0]),
                        0U, error) ||
      !trianglesBuffer.write(
          packedTriangles.data(),
          packedTriangles.size() * sizeof(packedTriangles[0]), 0U, error) ||
      !weightsBuffer.write(weights.data(), weights.size() * sizeof(float), 0U,
                           error)) {
    return false;
  }
  if (!triangleHit_.intersect(origins, directions, trianglesBuffer, rays.size(),
                              triangles.size(), hits, rays.size(), error)) {
    return false;
  }
  std::size_t compactCount = 0U;
  if (!rayHitBatch_.compact(hits, weightsBuffer, rays.size(),
                            static_cast<std::uint32_t>(triangles.size()),
                            rayIds, surfaceIds, compactWeights, rays.size(),
                            compactCount, error)) {
    return false;
  }
  std::size_t reducedCount = 0U;
  if (!reducer_.reduce(rayIds, surfaceIds, compactWeights, compactCount,
                       static_cast<std::uint32_t>(triangles.size()),
                       finalSurface, finalWeight, output.surfaceId.size(),
                       reducedCount, error)) {
    return false;
  }
  std::vector<std::uint32_t> stagedSurface(reducedCount);
  std::vector<float> stagedWeight(reducedCount);
  if (reducedCount != 0U &&
      (!finalSurface.read(stagedSurface.data(),
                          reducedCount * sizeof(std::uint32_t), 0U, error) ||
       !finalWeight.read(stagedWeight.data(), reducedCount * sizeof(float), 0U,
                         error))) {
    return false;
  }
  std::copy(stagedSurface.begin(), stagedSurface.end(),
            output.surfaceId.begin());
  std::copy(stagedWeight.begin(), stagedWeight.end(), output.weight.begin());
  output.count = reducedCount;
  return true;
}

} // namespace viennaps::vulkan::ray
