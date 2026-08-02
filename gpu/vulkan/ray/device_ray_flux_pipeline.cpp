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
      (spirv.triangleBvh.empty()
           ? false
           : !triangleBvh_.initialize(*session_, spirv.triangleBvh, error)) ||
      !compactor_.initialize(*session_, spirv.recordCompaction,
                             spirv.reductionScan, error) ||
      !sorter_.initialize(*session_, spirv.radixHistogram, spirv.radixPrefix,
                          spirv.radixScatter, error) ||
      !reducer_.initialize(*session_, spirv.surfaceSegments,
                           spirv.surfaceReduce, spirv.reductionScan, error) ||
      !session_->commandContext().allocatePrimary(commandBuffer_, error) ||
      !fence_.create(session_->device(), error)) {
    reset();
    return false;
  }
  useTriangleBvh_ = !spirv.triangleBvh.empty();
  return true;
}

void DeviceRayFluxPipeline::reset() {
  const bool ownsSession = session_ == &ownedSession_;
  fence_.destroy();
  commandBuffer_ = VK_NULL_HANDLE;
  scanScratch_.reset();
  lastComputeSubmissionCount_ = 0U;
  reducer_.reset();
  sorter_.reset();
  compactor_.reset();
  triangleHit_.reset();
  triangleBvh_.reset();
  useTriangleBvh_ = false;
  preparedGeometry_ = false;
  reusePrepared_ = false;
  preparedTriangles_.clear();
  session_ = nullptr;
  if (ownsSession)
    ownedSession_.reset();
}

bool DeviceRayFluxPipeline::isInitialized() const {
  return session_ != nullptr && session_->isValid() &&
         triangleHit_.isInitialized() && compactor_.isInitialized() &&
         sorter_.isInitialized() && reducer_.isInitialized() &&
         commandBuffer_ != VK_NULL_HANDLE && fence_.get() != VK_NULL_HANDLE &&
         (!useTriangleBvh_ || triangleBvh_.isInitialized());
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

std::uint32_t DeviceRayFluxPipeline::lastComputeSubmissionCount() const {
  return lastComputeSubmissionCount_;
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
  lastComputeSubmissionCount_ = 0U;
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
  if (useTriangleBvh_ && !reusePrepared_ &&
      !triangleBvh_.build(triangles, error))
    return false;
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
      ((!useTriangleBvh_) && !triangleHit_.createTriangleBuffer(
                                 triangles.size(), triangleBuffer, error)) ||
      !triangleHit_.createHitBuffer(capacity, hits, error) ||
      !weightBuffer.create(*session_, static_cast<VkDeviceSize>(weightBytes),
                           error) ||
      !compacted.flags.create(*session_, static_cast<VkDeviceSize>(idBytes),
                              error) ||
      !compacted.offsets.create(*session_, static_cast<VkDeviceSize>(idBytes),
                                error) ||
      !compacted.count.create(*session_, sizeof(std::uint32_t), error) ||
      !compactor_.createRecordBuffer(capacity, compacted.records, error) ||
      !compactor_.createRecordBuffer(capacity, sortedRecords, error) ||
      !reduced.flags.create(*session_, static_cast<VkDeviceSize>(idBytes),
                            error) ||
      !reduced.offsets.create(*session_, static_cast<VkDeviceSize>(idBytes),
                              error) ||
      !reduced.count.create(*session_, sizeof(std::uint32_t), error) ||
      !reduced.status.create(*session_, sizeof(std::uint32_t), error) ||
      !outputSurface.create(*session_, static_cast<VkDeviceSize>(idBytes),
                            error) ||
      !outputWeight.create(*session_, static_cast<VkDeviceSize>(weightBytes),
                           error))
    return false;
  if (!triangleHit_.uploadRays(rays, origins, directions, error) ||
      ((!useTriangleBvh_) &&
       !triangleHit_.uploadTriangles(triangles, triangleBuffer, error)) ||
      !weightBuffer.upload(*session_, weights.data(),
                           static_cast<VkDeviceSize>(weightBytes), 0U, error) ||
      vkResetCommandBuffer(commandBuffer_, 0U) != VK_SUCCESS)
    return false;

  VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  if (vkBeginCommandBuffer(commandBuffer_, &begin) != VK_SUCCESS)
    return fail(error, "failed to begin device ray-flux command buffer");
  vkCmdFillBuffer(commandBuffer_, reduced.status.handle(), 0U,
                  sizeof(std::uint32_t), 0U);
  scanScratch_.reset();
  const auto recordFailure = [&]() {
    vkEndCommandBuffer(commandBuffer_);
    return false;
  };
  if ((useTriangleBvh_
           ? !triangleBvh_.recordDispatch(commandBuffer_, origins, directions,
                                          capacity, hits, capacity, error)
           : !triangleHit_.recordDispatch(
                 commandBuffer_, origins, directions, triangleBuffer, capacity,
                 triangles.size(), hits, capacity, error)) ||
      !compactor_.recordCompact(commandBuffer_, hits, weightBuffer, capacity,
                                static_cast<std::uint32_t>(triangles.size()),
                                capacity, compacted, scanScratch_, error) ||
      !sorter_.recordSort(commandBuffer_, compacted.records, compacted.count,
                          capacity, sortedRecords, capacity, error) ||
      !reducer_.recordReduce(commandBuffer_, sortedRecords, compacted.count,
                             capacity, outputSurface, outputWeight, capacity,
                             reduced, scanScratch_, error))
    return recordFailure();
  if (vkEndCommandBuffer(commandBuffer_) != VK_SUCCESS)
    return fail(error, "failed to end device ray-flux command buffer");
  VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
  submit.commandBufferCount = 1U;
  submit.pCommandBuffers = &commandBuffer_;
  if (vkQueueSubmit(session_->device().computeQueue(), 1U, &submit,
                    fence_.get()) != VK_SUCCESS)
    return fail(error, "failed to submit device ray-flux command buffer");
  lastComputeSubmissionCount_ = 1U;
  if (!fence_.wait(std::numeric_limits<std::uint64_t>::max(), error))
    return false;
  fence_.reset();

  std::uint32_t reductionStatus = 0U;
  if (!reduced.status.download(*session_, &reductionStatus,
                               sizeof(reductionStatus), 0U, error))
    return false;
  if (reductionStatus != 0U)
    return fail(error, "device ray-flux reduction left the strict FP32 domain");

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

bool DeviceRayFluxPipeline::prepareGeometry(std::span<const Triangle> triangles,
                                            std::string &error) {
  error.clear();
  if (!ready(error) || !useTriangleBvh_)
    return fail(error, "prepared geometry requires an initialized BVH path");
  preparedGeometry_ = false;
  preparedTriangles_.clear();
  if (triangles.empty())
    return fail(error, "prepared geometry cannot be empty");
  if (!triangleBvh_.build(triangles, error))
    return false;
  preparedTriangles_.assign(triangles.begin(), triangles.end());
  preparedGeometry_ = true;
  return true;
}

void DeviceRayFluxPipeline::resetPreparedGeometry() {
  preparedGeometry_ = false;
  preparedTriangles_.clear();
}

bool DeviceRayFluxPipeline::runGpuPrepared(std::span<const Ray> rays,
                                           std::span<const float> weights,
                                           RayFluxResult &output,
                                           std::string &error) {
  error.clear();
  if (!preparedGeometry_ || !useTriangleBvh_)
    return fail(error, "prepared geometry is unavailable");
  reusePrepared_ = true;
  const bool result = runGpu(rays, preparedTriangles_, weights, output, error);
  reusePrepared_ = false;
  return result;
}

} // namespace viennaps::vulkan::ray
