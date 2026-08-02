// Copyright 2026 ViennaPS
// SPDX-License-Identifier: MIT

#include "graph_diffusion.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <span>
#include <sstream>
#include <string>

namespace {

[[nodiscard]] bool setError(std::string &error, const std::string_view phase,
                            const std::string_view message) {
  error = std::string(phase);
  if (!message.empty()) {
    error += ": ";
    error += message;
  }
  return false;
}

constexpr std::string_view kEntryPoint{"main"};

[[nodiscard]] bool isNormalOrZero(const float value) {
  return value == 0.0F || std::isnormal(value);
}

template <class T>
[[nodiscard]] bool
hasCapacity(const viennaps::vulkan::runtime::HostVisibleBuffer &buffer,
            const std::size_t count) {
  return buffer.isValid() &&
         count <= static_cast<std::size_t>(buffer.size() / sizeof(T));
}

} // namespace

namespace viennaps::vulkan::surface {

SurfaceGraphDiffusionFp32::~SurfaceGraphDiffusionFp32() { reset(); }

bool SurfaceGraphDiffusionFp32::initialize(const std::string_view spirvPath,
                                           std::string &error) {
  error.clear();
  return setup(spirvPath, nullptr, error);
}

bool SurfaceGraphDiffusionFp32::initialize(runtime::ComputeSession &session,
                                           const std::string_view spirvPath,
                                           std::string &error) {
  error.clear();
  return setup(spirvPath, &session, error);
}

bool SurfaceGraphDiffusionFp32::setup(const std::string_view spirvPath,
                                      runtime::ComputeSession *externalSession,
                                      std::string &error) {
  if (spirvPath.empty()) {
    return setError(error, "initialization", "SPIR-V path is empty");
  }

  reset();
  const auto failInitialization = [this, &error]() {
    const auto detail = error;
    reset();
    return setError(error, "initialization", detail);
  };

  if (externalSession != nullptr) {
    if (!externalSession->isValid()) {
      error = "external compute session is not initialized";
      return failInitialization();
    }
    session_ = externalSession;
  } else {
    if (!ownedSession_.initialize(error)) {
      return failInitialization();
    }
    session_ = &ownedSession_;
  }

  runtime::SpirvProgram program{};
  if (!runtime::readSpirv(spirvPath, program, error) ||
      !shaderModule_.create(session_->device(), program, error)) {
    return failInitialization();
  }

  std::array<VkDescriptorSetLayoutBinding, 5U> bindings{};
  for (std::uint32_t binding = 0U; binding < bindings.size(); ++binding) {
    bindings[binding].binding = binding;
    bindings[binding].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    bindings[binding].descriptorCount = 1U;
    bindings[binding].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
  }
  if (!descriptorSetLayout_.create(
          session_->device(),
          std::span<const VkDescriptorSetLayoutBinding>(bindings.data(),
                                                        bindings.size()),
          error)) {
    return failInitialization();
  }

  const VkPushConstantRange pushRange{VK_SHADER_STAGE_COMPUTE_BIT, 0U,
                                      sizeof(PushConstants)};
  if (!pipelineLayout_.create(session_->device(), descriptorSetLayout_.get(),
                              std::span(&pushRange, 1U), error)) {
    return failInitialization();
  }
  if (!descriptorPool_.create(session_->device(), 1U, 5U,
                              VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, error) ||
      !descriptorPool_.allocate(descriptorSetLayout_.get(), descriptorSet_,
                                error)) {
    return failInitialization();
  }
  if (!session_->commandContext().allocatePrimary(commandBuffer_, error) ||
      !fence_.create(session_->device(), error)) {
    return failInitialization();
  }

  const runtime::ComputePipelineOptions options{kEntryPoint, {}, nullptr, 0U};
  if (!pipeline_.create(session_->device(), shaderModule_, pipelineLayout_,
                        options, error)) {
    return failInitialization();
  }
  return true;
}

void SurfaceGraphDiffusionFp32::reset() {
  const bool ownsSession = session_ == &ownedSession_;
  fence_.destroy();
  pipeline_.reset();
  descriptorPool_.reset();
  pipelineLayout_.reset();
  descriptorSetLayout_.reset();
  shaderModule_.reset();
  commandBuffer_ = VK_NULL_HANDLE;
  descriptorSet_ = VK_NULL_HANDLE;
  session_ = nullptr;
  if (ownsSession) {
    ownedSession_.reset();
  }
}

bool SurfaceGraphDiffusionFp32::isInitialized() const {
  return session_ != nullptr && session_->isValid() &&
         shaderModule_.get() != VK_NULL_HANDLE &&
         descriptorSetLayout_.get() != VK_NULL_HANDLE &&
         pipelineLayout_.get() != VK_NULL_HANDLE &&
         pipeline_.get() != VK_NULL_HANDLE &&
         descriptorSet_ != VK_NULL_HANDLE && commandBuffer_ != VK_NULL_HANDLE &&
         fence_.get() != VK_NULL_HANDLE;
}

bool SurfaceGraphDiffusionFp32::isReady(std::string &error) const {
  if (!isInitialized()) {
    return setError(error, "execution",
                    "graph diffusion model is not initialized");
  }
  return true;
}

bool SurfaceGraphDiffusionFp32::createFloatBuffer(
    const std::size_t elementCount, runtime::HostVisibleBuffer &buffer,
    std::string &error) {
  if (!isReady(error)) {
    return false;
  }
  const auto allocatedElements = elementCount == 0U ? 1U : elementCount;
  if (allocatedElements >
      std::numeric_limits<std::size_t>::max() / sizeof(float)) {
    return setError(error, "createFloatBuffer", "element count overflows");
  }
  return buffer.create(
      session_->device(),
      static_cast<VkDeviceSize>(allocatedElements * sizeof(float)),
      kFloatBufferUsage, kHostMemoryFlags, error);
}

bool SurfaceGraphDiffusionFp32::createIndexBuffer(
    const std::size_t elementCount, runtime::HostVisibleBuffer &buffer,
    std::string &error) {
  if (!isReady(error)) {
    return false;
  }
  const auto allocatedElements = elementCount == 0U ? 1U : elementCount;
  if (allocatedElements >
      std::numeric_limits<std::size_t>::max() / sizeof(std::uint32_t)) {
    return setError(error, "createIndexBuffer", "element count overflows");
  }
  return buffer.create(
      session_->device(),
      static_cast<VkDeviceSize>(allocatedElements * sizeof(std::uint32_t)),
      kIndexBufferUsage, kHostMemoryFlags, error);
}

bool SurfaceGraphDiffusionFp32::evaluate(
    runtime::HostVisibleBuffer &rowOffsets, const std::size_t rowOffsetCount,
    runtime::HostVisibleBuffer &columnIndices, const std::size_t nonzeroCount,
    runtime::HostVisibleBuffer &weights, const std::size_t weightCount,
    runtime::HostVisibleBuffer &field, const std::size_t fieldCount,
    runtime::HostVisibleBuffer &output, const std::size_t outputCapacity,
    const float diffusionStep, std::string &error) const {
  error.clear();
  if (!isReady(error)) {
    return false;
  }
  if (fieldCount > std::numeric_limits<std::uint32_t>::max() ||
      nonzeroCount > std::numeric_limits<std::uint32_t>::max()) {
    return setError(error, "validation", "CSR dimensions exceed shader ABI");
  }
  if (fieldCount == std::numeric_limits<std::size_t>::max() ||
      rowOffsetCount != fieldCount + 1U) {
    return setError(error, "validation", "row-offset count must be N + 1");
  }
  if (nonzeroCount != weightCount || outputCapacity < fieldCount) {
    return setError(error, "validation", "CSR and output lengths disagree");
  }
  if (!hasCapacity<std::uint32_t>(rowOffsets, rowOffsetCount) ||
      !hasCapacity<std::uint32_t>(columnIndices, nonzeroCount) ||
      !hasCapacity<float>(weights, weightCount) ||
      !hasCapacity<float>(field, fieldCount) ||
      !hasCapacity<float>(output, outputCapacity)) {
    return setError(error, "validation", "buffer capacity is insufficient");
  }
  const std::array<const runtime::HostVisibleBuffer *, 5U> buffers = {
      &rowOffsets, &columnIndices, &weights, &field, &output};
  for (std::size_t i = 0U; i < buffers.size(); ++i) {
    for (std::size_t j = i + 1U; j < buffers.size(); ++j) {
      if (buffers[i]->handle() == buffers[j]->handle()) {
        return setError(error, "validation", "CSR buffers must not alias");
      }
    }
    if (buffers[i]->ownerDevice() != session_->device().get()) {
      return setError(error, "validation", "buffer belongs to another device");
    }
  }
  if (!std::isfinite(diffusionStep) || !isNormalOrZero(diffusionStep)) {
    return setError(error, "validation",
                    "diffusion step must be finite normal FP32 or zero");
  }

  if (!rowOffsets.mappedPtr() && !rowOffsets.map(error)) {
    return false;
  }
  if (!columnIndices.mappedPtr() && !columnIndices.map(error)) {
    return false;
  }
  if (!weights.mappedPtr() && !weights.map(error)) {
    return false;
  }
  if (!field.mappedPtr() && !field.map(error)) {
    return false;
  }
  if (!output.mappedPtr() && !output.map(error)) {
    return false;
  }
  if (!rowOffsets.flush(error) || !columnIndices.flush(error) ||
      !weights.flush(error) || !field.flush(error) || !output.flush(error)) {
    return setError(error, "execution", "failed to flush host writes");
  }

  const auto *offsetValues =
      static_cast<const std::uint32_t *>(rowOffsets.mappedPtr());
  const auto *columnValues =
      static_cast<const std::uint32_t *>(columnIndices.mappedPtr());
  const auto *weightValues = static_cast<const float *>(weights.mappedPtr());
  const auto *fieldValues = static_cast<const float *>(field.mappedPtr());
  if (offsetValues[0U] != 0U) {
    return setError(error, "validation", "CSR row offsets must start at zero");
  }
  for (std::size_t row = 0U; row < fieldCount; ++row) {
    const auto begin = offsetValues[row];
    const auto end = offsetValues[row + 1U];
    if (begin > end || end > nonzeroCount) {
      return setError(error, "validation", "CSR row offsets are not monotonic");
    }
  }
  if (offsetValues[fieldCount] != nonzeroCount) {
    return setError(error, "validation", "CSR final offset disagrees with nnz");
  }
  for (std::size_t edge = 0U; edge < nonzeroCount; ++edge) {
    if (columnValues[edge] >= fieldCount) {
      return setError(error, "validation", "CSR column index is out of bounds");
    }
    if (!isNormalOrZero(weightValues[edge])) {
      return setError(error, "validation",
                      "CSR weights must be finite normal FP32 or zero");
    }
  }
  for (std::size_t i = 0U; i < fieldCount; ++i) {
    if (!isNormalOrZero(fieldValues[i])) {
      return setError(error, "validation",
                      "field values must be finite normal FP32 or zero");
    }

    // Mirror the shader's operation boundaries while checking that the strict
    // FP32 domain remains valid. The volatile temporaries preserve the CPU
    // oracle's order and prevent host-side contraction during validation.
    volatile float laplacian = 0.0F;
    for (std::size_t edge = offsetValues[i]; edge < offsetValues[i + 1U];
         ++edge) {
      volatile float product =
          weightValues[edge] * fieldValues[columnValues[edge]];
      if (!isNormalOrZero(product)) {
        return setError(error, "validation",
                        "CSR product leaves the strict FP32 domain");
      }
      laplacian = laplacian + product;
      if (!isNormalOrZero(laplacian)) {
        return setError(error, "validation",
                        "CSR row sum leaves the strict FP32 domain");
      }
    }
    volatile float scaled = diffusionStep * laplacian;
    if (!isNormalOrZero(scaled)) {
      return setError(error, "validation",
                      "diffusion scaling leaves the strict FP32 domain");
    }
    volatile float result = fieldValues[i] + scaled;
    if (!isNormalOrZero(result)) {
      return setError(error, "validation",
                      "diffusion result leaves the strict FP32 domain");
    }
  }

  if (fieldCount == 0U) {
    return true;
  }
  const auto groups = fieldCount / kWorkgroupSize +
                      (fieldCount % kWorkgroupSize != 0U ? 1U : 0U);
  if (groups >
      session_->selection().properties.limits.maxComputeWorkGroupCount[0]) {
    return setError(error, "dispatch",
                    "dispatch group count exceeds device limit");
  }

  const std::array<VkBuffer, 5U> handles = {
      rowOffsets.handle(), columnIndices.handle(), weights.handle(),
      field.handle(), output.handle()};
  const std::array<VkDeviceSize, 5U> ranges = {
      static_cast<VkDeviceSize>(rowOffsetCount * sizeof(std::uint32_t)),
      static_cast<VkDeviceSize>(nonzeroCount * sizeof(std::uint32_t)),
      static_cast<VkDeviceSize>(weightCount * sizeof(float)),
      static_cast<VkDeviceSize>(fieldCount * sizeof(float)),
      static_cast<VkDeviceSize>(fieldCount * sizeof(float))};
  std::array<VkDescriptorBufferInfo, 5U> infos{};
  std::array<VkWriteDescriptorSet, 5U> writes{};
  for (std::uint32_t binding = 0U; binding < writes.size(); ++binding) {
    infos[binding].buffer = handles[binding];
    infos[binding].range = ranges[binding];
    writes[binding].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[binding].dstSet = descriptorSet_;
    writes[binding].dstBinding = binding;
    writes[binding].descriptorCount = 1U;
    writes[binding].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    writes[binding].pBufferInfo = &infos[binding];
  }
  vkUpdateDescriptorSets(session_->device().get(),
                         static_cast<std::uint32_t>(writes.size()),
                         writes.data(), 0U, nullptr);

  std::array<VkBufferMemoryBarrier, 5U> preBarriers{};
  for (std::uint32_t binding = 0U; binding < preBarriers.size(); ++binding) {
    preBarriers[binding].sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
    preBarriers[binding].srcAccessMask = VK_ACCESS_HOST_WRITE_BIT;
    preBarriers[binding].dstAccessMask =
        binding == 4U ? VK_ACCESS_SHADER_WRITE_BIT : VK_ACCESS_SHADER_READ_BIT;
    preBarriers[binding].buffer = handles[binding];
    preBarriers[binding].size = ranges[binding];
  }
  VkCommandBufferBeginInfo beginInfo{
      VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  if (vkResetCommandBuffer(commandBuffer_, 0U) != VK_SUCCESS ||
      vkBeginCommandBuffer(commandBuffer_, &beginInfo) != VK_SUCCESS) {
    return setError(error, "dispatch", "failed to begin command buffer");
  }
  vkCmdPipelineBarrier(commandBuffer_, VK_PIPELINE_STAGE_HOST_BIT,
                       VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0U, 0U, nullptr,
                       static_cast<std::uint32_t>(preBarriers.size()),
                       preBarriers.data(), 0U, nullptr);
  vkCmdBindPipeline(commandBuffer_, VK_PIPELINE_BIND_POINT_COMPUTE,
                    pipeline_.get());
  vkCmdBindDescriptorSets(commandBuffer_, VK_PIPELINE_BIND_POINT_COMPUTE,
                          pipelineLayout_.get(), 0U, 1U, &descriptorSet_, 0U,
                          nullptr);
  const PushConstants pushConstants{static_cast<std::uint32_t>(fieldCount),
                                    static_cast<std::uint32_t>(nonzeroCount),
                                    diffusionStep};
  vkCmdPushConstants(commandBuffer_, pipelineLayout_.get(),
                     VK_SHADER_STAGE_COMPUTE_BIT, 0U, sizeof(PushConstants),
                     &pushConstants);
  vkCmdDispatch(commandBuffer_, static_cast<std::uint32_t>(groups), 1U, 1U);

  VkBufferMemoryBarrier postBarrier{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
  postBarrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
  postBarrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
  postBarrier.buffer = output.handle();
  postBarrier.size = ranges[4U];
  vkCmdPipelineBarrier(commandBuffer_, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                       VK_PIPELINE_STAGE_HOST_BIT, 0U, 0U, nullptr, 1U,
                       &postBarrier, 0U, nullptr);
  if (vkEndCommandBuffer(commandBuffer_) != VK_SUCCESS) {
    return setError(error, "dispatch", "failed to end command buffer");
  }
  VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
  submit.commandBufferCount = 1U;
  submit.pCommandBuffers = &commandBuffer_;
  if (vkQueueSubmit(session_->device().computeQueue(), 1U, &submit,
                    fence_.get()) != VK_SUCCESS) {
    return setError(error, "dispatch", "failed to submit command buffer");
  }
  if (!fence_.wait(10'000'000'000ULL, error)) {
    return false;
  }
  fence_.reset();
  if (!output.invalidate(error)) {
    return setError(error, "execution", "failed to invalidate graph output");
  }
  return true;
}

bool SurfaceGraphDiffusionFp32::evaluateDevice(
    runtime::DeviceBuffer &rowOffsets, const std::size_t rowOffsetCount,
    runtime::DeviceBuffer &columnIndices, const std::size_t nonzeroCount,
    runtime::DeviceBuffer &weights, const std::size_t weightCount,
    runtime::DeviceBuffer &field, const std::size_t fieldCount,
    runtime::DeviceBuffer &output, const std::size_t outputCapacity,
    const std::span<const std::uint32_t> rowOffsetValues,
    const std::span<const std::uint32_t> columnValues,
    const std::span<const float> weightValues,
    const std::span<const float> fieldValues, const float diffusionStep,
    std::string &error) const {
  error.clear();
  if (!isReady(error)) {
    return false;
  }
  if (fieldCount > std::numeric_limits<std::uint32_t>::max() ||
      nonzeroCount > std::numeric_limits<std::uint32_t>::max() ||
      rowOffsetCount != fieldCount + 1U || nonzeroCount != weightCount ||
      outputCapacity < fieldCount || rowOffsetValues.size() != rowOffsetCount ||
      columnValues.size() != nonzeroCount ||
      weightValues.size() != weightCount || fieldValues.size() != fieldCount) {
    return setError(error, "validation", "device CSR dimensions disagree");
  }
  if (rowOffsets.ownerDevice() != session_->device().get() ||
      columnIndices.ownerDevice() != session_->device().get() ||
      weights.ownerDevice() != session_->device().get() ||
      field.ownerDevice() != session_->device().get() ||
      output.ownerDevice() != session_->device().get()) {
    return setError(error, "validation",
                    "device buffer belongs to another device");
  }
  const auto generation = session_->generation();
  const std::array<const runtime::DeviceBuffer *, 5U> buffers = {
      &rowOffsets, &columnIndices, &weights, &field, &output};
  for (std::size_t i = 0U; i < buffers.size(); ++i) {
    if (!buffers[i]->isValid() ||
        buffers[i]->ownerSessionGeneration() != generation) {
      return setError(error, "validation", "device buffer session is stale");
    }
    for (std::size_t j = i + 1U; j < buffers.size(); ++j) {
      if (buffers[i]->handle() == buffers[j]->handle()) {
        return setError(error, "validation",
                        "device CSR buffers must not alias");
      }
    }
  }
  if (rowOffsets.size() < rowOffsetCount * sizeof(std::uint32_t) ||
      columnIndices.size() < nonzeroCount * sizeof(std::uint32_t) ||
      weights.size() < weightCount * sizeof(float) ||
      field.size() < fieldCount * sizeof(float) ||
      output.size() < outputCapacity * sizeof(float)) {
    return setError(error, "validation",
                    "device buffer capacity is insufficient");
  }
  if (!std::isfinite(diffusionStep) || !isNormalOrZero(diffusionStep)) {
    return setError(error, "validation",
                    "diffusion step is outside FP32 domain");
  }
  if (rowOffsetValues.empty() || rowOffsetValues.front() != 0U ||
      rowOffsetValues.back() != nonzeroCount) {
    return setError(error, "validation", "CSR row offsets are invalid");
  }
  for (std::size_t row = 0U; row < fieldCount; ++row) {
    if (rowOffsetValues[row] > rowOffsetValues[row + 1U]) {
      return setError(error, "validation", "CSR row offsets are not monotonic");
    }
  }
  for (const auto column : columnValues) {
    if (column >= fieldCount) {
      return setError(error, "validation", "CSR column index is out of bounds");
    }
  }
  for (const auto weight : weightValues) {
    if (!isNormalOrZero(weight)) {
      return setError(error, "validation", "CSR weight is outside FP32 domain");
    }
  }
  for (const auto value : fieldValues) {
    if (!isNormalOrZero(value)) {
      return setError(error, "validation",
                      "field value is outside FP32 domain");
    }
  }
  for (std::size_t row = 0U; row < fieldCount; ++row) {
    volatile float laplacian = 0.0F;
    for (std::size_t edge = rowOffsetValues[row];
         edge < rowOffsetValues[row + 1U]; ++edge) {
      volatile float product =
          weightValues[edge] * fieldValues[columnValues[edge]];
      if (!isNormalOrZero(product)) {
        return setError(error, "validation", "CSR product leaves FP32 domain");
      }
      laplacian = laplacian + product;
      if (!isNormalOrZero(laplacian)) {
        return setError(error, "validation", "CSR sum leaves FP32 domain");
      }
    }
    volatile float scaled = diffusionStep * laplacian;
    volatile float result = fieldValues[row] + scaled;
    if (!isNormalOrZero(scaled) || !isNormalOrZero(result)) {
      return setError(error, "validation",
                      "diffusion result leaves FP32 domain");
    }
  }
  if (fieldCount == 0U) {
    return true;
  }
  const auto groups = fieldCount / kWorkgroupSize +
                      (fieldCount % kWorkgroupSize != 0U ? 1U : 0U);
  if (groups >
      session_->selection().properties.limits.maxComputeWorkGroupCount[0]) {
    return setError(error, "dispatch",
                    "dispatch group count exceeds device limit");
  }

  const std::array<VkBuffer, 5U> handles = {
      rowOffsets.handle(), columnIndices.handle(), weights.handle(),
      field.handle(), output.handle()};
  const std::array<VkDeviceSize, 5U> ranges = {
      rowOffsetCount * sizeof(std::uint32_t),
      nonzeroCount * sizeof(std::uint32_t), weightCount * sizeof(float),
      fieldCount * sizeof(float), fieldCount * sizeof(float)};
  std::array<VkDescriptorBufferInfo, 5U> infos{};
  std::array<VkWriteDescriptorSet, 5U> writes{};
  for (std::uint32_t binding = 0U; binding < writes.size(); ++binding) {
    infos[binding] = {handles[binding], 0U, ranges[binding]};
    writes[binding] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                       nullptr,
                       descriptorSet_,
                       binding,
                       0U,
                       1U,
                       VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                       nullptr,
                       &infos[binding],
                       nullptr};
  }
  vkUpdateDescriptorSets(session_->device().get(),
                         static_cast<std::uint32_t>(writes.size()),
                         writes.data(), 0U, nullptr);
  VkCommandBufferBeginInfo beginInfo{
      VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  if (vkResetCommandBuffer(commandBuffer_, 0U) != VK_SUCCESS ||
      vkBeginCommandBuffer(commandBuffer_, &beginInfo) != VK_SUCCESS) {
    return setError(error, "dispatch", "failed to begin command buffer");
  }
  std::array<VkBufferMemoryBarrier, 5U> barriers{};
  for (std::uint32_t binding = 0U; binding < barriers.size(); ++binding) {
    barriers[binding] = {
        VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
        nullptr,
        VK_ACCESS_TRANSFER_WRITE_BIT,
        static_cast<VkAccessFlags>(binding == 4U ? VK_ACCESS_SHADER_WRITE_BIT
                                                 : VK_ACCESS_SHADER_READ_BIT),
        VK_QUEUE_FAMILY_IGNORED,
        VK_QUEUE_FAMILY_IGNORED,
        handles[binding],
        0U,
        ranges[binding]};
  }
  vkCmdPipelineBarrier(commandBuffer_, VK_PIPELINE_STAGE_TRANSFER_BIT,
                       VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0U, 0U, nullptr,
                       static_cast<std::uint32_t>(barriers.size()),
                       barriers.data(), 0U, nullptr);
  vkCmdBindPipeline(commandBuffer_, VK_PIPELINE_BIND_POINT_COMPUTE,
                    pipeline_.get());
  vkCmdBindDescriptorSets(commandBuffer_, VK_PIPELINE_BIND_POINT_COMPUTE,
                          pipelineLayout_.get(), 0U, 1U, &descriptorSet_, 0U,
                          nullptr);
  const PushConstants pushConstants{static_cast<std::uint32_t>(fieldCount),
                                    static_cast<std::uint32_t>(nonzeroCount),
                                    diffusionStep};
  vkCmdPushConstants(commandBuffer_, pipelineLayout_.get(),
                     VK_SHADER_STAGE_COMPUTE_BIT, 0U, sizeof(PushConstants),
                     &pushConstants);
  vkCmdDispatch(commandBuffer_, static_cast<std::uint32_t>(groups), 1U, 1U);
  VkBufferMemoryBarrier outputBarrier{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
                                      nullptr,
                                      VK_ACCESS_SHADER_WRITE_BIT,
                                      VK_ACCESS_TRANSFER_READ_BIT,
                                      VK_QUEUE_FAMILY_IGNORED,
                                      VK_QUEUE_FAMILY_IGNORED,
                                      output.handle(),
                                      0U,
                                      ranges[4U]};
  vkCmdPipelineBarrier(commandBuffer_, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                       VK_PIPELINE_STAGE_TRANSFER_BIT, 0U, 0U, nullptr, 1U,
                       &outputBarrier, 0U, nullptr);
  if (vkEndCommandBuffer(commandBuffer_) != VK_SUCCESS) {
    return setError(error, "dispatch", "failed to end command buffer");
  }
  VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
  submit.commandBufferCount = 1U;
  submit.pCommandBuffers = &commandBuffer_;
  if (vkQueueSubmit(session_->device().computeQueue(), 1U, &submit,
                    fence_.get()) != VK_SUCCESS ||
      !fence_.wait(10'000'000'000ULL, error)) {
    return setError(error, "dispatch",
                    "device graph diffusion submission failed");
  }
  fence_.reset();
  return true;
}

const runtime::VulkanDevice &SurfaceGraphDiffusionFp32::device() const {
  return session_->device();
}

} // namespace viennaps::vulkan::surface
