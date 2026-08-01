// Copyright 2026 ViennaPS
// SPDX-License-Identifier: MIT
//
// Production Vulkan radix-sort primitive wrapper. This layer composes the
// reusable Vulkan runtime RAII helpers.

#include "radix_sort_primitives.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>
#include <limits>
#include <sstream>
#include <vector>

#include <vulkan/vulkan.h>

namespace {

[[nodiscard]] bool setError(std::string &error, const std::string_view phase,
                            const std::string_view message) {
  error = std::string(phase) + ": " + std::string(message);
  return false;
}

[[nodiscard]] std::size_t ceilDiv(const std::size_t numerator,
                                  const std::size_t denominator) {
  return numerator / denominator + (numerator % denominator != 0u);
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
constexpr std::uint64_t kFenceTimeoutNs = 10'000'000'000ULL;

} // namespace

namespace viennaps::vulkan::primitives {

bool RadixSortPrimitives::initialize(const std::string_view spirvPath,
                                     std::string &error) {
  if (isInitialized()) {
    return true;
  }
  if (spirvPath.empty()) {
    return setError(error, "initialization", "SPIR-V path is empty");
  }

  reset();
  const auto failInitialization = [this, &error]() {
    const std::string detail = error;
    reset();
    return setError(error, "initialization", detail);
  };

  if (!instance_.create(error)) {
    return failInitialization();
  }
  viennaps::vulkan::runtime::ComputeDeviceSelection selection{};
  if (!viennaps::vulkan::runtime::pickFirstComputeDevice(instance_.get(),
                                                         selection, error)) {
    return failInitialization();
  }
  if (!device_.create(selection, error)) {
    return failInitialization();
  }

  viennaps::vulkan::runtime::SpirvProgram program{};
  if (!viennaps::vulkan::runtime::readSpirv(spirvPath, program, error) ||
      !shaderModule_.create(device_, program, error)) {
    return failInitialization();
  }

  std::array<VkDescriptorSetLayoutBinding, 5u> bindings{};
  for (std::size_t index = 0u; index < bindings.size(); ++index) {
    bindings[index].binding = static_cast<std::uint32_t>(index);
    bindings[index].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    bindings[index].descriptorCount = 1u;
    bindings[index].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
  }
  if (!descriptorSetLayout_.create(
          device_,
          std::span<const VkDescriptorSetLayoutBinding>(bindings.data(),
                                                        bindings.size()),
          error)) {
    return failInitialization();
  }

  const VkPushConstantRange pushConstantRange{VK_SHADER_STAGE_COMPUTE_BIT, 0u,
                                              sizeof(PushConstants)};
  if (!pipelineLayout_.create(device_, descriptorSetLayout_.get(),
                              std::span(&pushConstantRange, 1u), error)) {
    return failInitialization();
  }
  if (!descriptorPool_.create(device_, 1u, 5u,
                              VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, error) ||
      !descriptorPool_.allocate(descriptorSetLayout_.get(), descriptorSet_,
                                error) ||
      !commandContext_.create(device_, device_.computeQueueFamily(), error) ||
      !commandContext_.allocatePrimary(commandBuffer_, error) ||
      !fence_.create(device_, error)) {
    return failInitialization();
  }

  if (!createPipeline(histogramPipeline_, RadixSortOperation::histogram,
                      error) ||
      !createPipeline(scatterPipeline_, RadixSortOperation::scatter, error)) {
    return failInitialization();
  }
  return true;
}

void RadixSortPrimitives::reset() {
  fence_.destroy();
  commandContext_.reset();
  descriptorPool_.reset();
  histogramPipeline_.reset();
  scatterPipeline_.reset();
  pipelineLayout_.reset();
  descriptorSetLayout_.reset();
  shaderModule_.reset();
  device_.reset();
  instance_.reset();
  descriptorSet_ = VK_NULL_HANDLE;
  commandBuffer_ = VK_NULL_HANDLE;
}

bool RadixSortPrimitives::isInitialized() const {
  return instance_.isValid() && device_.isValid() &&
         shaderModule_.get() != VK_NULL_HANDLE &&
         descriptorSetLayout_.get() != VK_NULL_HANDLE &&
         pipelineLayout_.get() != VK_NULL_HANDLE &&
         histogramPipeline_.get() != VK_NULL_HANDLE &&
         scatterPipeline_.get() != VK_NULL_HANDLE &&
         descriptorSet_ != VK_NULL_HANDLE && commandBuffer_ != VK_NULL_HANDLE &&
         fence_.get() != VK_NULL_HANDLE &&
         commandContext_.pool() != VK_NULL_HANDLE;
}

bool RadixSortPrimitives::isReady(std::string &error) const {
  if (!isInitialized()) {
    return setError(error, "execution",
                    "radix sort primitives are not initialized");
  }
  return true;
}

bool RadixSortPrimitives::createPipeline(
    viennaps::vulkan::runtime::ComputePipeline &pipeline,
    const RadixSortOperation operation, std::string &error) {
  const auto operationValue = static_cast<std::uint32_t>(operation);
  const VkSpecializationMapEntry entry{0u, 0u, sizeof(operationValue)};
  const viennaps::vulkan::runtime::ComputePipelineOptions options{
      kPipelineEntryPoint,
      std::span<const VkSpecializationMapEntry>(&entry, 1u), &operationValue,
      sizeof(operationValue)};
  return pipeline.create(device_, shaderModule_, pipelineLayout_, options,
                         error);
}

bool RadixSortPrimitives::ensureBufferSize(std::string_view label,
                                           runtime::HostVisibleBuffer &buffer,
                                           const std::size_t elementCount,
                                           const std::size_t elementSize,
                                           std::string &error) const {
  if (!buffer.isValid()) {
    return setError(error, "buffer resize", std::string(label) + " is invalid");
  }
  const auto requiredBytes =
      static_cast<VkDeviceSize>(elementCount * elementSize);
  if (buffer.size() >= requiredBytes) {
    return true;
  }
  return setError(error, "buffer resize",
                  std::string(label) +
                      " capacity is smaller than requested size");
}

bool RadixSortPrimitives::createBuffer(const std::size_t elementCount,
                                       const std::size_t elementSize,
                                       runtime::HostVisibleBuffer &buffer,
                                       std::string &error) {
  if (elementSize == 0u ||
      elementCount > std::numeric_limits<std::size_t>::max() / elementSize) {
    return setError(error, "createBuffer", "requested size overflows");
  }
  const auto allocatedElements = std::max<std::size_t>(1u, elementCount);
  if (allocatedElements >
      std::numeric_limits<std::size_t>::max() / elementSize) {
    return setError(error, "createBuffer",
                    "requested buffer size overflows allocation expression");
  }
  const auto bytes = static_cast<VkDeviceSize>(allocatedElements * elementSize);
  if (bytes > device_.selection().properties.limits.maxStorageBufferRange) {
    return setError(error, "createBuffer",
                    "requested size exceeds maxStorageBufferRange");
  }
  return buffer.create(device_, bytes, kBufferUsage, kMemoryFlags, error);
}

bool RadixSortPrimitives::createKeyBuffer(std::size_t elementCount,
                                          runtime::HostVisibleBuffer &buffer,
                                          std::string &error) {
  return createBuffer(elementCount, sizeof(std::uint32_t), buffer, error);
}

bool RadixSortPrimitives::createValueBuffer(std::size_t elementCount,
                                            runtime::HostVisibleBuffer &buffer,
                                            std::string &error) {
  return createBuffer(elementCount, sizeof(std::uint32_t), buffer, error);
}

bool RadixSortPrimitives::validateLength(
    const std::string_view label,
    const viennaps::vulkan::runtime::HostVisibleBuffer &buffer,
    const std::size_t elementCount, const std::size_t elementSize,
    std::string &error) const {
  if (!buffer.isValid()) {
    return setError(error, "validation",
                    std::string(label) + " buffer is not initialized");
  }
  if (buffer.ownerDevice() != device_.get()) {
    return setError(error, "validation",
                    std::string(label) +
                        " buffer belongs to a different device");
  }
  const auto capacity = static_cast<std::size_t>(buffer.size() / elementSize);
  if (elementCount > capacity) {
    std::ostringstream out;
    out << std::string(label) << " exceeds buffer capacity (" << elementCount
        << " > " << capacity << ")";
    return setError(error, "validation", out.str());
  }
  return true;
}

bool RadixSortPrimitives::validateAlias(
    const std::string_view label,
    const viennaps::vulkan::runtime::HostVisibleBuffer &input,
    const viennaps::vulkan::runtime::HostVisibleBuffer &output,
    const bool allowAlias, std::string &error) const {
  if (!allowAlias && input.handle() == output.handle()) {
    return setError(error, "validation",
                    std::string(label) + ": buffers are aliased");
  }
  return true;
}

bool RadixSortPrimitives::validateSameDevice(
    std::string &error, const runtime::HostVisibleBuffer &left,
    const runtime::HostVisibleBuffer &right,
    const std::string_view label) const {
  if (!left.isValid() || !right.isValid()) {
    return setError(error, "validation",
                    std::string(label) +
                        ": buffer must be initialized before sort");
  }
  if (left.ownerDevice() != right.ownerDevice()) {
    return setError(error, "validation",
                    std::string(label) + ": buffers must use same device");
  }
  return true;
}

bool RadixSortPrimitives::validateInputLengths(
    std::string &error, const std::size_t keyCount,
    const std::size_t valueCount, const std::size_t sortedKeyCapacity,
    const std::size_t sortedValueCapacity) const {
  if (keyCount != valueCount) {
    return setError(error, "validation",
                    "input key and value element counts must match");
  }
  if (sortedKeyCapacity < keyCount || sortedValueCapacity < keyCount) {
    return setError(error, "validation",
                    "output capacity is smaller than input length");
  }
  return true;
}

bool RadixSortPrimitives::validateDeviceLimits(std::string &error,
                                               std::size_t elementCount) const {
  if (elementCount > std::numeric_limits<std::uint32_t>::max()) {
    return setError(error, "validation",
                    "elementCount exceeds 32-bit dispatch support");
  }
  if (kWorkgroupSize > device_.selection()
                           .properties.limits.maxComputeWorkGroupInvocations ||
      kWorkgroupSize >
          device_.selection().properties.limits.maxComputeWorkGroupSize[0]) {
    return setError(error, "validation",
                    "device does not support required workgroup size");
  }
  if (device_.selection().properties.limits.maxComputeWorkGroupCount[0] == 0u) {
    return setError(error, "validation",
                    "device reports no available workgroup count");
  }
  const auto &limits = device_.selection().properties.limits;
  if (kRequiredSharedMemoryBytes > limits.maxComputeSharedMemorySize) {
    return setError(error, "validation",
                    "device shared-memory limit is too small for radix tiles");
  }
  if (elementCount > limits.maxStorageBufferRange / sizeof(std::uint32_t)) {
    return setError(error, "validation",
                    "radix data range exceeds maxStorageBufferRange");
  }
  const auto workgroupCount = ceilDiv(elementCount, kWorkgroupSize);
  if (workgroupCount >
      limits.maxStorageBufferRange / (kBins * sizeof(std::uint32_t))) {
    return setError(error, "validation",
                    "radix histogram range exceeds maxStorageBufferRange");
  }
  return true;
}

bool RadixSortPrimitives::ensureMapped(runtime::HostVisibleBuffer &buffer,
                                       const std::string_view label,
                                       std::string &error) const {
  if (!buffer.isValid()) {
    return setError(error, "mapping",
                    std::string(label) + " is not initialized");
  }
  return buffer.mappedPtr() != nullptr || buffer.map(error);
}

bool RadixSortPrimitives::clearBuffer(runtime::HostVisibleBuffer &buffer,
                                      const std::size_t elementCount,
                                      std::string &error) const {
  if (elementCount == 0u) {
    return true;
  }
  if (!validateLength("histogram", buffer, elementCount, sizeof(std::uint32_t),
                      error)) {
    return false;
  }
  if (!ensureMapped(buffer, "histogram", error)) {
    return false;
  }
  std::memset(buffer.mappedPtr(), 0,
              static_cast<std::size_t>(elementCount * sizeof(std::uint32_t)));
  return buffer.flush(error);
}

bool RadixSortPrimitives::readHistogram(runtime::HostVisibleBuffer &buffer,
                                        const std::size_t elementCount,
                                        std::vector<std::uint32_t> &counts,
                                        std::string &error) const {
  if (!validateLength("histogram read", buffer, elementCount,
                      sizeof(std::uint32_t), error)) {
    return false;
  }
  if (!buffer.invalidate(error) ||
      !ensureMapped(buffer, "histogram read", error)) {
    return false;
  }
  counts.resize(elementCount);
  if (counts.empty()) {
    return true;
  }
  return buffer.read(
      counts.data(),
      static_cast<VkDeviceSize>(counts.size() * sizeof(std::uint32_t)), 0u,
      error);
}

bool RadixSortPrimitives::writeHistogram(
    runtime::HostVisibleBuffer &buffer,
    const std::vector<std::uint32_t> &counts, std::string &error) const {
  if (!validateLength("histogram write", buffer, counts.size(),
                      sizeof(std::uint32_t), error)) {
    return false;
  }
  if (!ensureMapped(buffer, "histogram write", error)) {
    return false;
  }
  if (counts.empty()) {
    return true;
  }
  return buffer.write(
             counts.data(),
             static_cast<VkDeviceSize>(counts.size() * sizeof(std::uint32_t)),
             0u, error) &&
         buffer.flush(error);
}

bool RadixSortPrimitives::materializePrefix(std::vector<std::uint32_t> &counts,
                                            const std::size_t workgroupCount,
                                            const std::size_t elementCount,
                                            std::string &error) const {
  if (workgroupCount == 0u ||
      workgroupCount > std::numeric_limits<std::size_t>::max() / kBins ||
      counts.size() != workgroupCount * kBins) {
    return setError(error, "materializePrefix", "histogram size mismatch");
  }
  std::uint64_t bucketBase = 0u;
  for (std::size_t bucket = 0u; bucket < kBins; ++bucket) {
    std::uint64_t next = bucketBase;
    for (std::size_t group = 0u; group < workgroupCount; ++group) {
      const auto index = group * kBins + bucket;
      const std::uint32_t count = counts[index];
      if (next > std::numeric_limits<std::uint32_t>::max()) {
        return setError(error, "materializePrefix",
                        "radix offset exceeds uint32 range");
      }
      counts[index] = static_cast<std::uint32_t>(next);
      next += count;
    }
    bucketBase = next;
  }
  if (bucketBase != elementCount) {
    return setError(error, "materializePrefix",
                    "GPU histogram total differs from input length");
  }
  return true;
}

bool RadixSortPrimitives::copyBuffer(runtime::HostVisibleBuffer &source,
                                     const std::size_t elementCount,
                                     runtime::HostVisibleBuffer &destination,
                                     std::string &error) const {
  if (!validateLength("copy source", source, elementCount,
                      sizeof(std::uint32_t), error) ||
      !validateLength("copy destination", destination, elementCount,
                      sizeof(std::uint32_t), error) ||
      !ensureMapped(source, "copy source", error) ||
      !ensureMapped(destination, "copy destination", error)) {
    return false;
  }
  if (!source.invalidate(error)) {
    return setError(error, "execution", "failed to invalidate source buffer");
  }
  std::vector<std::uint32_t> staging(elementCount);
  if (!source.read(
          staging.data(),
          static_cast<VkDeviceSize>(elementCount * sizeof(std::uint32_t)), 0u,
          error)) {
    return false;
  }
  if (!destination.write(
          staging.data(),
          static_cast<VkDeviceSize>(staging.size() * sizeof(std::uint32_t)), 0u,
          error) ||
      !destination.flush(error)) {
    return false;
  }
  return true;
}

bool RadixSortPrimitives::dispatch(const DispatchContext &context,
                                   const RadixSortOperation operation,
                                   std::string &error) const {
  if (!isReady(error)) {
    return false;
  }
  if (context.elementCount == 0u) {
    return true;
  }
  const auto dispatchX = ceilDiv(context.elementCount, kWorkgroupSize);
  if (dispatchX > std::numeric_limits<std::size_t>::max() / kBins) {
    return setError(error, "dispatch", "histogram length overflows");
  }
  const auto histogramElements = dispatchX * kBins;
  if (!validateDeviceLimits(error, context.elementCount) ||
      !validateLength("dispatch input keys", *context.inputKeys,
                      context.elementCount, sizeof(std::uint32_t), error) ||
      !validateLength("dispatch input values", *context.inputValues,
                      context.elementCount, sizeof(std::uint32_t), error) ||
      !validateLength("dispatch output keys", *context.outputKeys,
                      context.elementCount, sizeof(std::uint32_t), error) ||
      !validateLength("dispatch output values", *context.outputValues,
                      context.elementCount, sizeof(std::uint32_t), error) ||
      !validateLength("dispatch histogram", *context.histogram,
                      histogramElements, sizeof(std::uint32_t), error)) {
    return false;
  }
  if (!ensureMapped(*context.inputKeys, "dispatch input keys", error) ||
      !ensureMapped(*context.inputValues, "dispatch input values", error) ||
      !ensureMapped(*context.outputKeys, "dispatch output keys", error) ||
      !ensureMapped(*context.outputValues, "dispatch output values", error) ||
      !ensureMapped(*context.histogram, "dispatch histogram", error)) {
    return false;
  }
  if (!context.inputKeys->flush(error) || !context.inputValues->flush(error) ||
      !context.outputKeys->flush(error) ||
      !context.outputValues->flush(error) || !context.histogram->flush(error)) {
    return false;
  }

  if (dispatchX >
      device_.selection().properties.limits.maxComputeWorkGroupCount[0]) {
    return setError(error, "dispatch", "dispatch group count exceeds limit");
  }
  if (dispatchX == 0u ||
      dispatchX > std::numeric_limits<std::uint32_t>::max()) {
    return setError(error, "dispatch", "dispatch group count is invalid");
  }

  VkCommandBufferBeginInfo beginInfo{};
  beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
  beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  if (vkResetCommandBuffer(commandBuffer_, 0u) != VK_SUCCESS) {
    return setError(error, "dispatch", "vkResetCommandBuffer failed");
  }
  if (vkBeginCommandBuffer(commandBuffer_, &beginInfo) != VK_SUCCESS) {
    return setError(error, "dispatch", "vkBeginCommandBuffer failed");
  }

  const std::size_t dataBytes = context.elementCount * sizeof(std::uint32_t);
  const std::size_t histogramBytes = histogramElements * sizeof(std::uint32_t);

  std::array<VkDescriptorBufferInfo, 5u> infos{};
  infos[0u] = VkDescriptorBufferInfo{context.inputKeys->handle(), 0u,
                                     std::max<std::size_t>(1u, dataBytes)};
  infos[1u] = VkDescriptorBufferInfo{context.inputValues->handle(), 0u,
                                     std::max<std::size_t>(1u, dataBytes)};
  infos[2u] = VkDescriptorBufferInfo{context.outputKeys->handle(), 0u,
                                     std::max<std::size_t>(1u, dataBytes)};
  infos[3u] = VkDescriptorBufferInfo{context.outputValues->handle(), 0u,
                                     std::max<std::size_t>(1u, dataBytes)};
  infos[4u] = VkDescriptorBufferInfo{context.histogram->handle(), 0u,
                                     std::max<std::size_t>(1u, histogramBytes)};

  std::array<VkWriteDescriptorSet, 5u> writes{};
  for (std::uint32_t index = 0u; index < writes.size(); ++index) {
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

  std::vector<VkBufferMemoryBarrier> preBarriers;
  std::vector<VkBufferMemoryBarrier> postBarriers;
  preBarriers.reserve(5u);
  postBarriers.reserve(3u);

  preBarriers.push_back(
      makeBarrier(context.inputKeys->handle(), dataBytes,
                  VK_ACCESS_HOST_READ_BIT | VK_ACCESS_HOST_WRITE_BIT,
                  VK_ACCESS_SHADER_READ_BIT));
  preBarriers.push_back(
      makeBarrier(context.inputValues->handle(), dataBytes,
                  VK_ACCESS_HOST_READ_BIT | VK_ACCESS_HOST_WRITE_BIT,
                  VK_ACCESS_SHADER_READ_BIT));

  if (operation == RadixSortOperation::scatter) {
    preBarriers.push_back(makeBarrier(context.histogram->handle(),
                                      histogramBytes, VK_ACCESS_HOST_WRITE_BIT,
                                      VK_ACCESS_SHADER_READ_BIT));
    preBarriers.push_back(makeBarrier(context.outputKeys->handle(), dataBytes,
                                      VK_ACCESS_HOST_WRITE_BIT,
                                      VK_ACCESS_SHADER_WRITE_BIT));
    preBarriers.push_back(makeBarrier(context.outputValues->handle(), dataBytes,
                                      VK_ACCESS_HOST_WRITE_BIT,
                                      VK_ACCESS_SHADER_WRITE_BIT));
  } else {
    preBarriers.push_back(makeBarrier(context.histogram->handle(),
                                      histogramBytes, VK_ACCESS_HOST_WRITE_BIT,
                                      VK_ACCESS_SHADER_WRITE_BIT));
  }

  if (operation == RadixSortOperation::histogram) {
    postBarriers.push_back(
        makeBarrier(context.histogram->handle(), histogramBytes,
                    VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT));
  } else {
    postBarriers.push_back(makeBarrier(context.outputKeys->handle(), dataBytes,
                                       VK_ACCESS_SHADER_WRITE_BIT,
                                       VK_ACCESS_HOST_READ_BIT));
    postBarriers.push_back(makeBarrier(context.outputValues->handle(),
                                       dataBytes, VK_ACCESS_SHADER_WRITE_BIT,
                                       VK_ACCESS_HOST_READ_BIT));
    postBarriers.push_back(
        makeBarrier(context.histogram->handle(), histogramBytes,
                    VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_HOST_READ_BIT));
  }

  VkPipeline pipeline = VK_NULL_HANDLE;
  switch (operation) {
  case RadixSortOperation::histogram:
    pipeline = histogramPipeline_.get();
    break;
  case RadixSortOperation::scatter:
    pipeline = scatterPipeline_.get();
    break;
  default:
    return setError(error, "dispatch", "unsupported radix operation");
  }

  vkCmdPipelineBarrier(commandBuffer_, VK_PIPELINE_STAGE_HOST_BIT,
                       VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0u, 0u, nullptr,
                       static_cast<std::uint32_t>(preBarriers.size()),
                       preBarriers.data(), 0u, nullptr);
  vkCmdBindPipeline(commandBuffer_, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
  vkCmdBindDescriptorSets(commandBuffer_, VK_PIPELINE_BIND_POINT_COMPUTE,
                          pipelineLayout_.get(), 0u, 1u, &descriptorSet_, 0u,
                          nullptr);

  const PushConstants constants{
      static_cast<std::uint32_t>(context.elementCount), context.bitShift};
  vkCmdPushConstants(commandBuffer_, pipelineLayout_.get(),
                     VK_SHADER_STAGE_COMPUTE_BIT, 0u, sizeof(PushConstants),
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
  if (!fence_.wait(kFenceTimeoutNs, error)) {
    return false;
  }
  fence_.reset();

  if (operation == RadixSortOperation::histogram) {
    if (!context.histogram->invalidate(error)) {
      return setError(error, "execution", "failed to invalidate histogram");
    }
  } else {
    if (!context.outputKeys->invalidate(error) ||
        !context.outputValues->invalidate(error) ||
        !context.histogram->invalidate(error)) {
      return setError(error, "execution", "failed to invalidate outputs");
    }
  }
  return true;
}

bool RadixSortPrimitives::sortByKey(
    runtime::HostVisibleBuffer &inputKeys, const std::size_t inputElementCount,
    runtime::HostVisibleBuffer &inputValues, const std::size_t inputValueCount,
    runtime::HostVisibleBuffer &sortedKeys, const std::size_t sortedKeyCapacity,
    runtime::HostVisibleBuffer &sortedValues,
    const std::size_t sortedValueCapacity, std::string &error,
    const RadixSortOptions options) {
  if (!isReady(error)) {
    return false;
  }
  if (!validateInputLengths(error, inputElementCount, inputValueCount,
                            sortedKeyCapacity, sortedValueCapacity)) {
    return false;
  }
  if (!validateLength("sort input keys", inputKeys, inputElementCount,
                      sizeof(std::uint32_t), error) ||
      !validateLength("sort input values", inputValues, inputElementCount,
                      sizeof(std::uint32_t), error) ||
      !validateLength("sort output keys", sortedKeys, sortedKeyCapacity,
                      sizeof(std::uint32_t), error) ||
      !validateLength("sort output values", sortedValues, sortedValueCapacity,
                      sizeof(std::uint32_t), error) ||
      !validateAlias("input key/value relation", inputKeys, inputValues, false,
                     error) ||
      !validateAlias("output key/value relation", sortedKeys, sortedValues,
                     false, error) ||
      !validateAlias("cross key/value relation", inputKeys, sortedValues, false,
                     error) ||
      !validateAlias("cross value/key relation", inputValues, sortedKeys, false,
                     error) ||
      !validateSameDevice(error, inputKeys, inputValues,
                          "validation key/value relation") ||
      !validateSameDevice(error, inputKeys, sortedKeys,
                          "validation key output relation") ||
      !validateSameDevice(error, inputValues, sortedValues,
                          "validation value output relation") ||
      !validateDeviceLimits(error, inputElementCount)) {
    return false;
  }

  const bool keysInPlace = inputKeys.handle() == sortedKeys.handle();
  const bool valuesInPlace = inputValues.handle() == sortedValues.handle();
  if (keysInPlace != valuesInPlace ||
      (keysInPlace && !options.allowInPlaceSort)) {
    return setError(
        error, "validation",
        "in-place sort requires both key/value pairs and explicit opt-in");
  }
  const bool inPlaceRequested = keysInPlace && valuesInPlace;

  if (inputElementCount == 0u) {
    return true;
  }

  const auto workgroupCount = ceilDiv(inputElementCount, kWorkgroupSize);
  if (workgroupCount > std::numeric_limits<std::size_t>::max() / kBins) {
    return setError(error, "validation", "histogram length overflows");
  }
  const auto histogramElements = workgroupCount * kBins;

  runtime::HostVisibleBuffer histogram{};
  runtime::HostVisibleBuffer scratchKeys{};
  runtime::HostVisibleBuffer scratchValues{};
  if (!createKeyBuffer(histogramElements, histogram, error) ||
      !createKeyBuffer(inputElementCount, scratchKeys, error) ||
      !createValueBuffer(inputElementCount, scratchValues, error)) {
    return false;
  }

  const runtime::HostVisibleBuffer *sourceKeys = &inputKeys;
  const runtime::HostVisibleBuffer *sourceValues = &inputValues;
  runtime::HostVisibleBuffer *destinationKeys =
      inPlaceRequested ? &scratchKeys : &sortedKeys;
  runtime::HostVisibleBuffer *destinationValues =
      inPlaceRequested ? &scratchValues : &sortedValues;

  std::vector<std::uint32_t> counts(histogramElements);

  for (std::size_t pass = 0u; pass < kPasses; ++pass) {
    if (!clearBuffer(histogram, histogramElements, error)) {
      return false;
    }
    if (!dispatch({const_cast<runtime::HostVisibleBuffer *>(sourceKeys),
                   const_cast<runtime::HostVisibleBuffer *>(sourceValues),
                   destinationKeys, destinationValues, &histogram,
                   inputElementCount, kBitShifts[pass]},
                  RadixSortOperation::histogram, error) ||
        !readHistogram(histogram, histogramElements, counts, error) ||
        !materializePrefix(counts, workgroupCount, inputElementCount, error) ||
        !writeHistogram(histogram, counts, error) ||
        !dispatch({const_cast<runtime::HostVisibleBuffer *>(sourceKeys),
                   const_cast<runtime::HostVisibleBuffer *>(sourceValues),
                   destinationKeys, destinationValues, &histogram,
                   inputElementCount, kBitShifts[pass]},
                  RadixSortOperation::scatter, error)) {
      return false;
    }
    sourceKeys = destinationKeys;
    sourceValues = destinationValues;
    if (inPlaceRequested) {
      destinationKeys = sourceKeys->handle() == inputKeys.handle()
                            ? &scratchKeys
                            : &inputKeys;
      destinationValues = sourceValues->handle() == inputValues.handle()
                              ? &scratchValues
                              : &inputValues;
    } else {
      destinationKeys = sourceKeys->handle() == sortedKeys.handle()
                            ? &scratchKeys
                            : &sortedKeys;
      destinationValues = sourceValues->handle() == sortedValues.handle()
                              ? &scratchValues
                              : &sortedValues;
    }
  }

  if (inPlaceRequested) {
    if (sourceKeys->handle() != inputKeys.handle()) {
      if (!copyBuffer(*const_cast<runtime::HostVisibleBuffer *>(sourceKeys),
                      inputElementCount, inputKeys, error) ||
          !copyBuffer(*const_cast<runtime::HostVisibleBuffer *>(sourceValues),
                      inputElementCount, inputValues, error)) {
        return false;
      }
    }
    return true;
  }

  if (!copyBuffer(*const_cast<runtime::HostVisibleBuffer *>(sourceKeys),
                  inputElementCount, sortedKeys, error) ||
      !copyBuffer(*const_cast<runtime::HostVisibleBuffer *>(sourceValues),
                  inputElementCount, sortedValues, error)) {
    return false;
  }
  return true;
}

const runtime::VulkanDevice &RadixSortPrimitives::device() const {
  return device_;
}

} // namespace viennaps::vulkan::primitives
