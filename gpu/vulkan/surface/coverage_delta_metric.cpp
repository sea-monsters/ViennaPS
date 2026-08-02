// Copyright 2026 ViennaPS
// SPDX-License-Identifier: MIT

#include "coverage_delta_metric.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <sstream>
#include <string_view>

namespace {

[[nodiscard]] bool fail(std::string &error, const std::string_view phase,
                        const std::string_view message) {
  error = std::string(phase);
  if (!message.empty()) {
    error += ": ";
    error += message;
  }
  return false;
}

[[nodiscard]] bool normalOrZero(const float value) {
  return value == 0.0F || std::isnormal(value);
}

template <class Buffer>
[[nodiscard]] bool hasFloatCapacity(const Buffer &buffer,
                                    const std::size_t count) {
  return buffer.isValid() &&
         count <= static_cast<std::size_t>(buffer.size() / sizeof(float));
}

[[nodiscard]] bool checkedProduct(const std::size_t left,
                                  const std::size_t right,
                                  std::size_t &product) {
  if (left != 0U && right > std::numeric_limits<std::size_t>::max() / left)
    return false;
  product = left * right;
  return true;
}

[[nodiscard]] bool strictOracle(std::span<const float> updated,
                                std::span<const float> previous,
                                const std::size_t channelCount,
                                const std::size_t pointCount,
                                std::string &error) {
  if (pointCount == 0U)
    return fail(error, "validation", "point count must be nonzero");
  for (std::size_t channel = 0U; channel < channelCount; ++channel) {
    volatile float sum = 0.0F;
    const auto base = channel * pointCount;
    for (std::size_t point = 0U; point < pointCount; ++point) {
      const float updatedValue = updated[base + point];
      const float previousValue = previous[base + point];
      if (!normalOrZero(updatedValue) || !normalOrZero(previousValue))
        return fail(error, "validation",
                    "coverage values must be finite normal FP32 or zero");
      volatile float difference = updatedValue - previousValue;
      if (!normalOrZero(difference))
        return fail(error, "validation",
                    "coverage difference leaves the strict FP32 domain");
      volatile float square = difference * difference;
      if (!normalOrZero(square))
        return fail(error, "validation",
                    "coverage square leaves the strict FP32 domain");
      sum = sum + square;
      if (!normalOrZero(sum))
        return fail(error, "validation",
                    "coverage sum leaves the strict FP32 domain");
    }
    volatile float mean = sum / static_cast<float>(pointCount);
    if (!normalOrZero(mean))
      return fail(error, "validation",
                  "coverage mean leaves the strict FP32 domain");
  }
  return true;
}

} // namespace

namespace viennaps::vulkan::surface {

CoverageDeltaMetricFp32::~CoverageDeltaMetricFp32() { reset(); }

bool CoverageDeltaMetricFp32::initialize(const std::string_view spirvPath,
                                         std::string &error) {
  error.clear();
  return setup(spirvPath, nullptr, error);
}

bool CoverageDeltaMetricFp32::initialize(runtime::ComputeSession &session,
                                         const std::string_view spirvPath,
                                         std::string &error) {
  error.clear();
  return setup(spirvPath, &session, error);
}

bool CoverageDeltaMetricFp32::setup(const std::string_view spirvPath,
                                    runtime::ComputeSession *externalSession,
                                    std::string &error) {
  if (spirvPath.empty())
    return fail(error, "initialization", "SPIR-V path is empty");
  reset();
  const auto failInitialization = [this, &error]() {
    const auto detail = error;
    reset();
    return fail(error, "initialization", detail);
  };
  if (externalSession != nullptr) {
    if (!externalSession->isValid()) {
      error = "external compute session is not initialized";
      return failInitialization();
    }
    session_ = externalSession;
  } else {
    if (!ownedSession_.initialize(error))
      return failInitialization();
    session_ = &ownedSession_;
  }
  runtime::SpirvProgram program{};
  if (!runtime::readSpirv(spirvPath, program, error) ||
      !shaderModule_.create(session_->device(), program, error))
    return failInitialization();

  std::array<VkDescriptorSetLayoutBinding, 3U> bindings{};
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
          error))
    return failInitialization();
  const VkPushConstantRange pushRange{VK_SHADER_STAGE_COMPUTE_BIT, 0U,
                                      sizeof(PushConstants)};
  if (!pipelineLayout_.create(session_->device(), descriptorSetLayout_.get(),
                              std::span(&pushRange, 1U), error) ||
      !descriptorPool_.create(session_->device(), 1U, 3U,
                              VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, error) ||
      !descriptorPool_.allocate(descriptorSetLayout_.get(), descriptorSet_,
                                error) ||
      !session_->commandContext().allocatePrimary(commandBuffer_, error) ||
      !fence_.create(session_->device(), error))
    return failInitialization();
  const runtime::ComputePipelineOptions options{"main", {}, nullptr, 0U};
  if (!pipeline_.create(session_->device(), shaderModule_, pipelineLayout_,
                        options, error))
    return failInitialization();
  return true;
}

