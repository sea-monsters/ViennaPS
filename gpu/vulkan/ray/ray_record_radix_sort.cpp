// Copyright 2026 ViennaPS
// SPDX-License-Identifier: MIT
#include "ray_record_radix_sort.hpp"

#include <array>
#include <limits>
#include <span>

namespace viennaps::vulkan::ray {
namespace {
bool fail(std::string &error, const char *message) {
  error = message;
  return false;
}
bool bytesFor(std::size_t count, std::size_t size, std::size_t &bytes) {
  if (size != 0U && count > std::numeric_limits<std::size_t>::max() / size)
    return false;
  bytes = count * size;
  return true;
}
} // namespace

DeviceRayRecordRadixSort::~DeviceRayRecordRadixSort() { reset(); }

bool DeviceRayRecordRadixSort::setup(std::string_view histogramSpirv,
                                     std::string_view prefixSpirv,
                                     std::string_view scatterSpirv,
                                     runtime::ComputeSession *external,
                                     std::string &error) {
  if (histogramSpirv.empty() || prefixSpirv.empty() || scatterSpirv.empty())
    return fail(error, "ray-record radix SPIR-V path is empty");
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
  const std::array<std::string_view, 3U> paths{histogramSpirv, prefixSpirv,
                                               scatterSpirv};
  for (std::size_t i = 0U; i < paths.size(); ++i) {
    runtime::SpirvProgram program{};
    if (!runtime::readSpirv(paths[i], program, error) ||
        !shaderModules_[i].create(session_->device(), program, error)) {
      reset();
      return false;
    }
  }
  std::array<VkDescriptorSetLayoutBinding, 8U> bindings{};
  for (std::uint32_t i = 0U; i < bindings.size(); ++i)
    bindings[i] = {i, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1U,
                   VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
  const VkPushConstantRange push{VK_SHADER_STAGE_COMPUTE_BIT, 0U, 24U};
  if (!descriptorSetLayout_.create(session_->device(), bindings, error) ||
      !pipelineLayout_.create(session_->device(), descriptorSetLayout_.get(),
                              std::span<const VkPushConstantRange>(&push, 1U),
                              error) ||
      !descriptorPool_.create(session_->device(), 16U, 8U,
                              VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, error)) {
    reset();
    return false;
  }
  for (auto &set : descriptorSets_)
    if (!descriptorPool_.allocate(descriptorSetLayout_.get(), set, error)) {
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

bool DeviceRayRecordRadixSort::initialize(std::string_view histogramSpirv,
                                          std::string_view prefixSpirv,
                                          std::string_view scatterSpirv,
                                          std::string &error) {
  error.clear();
  return setup(histogramSpirv, prefixSpirv, scatterSpirv, nullptr, error);
}
bool DeviceRayRecordRadixSort::initialize(runtime::ComputeSession &session,
                                          std::string_view histogramSpirv,
                                          std::string_view prefixSpirv,
                                          std::string_view scatterSpirv,
                                          std::string &error) {
  error.clear();
  return setup(histogramSpirv, prefixSpirv, scatterSpirv, &session, error);
}

void DeviceRayRecordRadixSort::reset() {
  const bool ownsSession = session_ == &ownedSession_;
  fence_.destroy();
  commandBuffer_ = VK_NULL_HANDLE;
  blockOffsets_.reset();
  digitBases_.reset();
  blockSums_.reset();
  offsets_.reset();
  histogram_.reset();
  scratchB_.reset();
  scratchA_.reset();
  for (auto &pipeline : pipelines_)
    pipeline.reset();
  descriptorPool_.reset();
  pipelineLayout_.reset();
  descriptorSetLayout_.reset();
  for (auto &module : shaderModules_)
    module.reset();
  descriptorSets_.fill(VK_NULL_HANDLE);
  scratchCapacity_ = 0U;
  scratchGroups_ = 0U;
  session_ = nullptr;
  if (ownsSession)
    ownedSession_.reset();
}

bool DeviceRayRecordRadixSort::isInitialized() const {
  if (session_ == nullptr || !session_->isValid() ||
      descriptorSetLayout_.get() == VK_NULL_HANDLE ||
      pipelineLayout_.get() == VK_NULL_HANDLE ||
      descriptorPool_.get() == VK_NULL_HANDLE ||
      commandBuffer_ == VK_NULL_HANDLE || fence_.get() == VK_NULL_HANDLE)
    return false;
  for (const auto &pipeline : pipelines_)
    if (pipeline.get() == VK_NULL_HANDLE)
      return false;
  for (const auto set : descriptorSets_)
    if (set == VK_NULL_HANDLE)
      return false;
  return true;
}
bool DeviceRayRecordRadixSort::ready(std::string &error) const {
  return isInitialized()
             ? true
             : fail(error, "ray-record radix sorter is not initialized");
}
const runtime::VulkanDevice &DeviceRayRecordRadixSort::device() const {
  static const runtime::VulkanDevice empty{};
  return session_ != nullptr ? session_->device() : empty;
}

bool DeviceRayRecordRadixSort::ensureScratch(std::size_t capacity,
                                             std::uint32_t groups,
                                             std::string &error) {
  if (scratchCapacity_ == capacity && scratchGroups_ == groups)
    return true;
  scratchA_.reset();
  scratchB_.reset();
  histogram_.reset();
  offsets_.reset();
  blockSums_.reset();
  blockOffsets_.reset();
  std::size_t recordBytes{}, histBytes{}, blockBytes{};
  const std::size_t tiles = (static_cast<std::size_t>(groups) + 255U) / 256U;
  if (!bytesFor(capacity, sizeof(RayRecord), recordBytes) ||
      !bytesFor(static_cast<std::size_t>(groups) * 16U, sizeof(std::uint32_t),
                histBytes) ||
      !bytesFor(tiles * 16U, sizeof(std::uint32_t), blockBytes))
    return fail(error, "ray-record radix scratch size overflows");
  if (!scratchA_.create(*session_, static_cast<VkDeviceSize>(recordBytes),
                        error) ||
      !scratchB_.create(*session_, static_cast<VkDeviceSize>(recordBytes),
                        error) ||
      !histogram_.create(*session_, static_cast<VkDeviceSize>(histBytes),
                         error) ||
      !offsets_.create(*session_, static_cast<VkDeviceSize>(histBytes),
                       error) ||
      !blockSums_.create(*session_, static_cast<VkDeviceSize>(blockBytes),
                         error) ||
      !blockOffsets_.create(*session_, static_cast<VkDeviceSize>(blockBytes),
                            error) ||
      !digitBases_.create(*session_, 16U * sizeof(std::uint32_t), error))
    return false;
  scratchCapacity_ = capacity;
  scratchGroups_ = groups;
  return true;
}

bool DeviceRayRecordRadixSort::sort(const runtime::DeviceBuffer &inputRecords,
                                    const runtime::DeviceBuffer &inputCount,
                                    std::size_t inputCapacity,
                                    runtime::DeviceBuffer &outputRecords,
                                    std::size_t outputCapacity,
                                    std::string &error) {
  error.clear();
  if (!ready(error))
    return false;
  if (inputCapacity > std::numeric_limits<std::uint32_t>::max() ||
      outputCapacity < inputCapacity)
    return fail(error, "ray-record radix capacity is invalid");
  if (inputCapacity == 0U)
    return true;
  std::size_t inputBytes{}, outputBytes{};
  if (!bytesFor(inputCapacity, sizeof(RayRecord), inputBytes) ||
      !bytesFor(outputCapacity, sizeof(RayRecord), outputBytes))
    return fail(error, "ray-record radix buffer size overflows");
  const auto groupsSize = (inputCapacity + 63U) / 64U;
  if (groupsSize > std::numeric_limits<std::uint32_t>::max())
    return fail(error, "ray-record radix dispatch dimensions overflow");
  const auto groups = static_cast<std::uint32_t>(groupsSize);
  if (groups > session_->device()
                   .selection()
                   .properties.limits.maxComputeWorkGroupCount[0])
    return fail(error,
                "ray-record radix dispatch exceeds device workgroup limit");
  const std::uint32_t tiles = (groups + 255U) / 256U;
  if (tiles > 256U)
    return fail(error, "ray-record radix prefix hierarchy exceeds 256 tiles");
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
                "ray-record radix buffer is invalid, stale, or undersized");
  if (inputRecords.handle() == inputCount.handle() ||
      inputRecords.handle() == outputRecords.handle() ||
      inputCount.handle() == outputRecords.handle())
    return fail(error, "ray-record radix buffers must not alias");
  if (!ensureScratch(inputCapacity, groups, error))
    return false;
  const std::array<VkBuffer, 16U> sources{
      inputRecords.handle(), scratchA_.handle(), scratchB_.handle(),
      scratchA_.handle(),    scratchB_.handle(), scratchA_.handle(),
      scratchB_.handle(),    scratchA_.handle(), scratchB_.handle(),
      scratchA_.handle(),    scratchB_.handle(), scratchA_.handle(),
      scratchB_.handle(),    scratchA_.handle(), scratchB_.handle(),
      scratchA_.handle()};
  std::array<VkBuffer, 16U> targets{};
  for (std::size_t pass = 0U; pass < 16U; ++pass)
    targets[pass] = pass == 15U ? outputRecords.handle()
                                : (pass % 2U == 0U ? scratchA_.handle()
                                                   : scratchB_.handle());
  for (std::size_t pass = 0U; pass < 16U; ++pass) {
    const std::array<VkDescriptorBufferInfo, 8U> infos{
        {{sources[pass], 0U, VK_WHOLE_SIZE},
         {targets[pass], 0U, VK_WHOLE_SIZE},
         {inputCount.handle(), 0U, sizeof(std::uint32_t)},
         {histogram_.handle(), 0U, VK_WHOLE_SIZE},
         {offsets_.handle(), 0U, VK_WHOLE_SIZE},
         {blockSums_.handle(), 0U, VK_WHOLE_SIZE},
         {blockOffsets_.handle(), 0U, VK_WHOLE_SIZE},
         {digitBases_.handle(), 0U, VK_WHOLE_SIZE}}};
    std::array<VkWriteDescriptorSet, 8U> writes{};
    for (std::uint32_t i = 0U; i < writes.size(); ++i)
      writes[i] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                   nullptr,
                   descriptorSets_[pass],
                   i,
                   0U,
                   1U,
                   VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                   nullptr,
                   &infos[i],
                   nullptr};
    vkUpdateDescriptorSets(session_->deviceHandle(), 8U, writes.data(), 0U,
                           nullptr);
  }
  if (vkResetCommandBuffer(commandBuffer_, 0U) != VK_SUCCESS)
    return fail(error, "failed to reset ray-record radix command buffer");
  VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  if (vkBeginCommandBuffer(commandBuffer_, &begin) != VK_SUCCESS)
    return fail(error, "failed to begin ray-record radix command buffer");
  const VkMemoryBarrier pre{
      VK_STRUCTURE_TYPE_MEMORY_BARRIER, nullptr,
      VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_SHADER_WRITE_BIT,
      VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT};
  vkCmdPipelineBarrier(commandBuffer_,
                       VK_PIPELINE_STAGE_TRANSFER_BIT |
                           VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                       VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0U, 1U, &pre, 0U,
                       nullptr, 0U, nullptr);
  for (std::uint32_t pass = 0U; pass < 16U; ++pass) {
    const std::uint32_t key = pass < 8U ? 0U : 1U;
    const std::uint32_t shift = (pass & 7U) * 4U;
    std::array<std::uint32_t, 6U> params{
        static_cast<std::uint32_t>(inputCapacity),
        groups,
        shift,
        key,
        0U,
        tiles};
    vkCmdBindDescriptorSets(commandBuffer_, VK_PIPELINE_BIND_POINT_COMPUTE,
                            pipelineLayout_.get(), 0U, 1U,
                            &descriptorSets_[pass], 0U, nullptr);
    vkCmdBindPipeline(commandBuffer_, VK_PIPELINE_BIND_POINT_COMPUTE,
                      pipelines_[0].get());
    vkCmdPushConstants(commandBuffer_, pipelineLayout_.get(),
                       VK_SHADER_STAGE_COMPUTE_BIT, 0U, 24U, params.data());
    vkCmdDispatch(commandBuffer_, groups, 1U, 1U);
    const VkMemoryBarrier barrier{
        VK_STRUCTURE_TYPE_MEMORY_BARRIER, nullptr, VK_ACCESS_SHADER_WRITE_BIT,
        VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT};
    vkCmdPipelineBarrier(commandBuffer_, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0U, 1U, &barrier,
                         0U, nullptr, 0U, nullptr);
    vkCmdBindPipeline(commandBuffer_, VK_PIPELINE_BIND_POINT_COMPUTE,
                      pipelines_[1].get());
    params[4] = 0U;
    vkCmdPushConstants(commandBuffer_, pipelineLayout_.get(),
                       VK_SHADER_STAGE_COMPUTE_BIT, 0U, 24U, params.data());
    vkCmdDispatch(commandBuffer_, tiles, 16U, 1U);
    vkCmdPipelineBarrier(commandBuffer_, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0U, 1U, &barrier,
                         0U, nullptr, 0U, nullptr);
    vkCmdBindPipeline(commandBuffer_, VK_PIPELINE_BIND_POINT_COMPUTE,
                      pipelines_[1].get());
    params[4] = 1U;
    vkCmdPushConstants(commandBuffer_, pipelineLayout_.get(),
                       VK_SHADER_STAGE_COMPUTE_BIT, 0U, 24U, params.data());
    vkCmdDispatch(commandBuffer_, 1U, 16U, 1U);
    vkCmdPipelineBarrier(commandBuffer_, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0U, 1U, &barrier,
                         0U, nullptr, 0U, nullptr);
    params[4] = 3U;
    vkCmdPushConstants(commandBuffer_, pipelineLayout_.get(),
                       VK_SHADER_STAGE_COMPUTE_BIT, 0U, 24U, params.data());
    vkCmdDispatch(commandBuffer_, 1U, 1U, 1U);
    vkCmdPipelineBarrier(commandBuffer_, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0U, 1U, &barrier,
                         0U, nullptr, 0U, nullptr);
    params[4] = 2U;
    vkCmdPushConstants(commandBuffer_, pipelineLayout_.get(),
                       VK_SHADER_STAGE_COMPUTE_BIT, 0U, 24U, params.data());
    vkCmdDispatch(commandBuffer_, tiles, 16U, 1U);
    vkCmdPipelineBarrier(commandBuffer_, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0U, 1U, &barrier,
                         0U, nullptr, 0U, nullptr);
    vkCmdBindPipeline(commandBuffer_, VK_PIPELINE_BIND_POINT_COMPUTE,
                      pipelines_[2].get());
    params[4] = 0U;
    vkCmdPushConstants(commandBuffer_, pipelineLayout_.get(),
                       VK_SHADER_STAGE_COMPUTE_BIT, 0U, 24U, params.data());
    vkCmdDispatch(commandBuffer_, groups, 1U, 1U);
    vkCmdPipelineBarrier(commandBuffer_, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0U, 1U, &barrier,
                         0U, nullptr, 0U, nullptr);
  }
  const VkMemoryBarrier post{
      VK_STRUCTURE_TYPE_MEMORY_BARRIER, nullptr, VK_ACCESS_SHADER_WRITE_BIT,
      VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT |
          VK_ACCESS_TRANSFER_READ_BIT};
  vkCmdPipelineBarrier(commandBuffer_, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                       VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT |
                           VK_PIPELINE_STAGE_TRANSFER_BIT,
                       0U, 1U, &post, 0U, nullptr, 0U, nullptr);
  if (vkEndCommandBuffer(commandBuffer_) != VK_SUCCESS)
    return fail(error, "failed to end ray-record radix command buffer");
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
