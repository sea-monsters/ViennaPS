// Copyright 2026 ViennaPS
// SPDX-License-Identifier: MIT
#include "ray_record_compaction.hpp"

#include <array>
#include <limits>
#include <utility>

namespace viennaps::vulkan::ray {
namespace {
bool fail(std::string &e, const char *m) {
  e = m;
  return false;
}
bool mul(std::size_t a, std::size_t b, std::size_t &out) {
  if (b != 0U && a > std::numeric_limits<std::size_t>::max() / b)
    return false;
  out = a * b;
  return true;
}
} // namespace

DeviceRayRecordCompactor::~DeviceRayRecordCompactor() { reset(); }

bool DeviceRayRecordCompactor::setup(const std::string_view compactionSpirv,
                                     const std::string_view reductionScanSpirv,
                                     runtime::ComputeSession *external,
                                     std::string &error) {
  if (compactionSpirv.empty() || reductionScanSpirv.empty())
    return fail(error, "ray-record compaction SPIR-V path is empty");
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
  if (!runtime::readSpirv(compactionSpirv, program, error) ||
      !shaderModule_.create(session_->device(), program, error) ||
      !scan_.initialize(*session_, reductionScanSpirv, error)) {
    reset();
    return false;
  }
  std::array<VkDescriptorSetLayoutBinding, 5U> bindings{};
  for (std::uint32_t i = 0; i < bindings.size(); ++i)
    bindings[i] = {i, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1U,
                   VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
  const VkPushConstantRange push{VK_SHADER_STAGE_COMPUTE_BIT, 0U,
                                 3U * sizeof(std::uint32_t)};
  if (!descriptorSetLayout_.create(session_->device(), bindings, error) ||
      !pipelineLayout_.create(session_->device(), descriptorSetLayout_.get(),
                              std::span<const VkPushConstantRange>(&push, 1),
                              error) ||
      !descriptorPool_.create(session_->device(), 1U, 5U,
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

bool DeviceRayRecordCompactor::initialize(
    const std::string_view compactionSpirv,
    const std::string_view reductionScanSpirv, std::string &error) {
  error.clear();
  return setup(compactionSpirv, reductionScanSpirv, nullptr, error);
}
bool DeviceRayRecordCompactor::initialize(
    runtime::ComputeSession &session, const std::string_view compactionSpirv,
    const std::string_view reductionScanSpirv, std::string &error) {
  error.clear();
  return setup(compactionSpirv, reductionScanSpirv, &session, error);
}

void DeviceRayRecordCompactor::reset() {
  const bool own = session_ == &ownedSession_;
  fence_.destroy();
  pipeline_.reset();
  descriptorPool_.reset();
  pipelineLayout_.reset();
  descriptorSetLayout_.reset();
  shaderModule_.reset();
  scan_.reset();
  descriptorSet_ = VK_NULL_HANDLE;
  commandBuffer_ = VK_NULL_HANDLE;
  session_ = nullptr;
  if (own)
    ownedSession_.reset();
}
bool DeviceRayRecordCompactor::isInitialized() const {
  return session_ != nullptr && session_->isValid() && scan_.isInitialized() &&
         shaderModule_.get() != VK_NULL_HANDLE &&
         descriptorSetLayout_.get() != VK_NULL_HANDLE &&
         pipelineLayout_.get() != VK_NULL_HANDLE &&
         descriptorPool_.get() != VK_NULL_HANDLE &&
         descriptorSet_ != VK_NULL_HANDLE && commandBuffer_ != VK_NULL_HANDLE &&
         fence_.get() != VK_NULL_HANDLE && pipeline_.get() != VK_NULL_HANDLE;
}
bool DeviceRayRecordCompactor::ready(std::string &error) const {
  return isInitialized()
             ? true
             : fail(error, "ray-record compactor is not initialized");
}
const runtime::VulkanDevice &DeviceRayRecordCompactor::device() const {
  static const runtime::VulkanDevice empty{};
  return session_ ? session_->device() : empty;
}
bool DeviceRayRecordCompactor::createRecordBuffer(
    const std::size_t capacity, runtime::DeviceBuffer &records,
    std::string &error) const {
  if (!ready(error))
    return false;
  std::size_t bytes{};
  if (!mul(capacity, sizeof(RayRecord), bytes))
    return fail(error, "record buffer size overflow");
  return records.create(
      *session_, static_cast<VkDeviceSize>(bytes == 0U ? 1U : bytes), error);
}

bool DeviceRayRecordCompactor::compact(const runtime::DeviceBuffer &hits,
                                       const runtime::DeviceBuffer &weights,
                                       const std::size_t rayCount,
                                       const std::uint32_t surfaceDomain,
                                       const std::size_t outputCapacity,
                                       RayRecordCompactionDeviceOutput &output,
                                       std::string &error) {
  error.clear();
  if (!ready(error))
    return false;
  if (rayCount > std::numeric_limits<std::uint32_t>::max() ||
      (rayCount != 0U && surfaceDomain == 0U))
    return fail(error, "ray-record compaction domain or count is invalid");
  std::size_t hitBytes{}, weightBytes{}, recordBytes{};
  if (!mul(rayCount, sizeof(TriangleHit), hitBytes) ||
      !mul(rayCount, sizeof(std::uint32_t), weightBytes) ||
      !mul(outputCapacity, sizeof(RayRecord), recordBytes))
    return fail(error, "ray-record compaction buffer size overflow");
  if (rayCount == 0U) {
    return true;
  }
  // The device-resident count cannot be inspected without a readback. Reject
  // a potentially insufficient caller allocation before any dispatch instead.
  if (outputCapacity < rayCount) {
    return fail(error, "ray-record output capacity is insufficient");
  }
  if (!hits.isValid() || !weights.isValid() || !output.records.isValid() ||
      hits.ownerDevice() != session_->deviceHandle() ||
      weights.ownerDevice() != session_->deviceHandle() ||
      output.records.ownerDevice() != session_->deviceHandle() ||
      hits.ownerSessionGeneration() != session_->generation() ||
      weights.ownerSessionGeneration() != session_->generation() ||
      output.records.ownerSessionGeneration() != session_->generation() ||
      hits.size() < hitBytes || weights.size() < weightBytes ||
      output.records.size() < recordBytes)
    return fail(
        error, "ray-record compaction buffer is invalid, stale, or undersized");
  if (hits.handle() == weights.handle() ||
      hits.handle() == output.records.handle() ||
      weights.handle() == output.records.handle())
    return fail(error, "ray-record compaction buffers must not alias");
  const auto bytes =
      static_cast<VkDeviceSize>(rayCount * sizeof(std::uint32_t));
  runtime::DeviceBuffer flags{}, offsets{}, count{};
  if (!flags.create(*session_, bytes, error) ||
      !offsets.create(*session_, bytes, error) ||
      !count.create(*session_, sizeof(std::uint32_t), error))
    return false;
  const std::array<VkBuffer, 5U> handles{hits.handle(), weights.handle(),
                                         flags.handle(), offsets.handle(),
                                         output.records.handle()};
  const auto dispatch = [&](const std::uint32_t mode) -> bool {
    std::array<VkDescriptorBufferInfo, 5U> infos{};
    std::array<VkWriteDescriptorSet, 5U> writes{};
    for (std::uint32_t i = 0; i < 5U; ++i) {
      infos[i] = {handles[i], 0U, VK_WHOLE_SIZE};
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
    vkUpdateDescriptorSets(session_->deviceHandle(), 5U, writes.data(), 0U,
                           nullptr);
    if (vkResetCommandBuffer(commandBuffer_, 0) != VK_SUCCESS)
      return fail(error, "failed to reset compaction command buffer");
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (vkBeginCommandBuffer(commandBuffer_, &begin) != VK_SUCCESS)
      return fail(error, "failed to begin compaction command buffer");
    std::array<VkBufferMemoryBarrier, 5U> pre{};
    for (std::size_t i = 0; i < 5U; ++i)
      pre[i] = {VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
                nullptr,
                VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_SHADER_WRITE_BIT,
                VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
                VK_QUEUE_FAMILY_IGNORED,
                VK_QUEUE_FAMILY_IGNORED,
                handles[i],
                0U,
                VK_WHOLE_SIZE};
    vkCmdPipelineBarrier(commandBuffer_,
                         VK_PIPELINE_STAGE_TRANSFER_BIT |
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0U, 0U, nullptr,
                         5U, pre.data(), 0U, nullptr);
    vkCmdBindPipeline(commandBuffer_, VK_PIPELINE_BIND_POINT_COMPUTE,
                      pipeline_.get());
    vkCmdBindDescriptorSets(commandBuffer_, VK_PIPELINE_BIND_POINT_COMPUTE,
                            pipelineLayout_.get(), 0U, 1U, &descriptorSet_, 0U,
                            nullptr);
    const std::array<std::uint32_t, 3U> pc{static_cast<std::uint32_t>(rayCount),
                                           surfaceDomain, mode};
    vkCmdPushConstants(commandBuffer_, pipelineLayout_.get(),
                       VK_SHADER_STAGE_COMPUTE_BIT, 0U, sizeof(pc), pc.data());
    vkCmdDispatch(commandBuffer_,
                  static_cast<std::uint32_t>((rayCount + 63U) / 64U), 1U, 1U);
    const auto outputBinding = mode == 0U ? 2U : 4U;
    const VkBufferMemoryBarrier post{
        VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
        nullptr,
        VK_ACCESS_SHADER_WRITE_BIT,
        VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT |
            (mode == 0U ? 0U : VK_ACCESS_TRANSFER_READ_BIT),
        VK_QUEUE_FAMILY_IGNORED,
        VK_QUEUE_FAMILY_IGNORED,
        handles[outputBinding],
        0U,
        VK_WHOLE_SIZE};
    vkCmdPipelineBarrier(commandBuffer_, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         mode == 0U ? VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT
                                    : VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT |
                                          VK_PIPELINE_STAGE_TRANSFER_BIT,
                         0U, 0U, nullptr, 1U, &post, 0U, nullptr);
    if (vkEndCommandBuffer(commandBuffer_) != VK_SUCCESS)
      return fail(error, "failed to end compaction command buffer");
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.commandBufferCount = 1U;
    submit.pCommandBuffers = &commandBuffer_;
    if (vkQueueSubmit(session_->device().computeQueue(), 1U, &submit,
                      fence_.get()) != VK_SUCCESS ||
        !fence_.wait(10'000'000'000ULL, error))
      return false;
    fence_.reset();
    return true;
  };
  if (!dispatch(0U) ||
      !scan_.exclusiveScanInt(flags, rayCount, offsets, rayCount, error) ||
      !scan_.writeCompactionCount(flags, offsets, rayCount, count, error))
    return false;
  if (!dispatch(1U))
    return false;
  RayRecordCompactionDeviceOutput local{};
  local.flags = std::move(flags);
  local.offsets = std::move(offsets);
  local.count = std::move(count);
  local.records = std::move(output.records);
  local.inputCount = static_cast<std::uint32_t>(rayCount);
  local.sessionGeneration = session_->generation();
  output = std::move(local);
  return true;
}
} // namespace viennaps::vulkan::ray
