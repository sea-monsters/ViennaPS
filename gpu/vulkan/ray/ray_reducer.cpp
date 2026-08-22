// Copyright 2026 ViennaPS
// SPDX-License-Identifier: MIT

#include "ray_reducer.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <numeric>
#include <utility>
#include <vector>

namespace {

using viennaps::vulkan::ray::RayRecordSoA;
using viennaps::vulkan::ray::RayReduction;

[[nodiscard]] bool fail(std::string &error, const char *message) {
  error = message;
  return false;
}

[[nodiscard]] bool normalOrZero(const float value) {
  return value == 0.0F || (std::isfinite(value) && std::isnormal(value));
}

[[nodiscard]] bool rangeOverlaps(const void *a, const std::size_t aBytes,
                                 const void *b, const std::size_t bBytes) {
  if (aBytes == 0U || bBytes == 0U || a == nullptr || b == nullptr) {
    return false;
  }
  const auto beginA = reinterpret_cast<std::uintptr_t>(a);
  const auto beginB = reinterpret_cast<std::uintptr_t>(b);
  return beginA < beginB + bBytes && beginB < beginA + aBytes;
}

} // namespace

namespace viennaps::vulkan::ray {

DeterministicRayReducer::~DeterministicRayReducer() { reset(); }

DeterministicRayReducer::DeterministicRayReducer(
    DeterministicRayReducer &&other) noexcept
    : ownedSession_(std::move(other.ownedSession_)), session_(other.session_),
      shaderModule_(std::move(other.shaderModule_)),
      descriptorSetLayout_(std::move(other.descriptorSetLayout_)),
      pipelineLayout_(std::move(other.pipelineLayout_)),
      pipeline_(std::move(other.pipeline_)),
      descriptorPool_(std::move(other.descriptorPool_)),
      commandContext_(std::move(other.commandContext_)),
      fence_(std::move(other.fence_)), descriptorSet_(other.descriptorSet_),
      commandBuffer_(other.commandBuffer_) {
  if (session_ == &other.ownedSession_) {
    session_ = &ownedSession_;
  }
  other.session_ = nullptr;
  other.descriptorSet_ = VK_NULL_HANDLE;
  other.commandBuffer_ = VK_NULL_HANDLE;
}

DeterministicRayReducer &
DeterministicRayReducer::operator=(DeterministicRayReducer &&other) noexcept {
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
    commandContext_ = std::move(other.commandContext_);
    fence_ = std::move(other.fence_);
    descriptorSet_ = std::exchange(other.descriptorSet_, VK_NULL_HANDLE);
    commandBuffer_ = std::exchange(other.commandBuffer_, VK_NULL_HANDLE);
    other.session_ = nullptr;
  }
  return *this;
}

