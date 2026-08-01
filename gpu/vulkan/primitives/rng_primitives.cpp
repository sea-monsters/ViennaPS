// Copyright 2026 ViennaPS
// SPDX-License-Identifier: MIT
//
// Production Vulkan RNG primitive wrapper. This layer composes the reusable
// Vulkan runtime RAII helpers and keeps the GPU-side generation deterministic.

#include "rng_primitives.hpp"

#include <array>
#include <cstddef>
#include <limits>
#include <span>
#include <sstream>
#include <string>
#include <string_view>

namespace {

[[nodiscard]] bool setError(std::string &error, const std::string_view phase,
                            const std::string_view message) {
  if (message.empty()) {
    error = std::string(phase) + ": operation failed";
  } else {
    error = std::string(phase) + ": " + std::string(message);
  }
  return false;
}

constexpr std::string_view kPipelineEntryPoint{"main"};

struct PushConstants {
  std::uint32_t elementCount;
  std::uint32_t seed;
  std::uint32_t counterOffset;
};

} // namespace

namespace viennaps::vulkan::primitives {

bool RNGPrimitives::initialize(const std::string_view spirvPath,
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
  if (!viennaps::vulkan::runtime::readSpirv(spirvPath, program, error)) {
    return failInitialization();
  }
  if (!shaderModule_.create(device_, program, error)) {
    return failInitialization();
  }

  std::array<VkDescriptorSetLayoutBinding, 2u> layoutBindings{};
  layoutBindings[0].binding = 0u;
  layoutBindings[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  layoutBindings[0].descriptorCount = 1u;
  layoutBindings[0].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
  layoutBindings[1].binding = 1u;
  layoutBindings[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  layoutBindings[1].descriptorCount = 1u;
  layoutBindings[1].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
  if (!descriptorSetLayout_.create(
          device_,
          std::span<const VkDescriptorSetLayoutBinding>(layoutBindings.data(),
                                                        layoutBindings.size()),
          error)) {
    return failInitialization();
  }

  const VkPushConstantRange pushConstantRange{VK_SHADER_STAGE_COMPUTE_BIT, 0u,
                                              sizeof(PushConstants)};
  if (!pipelineLayout_.create(device_, descriptorSetLayout_.get(),
                              std::span(&pushConstantRange, 1u), error)) {
    return failInitialization();
  }

  if (!descriptorPool_.create(device_, 1u, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                              error)) {
    return failInitialization();
  }
  if (!descriptorPool_.allocate(descriptorSetLayout_.get(), descriptorSet_,
                                error)) {
    return failInitialization();
  }
  if (!commandContext_.create(device_, device_.computeQueueFamily(), error)) {
    return failInitialization();
  }
  if (!commandContext_.allocatePrimary(commandBuffer_, error)) {
    return failInitialization();
  }
  if (!fence_.create(device_, error)) {
    return failInitialization();
  }

  if (!createPipeline(u32Pipeline_, RNGOperation::generateUint32, error) ||
      !createPipeline(f32Pipeline_, RNGOperation::generateFloat01, error)) {
    return failInitialization();
  }
  return true;
}

void RNGPrimitives::reset() {
  fence_.destroy();
  commandContext_.reset();
  descriptorPool_.reset();
  u32Pipeline_.reset();
  f32Pipeline_.reset();
  pipelineLayout_.reset();
  descriptorSetLayout_.reset();
  shaderModule_.reset();
  device_.reset();
  instance_.reset();
  descriptorSet_ = VK_NULL_HANDLE;
  commandBuffer_ = VK_NULL_HANDLE;
}

bool RNGPrimitives::isInitialized() const {
  return instance_.isValid() && device_.isValid() &&
         shaderModule_.get() != VK_NULL_HANDLE &&
         descriptorSetLayout_.get() != VK_NULL_HANDLE &&
         pipelineLayout_.get() != VK_NULL_HANDLE &&
         u32Pipeline_.get() != VK_NULL_HANDLE &&
         f32Pipeline_.get() != VK_NULL_HANDLE &&
         descriptorSet_ != VK_NULL_HANDLE && commandBuffer_ != VK_NULL_HANDLE &&
         fence_.get() != VK_NULL_HANDLE &&
         commandContext_.pool() != VK_NULL_HANDLE;
}

bool RNGPrimitives::isReady(std::string &error) const {
  if (!isInitialized()) {
    return setError(error, "execution", "RNG primitives are not initialized");
  }
  if (device_.get() == VK_NULL_HANDLE || instance_.get() == VK_NULL_HANDLE) {
    return setError(error, "execution",
                    "runtime device or instance is missing");
  }
  return true;
}

bool RNGPrimitives::createPipeline(
    viennaps::vulkan::runtime::ComputePipeline &pipeline,
    const RNGOperation operation, std::string &error) {
  const auto operationValue = static_cast<std::uint32_t>(operation);
  const VkSpecializationMapEntry entry{0u, 0u, sizeof(std::uint32_t)};
  const viennaps::vulkan::runtime::ComputePipelineOptions options{
      kPipelineEntryPoint,
      std::span<const VkSpecializationMapEntry>(&entry, 1u), &operationValue,
      sizeof(operationValue)};
  return pipeline.create(device_, shaderModule_, pipelineLayout_, options,
                         error);
}

bool RNGPrimitives::createBuffer(std::size_t elementCount,
                                 runtime::HostVisibleBuffer &buffer,
                                 const std::size_t elementSize,
                                 std::string &error) {
  if (!isReady(error)) {
    return false;
  }
  const auto allocatedElements = elementCount == 0u ? 1u : elementCount;
  if (allocatedElements >
      (std::numeric_limits<std::size_t>::max() / elementSize)) {
    return setError(error, "createBuffer",
                    "requested element count overflows Vulkan buffer size");
  }
  const auto bytes = static_cast<VkDeviceSize>(allocatedElements * elementSize);
  return buffer.create(device_, bytes, kBufferUsage, kMemoryPropertyFlags,
                       error);
}

bool RNGPrimitives::createUint32Buffer(std::size_t elementCount,
                                       runtime::HostVisibleBuffer &buffer,
                                       std::string &error) {
  return createBuffer(elementCount, buffer, kUint32Size, error);
}

bool RNGPrimitives::createFloatBuffer(std::size_t elementCount,
                                      runtime::HostVisibleBuffer &buffer,
                                      std::string &error) {
  return createBuffer(elementCount, buffer, kFloatSize, error);
}

bool RNGPrimitives::validateOutputLength(
    const std::string_view label,
    const viennaps::vulkan::runtime::HostVisibleBuffer &buffer,
    const std::size_t elementCount, const std::size_t elementSize,
    std::string &error) const {
  if (!buffer.isValid()) {
    return setError(error, "validation",
                    std::string(label) + ": buffer is not initialized");
  }
  if (elementCount == 0u) {
    return true;
  }
  const auto maxElements =
      static_cast<std::size_t>(buffer.size() / elementSize);
  if (elementCount > maxElements) {
    std::ostringstream out;
    out << std::string(label) << ": requested " << elementCount
        << " elements but buffer has " << maxElements;
    return setError(error, "validation", out.str());
  }
  return true;
}

bool RNGPrimitives::ensureMapped(runtime::HostVisibleBuffer &buffer,
                                 const std::string_view label,
                                 std::string &error) const {
  if (!buffer.isValid()) {
    std::ostringstream out;
    out << std::string(label) << ": buffer is not initialized";
    return setError(error, "execution", out.str());
  }
  if (!buffer.mappedPtr() && !buffer.map(error)) {
    return false;
  }
  return true;
}

bool RNGPrimitives::isDispatchLengthSupported(
    const std::size_t elementCount, std::string &error,
    const std::uint32_t counterOffset) const {
  if (!isReady(error)) {
    return false;
  }
  if (elementCount == 0u) {
    return true;
  }
  if (elementCount > std::numeric_limits<std::uint32_t>::max() ||
      elementCount > (std::numeric_limits<std::size_t>::max() / kUint32Size)) {
    return setError(error, "dispatch",
                    "elementCount exceeds the uint32 dispatch limit");
  }
  const auto lastIndex = static_cast<std::uint32_t>(elementCount - 1u);
  if (counterOffset > std::numeric_limits<std::uint32_t>::max() - lastIndex) {
    return setError(error, "dispatch", "counter range overflows uint32");
  }

  const auto dispatchGroups =
      elementCount / kWorkgroupSize + (elementCount % kWorkgroupSize != 0u);
  const auto maxDispatchGroups =
      device_.selection().properties.limits.maxComputeWorkGroupCount[0];
  if (dispatchGroups > maxDispatchGroups) {
    return setError(error, "dispatch",
                    "dispatch group count exceeds the device limit");
  }
  return true;
}

bool RNGPrimitives::dispatch(const DispatchContext &context,
                             std::string &error) {
  if (!isReady(error)) {
    return false;
  }
  if (context.elementCount == 0u) {
    return true;
  }
  if (!isDispatchLengthSupported(context.elementCount, error,
                                 context.counterOffset)) {
    return false;
  }

  const auto outputElementSize =
      (context.operation == RNGOperation::generateUint32) ? kUint32Size
                                                          : kFloatSize;
  if (!validateOutputLength("execution output", *context.output,
                            context.elementCount, outputElementSize, error)) {
    return false;
  }
  if (!ensureMapped(*context.output, "execution", error)) {
    return false;
  }

  runtime::HostVisibleBuffer scratch{};
  const auto scratchElementSize =
      (context.operation == RNGOperation::generateUint32) ? kFloatSize
                                                          : kUint32Size;
  if (!createBuffer(1u, scratch, scratchElementSize, error)) {
    return false;
  }
  if (!ensureMapped(scratch, "execution", error)) {
    return false;
  }

  if (!context.output->flush(error) || !scratch.flush(error)) {
    return setError(error, "execution", "failed to flush host writes");
  }

  const auto bytesToProcess =
      static_cast<VkDeviceSize>(context.elementCount * outputElementSize);
  const auto dispatchGroups =
      static_cast<std::uint32_t>(context.elementCount / kWorkgroupSize +
                                 (context.elementCount % kWorkgroupSize != 0u));

  VkBufferMemoryBarrier preBarrier{};
  preBarrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
  preBarrier.srcAccessMask = VK_ACCESS_HOST_WRITE_BIT;
  preBarrier.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
  preBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  preBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  preBarrier.buffer = context.output->handle();
  preBarrier.offset = 0u;
  preBarrier.size = bytesToProcess;

  VkDescriptorBufferInfo outputU32{};
  outputU32.buffer = context.operation == RNGOperation::generateUint32
                         ? context.output->handle()
                         : scratch.handle();
  outputU32.offset = 0u;
  outputU32.range =
      context.operation == RNGOperation::generateUint32
          ? static_cast<VkDeviceSize>(context.elementCount * kUint32Size)
          : scratchElementSize;

  VkDescriptorBufferInfo outputF32{};
  outputF32.buffer = context.operation == RNGOperation::generateFloat01
                         ? context.output->handle()
                         : scratch.handle();
  outputF32.offset = 0u;
  outputF32.range =
      context.operation == RNGOperation::generateFloat01
          ? static_cast<VkDeviceSize>(context.elementCount * kFloatSize)
          : scratchElementSize;

  std::array<VkWriteDescriptorSet, 2u> writes{};
  writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
  writes[0].dstSet = descriptorSet_;
  writes[0].dstBinding = 0u;
  writes[0].descriptorCount = 1u;
  writes[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  writes[0].pBufferInfo = &outputU32;

  writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
  writes[1].dstSet = descriptorSet_;
  writes[1].dstBinding = 1u;
  writes[1].descriptorCount = 1u;
  writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  writes[1].pBufferInfo = &outputF32;

  vkUpdateDescriptorSets(device_.get(),
                         static_cast<std::uint32_t>(writes.size()),
                         writes.data(), 0u, nullptr);

  if (context.operation != RNGOperation::generateUint32 &&
      context.operation != RNGOperation::generateFloat01) {
    return setError(error, "dispatch", "unsupported RNG operation");
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

  vkCmdPipelineBarrier(commandBuffer_, VK_PIPELINE_STAGE_HOST_BIT,
                       VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0u, 0u, nullptr,
                       1u, &preBarrier, 0u, nullptr);

  VkPipeline pipeline = VK_NULL_HANDLE;
  switch (context.operation) {
  case RNGOperation::generateUint32:
    pipeline = u32Pipeline_.get();
    break;
  case RNGOperation::generateFloat01:
    pipeline = f32Pipeline_.get();
    break;
  default:
    return setError(error, "dispatch", "unsupported RNG operation");
  }

  vkCmdBindPipeline(commandBuffer_, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
  vkCmdBindDescriptorSets(commandBuffer_, VK_PIPELINE_BIND_POINT_COMPUTE,
                          pipelineLayout_.get(), 0u, 1u, &descriptorSet_, 0u,
                          nullptr);

  const PushConstants constants{
      static_cast<std::uint32_t>(context.elementCount), context.seed,
      context.counterOffset};
  vkCmdPushConstants(commandBuffer_, pipelineLayout_.get(),
                     VK_SHADER_STAGE_COMPUTE_BIT, 0u, sizeof(PushConstants),
                     &constants);
  vkCmdDispatch(commandBuffer_, dispatchGroups, 1u, 1u);

  VkBufferMemoryBarrier postBarrier{};
  postBarrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
  postBarrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
  postBarrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
  postBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  postBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  postBarrier.buffer = context.output->handle();
  postBarrier.offset = 0u;
  postBarrier.size = bytesToProcess;
  vkCmdPipelineBarrier(commandBuffer_, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                       VK_PIPELINE_STAGE_HOST_BIT, 0u, 0u, nullptr, 1u,
                       &postBarrier, 0u, nullptr);

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

  if (!context.output->invalidate(error)) {
    return setError(error, "execution",
                    "failed to invalidate output buffer for CPU readback");
  }
  return true;
}

bool RNGPrimitives::generate(std::size_t elementCount, const std::uint32_t seed,
                             runtime::HostVisibleBuffer &output,
                             std::string &error,
                             const std::uint32_t counterOffset) {
  if (!isReady(error)) {
    return false;
  }
  return dispatch({RNGOperation::generateUint32, &output, elementCount, seed,
                   counterOffset},
                  error);
}

bool RNGPrimitives::generateFloat01(const std::size_t elementCount,
                                    const std::uint32_t seed,
                                    runtime::HostVisibleBuffer &output,
                                    std::string &error,
                                    const std::uint32_t counterOffset) {
  if (!isReady(error)) {
    return false;
  }
  return dispatch({RNGOperation::generateFloat01, &output, elementCount, seed,
                   counterOffset},
                  error);
}

const runtime::VulkanDevice &RNGPrimitives::device() const { return device_; }

} // namespace viennaps::vulkan::primitives
