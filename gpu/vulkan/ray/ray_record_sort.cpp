// Copyright 2026 ViennaPS
// SPDX-License-Identifier: MIT
#include "ray_record_sort.hpp"

#include <array>
#include <limits>
#include <span>

namespace viennaps::vulkan::ray {
namespace {
[[nodiscard]] bool fail(std::string &error, const char *message) {
  error = message;
  return false;
}
[[nodiscard]] bool bytesFor(const std::size_t count, const std::size_t size,
                            std::size_t &bytes) {
  if (size != 0U && count > std::numeric_limits<std::size_t>::max() / size)
    return false;
  bytes = count * size;
  return true;
}
} // namespace

DeviceRayRecordSort::~DeviceRayRecordSort() { reset(); }

bool DeviceRayRecordSort::setup(const std::string_view spirvPath,
                                runtime::ComputeSession *external,
                                std::string &error) {
  if (spirvPath.empty())
    return fail(error, "ray-record sort SPIR-V path is empty");
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
  runtime::SpirvProgram program{};
  if (!runtime::readSpirv(spirvPath, program, error) ||
      !shaderModule_.create(session_->device(), program, error)) {
    reset();
    return false;
  }
  std::array<VkDescriptorSetLayoutBinding, 3U> bindings{};
  for (std::uint32_t i = 0U; i < bindings.size(); ++i)
    bindings[i] = {i, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1U,
                   VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
  const VkPushConstantRange push{VK_SHADER_STAGE_COMPUTE_BIT, 0U,
                                 sizeof(std::uint32_t)};
  if (!descriptorSetLayout_.create(session_->device(), bindings, error) ||
      !pipelineLayout_.create(session_->device(), descriptorSetLayout_.get(),
                              std::span<const VkPushConstantRange>(&push, 1U),
                              error) ||
      !descriptorPool_.create(session_->device(), 1U, 3U,
                              VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, error) ||
      !descriptorPool_.allocate(descriptorSetLayout_.get(), descriptorSet_,
                                error) ||
      !session_->commandContext().allocatePrimary(commandBuffer_, error) ||
      !fence_.create(session_->device(), error) ||
      !pipeline_.create(session_->device(), shaderModule_, pipelineLayout_,
                        runtime::ComputePipelineOptions{}, error)) {
    reset();
    return false;
  }
  return true;
}

bool DeviceRayRecordSort::initialize(const std::string_view spirvPath,
                                     std::string &error) {
  error.clear();
  return setup(spirvPath, nullptr, error);
}
bool DeviceRayRecordSort::initialize(runtime::ComputeSession &session,
                                     const std::string_view spirvPath,
                                     std::string &error) {
  error.clear();
  return setup(spirvPath, &session, error);
}

void DeviceRayRecordSort::reset() {
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
  if (ownsSession)
    ownedSession_.reset();
}

bool DeviceRayRecordSort::isInitialized() const {
  return session_ != nullptr && session_->isValid() &&
         shaderModule_.get() != VK_NULL_HANDLE &&
         descriptorSetLayout_.get() != VK_NULL_HANDLE &&
         pipelineLayout_.get() != VK_NULL_HANDLE &&
         pipeline_.get() != VK_NULL_HANDLE &&
         descriptorPool_.get() != VK_NULL_HANDLE &&
         descriptorSet_ != VK_NULL_HANDLE && commandBuffer_ != VK_NULL_HANDLE &&
         fence_.get() != VK_NULL_HANDLE;
}
bool DeviceRayRecordSort::ready(std::string &error) const {
  return isInitialized() ? true
                         : fail(error, "ray-record sorter is not initialized");
}
const runtime::VulkanDevice &DeviceRayRecordSort::device() const {
  static const runtime::VulkanDevice empty{};
  return session_ != nullptr ? session_->device() : empty;
}

bool DeviceRayRecordSort::sort(const runtime::DeviceBuffer &inputRecords,
                               const runtime::DeviceBuffer &inputCount,
                               const std::size_t inputCapacity,
                               runtime::DeviceBuffer &outputRecords,
                               const std::size_t outputCapacity,
                               std::string &error) {
  error.clear();
  if (!ready(error))
    return false;
  if (inputCapacity > std::numeric_limits<std::uint32_t>::max() ||
      outputCapacity < inputCapacity)
    return fail(error, "ray-record sort capacity is invalid");
  std::size_t inputBytes{}, outputBytes{};
  if (!bytesFor(inputCapacity, sizeof(RayRecord), inputBytes) ||
      !bytesFor(outputCapacity, sizeof(RayRecord), outputBytes))
    return fail(error, "ray-record sort buffer size overflows");
  if (inputCapacity == 0U)
    return true;
  if (!inputRecords.isValid() || !inputCount.isValid() ||
      !outputRecords.isValid() ||
      inputRecords.ownerDevice() != session_->deviceHandle() ||
      inputCount.ownerDevice() != session_->deviceHandle() ||
      outputRecords.ownerDevice() != session_->deviceHandle() ||
      inputRecords.ownerSessionGeneration() != session_->generation() ||
      inputCount.ownerSessionGeneration() != session_->generation() ||
      outputRecords.ownerSessionGeneration() != session_->generation() ||
      inputRecords.size() < static_cast<VkDeviceSize>(inputBytes) ||
      inputCount.size() < sizeof(std::uint32_t) ||
      outputRecords.size() < static_cast<VkDeviceSize>(outputBytes))
    return fail(error,
                "ray-record sort buffer is invalid, stale, or undersized");
  if (inputRecords.handle() == inputCount.handle() ||
      inputRecords.handle() == outputRecords.handle() ||
      inputCount.handle() == outputRecords.handle())
    return fail(error, "ray-record sort buffers must not alias");

  const std::array<VkDescriptorBufferInfo, 3U> infos{
      {{inputRecords.handle(), 0U, VK_WHOLE_SIZE},
       {inputCount.handle(), 0U, sizeof(std::uint32_t)},
       {outputRecords.handle(), 0U, VK_WHOLE_SIZE}}};
  std::array<VkWriteDescriptorSet, 3U> writes{};
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
  vkUpdateDescriptorSets(session_->deviceHandle(), 3U, writes.data(), 0U,
                         nullptr);
  if (vkResetCommandBuffer(commandBuffer_, 0U) != VK_SUCCESS)
    return fail(error, "failed to reset ray-record sort command buffer");
  VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  if (vkBeginCommandBuffer(commandBuffer_, &begin) != VK_SUCCESS)
    return fail(error, "failed to begin ray-record sort command buffer");
  const std::array<VkBufferMemoryBarrier, 3U> pre{
      {{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER, nullptr,
        VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_SHADER_WRITE_BIT,
        VK_ACCESS_SHADER_READ_BIT, VK_QUEUE_FAMILY_IGNORED,
        VK_QUEUE_FAMILY_IGNORED, inputRecords.handle(), 0U, VK_WHOLE_SIZE},
       {VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER, nullptr,
        VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_SHADER_WRITE_BIT,
        VK_ACCESS_SHADER_READ_BIT, VK_QUEUE_FAMILY_IGNORED,
        VK_QUEUE_FAMILY_IGNORED, inputCount.handle(), 0U, VK_WHOLE_SIZE},
       {VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER, nullptr,
        VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_SHADER_WRITE_BIT,
        VK_ACCESS_SHADER_WRITE_BIT, VK_QUEUE_FAMILY_IGNORED,
        VK_QUEUE_FAMILY_IGNORED, outputRecords.handle(), 0U, VK_WHOLE_SIZE}}};
  vkCmdPipelineBarrier(
      commandBuffer_,
      VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
      VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0U, 0U, nullptr,
      static_cast<std::uint32_t>(pre.size()), pre.data(), 0U, nullptr);
  vkCmdBindPipeline(commandBuffer_, VK_PIPELINE_BIND_POINT_COMPUTE,
                    pipeline_.get());
  vkCmdBindDescriptorSets(commandBuffer_, VK_PIPELINE_BIND_POINT_COMPUTE,
                          pipelineLayout_.get(), 0U, 1U, &descriptorSet_, 0U,
                          nullptr);
  const auto capacity = static_cast<std::uint32_t>(inputCapacity);
  vkCmdPushConstants(commandBuffer_, pipelineLayout_.get(),
                     VK_SHADER_STAGE_COMPUTE_BIT, 0U, sizeof(capacity),
                     &capacity);
  const auto dispatchGroups =
      static_cast<std::uint32_t>((inputCapacity + 63U) / 64U);
  vkCmdDispatch(commandBuffer_, dispatchGroups, 1U, 1U);
  const VkBufferMemoryBarrier post{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
                                   nullptr,
                                   VK_ACCESS_SHADER_WRITE_BIT,
                                   VK_ACCESS_SHADER_READ_BIT |
                                       VK_ACCESS_SHADER_WRITE_BIT |
                                       VK_ACCESS_TRANSFER_READ_BIT,
                                   VK_QUEUE_FAMILY_IGNORED,
                                   VK_QUEUE_FAMILY_IGNORED,
                                   outputRecords.handle(),
                                   0U,
                                   VK_WHOLE_SIZE};
  vkCmdPipelineBarrier(commandBuffer_, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                       VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT |
                           VK_PIPELINE_STAGE_TRANSFER_BIT,
                       0U, 0U, nullptr, 1U, &post, 0U, nullptr);
  if (vkEndCommandBuffer(commandBuffer_) != VK_SUCCESS)
    return fail(error, "failed to end ray-record sort command buffer");
  VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
  submit.commandBufferCount = 1U;
  submit.pCommandBuffers = &commandBuffer_;
  if (vkQueueSubmit(session_->device().computeQueue(), 1U, &submit,
                    fence_.get()) != VK_SUCCESS ||
      !fence_.wait(10'000'000'000ULL, error))
    return false;
  fence_.reset();
  return true;
}
} // namespace viennaps::vulkan::ray
