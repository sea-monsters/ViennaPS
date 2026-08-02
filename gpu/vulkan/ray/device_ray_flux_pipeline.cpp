// Copyright 2026 ViennaPS
// SPDX-License-Identifier: MIT

#include "device_ray_flux_pipeline.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <vector>

namespace viennaps::vulkan::ray {
namespace {

bool fail(std::string &error, const char *message) {
  error = message;
  return false;
}

bool strictFp32(float value) {
  return std::isfinite(value) && (value == 0.0F || std::isnormal(value));
}

bool validOutput(const RayFluxResult &output) {
  return output.surfaceId.size() == output.weight.size() &&
         (output.surfaceId.empty() || (output.surfaceId.data() != nullptr &&
                                       output.weight.data() != nullptr));
}

bool validateInputs(std::span<const Ray> rays,
                    std::span<const Triangle> triangles,
                    std::span<const float> weights, std::string &error) {
  if (rays.size() != weights.size())
    return fail(error, "rays and weights have different lengths");
  if (rays.size() > std::numeric_limits<std::uint32_t>::max() ||
      triangles.size() > std::numeric_limits<std::uint32_t>::max())
    return fail(error, "ray or triangle count exceeds uint32 index range");
  if ((!rays.empty() && rays.data() == nullptr) ||
      (!triangles.empty() && triangles.data() == nullptr) ||
      (!weights.empty() && weights.data() == nullptr))
    return fail(error, "ray, triangle, or weight span has a null pointer");
  for (const auto &ray : rays) {
    for (const float value : ray.origin)
      if (!strictFp32(value))
        return fail(error, "ray origin contains a non-normal FP32 value");
    for (const float value : ray.direction)
      if (!strictFp32(value))
        return fail(error, "ray direction contains a non-normal FP32 value");
    if (!strictFp32(ray.tMin) || !strictFp32(ray.tMax) || ray.tMin < 0.0F ||
        ray.tMax < ray.tMin)
      return fail(error, "ray near/far limits are outside the FP32 domain");
  }
  for (const auto &triangle : triangles)
    for (const auto vertex : {triangle.a, triangle.b, triangle.c})
      for (const float value : vertex)
        if (!strictFp32(value))
          return fail(error,
                      "triangle vertex contains a non-normal FP32 value");
  for (const float value : weights)
    if (!strictFp32(value))
      return fail(error,
                  "ray weights must be finite normal FP32 values or zero");
  return true;
}

bool bytesFor(std::size_t count, std::size_t elementBytes, std::size_t &bytes) {
  if (elementBytes != 0U &&
      count > std::numeric_limits<std::size_t>::max() / elementBytes)
    return false;
  bytes = count * elementBytes;
  return true;
}

} // namespace

DeviceRayFluxPipeline::~DeviceRayFluxPipeline() { reset(); }

bool DeviceRayFluxPipeline::initialize(const DeviceRayFluxSpirv &spirv,
                                       std::string &error) {
  error.clear();
  return setup(nullptr, spirv, error);
}

bool DeviceRayFluxPipeline::initialize(runtime::ComputeSession &session,
                                       const DeviceRayFluxSpirv &spirv,
                                       std::string &error) {
  error.clear();
  return setup(&session, spirv, error);
}

bool DeviceRayFluxPipeline::setup(runtime::ComputeSession *external,
                                  const DeviceRayFluxSpirv &spirv,
                                  std::string &error) {
  reset();
  if (external != nullptr) {
    if (!external->isValid())
      return fail(error, "external compute session is not initialized");
    session_ = external;
  } else {
    if (!ownedSession_.initialize(error))
      return false;
    session_ = &ownedSession_;
  }
  if (!triangleHit_.initialize(*session_, spirv.triangleHit, error) ||
      !compactor_.initialize(*session_, spirv.recordCompaction,
                             spirv.reductionScan, error) ||
      !sorter_.initialize(*session_, spirv.radixHistogram, spirv.radixPrefix,
                          spirv.radixScatter, error) ||
      !reducer_.initialize(*session_, spirv.surfaceSegments,
                           spirv.surfaceReduce, spirv.reductionScan, error)) {
    reset();
    return false;
  }
  return true;
}

void DeviceRayFluxPipeline::reset() {
  const bool ownsSession = session_ == &ownedSession_;
  reducer_.reset();
  sorter_.reset();
  compactor_.reset();
  triangleHit_.reset();
  session_ = nullptr;
  if (ownsSession)
    ownedSession_.reset();
}

bool DeviceRayFluxPipeline::isInitialized() const {
  return session_ != nullptr && session_->isValid() &&
         triangleHit_.isInitialized() && compactor_.isInitialized() &&
         sorter_.isInitialized() && reducer_.isInitialized();
}

const runtime::VulkanDevice &DeviceRayFluxPipeline::device() const {
  static const runtime::VulkanDevice empty{};
  return session_ == nullptr ? empty : session_->device();
}

bool DeviceRayFluxPipeline::ready(std::string &error) const {
  return isInitialized()
             ? true
             : fail(error, "device ray-flux pipeline is not initialized");
}

bool DeviceRayFluxPipeline::runCpu(std::span<const Ray> rays,
                                   std::span<const Triangle> triangles,
                                   std::span<const float> weights,
                                   RayFluxResult &output,
                                   std::string &error) const {
  RayFluxPipeline cpu;
  return cpu.runCpu(rays, triangles, weights, output, error);
}

bool DeviceRayFluxPipeline::runGpu(std::span<const Ray> rays,
                                   std::span<const Triangle> triangles,
                                   std::span<const float> weights,
                                   RayFluxResult &output, std::string &error) {
  error.clear();
  if (!ready(error))
    return false;
  if (!validOutput(output) ||
      !validateInputs(rays, triangles, weights, error)) {
    if (error.empty())
      error = "ray-flux output columns have different lengths";
    return false;
  }
  if (rays.empty())
    return true;
  if (triangles.empty()) {
    output.count = 0U;
    return true;
  }
  if (output.surfaceId.size() < rays.size())
    return fail(error,
                "device ray-flux output capacity must cover every input ray");

  const auto capacity = rays.size();
  std::size_t weightBytes{}, idBytes{};
  if (!bytesFor(capacity, sizeof(float), weightBytes) ||
      !bytesFor(capacity, sizeof(std::uint32_t), idBytes))
    return fail(error, "device ray-flux buffer size overflow");

  runtime::DeviceBuffer origins, directions, triangleBuffer, hits, weightBuffer,
      sortedRecords, outputSurface, outputWeight;
  RayRecordCompactionDeviceOutput compacted{};
  DeviceRaySurfaceReductionOutput reduced{};
  if (!triangleHit_.createRayBuffers(capacity, origins, directions, error) ||
      !triangleHit_.createTriangleBuffer(triangles.size(), triangleBuffer,
                                         error) ||
      !triangleHit_.createHitBuffer(capacity, hits, error) ||
      !weightBuffer.create(*session_, static_cast<VkDeviceSize>(weightBytes),
                           error) ||
      !compactor_.createRecordBuffer(capacity, compacted.records, error) ||
      !compactor_.createRecordBuffer(capacity, sortedRecords, error) ||
      !outputSurface.create(*session_, static_cast<VkDeviceSize>(idBytes),
                            error) ||
      !outputWeight.create(*session_, static_cast<VkDeviceSize>(weightBytes),
                           error))
    return false;
  if (!triangleHit_.uploadRays(rays, origins, directions, error) ||
      !triangleHit_.uploadTriangles(triangles, triangleBuffer, error) ||
      !weightBuffer.upload(*session_, weights.data(),
                           static_cast<VkDeviceSize>(weightBytes), 0U, error) ||
      !triangleHit_.dispatch(origins, directions, triangleBuffer, capacity,
                             triangles.size(), hits, capacity, error) ||
      !compactor_.compact(hits, weightBuffer, capacity,
                          static_cast<std::uint32_t>(triangles.size()),
                          capacity, compacted, error) ||
      !sorter_.sort(compacted.records, compacted.count, capacity, sortedRecords,
                    capacity, error) ||
      !reducer_.reduce(sortedRecords, compacted.count, capacity, outputSurface,
                       outputWeight, capacity, reduced, error))
    return false;

  std::uint32_t resultCount = 0U;
  if (!reduced.count.download(*session_, &resultCount, sizeof(resultCount), 0U,
                              error))
    return false;
  if (resultCount > capacity)
    return fail(error, "device ray-flux result count exceeds input capacity");

  std::vector<std::uint32_t> stagedSurface(resultCount);
  std::vector<float> stagedWeight(resultCount);
  if (resultCount != 0U &&
      (!outputSurface.download(*session_, stagedSurface.data(),
                               resultCount * sizeof(std::uint32_t), 0U,
                               error) ||
       !outputWeight.download(*session_, stagedWeight.data(),
                              resultCount * sizeof(float), 0U, error)))
    return false;
  std::copy(stagedSurface.begin(), stagedSurface.end(),
            output.surfaceId.begin());
  std::copy(stagedWeight.begin(), stagedWeight.end(), output.weight.begin());
  output.count = resultCount;
  return true;
}

} // namespace viennaps::vulkan::ray