void CoverageDeltaMetricFp32::reset() {
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
  if (ownsSession)
    ownedSession_.reset();
}

bool CoverageDeltaMetricFp32::isInitialized() const {
  return session_ != nullptr && session_->isValid() &&
         shaderModule_.get() != VK_NULL_HANDLE &&
         descriptorSetLayout_.get() != VK_NULL_HANDLE &&
         pipelineLayout_.get() != VK_NULL_HANDLE &&
         pipeline_.get() != VK_NULL_HANDLE && descriptorSet_ != VK_NULL_HANDLE &&
         commandBuffer_ != VK_NULL_HANDLE && fence_.get() != VK_NULL_HANDLE;
}

bool CoverageDeltaMetricFp32::isReady(std::string &error) const {
  if (!isInitialized())
    return fail(error, "execution", "coverage delta metric is not initialized");
  return true;
}

bool CoverageDeltaMetricFp32::createFloatBuffer(
    const std::size_t elementCount, runtime::HostVisibleBuffer &buffer,
    std::string &error) {
  if (!isReady(error))
    return false;
  const auto allocated = elementCount == 0U ? 1U : elementCount;
  if (allocated > std::numeric_limits<std::size_t>::max() / sizeof(float))
    return fail(error, "createFloatBuffer", "element count overflows");
  return buffer.create(session_->device(),
                       static_cast<VkDeviceSize>(allocated * sizeof(float)),
                       kFloatBufferUsage, kHostMemoryFlags, error);
}

