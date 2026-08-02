// Copyright 2026 ViennaPS
// SPDX-License-Identifier: MIT

#include "ray_hit_batch.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstring>
#include <limits>
#include <utility>
#include <vector>

namespace {

using viennaps::vulkan::ray::RayHitBatch;
using viennaps::vulkan::ray::TriangleHit;

[[nodiscard]] bool fail(std::string &error, const char *message) {
  error = message;
  return false;
}

[[nodiscard]] bool strictFp32(const float value) {
  return std::isfinite(value) && (value == 0.0F || std::isnormal(value));
}

[[nodiscard]] bool overlaps(const void *left, const std::size_t leftBytes,
                            const void *right, const std::size_t rightBytes) {
  if (leftBytes == 0U || rightBytes == 0U || left == nullptr ||
      right == nullptr) {
    return false;
  }
  const auto leftBegin = reinterpret_cast<std::uintptr_t>(left);
  const auto rightBegin = reinterpret_cast<std::uintptr_t>(right);
  return leftBegin < rightBegin + rightBytes &&
         rightBegin < leftBegin + leftBytes;
}

[[nodiscard]] bool validHit(const TriangleHit &hit,
                            const std::uint32_t surfaceDomain) {
  if (hit.isMiss()) {
    return hit.t == std::numeric_limits<float>::max() && hit.u == 0.0F &&
           hit.v == 0.0F;
  }
  return hit.triangleIndex < surfaceDomain && strictFp32(hit.t) &&
         strictFp32(hit.u) && strictFp32(hit.v) && hit.u >= 0.0F &&
         hit.u <= 1.0F && hit.v >= 0.0F && hit.v <= 1.0F &&
         hit.u + hit.v <= 1.0F;
}

[[nodiscard]] bool validateOutput(const RayHitBatch &output) {
  return output.rayId.size() == output.surfaceId.size() &&
         output.rayId.size() == output.weight.size();
}

} // namespace

