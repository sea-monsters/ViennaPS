// Copyright 2026 ViennaPS
// SPDX-License-Identifier: MIT

#include "reduction_scan_primitives.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <sstream>
#include <vector>

namespace {

[[nodiscard]] bool setError(std::string &error, const std::string_view phase,
                            const std::string_view message) {
  error = std::string(phase) + ": " +
          (message.empty() ? "operation failed" : std::string(message));
  return false;
}

[[nodiscard]] std::size_t ceilDiv(const std::size_t value,
                                  const std::size_t divisor) {
  return value / divisor + (value % divisor != 0u);
}

[[nodiscard]] VkBufferMemoryBarrier
makeBarrier(const VkBuffer buffer, const VkDeviceSize size,
            const VkAccessFlags sourceAccess,
            const VkAccessFlags destinationAccess) {
  VkBufferMemoryBarrier barrier{};
  barrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
  barrier.srcAccessMask = sourceAccess;
  barrier.dstAccessMask = destinationAccess;
  barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  barrier.buffer = buffer;
  barrier.offset = 0u;
  barrier.size = size;
  return barrier;
}

constexpr std::string_view kPipelineEntryPoint{"main"};

} // namespace

namespace viennaps::vulkan::primitives {

bool ReductionScanPrimitives::initialize(const std::string_view spirvPath,
                                         std::string &error) {
  if (isInitialized()) {
    return true;
  }
  if (spirvPath.empty()) {
    return setError(error, "initialization", "SPIR-V path is empty");
  }

  reset();
  const auto fail = [this, &error]() {
    const std::string detail = error;
    reset();
    return setError(error, "initialization", detail);
  };

  if (!instance_.create(error)) {
    return fail();
  }
  runtime::ComputeDeviceSelection selection{};
  if (!runtime::pickFirstComputeDevice(instance_.get(), selection, error) ||
      !device_.create(selection, error) || !validateDeviceLimits(error)) {
    return fail();
  }

  runtime::SpirvProgram program{};
  if (!runtime::readSpirv(spirvPath, program, error) ||
      !shaderModule_.create(device_, program, error)) {
    return fail();
  }

  std::array<VkDescriptorSetLayoutBinding, 5u> bindings{};
  for (std::uint32_t index = 0u; index < bindings.size(); ++index) {
    bindings[index].binding = index;
    bindings[index].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    bindings[index].descriptorCount = 1u;
    bindings[index].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
  }
  if (!descriptorSetLayout_.create(device_, bindings, error)) {
    return fail();
  }

  const VkPushConstantRange pushRange{VK_SHADER_STAGE_COMPUTE_BIT, 0u,
                                      sizeof(PushConstants)};
  if (!pipelineLayout_.create(device_, descriptorSetLayout_.get(),
                              std::span(&pushRange, 1u), error)) {
    return fail();
  }

  if (!descriptorPool_.create(device_, 1u, 5u,
                              VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, error) ||
      !descriptorPool_.allocate(descriptorSetLayout_.get(), descriptorSet_,
                                error) ||
      !commandContext_.create(device_, device_.computeQueueFamily(), error) ||
      !commandContext_.allocatePrimary(commandBuffer_, error) ||
      !fence_.create(device_, error)) {
    return fail();
  }

  if (!createPipeline(reducePipeline_,
                      ReductionScanOperation::reduceFloatBlocks, error) ||
      !createPipeline(scanBlocksPipeline_,
                      ReductionScanOperation::exclusiveScanIntBlocks, error) ||
      !createPipeline(scanAddOffsetsPipeline_,
                      ReductionScanOperation::exclusiveScanIntAddOffsets,
                      error) ||
      !createPipeline(normalizeFlagsPipeline_,
                      ReductionScanOperation::normalizeFlags, error) ||
      !createPipeline(compactionCountPipeline_,
                      ReductionScanOperation::writeCompactionCount, error) ||
      !createPipeline(compactFloatPipeline_,
                      ReductionScanOperation::compactFloat, error) ||
      !createPipeline(compactUInt32Pipeline_,
                      ReductionScanOperation::compactUInt32, error) ||
      !ensureDummyBuffers(error)) {
    return fail();
  }
  return true;
}

void ReductionScanPrimitives::reset() {
  dummyInt_.reset();
  dummyFloat_.reset();
  fence_.destroy();
  commandContext_.reset();
  descriptorPool_.reset();
  compactUInt32Pipeline_.reset();
  compactFloatPipeline_.reset();
  compactionCountPipeline_.reset();
  normalizeFlagsPipeline_.reset();
  scanAddOffsetsPipeline_.reset();
  scanBlocksPipeline_.reset();
  reducePipeline_.reset();
  pipelineLayout_.reset();
  descriptorSetLayout_.reset();
  shaderModule_.reset();
  device_.reset();
  instance_.reset();
  descriptorSet_ = VK_NULL_HANDLE;
  commandBuffer_ = VK_NULL_HANDLE;
}

bool ReductionScanPrimitives::isInitialized() const {
  return instance_.isValid() && device_.isValid() &&
         shaderModule_.get() != VK_NULL_HANDLE &&
         descriptorSetLayout_.get() != VK_NULL_HANDLE &&
         pipelineLayout_.get() != VK_NULL_HANDLE &&
         reducePipeline_.get() != VK_NULL_HANDLE &&
         scanBlocksPipeline_.get() != VK_NULL_HANDLE &&
         scanAddOffsetsPipeline_.get() != VK_NULL_HANDLE &&
         normalizeFlagsPipeline_.get() != VK_NULL_HANDLE &&
         compactionCountPipeline_.get() != VK_NULL_HANDLE &&
         compactFloatPipeline_.get() != VK_NULL_HANDLE &&
         compactUInt32Pipeline_.get() != VK_NULL_HANDLE &&
         descriptorPool_.get() != VK_NULL_HANDLE &&
         descriptorSet_ != VK_NULL_HANDLE && commandBuffer_ != VK_NULL_HANDLE &&
         fence_.get() != VK_NULL_HANDLE &&
         commandContext_.pool() != VK_NULL_HANDLE;
}

bool ReductionScanPrimitives::isReady(std::string &error) const {
  if (!isInitialized()) {
    return setError(error, "execution",
                    "reduction/scan primitives are not initialized");
  }
  return true;
}

bool ReductionScanPrimitives::validateDeviceLimits(std::string &error) const {
  const auto &limits = device_.selection().properties.limits;
  if (kWorkgroupSize > limits.maxComputeWorkGroupInvocations ||
      kWorkgroupSize > limits.maxComputeWorkGroupSize[0]) {
    return setError(error, "device validation",
                    "256-thread workgroups are not supported");
  }
  if (limits.maxComputeWorkGroupCount[0] == 0u) {
    return setError(error, "device validation",
                    "the device exposes no X workgroups");
  }
  return true;
}

bool ReductionScanPrimitives::createPipeline(
    runtime::ComputePipeline &pipeline, const ReductionScanOperation operation,
    std::string &error) {
  const auto value = static_cast<std::uint32_t>(operation);
  const VkSpecializationMapEntry entry{0u, 0u, sizeof(value)};
  const runtime::ComputePipelineOptions options{
      kPipelineEntryPoint, std::span(&entry, 1u), &value, sizeof(value)};
  return pipeline.create(device_, shaderModule_, pipelineLayout_, options,
                         error);
}

bool ReductionScanPrimitives::createBuffer(const std::size_t elementCount,
                                           const std::size_t elementSize,
                                           runtime::HostVisibleBuffer &buffer,
                                           std::string &error) {
  if (!isReady(error)) {
    return false;
  }
  if (elementSize == 0u ||
      elementCount > std::numeric_limits<std::size_t>::max() / elementSize) {
    return setError(error, "buffer creation", "requested size overflows");
  }
  const auto allocatedCount = std::max<std::size_t>(1u, elementCount);
  const auto bytes = static_cast<VkDeviceSize>(allocatedCount * elementSize);
  if (bytes > device_.selection().properties.limits.maxStorageBufferRange) {
    return setError(error, "buffer creation",
                    "requested size exceeds maxStorageBufferRange");
  }
  return buffer.create(device_, bytes, kBufferUsage, kMemoryFlags, error);
}

bool ReductionScanPrimitives::createFloatBuffer(
    const std::size_t elementCount, runtime::HostVisibleBuffer &buffer,
    std::string &error) {
  return createBuffer(elementCount, sizeof(float), buffer, error);
}

bool ReductionScanPrimitives::createIntBuffer(
    const std::size_t elementCount, runtime::HostVisibleBuffer &buffer,
    std::string &error) {
  return createBuffer(elementCount, sizeof(std::int32_t), buffer, error);
}

bool ReductionScanPrimitives::validateFloatLength(
    const std::string_view label, const runtime::HostVisibleBuffer &buffer,
    const std::size_t elementCount, std::string &error) const {
  if (!buffer.isValid()) {
    return setError(error, "validation",
                    std::string(label) + " buffer is not initialized");
  }
  if (buffer.ownerDevice() != device_.get()) {
    return setError(error, "validation",
                    std::string(label) + " belongs to a different device");
  }
  if (buffer.size() >
      device_.selection().properties.limits.maxStorageBufferRange) {
    return setError(error, "validation",
                    std::string(label) + " exceeds maxStorageBufferRange");
  }
  const auto capacity = static_cast<std::size_t>(buffer.size() / sizeof(float));
  if (elementCount > capacity) {
    return setError(error, "validation",
                    std::string(label) + " exceeds buffer capacity");
  }
  return true;
}

bool ReductionScanPrimitives::validateIntLength(
    const std::string_view label, const runtime::HostVisibleBuffer &buffer,
    const std::size_t elementCount, std::string &error) const {
  if (!buffer.isValid()) {
    return setError(error, "validation",
                    std::string(label) + " buffer is not initialized");
  }
  if (buffer.ownerDevice() != device_.get()) {
    return setError(error, "validation",
                    std::string(label) + " belongs to a different device");
  }
  if (buffer.size() >
      device_.selection().properties.limits.maxStorageBufferRange) {
    return setError(error, "validation",
                    std::string(label) + " exceeds maxStorageBufferRange");
  }
  const auto capacity =
      static_cast<std::size_t>(buffer.size() / sizeof(std::int32_t));
  if (elementCount > capacity) {
    return setError(error, "validation",
                    std::string(label) + " exceeds buffer capacity");
  }
  return true;
}

bool ReductionScanPrimitives::validateAlias(
    const std::string_view label, const runtime::HostVisibleBuffer &input,
    const runtime::HostVisibleBuffer &output, const bool allowInPlace,
    std::string &error) const {
  if (!allowInPlace && input.handle() == output.handle()) {
    return setError(error, "validation",
                    std::string(label) + " in-place aliasing is disallowed");
  }
  return true;
}

bool ReductionScanPrimitives::ensureMapped(runtime::HostVisibleBuffer &buffer,
                                           const std::string_view label,
                                           std::string &error) const {
  if (!buffer.isValid()) {
    return setError(error, "mapping",
                    std::string(label) + " buffer is not initialized");
  }
  return buffer.mappedPtr() != nullptr || buffer.map(error);
}

bool ReductionScanPrimitives::ensureDummyBuffers(std::string &error) {
  if (dummyFloat_.isValid() && dummyInt_.isValid()) {
    return true;
  }
  return createFloatBuffer(1u, dummyFloat_, error) &&
         createIntBuffer(1u, dummyInt_, error);
}

bool ReductionScanPrimitives::updateDescriptors(
    runtime::HostVisibleBuffer &floatInput,
    runtime::HostVisibleBuffer &floatOutput,
    runtime::HostVisibleBuffer &intInput, runtime::HostVisibleBuffer &intOutput,
    runtime::HostVisibleBuffer &intAux, std::string &error) {
  const std::array<runtime::HostVisibleBuffer *, 5u> buffers{
      &floatInput, &floatOutput, &intInput, &intOutput, &intAux};
  std::array<VkDescriptorBufferInfo, 5u> infos{};
  std::array<VkWriteDescriptorSet, 5u> writes{};
  for (std::uint32_t index = 0u; index < buffers.size(); ++index) {
    if (!buffers[index]->isValid()) {
      return setError(error, "descriptors", "a binding buffer is invalid");
    }
    infos[index].buffer = buffers[index]->handle();
    infos[index].offset = 0u;
    infos[index].range = buffers[index]->size();
    writes[index].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[index].dstSet = descriptorSet_;
    writes[index].dstBinding = index;
    writes[index].descriptorCount = 1u;
    writes[index].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    writes[index].pBufferInfo = &infos[index];
  }
  vkUpdateDescriptorSets(device_.get(),
                         static_cast<std::uint32_t>(writes.size()),
                         writes.data(), 0u, nullptr);
  return true;
}

bool ReductionScanPrimitives::dispatchKernel(
    runtime::ComputePipeline &pipeline, const std::size_t dispatchX,
    const PushConstants &constants,
    const std::span<const VkBufferMemoryBarrier> preBarriers,
    const std::span<const VkBufferMemoryBarrier> postBarriers,
    std::string &error) {
  if (!isReady(error) || dispatchX == 0u) {
    return dispatchX == 0u
               ? setError(error, "dispatch", "workgroup count is zero")
               : false;
  }
  const auto maxGroups =
      device_.selection().properties.limits.maxComputeWorkGroupCount[0];
  if (dispatchX > maxGroups ||
      dispatchX > std::numeric_limits<std::uint32_t>::max()) {
    return setError(error, "dispatch", "workgroup count exceeds device limit");
  }

  if (vkResetCommandBuffer(commandBuffer_, 0u) != VK_SUCCESS) {
    return setError(error, "dispatch", "vkResetCommandBuffer failed");
  }
  VkCommandBufferBeginInfo begin{};
  begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
  begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  if (vkBeginCommandBuffer(commandBuffer_, &begin) != VK_SUCCESS) {
    return setError(error, "dispatch", "vkBeginCommandBuffer failed");
  }
  vkCmdPipelineBarrier(commandBuffer_, VK_PIPELINE_STAGE_HOST_BIT,
                       VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0u, 0u, nullptr,
                       static_cast<std::uint32_t>(preBarriers.size()),
                       preBarriers.data(), 0u, nullptr);
  vkCmdBindPipeline(commandBuffer_, VK_PIPELINE_BIND_POINT_COMPUTE,
                    pipeline.get());
  vkCmdBindDescriptorSets(commandBuffer_, VK_PIPELINE_BIND_POINT_COMPUTE,
                          pipelineLayout_.get(), 0u, 1u, &descriptorSet_, 0u,
                          nullptr);
  vkCmdPushConstants(commandBuffer_, pipelineLayout_.get(),
                     VK_SHADER_STAGE_COMPUTE_BIT, 0u, sizeof(constants),
                     &constants);
  vkCmdDispatch(commandBuffer_, static_cast<std::uint32_t>(dispatchX), 1u, 1u);
  vkCmdPipelineBarrier(commandBuffer_, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                       VK_PIPELINE_STAGE_HOST_BIT, 0u, 0u, nullptr,
                       static_cast<std::uint32_t>(postBarriers.size()),
                       postBarriers.data(), 0u, nullptr);
  if (vkEndCommandBuffer(commandBuffer_) != VK_SUCCESS) {
    return setError(error, "dispatch", "vkEndCommandBuffer failed");
  }

  VkSubmitInfo submit{};
  submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
  submit.commandBufferCount = 1u;
  submit.pCommandBuffers = &commandBuffer_;
  if (vkQueueSubmit(device_.computeQueue(), 1u, &submit, fence_.get()) !=
      VK_SUCCESS) {
    return setError(error, "dispatch", "vkQueueSubmit failed");
  }
  if (!fence_.wait(10'000'000'000ULL, error)) {
    return false;
  }
  fence_.reset();
  return true;
}

bool ReductionScanPrimitives::dispatchReduce(runtime::HostVisibleBuffer &input,
                                             runtime::HostVisibleBuffer &output,
                                             const std::size_t elementCount,
                                             const bool inputIsTriples,
                                             std::string &error) {
  const auto blockCount = ceilDiv(elementCount, kWorkgroupSize);
  const auto inputElements = inputIsTriples ? elementCount * 3u : elementCount;
  if (!validateFloatLength("reduction input", input, inputElements, error) ||
      !validateFloatLength("reduction output", output, blockCount * 3u,
                           error) ||
      !ensureMapped(input, "reduction input", error) ||
      !ensureMapped(output, "reduction output", error) || !input.flush(error) ||
      !output.flush(error) ||
      !updateDescriptors(input, output, dummyInt_, dummyInt_, dummyInt_,
                         error)) {
    return false;
  }
  const std::array pre{
      makeBarrier(input.handle(), input.size(), VK_ACCESS_HOST_WRITE_BIT,
                  VK_ACCESS_SHADER_READ_BIT),
      makeBarrier(output.handle(), output.size(), VK_ACCESS_HOST_WRITE_BIT,
                  VK_ACCESS_SHADER_WRITE_BIT)};
  const std::array post{makeBarrier(output.handle(), output.size(),
                                    VK_ACCESS_SHADER_WRITE_BIT,
                                    VK_ACCESS_HOST_READ_BIT)};
  if (!dispatchKernel(
          reducePipeline_, blockCount,
          {static_cast<std::uint32_t>(elementCount), inputIsTriples ? 1u : 0u},
          pre, post, error)) {
    return false;
  }
  return output.invalidate(error);
}

bool ReductionScanPrimitives::dispatchScanBlocks(
    runtime::HostVisibleBuffer &input, runtime::HostVisibleBuffer &output,
    runtime::HostVisibleBuffer &blockSums, const std::size_t elementCount,
    std::string &error) {
  const auto blockCount = ceilDiv(elementCount, kWorkgroupSize);
  if (!validateIntLength("scan input", input, elementCount, error) ||
      !validateIntLength("scan output", output, elementCount, error) ||
      !validateIntLength("scan block sums", blockSums, blockCount, error) ||
      !ensureMapped(input, "scan input", error) ||
      !ensureMapped(output, "scan output", error) ||
      !ensureMapped(blockSums, "scan block sums", error) ||
      !input.flush(error) || !output.flush(error) || !blockSums.flush(error) ||
      !updateDescriptors(dummyFloat_, dummyFloat_, input, output, blockSums,
                         error)) {
    return false;
  }

  std::array<VkBufferMemoryBarrier, 3u> pre{};
  std::size_t preCount = 0u;
  if (input.handle() == output.handle()) {
    pre[preCount++] =
        makeBarrier(input.handle(), input.size(), VK_ACCESS_HOST_WRITE_BIT,
                    VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
  } else {
    pre[preCount++] =
        makeBarrier(input.handle(), input.size(), VK_ACCESS_HOST_WRITE_BIT,
                    VK_ACCESS_SHADER_READ_BIT);
    pre[preCount++] =
        makeBarrier(output.handle(), output.size(), VK_ACCESS_HOST_WRITE_BIT,
                    VK_ACCESS_SHADER_WRITE_BIT);
  }
  pre[preCount++] =
      makeBarrier(blockSums.handle(), blockSums.size(),
                  VK_ACCESS_HOST_WRITE_BIT, VK_ACCESS_SHADER_WRITE_BIT);
  const std::array post{
      makeBarrier(output.handle(), output.size(), VK_ACCESS_SHADER_WRITE_BIT,
                  VK_ACCESS_HOST_READ_BIT),
      makeBarrier(blockSums.handle(), blockSums.size(),
                  VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT)};
  if (!dispatchKernel(scanBlocksPipeline_, blockCount,
                      {static_cast<std::uint32_t>(elementCount), 0u},
                      std::span(pre.data(), preCount), post, error)) {
    return false;
  }
  return output.invalidate(error) && blockSums.invalidate(error);
}

bool ReductionScanPrimitives::dispatchScanAddOffsets(
    runtime::HostVisibleBuffer &output,
    runtime::HostVisibleBuffer &blockOffsets, const std::size_t elementCount,
    std::string &error) {
  const auto blockCount = ceilDiv(elementCount, kWorkgroupSize);
  if (!validateIntLength("scan output", output, elementCount, error) ||
      !validateIntLength("scan block offsets", blockOffsets, blockCount,
                         error) ||
      !ensureMapped(output, "scan output", error) ||
      !ensureMapped(blockOffsets, "scan block offsets", error) ||
      !output.flush(error) || !blockOffsets.flush(error) ||
      !updateDescriptors(dummyFloat_, dummyFloat_, dummyInt_, output,
                         blockOffsets, error)) {
    return false;
  }
  const std::array pre{
      makeBarrier(output.handle(), output.size(), VK_ACCESS_HOST_WRITE_BIT,
                  VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT),
      makeBarrier(blockOffsets.handle(), blockOffsets.size(),
                  VK_ACCESS_HOST_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT)};
  const std::array post{makeBarrier(output.handle(), output.size(),
                                    VK_ACCESS_SHADER_WRITE_BIT,
                                    VK_ACCESS_HOST_READ_BIT)};
  if (!dispatchKernel(scanAddOffsetsPipeline_, blockCount,
                      {static_cast<std::uint32_t>(elementCount), 0u}, pre, post,
                      error)) {
    return false;
  }
  return output.invalidate(error);
}

bool ReductionScanPrimitives::dispatchNormalizeFlags(
    runtime::HostVisibleBuffer &flags,
    runtime::HostVisibleBuffer &normalizedFlags, const std::size_t elementCount,
    std::string &error) {
  const auto blockCount = ceilDiv(elementCount, kWorkgroupSize);
  if (!validateIntLength("compaction flags", flags, elementCount, error) ||
      !validateIntLength("normalized compaction flags", normalizedFlags,
                         elementCount, error) ||
      !validateAlias("flag normalization", flags, normalizedFlags, false,
                     error) ||
      !ensureMapped(flags, "compaction flags", error) ||
      !ensureMapped(normalizedFlags, "normalized compaction flags", error) ||
      !flags.flush(error) || !normalizedFlags.flush(error) ||
      !updateDescriptors(dummyFloat_, dummyFloat_, flags, normalizedFlags,
                         dummyInt_, error)) {
    return false;
  }
  const std::array pre{
      makeBarrier(flags.handle(), flags.size(), VK_ACCESS_HOST_WRITE_BIT,
                  VK_ACCESS_SHADER_READ_BIT),
      makeBarrier(normalizedFlags.handle(), normalizedFlags.size(),
                  VK_ACCESS_HOST_WRITE_BIT, VK_ACCESS_SHADER_WRITE_BIT)};
  const std::array post{
      makeBarrier(normalizedFlags.handle(), normalizedFlags.size(),
                  VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT)};
  if (!dispatchKernel(normalizeFlagsPipeline_, blockCount,
                      {static_cast<std::uint32_t>(elementCount), 0u}, pre, post,
                      error)) {
    return false;
  }
  return normalizedFlags.invalidate(error);
}

bool ReductionScanPrimitives::dispatchCompactionCount(
    runtime::HostVisibleBuffer &normalizedFlags,
    runtime::HostVisibleBuffer &offsets, runtime::HostVisibleBuffer &count,
    const std::size_t elementCount, std::size_t &selectedCount,
    std::string &error) {
  if (!validateIntLength("normalized compaction flags", normalizedFlags,
                         elementCount, error) ||
      !validateIntLength("compaction offsets", offsets, elementCount, error) ||
      !validateIntLength("compaction count", count, 1u, error) ||
      !ensureMapped(normalizedFlags, "normalized compaction flags", error) ||
      !ensureMapped(offsets, "compaction offsets", error) ||
      !ensureMapped(count, "compaction count", error) ||
      !normalizedFlags.flush(error) || !offsets.flush(error) ||
      !count.flush(error) ||
      !updateDescriptors(dummyFloat_, dummyFloat_, normalizedFlags, offsets,
                         count, error)) {
    return false;
  }
  const std::array pre{
      makeBarrier(normalizedFlags.handle(), normalizedFlags.size(),
                  VK_ACCESS_HOST_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT),
      makeBarrier(offsets.handle(), offsets.size(), VK_ACCESS_HOST_WRITE_BIT,
                  VK_ACCESS_SHADER_READ_BIT),
      makeBarrier(count.handle(), count.size(), VK_ACCESS_HOST_WRITE_BIT,
                  VK_ACCESS_SHADER_WRITE_BIT)};
  const std::array post{makeBarrier(count.handle(), count.size(),
                                    VK_ACCESS_SHADER_WRITE_BIT,
                                    VK_ACCESS_HOST_READ_BIT)};
  if (!dispatchKernel(compactionCountPipeline_, 1u,
                      {static_cast<std::uint32_t>(elementCount), 0u}, pre, post,
                      error) ||
      !count.invalidate(error)) {
    return false;
  }
  std::uint32_t result = 0u;
  std::memcpy(&result, count.mappedPtr(), sizeof(result));
  selectedCount = result;
  if (selectedCount > elementCount) {
    return setError(error, "compaction",
                    "GPU selected count exceeds the input length");
  }
  return true;
}

bool ReductionScanPrimitives::dispatchCompactionScatter(
    runtime::HostVisibleBuffer &input, runtime::HostVisibleBuffer &output,
    runtime::HostVisibleBuffer &normalizedFlags,
    runtime::HostVisibleBuffer &offsets, const std::size_t elementCount,
    const bool inputIsFloat, std::string &error) {
  const auto blockCount = ceilDiv(elementCount, kWorkgroupSize);
  const bool validInput =
      inputIsFloat
          ? validateFloatLength("compaction input", input, elementCount, error)
          : validateIntLength("compaction input", input, elementCount, error);
  if (!validInput ||
      !validateIntLength("normalized compaction flags", normalizedFlags,
                         elementCount, error) ||
      !validateIntLength("compaction offsets", offsets, elementCount, error) ||
      !ensureMapped(input, "compaction input", error) ||
      !ensureMapped(output, "compaction output", error) ||
      !ensureMapped(normalizedFlags, "normalized compaction flags", error) ||
      !ensureMapped(offsets, "compaction offsets", error) ||
      !input.flush(error) || !output.flush(error) ||
      !normalizedFlags.flush(error) || !offsets.flush(error) ||
      !updateDescriptors(input, output, normalizedFlags, offsets, dummyInt_,
                         error)) {
    return false;
  }
  const std::array pre{
      makeBarrier(input.handle(), input.size(), VK_ACCESS_HOST_WRITE_BIT,
                  VK_ACCESS_SHADER_READ_BIT),
      makeBarrier(normalizedFlags.handle(), normalizedFlags.size(),
                  VK_ACCESS_HOST_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT),
      makeBarrier(offsets.handle(), offsets.size(), VK_ACCESS_HOST_WRITE_BIT,
                  VK_ACCESS_SHADER_READ_BIT),
      makeBarrier(output.handle(), output.size(), VK_ACCESS_HOST_WRITE_BIT,
                  VK_ACCESS_SHADER_WRITE_BIT)};
  const std::array post{makeBarrier(output.handle(), output.size(),
                                    VK_ACCESS_SHADER_WRITE_BIT,
                                    VK_ACCESS_HOST_READ_BIT)};
  auto &pipeline =
      inputIsFloat ? compactFloatPipeline_ : compactUInt32Pipeline_;
  if (!dispatchKernel(pipeline, blockCount,
                      {static_cast<std::uint32_t>(elementCount), 0u}, pre, post,
                      error)) {
    return false;
  }
  return output.invalidate(error);
}

bool ReductionScanPrimitives::scanIntRecursive(
    runtime::HostVisibleBuffer &input, const std::size_t elementCount,
    runtime::HostVisibleBuffer &output, std::string &error) {
  const auto blockCount = ceilDiv(elementCount, kWorkgroupSize);
  runtime::HostVisibleBuffer blockSums{};
  if (!createIntBuffer(blockCount, blockSums, error) ||
      !dispatchScanBlocks(input, output, blockSums, elementCount, error)) {
    return false;
  }
  if (blockCount == 1u) {
    return true;
  }
  runtime::HostVisibleBuffer blockOffsets{};
  if (!createIntBuffer(blockCount, blockOffsets, error) ||
      !scanIntRecursive(blockSums, blockCount, blockOffsets, error)) {
    return false;
  }
  return dispatchScanAddOffsets(output, blockOffsets, elementCount, error);
}

bool ReductionScanPrimitives::reduceSumMinMax(runtime::HostVisibleBuffer &input,
                                              const std::size_t elementCount,
                                              ReductionScanStats &stats,
                                              std::string &error) {
  if (!isReady(error) ||
      !validateFloatLength("reduction input", input, elementCount, error)) {
    return false;
  }
  if (elementCount == 0u) {
    stats = {};
    return true;
  }
  if (elementCount > std::numeric_limits<std::uint32_t>::max()) {
    return setError(error, "validation", "reduction length exceeds uint32");
  }

  runtime::HostVisibleBuffer partialA{};
  runtime::HostVisibleBuffer partialB{};
  runtime::HostVisibleBuffer *currentInput = &input;
  runtime::HostVisibleBuffer *currentOutput = &partialA;
  std::size_t currentCount = elementCount;
  bool inputIsTriples = false;
  while (true) {
    const auto blockCount = ceilDiv(currentCount, kWorkgroupSize);
    if (!createFloatBuffer(blockCount * 3u, *currentOutput, error) ||
        !dispatchReduce(*currentInput, *currentOutput, currentCount,
                        inputIsTriples, error)) {
      return false;
    }
    if (blockCount == 1u) {
      if (!ensureMapped(*currentOutput, "reduction result", error) ||
          !currentOutput->invalidate(error)) {
        return false;
      }
      std::array<float, 3u> result{};
      std::memcpy(result.data(), currentOutput->mappedPtr(), sizeof(result));
      stats = {result[0], result[1], result[2]};
      return true;
    }
    currentInput = currentOutput;
    currentOutput = currentOutput == &partialA ? &partialB : &partialA;
    currentCount = blockCount;
    inputIsTriples = true;
  }
}

bool ReductionScanPrimitives::exclusiveScanInt(
    runtime::HostVisibleBuffer &input, const std::size_t inputElementCount,
    runtime::HostVisibleBuffer &output, const std::size_t outputElementCount,
    std::string &error, const ReductionScanOptions options) {
  if (!isReady(error)) {
    return false;
  }
  if (inputElementCount != outputElementCount) {
    return setError(error, "validation",
                    "scan input and output lengths must match");
  }
  if (!validateIntLength("scan input", input, inputElementCount, error) ||
      !validateIntLength("scan output", output, outputElementCount, error) ||
      !validateAlias("scan", input, output, options.allowInPlaceScan, error)) {
    return false;
  }
  if (inputElementCount == 0u) {
    return true;
  }
  if (inputElementCount > std::numeric_limits<std::uint32_t>::max()) {
    return setError(error, "validation", "scan length exceeds uint32");
  }
  return scanIntRecursive(input, inputElementCount, output, error);
}

bool ReductionScanPrimitives::stableCompact(
    runtime::HostVisibleBuffer &input, const std::size_t inputElementCount,
    runtime::HostVisibleBuffer &flags, const std::size_t flagElementCount,
    runtime::HostVisibleBuffer &output, const std::size_t outputElementCapacity,
    std::size_t &selectedCount, const bool inputIsFloat, std::string &error) {
  selectedCount = 0u;
  if (!isReady(error)) {
    return false;
  }
  if (inputElementCount != flagElementCount) {
    return setError(error, "validation",
                    "compaction input and flag lengths must match");
  }
  if (inputElementCount > std::numeric_limits<std::uint32_t>::max()) {
    return setError(error, "validation", "compaction length exceeds uint32");
  }

  const bool validInput = inputIsFloat
                              ? validateFloatLength("compaction input", input,
                                                    inputElementCount, error)
                              : validateIntLength("compaction input", input,
                                                  inputElementCount, error);
  const bool validOutput =
      inputIsFloat ? validateFloatLength("compaction output", output,
                                         outputElementCapacity, error)
                   : validateIntLength("compaction output", output,
                                       outputElementCapacity, error);
  if (!validInput || !validOutput ||
      !validateIntLength("compaction flags", flags, flagElementCount, error) ||
      !validateAlias("compaction", input, output, false, error)) {
    return false;
  }
  if (flags.handle() == input.handle() || flags.handle() == output.handle()) {
    return setError(error, "validation",
                    "compaction flags must not alias input or output");
  }
  if (inputElementCount == 0u) {
    return true;
  }

  runtime::HostVisibleBuffer normalizedFlags{};
  runtime::HostVisibleBuffer offsets{};
  runtime::HostVisibleBuffer count{};
  if (!createIntBuffer(inputElementCount, normalizedFlags, error) ||
      !createIntBuffer(inputElementCount, offsets, error) ||
      !createIntBuffer(1u, count, error) ||
      !dispatchNormalizeFlags(flags, normalizedFlags, inputElementCount,
                              error) ||
      !scanIntRecursive(normalizedFlags, inputElementCount, offsets, error) ||
      !dispatchCompactionCount(normalizedFlags, offsets, count,
                               inputElementCount, selectedCount, error)) {
    return false;
  }
  if (selectedCount > outputElementCapacity) {
    return setError(
        error, "validation",
        "compaction output capacity is smaller than selected count");
  }
  if (selectedCount == 0u) {
    return true;
  }
  return dispatchCompactionScatter(input, output, normalizedFlags, offsets,
                                   inputElementCount, inputIsFloat, error);
}

bool ReductionScanPrimitives::stableCompactFloat(
    runtime::HostVisibleBuffer &input, const std::size_t inputElementCount,
    runtime::HostVisibleBuffer &flags, const std::size_t flagElementCount,
    runtime::HostVisibleBuffer &output, const std::size_t outputElementCapacity,
    std::size_t &selectedCount, std::string &error) {
  return stableCompact(input, inputElementCount, flags, flagElementCount,
                       output, outputElementCapacity, selectedCount, true,
                       error);
}

bool ReductionScanPrimitives::stableCompactUInt32(
    runtime::HostVisibleBuffer &input, const std::size_t inputElementCount,
    runtime::HostVisibleBuffer &flags, const std::size_t flagElementCount,
    runtime::HostVisibleBuffer &output, const std::size_t outputElementCapacity,
    std::size_t &selectedCount, std::string &error) {
  return stableCompact(input, inputElementCount, flags, flagElementCount,
                       output, outputElementCapacity, selectedCount, false,
                       error);
}

const runtime::VulkanDevice &ReductionScanPrimitives::device() const {
  return device_;
}

} // namespace viennaps::vulkan::primitives
