// Copyright 2026 ViennaPS
// SPDX-License-Identifier: MIT
#include "ray_surface_reduction.hpp"

#include <array>
#include <limits>
#include <span>

namespace viennaps::vulkan::ray {
namespace {
constexpr std::uint32_t kWorkgroupSize = 64U;

bool fail(std::string &error, const char *message) {
  error = message;
  return false;
}

bool bytesFor(std::size_t count, std::size_t elementBytes, std::size_t &bytes) {
  if (elementBytes != 0U &&
      count > std::numeric_limits<std::size_t>::max() / elementBytes)
    return false;
  bytes = count * elementBytes;
  return true;
}

std::size_t ceilDivide(std::size_t dividend, std::size_t divisor) {
  return dividend / divisor +
         static_cast<std::size_t>(dividend % divisor != 0U);
}
} // namespace

DeviceRaySurfaceReducer::~DeviceRaySurfaceReducer() { reset(); }

bool DeviceRaySurfaceReducer::setup(std::string_view segmentSpirv,
                                    std::string_view reduceSpirv,
                                    std::string_view scanSpirv,
                                    runtime::ComputeSession *external,
                                    std::string &error) {
  if (segmentSpirv.empty() || reduceSpirv.empty() || scanSpirv.empty())
    return fail(error, "ray surface-reduction SPIR-V path is empty");
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
  if (!scan_.initialize(*session_, scanSpirv, error)) {
    reset();
    return false;
  }
  const std::array<std::string_view, 2U> paths{segmentSpirv, reduceSpirv};
  for (std::size_t i = 0U; i < paths.size(); ++i) {
    runtime::SpirvProgram program{};
    if (!runtime::readSpirv(paths[i], program, error) ||
        !shaderModules_[i].create(session_->device(), program, error)) {
      reset();
      return false;
    }
  }
  std::array<VkDescriptorSetLayoutBinding, 6U> bindings{};
  for (std::uint32_t i = 0U; i < bindings.size(); ++i)
    bindings[i] = {i, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1U,
                   VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
  const VkPushConstantRange push{VK_SHADER_STAGE_COMPUTE_BIT, 0U,
                                 sizeof(std::uint32_t)};
  if (!descriptorSetLayout_.create(session_->device(), bindings, error) ||
      !pipelineLayout_.create(session_->device(), descriptorSetLayout_.get(),
                              std::span<const VkPushConstantRange>(&push, 1U),
                              error) ||
      !descriptorPool_.create(session_->device(), 1U,
                              static_cast<std::uint32_t>(bindings.size()),
                              VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, error) ||
      !descriptorPool_.allocate(descriptorSetLayout_.get(), descriptorSet_,
                                error)) {
    reset();
    return false;
  }
  for (std::size_t i = 0U; i < pipelines_.size(); ++i)
    if (!pipelines_[i].create(session_->device(), shaderModules_[i],
                              pipelineLayout_,
                              runtime::ComputePipelineOptions{}, error)) {
      reset();
      return false;
    }
  if (!session_->commandContext().allocatePrimary(commandBuffer_, error) ||
      !fence_.create(session_->device(), error)) {
    reset();
    return false;
  }
  return true;
}

bool DeviceRaySurfaceReducer::initialize(std::string_view segmentSpirv,
                                         std::string_view reduceSpirv,
                                         std::string_view scanSpirv,
                                         std::string &error) {
  error.clear();
  return setup(segmentSpirv, reduceSpirv, scanSpirv, nullptr, error);
}

bool DeviceRaySurfaceReducer::initialize(runtime::ComputeSession &session,
                                         std::string_view segmentSpirv,
                                         std::string_view reduceSpirv,
                                         std::string_view scanSpirv,
                                         std::string &error) {
  error.clear();
  return setup(segmentSpirv, reduceSpirv, scanSpirv, &session, error);
}

void DeviceRaySurfaceReducer::reset() {
  const bool ownsSession = session_ == &ownedSession_;
  fence_.destroy();
  commandBuffer_ = VK_NULL_HANDLE;
  descriptorSet_ = VK_NULL_HANDLE;
  for (auto &pipeline : pipelines_)
    pipeline.reset();
  descriptorPool_.reset();
  pipelineLayout_.reset();
  descriptorSetLayout_.reset();
  for (auto &module : shaderModules_)
    module.reset();
  scan_.reset();
  session_ = nullptr;
  if (ownsSession)
    ownedSession_.reset();
}

bool DeviceRaySurfaceReducer::isInitialized() const {
  if (session_ == nullptr || !session_->isValid() || !scan_.isInitialized() ||
      descriptorSetLayout_.get() == VK_NULL_HANDLE ||
      pipelineLayout_.get() == VK_NULL_HANDLE ||
      descriptorPool_.get() == VK_NULL_HANDLE ||
      descriptorSet_ == VK_NULL_HANDLE || commandBuffer_ == VK_NULL_HANDLE ||
      fence_.get() == VK_NULL_HANDLE)
    return false;
  for (const auto &pipeline : pipelines_)
    if (pipeline.get() == VK_NULL_HANDLE)
      return false;
  return true;
}

bool DeviceRaySurfaceReducer::ready(std::string &error) const {
  return isInitialized()
             ? true
             : fail(error, "ray surface reducer is not initialized");
}

const runtime::VulkanDevice &DeviceRaySurfaceReducer::device() const {
  static const runtime::VulkanDevice empty{};
  return session_ == nullptr ? empty : session_->device();
}

bool DeviceRaySurfaceReducer::reduce(const runtime::DeviceBuffer &inputRecords,
                                     const runtime::DeviceBuffer &inputCount,
                                     const std::size_t inputCapacity,
                                     runtime::DeviceBuffer &outputSurfaceId,
                                     runtime::DeviceBuffer &outputWeight,
                                     const std::size_t outputCapacity,
                                     DeviceRaySurfaceReductionOutput &output,
                                     std::string &error) {
  error.clear();
  if (!ready(error))
    return false;
  if (inputCapacity > std::numeric_limits<std::uint32_t>::max() ||
      outputCapacity < inputCapacity)
    return fail(error, "ray surface-reduction capacity is invalid");
  if (inputCapacity == 0U)
    return true;
  std::size_t recordBytes{}, indexBytes{}, weightBytes{};
  if (!bytesFor(inputCapacity, sizeof(RayRecord), recordBytes) ||
      !bytesFor(inputCapacity, sizeof(std::uint32_t), indexBytes) ||
      !bytesFor(inputCapacity, sizeof(float), weightBytes))
    return fail(error, "ray surface-reduction buffer size overflows");
  const auto groups = ceilDivide(inputCapacity, kWorkgroupSize);
  const auto &limits = session_->device().selection().properties.limits;
  if (groups > limits.maxComputeWorkGroupCount[0] ||
      limits.maxComputeWorkGroupInvocations < kWorkgroupSize ||
      limits.maxComputeWorkGroupSize[0] < kWorkgroupSize)
    return fail(
        error, "ray surface-reduction dispatch exceeds device workgroup limit");
  const auto storageLimit = limits.maxStorageBufferRange;
  if (static_cast<VkDeviceSize>(recordBytes) > storageLimit ||
      static_cast<VkDeviceSize>(indexBytes) > storageLimit ||
      static_cast<VkDeviceSize>(weightBytes) > storageLimit)
    return fail(error,
                "ray surface-reduction buffers exceed storage-buffer range");
  const auto sameSession = [&](const runtime::DeviceBuffer &buffer) {
    return buffer.isValid() &&
           buffer.ownerDevice() == session_->deviceHandle() &&
           buffer.ownerSessionGeneration() == session_->generation();
  };
  if (!sameSession(inputRecords) || !sameSession(inputCount) ||
      !sameSession(outputSurfaceId) || !sameSession(outputWeight) ||
      inputRecords.size() < static_cast<VkDeviceSize>(recordBytes) ||
      inputCount.size() < sizeof(std::uint32_t) ||
      outputSurfaceId.size() < static_cast<VkDeviceSize>(indexBytes) ||
      outputWeight.size() < static_cast<VkDeviceSize>(weightBytes))
    return fail(
        error, "ray surface-reduction buffer is invalid, stale, or undersized");
  const std::array<VkBuffer, 4U> callerHandles{
      inputRecords.handle(), inputCount.handle(), outputSurfaceId.handle(),
      outputWeight.handle()};
  for (std::size_t i = 0U; i < callerHandles.size(); ++i)
    for (std::size_t j = i + 1U; j < callerHandles.size(); ++j)
      if (callerHandles[i] == callerHandles[j])
        return fail(error, "ray surface-reduction buffers must not alias");

  runtime::DeviceBuffer flags{}, offsets{}, count{};
  if (!flags.create(*session_, static_cast<VkDeviceSize>(indexBytes), error) ||
      !offsets.create(*session_, static_cast<VkDeviceSize>(indexBytes),
                      error) ||
      !count.create(*session_, sizeof(std::uint32_t), error))
    return false;
  const std::array<VkDescriptorBufferInfo, 6U> infos{
      {{inputRecords.handle(), 0U, static_cast<VkDeviceSize>(recordBytes)},
       {inputCount.handle(), 0U, sizeof(std::uint32_t)},
       {flags.handle(), 0U, static_cast<VkDeviceSize>(indexBytes)},
       {offsets.handle(), 0U, static_cast<VkDeviceSize>(indexBytes)},
       {outputSurfaceId.handle(), 0U, static_cast<VkDeviceSize>(indexBytes)},
       {outputWeight.handle(), 0U, static_cast<VkDeviceSize>(weightBytes)}}};
  std::array<VkWriteDescriptorSet, 6U> writes{};
  for (std::uint32_t i = 0U; i < writes.size(); ++i)
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
  vkUpdateDescriptorSets(session_->deviceHandle(),
                         static_cast<std::uint32_t>(writes.size()),
                         writes.data(), 0U, nullptr);
  const std::uint32_t capacity = static_cast<std::uint32_t>(inputCapacity);
  const VkMemoryBarrier pre{
      VK_STRUCTURE_TYPE_MEMORY_BARRIER, nullptr,
      VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_SHADER_WRITE_BIT,
      VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT};
  const VkMemoryBarrier computeBarrier{
      VK_STRUCTURE_TYPE_MEMORY_BARRIER, nullptr, VK_ACCESS_SHADER_WRITE_BIT,
      VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT};
  if (vkResetCommandBuffer(commandBuffer_, 0U) != VK_SUCCESS)
    return fail(error, "failed to reset ray surface-reduction command buffer");
  VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  if (vkBeginCommandBuffer(commandBuffer_, &begin) != VK_SUCCESS)
    return fail(error, "failed to begin ray surface-reduction command buffer");
  vkCmdPipelineBarrier(commandBuffer_,
                       VK_PIPELINE_STAGE_TRANSFER_BIT |
                           VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                       VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0U, 1U, &pre, 0U,
                       nullptr, 0U, nullptr);
  vkCmdBindDescriptorSets(commandBuffer_, VK_PIPELINE_BIND_POINT_COMPUTE,
                          pipelineLayout_.get(), 0U, 1U, &descriptorSet_, 0U,
                          nullptr);
  vkCmdBindPipeline(commandBuffer_, VK_PIPELINE_BIND_POINT_COMPUTE,
                    pipelines_[0].get());
  vkCmdPushConstants(commandBuffer_, pipelineLayout_.get(),
                     VK_SHADER_STAGE_COMPUTE_BIT, 0U, sizeof(capacity),
                     &capacity);
  vkCmdDispatch(commandBuffer_, static_cast<std::uint32_t>(groups), 1U, 1U);
  vkCmdPipelineBarrier(commandBuffer_, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                       VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0U, 1U,
                       &computeBarrier, 0U, nullptr, 0U, nullptr);
  if (vkEndCommandBuffer(commandBuffer_) != VK_SUCCESS)
    return fail(error, "failed to end ray surface-segment command buffer");
  VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
  submit.commandBufferCount = 1U;
  submit.pCommandBuffers = &commandBuffer_;
  if (vkQueueSubmit(session_->device().computeQueue(), 1U, &submit,
                    fence_.get()) != VK_SUCCESS ||
      !fence_.wait(10'000'000'000ULL, error))
    return false;
  fence_.reset();
  if (!scan_.exclusiveScanInt(flags, inputCapacity, offsets, inputCapacity,
                              error) ||
      !scan_.writeCompactionCount(flags, offsets, inputCapacity, count, error))
    return false;

  if (vkResetCommandBuffer(commandBuffer_, 0U) != VK_SUCCESS)
    return fail(error, "failed to reset ray surface-reduction command buffer");
  if (vkBeginCommandBuffer(commandBuffer_, &begin) != VK_SUCCESS)
    return fail(error, "failed to begin ray surface-reduction command buffer");
  vkCmdPipelineBarrier(commandBuffer_,
                       VK_PIPELINE_STAGE_TRANSFER_BIT |
                           VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                       VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0U, 1U, &pre, 0U,
                       nullptr, 0U, nullptr);
  vkCmdBindDescriptorSets(commandBuffer_, VK_PIPELINE_BIND_POINT_COMPUTE,
                          pipelineLayout_.get(), 0U, 1U, &descriptorSet_, 0U,
                          nullptr);
  vkCmdBindPipeline(commandBuffer_, VK_PIPELINE_BIND_POINT_COMPUTE,
                    pipelines_[1].get());
  vkCmdPushConstants(commandBuffer_, pipelineLayout_.get(),
                     VK_SHADER_STAGE_COMPUTE_BIT, 0U, sizeof(capacity),
                     &capacity);
  vkCmdDispatch(commandBuffer_, static_cast<std::uint32_t>(groups), 1U, 1U);
  const VkMemoryBarrier post{
      VK_STRUCTURE_TYPE_MEMORY_BARRIER, nullptr, VK_ACCESS_SHADER_WRITE_BIT,
      VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT};
  vkCmdPipelineBarrier(commandBuffer_, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                       VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT |
                           VK_PIPELINE_STAGE_TRANSFER_BIT,
                       0U, 1U, &post, 0U, nullptr, 0U, nullptr);
  if (vkEndCommandBuffer(commandBuffer_) != VK_SUCCESS)
    return fail(error, "failed to end ray surface-reduction command buffer");
  if (vkQueueSubmit(session_->device().computeQueue(), 1U, &submit,
                    fence_.get()) != VK_SUCCESS ||
      !fence_.wait(10'000'000'000ULL, error))
    return false;
  fence_.reset();
  output.flags = std::move(flags);
  output.offsets = std::move(offsets);
  output.count = std::move(count);
  output.inputCapacity = capacity;
  output.sessionGeneration = session_->generation();
  return true;
}

} // namespace viennaps::vulkan::ray