bool CoverageDeltaMetricFp32::evaluate(
    runtime::HostVisibleBuffer &updated, const std::size_t updatedCount,
    runtime::HostVisibleBuffer &previous, const std::size_t previousCount,
    runtime::HostVisibleBuffer &output, const std::size_t channelCount,
    const std::size_t pointCount, const std::size_t outputCapacity,
    std::string &error) const {
  error.clear();
  if (!isReady(error))
    return false;
  std::size_t totalCount = 0U;
  if (!checkedProduct(channelCount, pointCount, totalCount) ||
      totalCount != updatedCount || totalCount != previousCount ||
      updatedCount > std::numeric_limits<std::size_t>::max() / sizeof(float) ||
      previousCount > std::numeric_limits<std::size_t>::max() / sizeof(float) ||
      outputCapacity > std::numeric_limits<std::size_t>::max() / sizeof(float) ||
      channelCount > std::numeric_limits<std::uint32_t>::max() ||
      pointCount > std::numeric_limits<std::uint32_t>::max() ||
      outputCapacity < channelCount || pointCount == 0U)
    return fail(error, "validation", "coverage dimensions are invalid");
  if (!hasFloatCapacity(updated, updatedCount) ||
      !hasFloatCapacity(previous, previousCount) ||
      !hasFloatCapacity(output, outputCapacity))
    return fail(error, "validation", "coverage buffer capacity is insufficient");
  const std::array<const runtime::HostVisibleBuffer *, 3U> buffers = {
      &updated, &previous, &output};
  for (std::size_t i = 0U; i < buffers.size(); ++i) {
    if (buffers[i]->ownerDevice() != session_->device().get())
      return fail(error, "validation", "coverage buffer belongs to another device");
    for (std::size_t j = i + 1U; j < buffers.size(); ++j)
      if (buffers[i]->handle() == buffers[j]->handle())
        return fail(error, "validation", "coverage buffers must not alias");
  }
  if (!updated.mappedPtr() && !updated.map(error))
    return false;
  if (!previous.mappedPtr() && !previous.map(error))
    return false;
  if (!updated.flush(error) || !previous.flush(error))
    return fail(error, "execution", "failed to flush coverage inputs");
  const auto *updatedValues = static_cast<const float *>(updated.mappedPtr());
  const auto *previousValues = static_cast<const float *>(previous.mappedPtr());
  if (!strictOracle(std::span(updatedValues, updatedCount),
                    std::span(previousValues, previousCount), channelCount,
                    pointCount, error))
    return false;
  if (channelCount == 0U)
    return true;
  const auto groups = channelCount / kWorkgroupSize +
                      (channelCount % kWorkgroupSize != 0U ? 1U : 0U);
  if (groups > session_->selection().properties.limits.maxComputeWorkGroupCount[0])
    return fail(error, "dispatch", "channel count exceeds device limit");

  runtime::HostVisibleBuffer scratch;
  if (!const_cast<CoverageDeltaMetricFp32 *>(this)->createFloatBuffer(
          channelCount, scratch, error) ||
      !scratch.map(error))
    return false;
  const std::array<VkBuffer, 3U> handles = {updated.handle(), previous.handle(),
                                             scratch.handle()};
  const std::array<VkDeviceSize, 3U> ranges = {
      static_cast<VkDeviceSize>(updatedCount * sizeof(float)),
      static_cast<VkDeviceSize>(previousCount * sizeof(float)),
      static_cast<VkDeviceSize>(channelCount * sizeof(float))};
  std::array<VkDescriptorBufferInfo, 3U> infos{};
  std::array<VkWriteDescriptorSet, 3U> writes{};
  for (std::uint32_t binding = 0U; binding < writes.size(); ++binding) {
    infos[binding] = {handles[binding], 0U, ranges[binding]};
    writes[binding] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr,
                       descriptorSet_, binding, 0U, 1U,
                       VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, nullptr,
                       &infos[binding], nullptr};
  }
  vkUpdateDescriptorSets(session_->device().get(),
                         static_cast<std::uint32_t>(writes.size()),
                         writes.data(), 0U, nullptr);
  VkCommandBufferBeginInfo beginInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  if (vkResetCommandBuffer(commandBuffer_, 0U) != VK_SUCCESS ||
      vkBeginCommandBuffer(commandBuffer_, &beginInfo) != VK_SUCCESS)
    return fail(error, "dispatch", "failed to begin command buffer");
  std::array<VkBufferMemoryBarrier, 3U> pre{};
  for (std::uint32_t binding = 0U; binding < pre.size(); ++binding) {
    pre[binding] = {VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
                    nullptr,
                    VK_ACCESS_HOST_WRITE_BIT,
                    static_cast<VkAccessFlags>(
                        binding == 2U ? VK_ACCESS_SHADER_WRITE_BIT
                                      : VK_ACCESS_SHADER_READ_BIT),
                    VK_QUEUE_FAMILY_IGNORED,
                    VK_QUEUE_FAMILY_IGNORED,
                    handles[binding],
                    0U,
                    ranges[binding]};
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
  const PushConstants constants{static_cast<std::uint32_t>(channelCount),
                                static_cast<std::uint32_t>(pointCount)};
  vkCmdPushConstants(commandBuffer_, pipelineLayout_.get(),
                     VK_SHADER_STAGE_COMPUTE_BIT, 0U, sizeof(constants),
                     &constants);
  vkCmdDispatch(commandBuffer_, static_cast<std::uint32_t>(groups), 1U, 1U);
  VkBufferMemoryBarrier post{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
                             nullptr,
                             VK_ACCESS_SHADER_WRITE_BIT,
                             VK_ACCESS_HOST_READ_BIT,
                             VK_QUEUE_FAMILY_IGNORED,
                             VK_QUEUE_FAMILY_IGNORED,
                             scratch.handle(),
                             0U,
                             ranges[2U]};
  vkCmdPipelineBarrier(commandBuffer_, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                       VK_PIPELINE_STAGE_HOST_BIT, 0U, 0U, nullptr, 1U, &post,
                       0U, nullptr);
  if (vkEndCommandBuffer(commandBuffer_) != VK_SUCCESS)
    return fail(error, "dispatch", "failed to end command buffer");
  VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
  submit.commandBufferCount = 1U;
  submit.pCommandBuffers = &commandBuffer_;
  if (vkQueueSubmit(session_->device().computeQueue(), 1U, &submit,
                    fence_.get()) != VK_SUCCESS)
    return fail(error, "dispatch", "failed to submit command buffer");
  if (!fence_.wait(10'000'000'000ULL, error))
    return false;
  fence_.reset();
  if (!scratch.invalidate(error))
    return fail(error, "execution", "failed to invalidate metric output");
  if (!output.mappedPtr() && !output.map(error))
    return false;
  std::memcpy(output.mappedPtr(), scratch.mappedPtr(),
              channelCount * sizeof(float));
  if (!output.flush(error))
    return fail(error, "execution", "failed to publish metric output");
  return true;
}

bool CoverageDeltaMetricFp32::evaluateDevice(
    runtime::DeviceBuffer &updated, const std::size_t updatedCount,
    runtime::DeviceBuffer &previous, const std::size_t previousCount,
    runtime::DeviceBuffer &output, const std::size_t channelCount,
    const std::size_t pointCount, const std::size_t outputCapacity,
    const std::span<const float> updatedValues,
    const std::span<const float> previousValues, std::string &error) const {
  error.clear();
  if (!isReady(error))
    return false;
  std::size_t totalCount = 0U;
  if (!checkedProduct(channelCount, pointCount, totalCount) ||
      totalCount != updatedCount || totalCount != previousCount ||
      updatedValues.size() != updatedCount || previousValues.size() != previousCount ||
      updatedCount > std::numeric_limits<std::size_t>::max() / sizeof(float) ||
      previousCount > std::numeric_limits<std::size_t>::max() / sizeof(float) ||
      outputCapacity > std::numeric_limits<std::size_t>::max() / sizeof(float) ||
      channelCount > std::numeric_limits<std::uint32_t>::max() ||
      pointCount > std::numeric_limits<std::uint32_t>::max() ||
      outputCapacity < channelCount || pointCount == 0U)
    return fail(error, "validation", "device coverage dimensions are invalid");
  const std::array<const runtime::DeviceBuffer *, 3U> buffers = {&updated,
                                                                  &previous,
                                                                  &output};
  for (std::size_t i = 0U; i < buffers.size(); ++i) {
    if (!buffers[i]->isValid() ||
        buffers[i]->ownerSessionGeneration() != session_->generation() ||
        buffers[i]->ownerDevice() != session_->device().get())
      return fail(error, "validation", "device buffer session is stale or foreign");
    for (std::size_t j = i + 1U; j < buffers.size(); ++j)
      if (buffers[i]->handle() == buffers[j]->handle())
        return fail(error, "validation", "device coverage buffers must not alias");
  }
  if (updated.size() < updatedCount * sizeof(float) ||
      previous.size() < previousCount * sizeof(float) ||
      output.size() < outputCapacity * sizeof(float))
    return fail(error, "validation", "device coverage buffer capacity is insufficient");
  if (!strictOracle(updatedValues, previousValues, channelCount, pointCount,
                    error))
    return false;
  if (channelCount == 0U)
    return true;
  const auto groups = channelCount / kWorkgroupSize +
                      (channelCount % kWorkgroupSize != 0U ? 1U : 0U);
  if (groups > session_->selection().properties.limits.maxComputeWorkGroupCount[0])
    return fail(error, "dispatch", "channel count exceeds device limit");
  runtime::DeviceBuffer scratch;
  const auto bytes = static_cast<VkDeviceSize>(channelCount * sizeof(float));
  if (!scratch.create(*session_, bytes, error))
    return false;
  const std::array<VkBuffer, 3U> handles = {updated.handle(), previous.handle(),
                                             scratch.handle()};
  const std::array<VkDeviceSize, 3U> ranges = {
      static_cast<VkDeviceSize>(updatedCount * sizeof(float)),
      static_cast<VkDeviceSize>(previousCount * sizeof(float)), bytes};
  std::array<VkDescriptorBufferInfo, 3U> infos{};
  std::array<VkWriteDescriptorSet, 3U> writes{};
  for (std::uint32_t binding = 0U; binding < writes.size(); ++binding) {
    infos[binding] = {handles[binding], 0U, ranges[binding]};
    writes[binding] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr,
                       descriptorSet_, binding, 0U, 1U,
                       VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, nullptr,
                       &infos[binding], nullptr};
  }
  vkUpdateDescriptorSets(session_->device().get(),
                         static_cast<std::uint32_t>(writes.size()),
                         writes.data(), 0U, nullptr);
  VkCommandBufferBeginInfo beginInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  if (vkResetCommandBuffer(commandBuffer_, 0U) != VK_SUCCESS ||
      vkBeginCommandBuffer(commandBuffer_, &beginInfo) != VK_SUCCESS)
    return fail(error, "dispatch", "failed to begin command buffer");
  std::array<VkBufferMemoryBarrier, 3U> barriers{};
  for (std::uint32_t binding = 0U; binding < barriers.size(); ++binding) {
    barriers[binding] = {VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
                         nullptr,
                         VK_ACCESS_TRANSFER_WRITE_BIT,
                         static_cast<VkAccessFlags>(
                             binding == 2U ? VK_ACCESS_SHADER_WRITE_BIT
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
  const PushConstants constants{static_cast<std::uint32_t>(channelCount),
                                static_cast<std::uint32_t>(pointCount)};
  vkCmdPushConstants(commandBuffer_, pipelineLayout_.get(),
                     VK_SHADER_STAGE_COMPUTE_BIT, 0U, sizeof(constants),
                     &constants);
  vkCmdDispatch(commandBuffer_, static_cast<std::uint32_t>(groups), 1U, 1U);
  VkBufferMemoryBarrier outputBarrier{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
                                      nullptr,
                                      VK_ACCESS_SHADER_WRITE_BIT,
                                      VK_ACCESS_TRANSFER_READ_BIT,
                                      VK_QUEUE_FAMILY_IGNORED,
                                      VK_QUEUE_FAMILY_IGNORED,
                                      scratch.handle(),
                                      0U,
                                      bytes};
  vkCmdPipelineBarrier(commandBuffer_, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                       VK_PIPELINE_STAGE_TRANSFER_BIT, 0U, 0U, nullptr, 1U,
                       &outputBarrier, 0U, nullptr);
  if (vkEndCommandBuffer(commandBuffer_) != VK_SUCCESS)
    return fail(error, "dispatch", "failed to end command buffer");
  VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
  submit.commandBufferCount = 1U;
  submit.pCommandBuffers = &commandBuffer_;
  if (vkQueueSubmit(session_->device().computeQueue(), 1U, &submit,
                    fence_.get()) != VK_SUCCESS ||
      !fence_.wait(10'000'000'000ULL, error))
    return fail(error, "dispatch", "device metric submission failed");
  fence_.reset();
  if (!scratch.copyTo(*session_, output, bytes, 0U, 0U, error))
    return false;
  return true;
}

const runtime::VulkanDevice &CoverageDeltaMetricFp32::device() const {
  return session_->device();
}

} // namespace viennaps::vulkan::surface