namespace viennaps::vulkan::ray {

bool compactCpu(const std::span<const TriangleHit> hits,
                const std::span<const float> rayWeights,
                const std::uint32_t surfaceDomain, RayHitBatch &output,
                std::string &error) {
  error.clear();
  const auto count = hits.size();
  if (count != rayWeights.size()) {
    return fail(error, "triangle hits and ray weights have different lengths");
  }
  if (count != 0U && surfaceDomain == 0U) {
    return fail(error, "surface domain must be nonzero for non-empty input");
  }
  if (!validateOutput(output)) {
    return fail(error, "ray-hit batch output columns have different lengths");
  }
  if ((!hits.empty() && hits.data() == nullptr) ||
      (!rayWeights.empty() && rayWeights.data() == nullptr) ||
      (!output.rayId.empty() && output.rayId.data() == nullptr) ||
      (!output.surfaceId.empty() && output.surfaceId.data() == nullptr) ||
      (!output.weight.empty() && output.weight.data() == nullptr)) {
    return fail(error, "ray-hit batch span has a null data pointer");
  }
  const std::array<std::pair<const void *, std::size_t>, 5U> ranges = {
      std::pair<const void *, std::size_t>{hits.data(), hits.size_bytes()},
      std::pair<const void *, std::size_t>{rayWeights.data(),
                                           rayWeights.size_bytes()},
      std::pair<const void *, std::size_t>{output.rayId.data(),
                                           output.rayId.size_bytes()},
      std::pair<const void *, std::size_t>{output.surfaceId.data(),
                                           output.surfaceId.size_bytes()},
      std::pair<const void *, std::size_t>{output.weight.data(),
                                           output.weight.size_bytes()}};
  for (std::size_t i = 0U; i < ranges.size(); ++i) {
    for (std::size_t j = i + 1U; j < ranges.size(); ++j) {
      if (overlaps(ranges[i].first, ranges[i].second, ranges[j].first,
                   ranges[j].second)) {
        return fail(error, "ray-hit batch input and output must not alias");
      }
    }
  }
  if (count == 0U) {
    return true;
  }

  std::size_t validCount = 0U;
  for (std::size_t i = 0U; i < count; ++i) {
    if (!strictFp32(rayWeights[i])) {
      return fail(error,
                  "ray weights must be finite normal FP32 values or zero");
    }
    if (!validHit(hits[i], surfaceDomain)) {
      return fail(error, "triangle hit is outside the strict hit domain");
    }
    if (!hits[i].isMiss()) {
      ++validCount;
    }
  }
  if (output.rayId.size() < validCount) {
    return fail(error, "ray-hit batch output capacity is insufficient");
  }

  // Commit only after all validation, preserving both the tail and count on
  // every rejected input.
  std::size_t written = 0U;
  for (std::size_t i = 0U; i < count; ++i) {
    if (!hits[i].isMiss()) {
      output.rayId[written] = static_cast<std::uint32_t>(i);
      output.surfaceId[written] = hits[i].triangleIndex;
      output.weight[written] = rayWeights[i];
      ++written;
    }
  }
  output.count = written;
  return true;
}

RayHitBatchPrimitive::~RayHitBatchPrimitive() { reset(); }

RayHitBatchPrimitive::RayHitBatchPrimitive(
    RayHitBatchPrimitive &&other) noexcept
    : ownedSession_(std::move(other.ownedSession_)), session_(other.session_),
      shaderModule_(std::move(other.shaderModule_)),
      descriptorSetLayout_(std::move(other.descriptorSetLayout_)),
      pipelineLayout_(std::move(other.pipelineLayout_)),
      pipeline_(std::move(other.pipeline_)),
      descriptorPool_(std::move(other.descriptorPool_)),
      fence_(std::move(other.fence_)), descriptorSet_(other.descriptorSet_),
      commandBuffer_(other.commandBuffer_) {
  if (session_ == &other.ownedSession_) {
    session_ = &ownedSession_;
  }
  other.session_ = nullptr;
  other.descriptorSet_ = VK_NULL_HANDLE;
  other.commandBuffer_ = VK_NULL_HANDLE;
}

RayHitBatchPrimitive &
RayHitBatchPrimitive::operator=(RayHitBatchPrimitive &&other) noexcept {
  if (this != &other) {
    reset();
    ownedSession_ = std::move(other.ownedSession_);
    session_ = other.session_ == &other.ownedSession_ ? &ownedSession_
                                                      : other.session_;
    shaderModule_ = std::move(other.shaderModule_);
    descriptorSetLayout_ = std::move(other.descriptorSetLayout_);
    pipelineLayout_ = std::move(other.pipelineLayout_);
    pipeline_ = std::move(other.pipeline_);
    descriptorPool_ = std::move(other.descriptorPool_);
    fence_ = std::move(other.fence_);
    descriptorSet_ = std::exchange(other.descriptorSet_, VK_NULL_HANDLE);
    commandBuffer_ = std::exchange(other.commandBuffer_, VK_NULL_HANDLE);
    other.session_ = nullptr;
  }
  return *this;
}

bool RayHitBatchPrimitive::setup(const std::string_view spirvPath,
                                 runtime::ComputeSession *externalSession,
                                 std::string &error) {
  if (spirvPath.empty()) {
    return fail(error, "ray-hit batch SPIR-V path is empty");
  }
  reset();
  if (externalSession != nullptr) {
    if (!externalSession->isValid()) {
      return fail(error, "external compute session is not initialized");
    }
    session_ = externalSession;
  } else {
    if (!ownedSession_.initialize(error)) {
      return false;
    }
    session_ = &ownedSession_;
  }
  runtime::SpirvProgram program{};
  if (!runtime::readSpirv(spirvPath, program, error) ||
      !shaderModule_.create(session_->device(), program, error)) {
    reset();
    return false;
  }
  std::array<VkDescriptorSetLayoutBinding, 6U> bindings{};
  for (std::uint32_t i = 0U; i < bindings.size(); ++i) {
    bindings[i] = {i, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1U,
                   VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
  }
  const VkPushConstantRange pushRange{VK_SHADER_STAGE_COMPUTE_BIT, 0U,
                                      3U * sizeof(std::uint32_t)};
  if (!descriptorSetLayout_.create(session_->device(), bindings, error) ||
      !pipelineLayout_.create(
          session_->device(), descriptorSetLayout_.get(),
          std::span<const VkPushConstantRange>(&pushRange, 1U), error) ||
      !descriptorPool_.create(session_->device(), 1U, 6U,
                              VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, error) ||
      !descriptorPool_.allocate(descriptorSetLayout_.get(), descriptorSet_,
                                error) ||
      !session_->commandContext().allocatePrimary(commandBuffer_, error) ||
      !fence_.create(session_->device(), error)) {
    reset();
    return false;
  }
  const runtime::ComputePipelineOptions options{"main", {}, nullptr, 0U};
  if (!pipeline_.create(session_->device(), shaderModule_, pipelineLayout_,
                        options, error)) {
    reset();
    return false;
  }
  return true;
}

bool RayHitBatchPrimitive::initialize(const std::string_view spirvPath,
                                      std::string &error) {
  error.clear();
  return setup(spirvPath, nullptr, error);
}

bool RayHitBatchPrimitive::initialize(runtime::ComputeSession &session,
                                      const std::string_view spirvPath,
                                      std::string &error) {
  error.clear();
  return setup(spirvPath, &session, error);
}

void RayHitBatchPrimitive::reset() {
  const bool ownsSession = session_ == &ownedSession_;
  fence_.destroy();
  pipeline_.reset();
  descriptorPool_.reset();
  pipelineLayout_.reset();
  descriptorSetLayout_.reset();
  shaderModule_.reset();
  descriptorSet_ = VK_NULL_HANDLE;
  commandBuffer_ = VK_NULL_HANDLE;
  session_ = nullptr;
  if (ownsSession) {
    ownedSession_.reset();
  }
}

bool RayHitBatchPrimitive::isInitialized() const {
  return session_ != nullptr && session_->isValid() &&
         shaderModule_.get() != VK_NULL_HANDLE &&
         descriptorSetLayout_.get() != VK_NULL_HANDLE &&
         pipelineLayout_.get() != VK_NULL_HANDLE &&
         pipeline_.get() != VK_NULL_HANDLE &&
         descriptorSet_ != VK_NULL_HANDLE && commandBuffer_ != VK_NULL_HANDLE &&
         fence_.get() != VK_NULL_HANDLE;
}

bool RayHitBatchPrimitive::ready(std::string &error) const {
  if (!isInitialized()) {
    return fail(error, "ray-hit batch primitive is not initialized");
  }
  return true;
}

bool RayHitBatchPrimitive::createBuffer(const std::size_t count,
                                        const std::size_t elementSize,
                                        runtime::HostVisibleBuffer &buffer,
                                        std::string &error) {
  if (!ready(error)) {
    return false;
  }
  const std::size_t allocated = count == 0U ? 1U : count;
  if (allocated > std::numeric_limits<std::size_t>::max() / elementSize) {
    return fail(error, "ray-hit batch buffer size overflows host size");
  }
  return buffer.create(session_->device(),
                       static_cast<VkDeviceSize>(allocated * elementSize),
                       kUsage, kMemory, error);
}

bool RayHitBatchPrimitive::createHitBuffer(const std::size_t count,
                                           runtime::HostVisibleBuffer &buffer,
                                           std::string &error) {
  return createBuffer(count, sizeof(TriangleHit), buffer, error);
}

bool RayHitBatchPrimitive::createWeightBuffer(
    const std::size_t count, runtime::HostVisibleBuffer &buffer,
    std::string &error) {
  return createBuffer(count, sizeof(float), buffer, error);
}

bool RayHitBatchPrimitive::createRayIdBuffer(const std::size_t count,
                                             runtime::HostVisibleBuffer &buffer,
                                             std::string &error) {
  return createBuffer(count, sizeof(std::uint32_t), buffer, error);
}

bool RayHitBatchPrimitive::createSurfaceIdBuffer(
    const std::size_t count, runtime::HostVisibleBuffer &buffer,
    std::string &error) {
  return createBuffer(count, sizeof(std::uint32_t), buffer, error);
}

bool RayHitBatchPrimitive::compact(
    runtime::HostVisibleBuffer &hits, runtime::HostVisibleBuffer &rayWeights,
    const std::size_t rayCount, const std::uint32_t surfaceDomain,
    runtime::HostVisibleBuffer &outputRayId,
    runtime::HostVisibleBuffer &outputSurfaceId,
    runtime::HostVisibleBuffer &outputWeight, const std::size_t outputCapacity,
    std::size_t &outputCount, std::string &error) {
  error.clear();
  if (!ready(error)) {
    return false;
  }
  if (rayCount > std::numeric_limits<std::uint32_t>::max() ||
      outputCapacity >= std::numeric_limits<std::uint32_t>::max() ||
      rayCount > hits.size() / sizeof(TriangleHit) ||
      rayCount > rayWeights.size() / sizeof(float) ||
      outputCapacity > outputRayId.size() / sizeof(std::uint32_t) ||
      outputCapacity > outputSurfaceId.size() / sizeof(std::uint32_t) ||
      outputCapacity > outputWeight.size() / sizeof(float)) {
    return fail(error,
                "ray-hit batch buffer capacity or index range is invalid");
  }
  if (rayCount != 0U && surfaceDomain == 0U) {
    return fail(error, "surface domain must be nonzero for non-empty input");
  }
  const std::array<runtime::HostVisibleBuffer *, 5U> buffers = {
      &hits, &rayWeights, &outputRayId, &outputSurfaceId, &outputWeight};
  for (std::size_t i = 0U; i < buffers.size(); ++i) {
    if (buffers[i]->ownerDevice() != session_->device().get()) {
      return fail(error,
                  "ray-hit batch buffer belongs to another Vulkan device");
    }
    for (std::size_t j = i + 1U; j < buffers.size(); ++j) {
      if (buffers[i]->handle() == buffers[j]->handle()) {
        return fail(error, "ray-hit batch input/output buffers must not alias");
      }
    }
  }
  if (rayCount == 0U) {
    return true;
  }
  if (!hits.map(error) || !rayWeights.map(error) || !outputRayId.map(error) ||
      !outputSurfaceId.map(error) || !outputWeight.map(error) ||
      !hits.flush(error) || !rayWeights.flush(error)) {
    return false;
  }
  const auto *hitData = static_cast<const TriangleHit *>(hits.mappedPtr());
  const auto *weightData = static_cast<const float *>(rayWeights.mappedPtr());
  std::size_t validCount = 0U;
  for (std::size_t i = 0U; i < rayCount; ++i) {
    if (!strictFp32(weightData[i])) {
      return fail(error,
                  "ray weights must be finite normal FP32 values or zero");
    }
    if (!validHit(hitData[i], surfaceDomain)) {
      return fail(error, "triangle hit is outside the strict hit domain");
    }
    if (!hitData[i].isMiss()) {
      ++validCount;
    }
  }
  if (validCount > outputCapacity) {
    return fail(error, "ray-hit batch output capacity is insufficient");
  }
  std::vector<std::uint32_t> expectedRay;
  std::vector<std::uint32_t> expectedSurface;
  std::vector<std::uint32_t> expectedWeightBits;
  expectedRay.reserve(validCount);
  expectedSurface.reserve(validCount);
  expectedWeightBits.reserve(validCount);
  for (std::size_t i = 0U; i < rayCount; ++i) {
    if (!hitData[i].isMiss()) {
      expectedRay.push_back(static_cast<std::uint32_t>(i));
      expectedSurface.push_back(hitData[i].triangleIndex);
      expectedWeightBits.push_back(std::bit_cast<std::uint32_t>(weightData[i]));
    }
  }

  runtime::HostVisibleBuffer temporaryRay{};
  runtime::HostVisibleBuffer temporarySurface{};
  runtime::HostVisibleBuffer temporaryWeight{};
  runtime::HostVisibleBuffer countBuffer{};
  if (!createRayIdBuffer(outputCapacity, temporaryRay, error) ||
      !createSurfaceIdBuffer(outputCapacity, temporarySurface, error) ||
      !createWeightBuffer(outputCapacity, temporaryWeight, error) ||
      !createBuffer(1U, sizeof(std::uint32_t), countBuffer, error) ||
      !temporaryRay.map(error) || !temporarySurface.map(error) ||
      !temporaryWeight.map(error) || !countBuffer.map(error)) {
    return false;
  }

  const std::array<VkBuffer, 6U> handles = {hits.handle(),
                                            rayWeights.handle(),
                                            temporaryRay.handle(),
                                            temporarySurface.handle(),
                                            temporaryWeight.handle(),
                                            countBuffer.handle()};
  const auto rayBytes = static_cast<VkDeviceSize>(
      std::max<std::size_t>(1U, rayCount) * sizeof(TriangleHit));
  const auto weightBytes = static_cast<VkDeviceSize>(
      std::max<std::size_t>(1U, rayCount) * sizeof(float));
  const auto outputBytes = static_cast<VkDeviceSize>(
      std::max<std::size_t>(1U, outputCapacity) * sizeof(std::uint32_t));
  const std::array<VkDeviceSize, 6U> ranges = {
      rayBytes,
      weightBytes,
      outputBytes,
      outputBytes,
      static_cast<VkDeviceSize>(std::max<std::size_t>(1U, outputCapacity) *
                                sizeof(float)),
      sizeof(std::uint32_t)};
  std::array<VkDescriptorBufferInfo, 6U> infos{};
  std::array<VkWriteDescriptorSet, 6U> writes{};
  for (std::uint32_t i = 0U; i < writes.size(); ++i) {
    infos[i] = {handles[i], 0U, ranges[i]};
    writes[i] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                 nullptr,
                 descriptorSet_,
                 i,
                 0U,
                 1U,
                 VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                 nullptr,
                 &infos[i],
                 nullptr};
  }
  vkUpdateDescriptorSets(session_->device().get(),
                         static_cast<std::uint32_t>(writes.size()),
                         writes.data(), 0U, nullptr);
  std::array<VkBufferMemoryBarrier, 6U> pre{};
  for (std::uint32_t i = 0U; i < pre.size(); ++i) {
    pre[i] = {VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
              nullptr,
              VK_ACCESS_HOST_WRITE_BIT,
              VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
              VK_QUEUE_FAMILY_IGNORED,
              VK_QUEUE_FAMILY_IGNORED,
              handles[i],
              0U,
              ranges[i]};
  }
  VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  if (vkResetCommandBuffer(commandBuffer_, 0U) != VK_SUCCESS ||
      vkBeginCommandBuffer(commandBuffer_, &begin) != VK_SUCCESS) {
    return fail(error, "failed to begin ray-hit batch command buffer");
  }
  vkCmdPipelineBarrier(commandBuffer_, VK_PIPELINE_STAGE_HOST_BIT,
                       VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0U, 0U, nullptr,
                       static_cast<std::uint32_t>(pre.size()), pre.data(), 0U,
                       nullptr);
  vkCmdBindPipeline(commandBuffer_, VK_PIPELINE_BIND_POINT_COMPUTE,
                    pipeline_.get());
  vkCmdBindDescriptorSets(commandBuffer_, VK_PIPELINE_BIND_POINT_COMPUTE,
                          pipelineLayout_.get(), 0U, 1U, &descriptorSet_, 0U,
                          nullptr);
  const std::array<std::uint32_t, 3U> push = {
      static_cast<std::uint32_t>(rayCount), surfaceDomain,
      static_cast<std::uint32_t>(outputCapacity)};
  vkCmdPushConstants(commandBuffer_, pipelineLayout_.get(),
                     VK_SHADER_STAGE_COMPUTE_BIT, 0U, sizeof(push),
                     push.data());
  vkCmdDispatch(commandBuffer_, 1U, 1U, 1U);
  std::array<VkBufferMemoryBarrier, 4U> post{};
  for (std::uint32_t i = 0U; i < post.size(); ++i) {
    const auto binding = 2U + i;
    post[i] = {VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
               nullptr,
               VK_ACCESS_SHADER_WRITE_BIT,
               VK_ACCESS_HOST_READ_BIT,
               VK_QUEUE_FAMILY_IGNORED,
               VK_QUEUE_FAMILY_IGNORED,
               handles[binding],
               0U,
               ranges[binding]};
  }
  vkCmdPipelineBarrier(commandBuffer_, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                       VK_PIPELINE_STAGE_HOST_BIT, 0U, 0U, nullptr,
                       static_cast<std::uint32_t>(post.size()), post.data(), 0U,
                       nullptr);
  if (vkEndCommandBuffer(commandBuffer_) != VK_SUCCESS) {
    return fail(error, "failed to end ray-hit batch command buffer");
  }
  VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
  submit.commandBufferCount = 1U;
  submit.pCommandBuffers = &commandBuffer_;
  if (vkQueueSubmit(session_->device().computeQueue(), 1U, &submit,
                    fence_.get()) != VK_SUCCESS ||
      !fence_.wait(10'000'000'000ULL, error)) {
    return false;
  }
  fence_.reset();
  if (!temporaryRay.invalidate(error) || !temporarySurface.invalidate(error) ||
      !temporaryWeight.invalidate(error) || !countBuffer.invalidate(error)) {
    return false;
  }
  const auto count =
      *static_cast<const std::uint32_t *>(countBuffer.mappedPtr());
  if (count > outputCapacity || count != validCount ||
      count != expectedRay.size()) {
    return fail(error, "shader produced an invalid ray-hit batch count");
  }
  const auto *rays =
      static_cast<const std::uint32_t *>(temporaryRay.mappedPtr());
  const auto *surfaces =
      static_cast<const std::uint32_t *>(temporarySurface.mappedPtr());
  const auto *weights = static_cast<const float *>(temporaryWeight.mappedPtr());
  for (std::uint32_t i = 0U; i < count; ++i) {
    if (rays[i] >= rayCount || surfaces[i] >= surfaceDomain ||
        !strictFp32(weights[i]) || (i != 0U && rays[i] <= rays[i - 1U]) ||
        rays[i] != expectedRay[i] || surfaces[i] != expectedSurface[i] ||
        std::bit_cast<std::uint32_t>(weights[i]) != expectedWeightBits[i]) {
      return fail(error, "shader produced an invalid ray-hit batch record");
    }
  }
  if (count != 0U) {
    const auto rayBytesOut = count * sizeof(std::uint32_t);
    const auto weightBytesOut = count * sizeof(float);
    if (!outputRayId.write(rays, rayBytesOut, 0U, error) ||
        !outputSurfaceId.write(surfaces, rayBytesOut, 0U, error) ||
        !outputWeight.write(weights, weightBytesOut, 0U, error)) {
      return false;
    }
  }
  outputCount = count;
  return true;
}

const runtime::VulkanDevice &RayHitBatchPrimitive::device() const {
  static const runtime::VulkanDevice empty{};
  return session_ == nullptr ? empty : session_->device();
}

} // namespace viennaps::vulkan::ray