bool DeterministicRayReducer::setup(const std::string_view spirvPath,
                                    runtime::ComputeSession *externalSession,
                                    std::string &error) {
  if (spirvPath.empty()) {
    return fail(error, "SPIR-V path is empty");
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
  std::array<VkDescriptorSetLayoutBinding, 7U> bindings{};
  for (std::uint32_t i = 0U; i < bindings.size(); ++i) {
    bindings[i] = {i, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1U,
                   VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
  }
  // Must mirror the push_constant block declared by ray_reducer.comp
  // (uint rayCount, uint surfaceDomain, uint outputCapacity).
  const VkPushConstantRange pushRange{VK_SHADER_STAGE_COMPUTE_BIT, 0U,
                                      sizeof(std::array<std::uint32_t, 3U>)};
  if (!descriptorSetLayout_.create(session_->device(), bindings, error) ||
      !pipelineLayout_.create(session_->device(), descriptorSetLayout_.get(),
                              std::span(&pushRange, 1U), error) ||
      !descriptorPool_.create(session_->device(), 1U, 7U,
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

bool DeterministicRayReducer::initialize(const std::string_view spirvPath,
                                         std::string &error) {
  error.clear();
  return setup(spirvPath, nullptr, error);
}

bool DeterministicRayReducer::initialize(runtime::ComputeSession &session,
                                         const std::string_view spirvPath,
                                         std::string &error) {
  error.clear();
  return setup(spirvPath, &session, error);
}

void DeterministicRayReducer::reset() {
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

bool DeterministicRayReducer::isInitialized() const {
  return session_ != nullptr && session_->isValid() &&
         shaderModule_.get() != VK_NULL_HANDLE &&
         descriptorSetLayout_.get() != VK_NULL_HANDLE &&
         pipelineLayout_.get() != VK_NULL_HANDLE &&
         pipeline_.get() != VK_NULL_HANDLE &&
         descriptorSet_ != VK_NULL_HANDLE && commandBuffer_ != VK_NULL_HANDLE &&
         fence_.get() != VK_NULL_HANDLE;
}

bool DeterministicRayReducer::ready(std::string &error) const {
  if (!isInitialized()) {
    return fail(error, "ray reducer is not initialized");
  }
  return true;
}

bool DeterministicRayReducer::createBuffer(const std::size_t count,
                                           const std::size_t elementSize,
                                           runtime::HostVisibleBuffer &buffer,
                                           std::string &error) {
  if (!ready(error)) {
    return false;
  }
  const std::size_t allocated = count == 0U ? 1U : count;
  if (allocated > std::numeric_limits<std::size_t>::max() / elementSize) {
    return fail(error, "buffer size overflows host size");
  }
  return buffer.create(session_->device(),
                       static_cast<VkDeviceSize>(allocated * elementSize),
                       kUsage, kMemory, error);
}

bool DeterministicRayReducer::createRayIdBuffer(
    const std::size_t count, runtime::HostVisibleBuffer &buffer,
    std::string &error) {
  return createBuffer(count, sizeof(std::uint32_t), buffer, error);
}

bool DeterministicRayReducer::createSurfaceIdBuffer(
    const std::size_t count, runtime::HostVisibleBuffer &buffer,
    std::string &error) {
  return createBuffer(count, sizeof(std::uint32_t), buffer, error);
}

bool DeterministicRayReducer::createWeightBuffer(
    const std::size_t count, runtime::HostVisibleBuffer &buffer,
    std::string &error) {
  return createBuffer(count, sizeof(float), buffer, error);
}

bool DeterministicRayReducer::reduce(
    runtime::HostVisibleBuffer &rayId, runtime::HostVisibleBuffer &surfaceId,
    runtime::HostVisibleBuffer &weight, const std::size_t rayCount,
    const std::uint32_t surfaceDomain,
    runtime::HostVisibleBuffer &outputSurfaceId,
    runtime::HostVisibleBuffer &outputWeight, const std::size_t outputCapacity,
    std::size_t &outputCount, std::string &error) {
  error.clear();
  if (!ready(error)) {
    return false;
  }
  if (rayCount > std::numeric_limits<std::uint32_t>::max() ||
      outputCapacity >= std::numeric_limits<std::uint32_t>::max() ||
      rayCount > rayId.size() / sizeof(std::uint32_t) ||
      rayCount > surfaceId.size() / sizeof(std::uint32_t) ||
      rayCount > weight.size() / sizeof(float) ||
      outputCapacity > outputSurfaceId.size() / sizeof(std::uint32_t) ||
      outputCapacity > outputWeight.size() / sizeof(float) ||
      outputSurfaceId.size() < outputCapacity * sizeof(std::uint32_t) ||
      outputWeight.size() < outputCapacity * sizeof(float)) {
    return fail(error, "buffer capacity is insufficient");
  }
  if (rayCount != 0U && surfaceDomain == 0U) {
    return fail(error, "surface domain must be nonzero for non-empty input");
  }
  const std::array<runtime::HostVisibleBuffer *, 5U> buffers = {
      &rayId, &surfaceId, &weight, &outputSurfaceId, &outputWeight};
  for (std::size_t i = 0U; i < buffers.size(); ++i) {
    if (buffers[i]->ownerDevice() != session_->device().get()) {
      return fail(error, "buffer belongs to another Vulkan device");
    }
    for (std::size_t j = i + 1U; j < buffers.size(); ++j) {
      if (buffers[i]->handle() == buffers[j]->handle()) {
        return fail(error, "ray input/output buffers must not alias");
      }
    }
  }
  if (!rayId.map(error) || !surfaceId.map(error) || !weight.map(error) ||
      !outputSurfaceId.map(error) || !outputWeight.map(error) ||
      !rayId.flush(error) || !surfaceId.flush(error) || !weight.flush(error)) {
    return false;
  }
  const auto *surfaces =
      static_cast<const std::uint32_t *>(surfaceId.mappedPtr());
  const auto *weights = static_cast<const float *>(weight.mappedPtr());
  for (std::size_t i = 0U; i < rayCount; ++i) {
    if (surfaces[i] >= surfaceDomain || !normalOrZero(weights[i])) {
      return fail(error,
                  "ray index or weight is outside the strict FP32 domain");
    }
  }

  runtime::HostVisibleBuffer scratch{};
  runtime::HostVisibleBuffer countBuffer{};
  runtime::HostVisibleBuffer temporarySurface{};
  runtime::HostVisibleBuffer temporaryWeight{};
  if (!createBuffer(rayCount, sizeof(std::uint32_t), scratch, error) ||
      !createBuffer(1U, sizeof(std::uint32_t), countBuffer, error) ||
      !createSurfaceIdBuffer(outputCapacity, temporarySurface, error) ||
      !createWeightBuffer(outputCapacity, temporaryWeight, error)) {
    return false;
  }
  if (!scratch.map(error) || !countBuffer.map(error) ||
      !temporarySurface.map(error) || !temporaryWeight.map(error)) {
    return false;
  }
  std::vector<std::uint32_t> zeros(rayCount, 0U);
  if (rayCount != 0U && !scratch.write(zeros.data(),
                                       static_cast<VkDeviceSize>(
                                           rayCount * sizeof(std::uint32_t)),
                                       0U, error)) {
    return false;
  }
  std::uint32_t zero = 0U;
  if (!countBuffer.write(&zero, sizeof(zero), 0U, error) ||
      !scratch.flush(error) || !countBuffer.flush(error)) {
    return false;
  }

  const std::array<VkBuffer, 7U> handles = {
      rayId.handle(),           surfaceId.handle(),
      weight.handle(),          temporarySurface.handle(),
      temporaryWeight.handle(), scratch.handle(),
      countBuffer.handle()};
  const std::array<VkDeviceSize, 7U> ranges = {
      static_cast<VkDeviceSize>(std::max<std::size_t>(1U, rayCount) *
                                sizeof(std::uint32_t)),
      static_cast<VkDeviceSize>(std::max<std::size_t>(1U, rayCount) *
                                sizeof(std::uint32_t)),
      static_cast<VkDeviceSize>(std::max<std::size_t>(1U, rayCount) *
                                sizeof(float)),
      static_cast<VkDeviceSize>(std::max<std::size_t>(1U, outputCapacity) *
                                sizeof(std::uint32_t)),
      static_cast<VkDeviceSize>(std::max<std::size_t>(1U, outputCapacity) *
                                sizeof(float)),
      static_cast<VkDeviceSize>(std::max<std::size_t>(1U, rayCount) *
                                sizeof(std::uint32_t)),
      sizeof(std::uint32_t)};
  std::array<VkDescriptorBufferInfo, 7U> infos{};
  std::array<VkWriteDescriptorSet, 7U> writes{};
  for (std::uint32_t i = 0U; i < writes.size(); ++i) {
    infos[i] = {handles[i], 0U, ranges[i]};
    writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[i].dstSet = descriptorSet_;
    writes[i].dstBinding = i;
    writes[i].descriptorCount = 1U;
    writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    writes[i].pBufferInfo = &infos[i];
  }
  vkUpdateDescriptorSets(session_->device().get(),
                         static_cast<std::uint32_t>(writes.size()),
                         writes.data(), 0U, nullptr);
  std::array<VkBufferMemoryBarrier, 7U> pre{};
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
    return fail(error, "failed to begin ray reducer command buffer");
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
  std::array<VkBufferMemoryBarrier, 3U> post{};
  for (std::uint32_t i = 0U; i < post.size(); ++i) {
    const auto binding = 3U + i;
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
    return fail(error, "failed to end ray reducer command buffer");
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
  if (!temporarySurface.invalidate(error) ||
      !temporaryWeight.invalidate(error) || !countBuffer.invalidate(error)) {
    return false;
  }
  const auto count =
      *static_cast<const std::uint32_t *>(countBuffer.mappedPtr());
  if (count > outputCapacity) {
    return fail(error, "shader produced more records than output capacity");
  }
  const auto *reducedSurfaces =
      static_cast<const std::uint32_t *>(temporarySurface.mappedPtr());
  const auto *reducedWeights =
      static_cast<const float *>(temporaryWeight.mappedPtr());
  for (std::uint32_t i = 0U; i < count; ++i) {
    if (reducedSurfaces[i] >= surfaceDomain ||
        !normalOrZero(reducedWeights[i]) ||
        (i != 0U && reducedSurfaces[i] <= reducedSurfaces[i - 1U])) {
      return fail(error, "shader output violates strict surface/FP32 domain");
    }
  }
  if (count != 0U) {
    if (!outputSurfaceId.write(temporarySurface.mappedPtr(),
                               count * sizeof(std::uint32_t), 0U, error) ||
        !outputWeight.write(temporaryWeight.mappedPtr(), count * sizeof(float),
                            0U, error)) {
      return false;
    }
  }
  outputCount = count;
  return true;
}

const runtime::VulkanDevice &DeterministicRayReducer::device() const {
  static const runtime::VulkanDevice empty{};
  return session_ == nullptr ? empty : session_->device();
}

bool reduceCpu(const RayRecordSoA &input, const std::uint32_t surfaceDomain,
               RayReduction &output, std::string &error) {
  error.clear();
  if (input.rayId.size() != input.surfaceId.size() ||
      input.rayId.size() != input.weight.size()) {
    return fail(error, "ray SoA columns have different lengths");
  }
  const auto count = input.rayId.size();
  if ((count != 0U &&
       (input.rayId.data() == nullptr || input.surfaceId.data() == nullptr ||
        input.weight.data() == nullptr)) ||
      (!output.surfaceId.empty() && output.surfaceId.data() == nullptr) ||
      (!output.weight.empty() && output.weight.data() == nullptr)) {
    return fail(error, "ray SoA contains a null data pointer");
  }
  if (!input.normal.empty() && input.normal.size() != count * 3U) {
    return fail(error, "normal column must contain three values per ray");
  }
  if (count != 0U && surfaceDomain == 0U) {
    return fail(error, "surface domain must be nonzero for non-empty input");
  }
  if (output.surfaceId.size() != output.weight.size() ||
      output.surfaceId.empty() && count != 0U) {
    return fail(error, "output capacity is insufficient");
  }
  const std::array<std::pair<const void *, std::size_t>, 5U> columns = {
      std::pair<const void *, std::size_t>{input.rayId.data(),
                                           count * sizeof(std::uint32_t)},
      std::pair<const void *, std::size_t>{input.surfaceId.data(),
                                           count * sizeof(std::uint32_t)},
      std::pair<const void *, std::size_t>{input.weight.data(),
                                           count * sizeof(float)},
      std::pair<const void *, std::size_t>{output.surfaceId.data(),
                                           output.surfaceId.size() *
                                               sizeof(std::uint32_t)},
      std::pair<const void *, std::size_t>{
          output.weight.data(), output.weight.size() * sizeof(float)}};
  for (std::size_t i = 0U; i < 5U; ++i) {
    for (std::size_t j = i + 1U; j < 5U; ++j) {
      if ((i < 3U || j < 3U) &&
          rangeOverlaps(columns[i].first, columns[i].second, columns[j].first,
                        columns[j].second)) {
        return fail(error, "input and output columns must not alias");
      }
    }
  }
  for (std::size_t i = 0U; i < count; ++i) {
    if (input.surfaceId[i] >= surfaceDomain) {
      return fail(error, "surface index is outside the declared domain");
    }
    if (!normalOrZero(input.weight[i])) {
      return fail(error, "weights must be finite normal FP32 values or zero");
    }
    if (!input.normal.empty()) {
      const float nx = input.normal[3U * i];
      const float ny = input.normal[3U * i + 1U];
      const float nz = input.normal[3U * i + 2U];
      if (!normalOrZero(nx) || !normalOrZero(ny) || !normalOrZero(nz)) {
        return fail(error,
                    "normal components must be finite normal FP32 or zero");
      }
      const bool zero = nx == 0.0F && ny == 0.0F && nz == 0.0F;
      const float length2 = nx * nx + ny * ny + nz * nz;
      if (!zero || length2 != 0.0F) {
        if (!std::isfinite(length2) || std::abs(length2 - 1.0F) > 1.0e-3F) {
          return fail(error, "normal must be zero or unit length");
        }
      }
    }
  }

  std::vector<std::size_t> order(count);
  std::iota(order.begin(), order.end(), 0U);
  std::stable_sort(order.begin(), order.end(),
                   [&](const auto left, const auto right) {
                     if (input.surfaceId[left] != input.surfaceId[right]) {
                       return input.surfaceId[left] < input.surfaceId[right];
                     }
                     if (input.rayId[left] != input.rayId[right]) {
                       return input.rayId[left] < input.rayId[right];
                     }
                     return left < right;
                   });

  std::vector<std::pair<std::uint32_t, float>> reduced;
  reduced.reserve(count);
  for (const auto index : order) {
    const auto surface = input.surfaceId[index];
    if (reduced.empty() || reduced.back().first != surface) {
      reduced.emplace_back(surface, input.weight[index]);
    } else {
      volatile float sum = reduced.back().second;
      sum = sum + input.weight[index];
      reduced.back().second = sum;
      if (!normalOrZero(reduced.back().second)) {
        return fail(error, "FP32 reduction leaves the strict numeric domain");
      }
    }
  }
  if (reduced.size() > output.surfaceId.size()) {
    return fail(error, "output capacity is insufficient for unique surfaces");
  }
  // Commit only after every validation and reduction step succeeds.
  for (std::size_t i = 0U; i < reduced.size(); ++i) {
    output.surfaceId[i] = reduced[i].first;
    output.weight[i] = reduced[i].second;
  }
  output.count = reduced.size();
  return true;
}

} // namespace viennaps::vulkan::ray
