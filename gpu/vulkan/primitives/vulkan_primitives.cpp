// Copyright 2026 ViennaPS
// SPDX-License-Identifier: MIT
//
// Production Vulkan elementwise primitive wrapper. This layer composes the
// reusable Vulkan runtime RAII helpers and intentionally avoids
// re-instantiating Vulkan instance/device/buffer lifecycles.

#include "vulkan_primitives.hpp"

#include <array>
#include <cstddef>
#include <limits>
#include <sstream>
#include <string>

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
  float scalarA;
  float scalarB;
};

} // namespace

namespace viennaps::vulkan::primitives {

bool ElementwisePrimitives::initialize(const std::string_view spirvPath,
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

  if (!createPipeline(fillPipeline_, ElementwiseOperation::fill, error) ||
      !createPipeline(copyPipeline_, ElementwiseOperation::copy, error) ||
      !createPipeline(affinePipeline_, ElementwiseOperation::affine, error)) {
    return failInitialization();
  }
  return true;
}

void ElementwisePrimitives::reset() {
  fence_.destroy();
  commandContext_.reset();
  descriptorPool_.reset();
  fillPipeline_.reset();
  copyPipeline_.reset();
  affinePipeline_.reset();
  pipelineLayout_.reset();
  descriptorSetLayout_.reset();
  shaderModule_.reset();
  device_.reset();
  instance_.reset();
  descriptorSet_ = VK_NULL_HANDLE;
  commandBuffer_ = VK_NULL_HANDLE;
}

bool ElementwisePrimitives::isInitialized() const {
  return instance_.isValid() && device_.isValid() &&
         shaderModule_.get() != VK_NULL_HANDLE &&
         descriptorSetLayout_.get() != VK_NULL_HANDLE &&
         pipelineLayout_.get() != VK_NULL_HANDLE &&
         fillPipeline_.get() != VK_NULL_HANDLE &&
         copyPipeline_.get() != VK_NULL_HANDLE &&
         affinePipeline_.get() != VK_NULL_HANDLE &&
         descriptorSet_ != VK_NULL_HANDLE && commandBuffer_ != VK_NULL_HANDLE &&
         fence_.get() != VK_NULL_HANDLE &&
         commandContext_.pool() != VK_NULL_HANDLE;
}

bool ElementwisePrimitives::isReady(std::string &error) const {
  if (!isInitialized()) {
    return setError(error, "execution",
                    "elementwise primitives are not initialized");
  }
  if (device_.get() == VK_NULL_HANDLE || instance_.get() == VK_NULL_HANDLE) {
    return setError(error, "execution",
                    "runtime device or instance is missing");
  }
  return true;
}

bool ElementwisePrimitives::createPipeline(
    viennaps::vulkan::runtime::ComputePipeline &pipeline,
    const ElementwiseOperation operation, std::string &error) {
  const auto operationValue = static_cast<std::uint32_t>(operation);
  const VkSpecializationMapEntry entry{0u, 0u, sizeof(std::uint32_t)};
  const viennaps::vulkan::runtime::ComputePipelineOptions options{
      kPipelineEntryPoint,
      std::span<const VkSpecializationMapEntry>(&entry, 1u), &operationValue,
      sizeof(operationValue)};
  return pipeline.create(device_, shaderModule_, pipelineLayout_, options,
                         error);
}

bool ElementwisePrimitives::createFloatBuffer(
    const std::size_t elementCount,
    viennaps::vulkan::runtime::HostVisibleBuffer &buffer, std::string &error) {
  if (!isReady(error)) {
    return false;
  }

  const auto allocatedElements = elementCount == 0u ? 1u : elementCount;
  if (allocatedElements >
      std::numeric_limits<std::size_t>::max() / sizeof(float)) {
    return setError(error, "createFloatBuffer",
                    "requested element count overflows Vulkan device size");
  }
  const auto bytes =
      static_cast<VkDeviceSize>(allocatedElements * sizeof(float));
  if (!buffer.create(device_, bytes, kFloatBufferUsage, kFloatMemoryFlags,
                     error)) {
    return false;
  }
  return true;
}

bool ElementwisePrimitives::validateLength(
    const std::string_view label,
    const viennaps::vulkan::runtime::HostVisibleBuffer &buffer,
    const std::size_t elementCount, std::string &error) const {
  if (!buffer.isValid()) {
    return setError(error, "validation",
                    std::string(label) + ": buffer is not initialized");
  }
  if (elementCount == 0u) {
    return true;
  }
  const auto maxElements =
      static_cast<std::size_t>(buffer.size() / sizeof(float));
  if (elementCount > maxElements) {
    std::ostringstream out;
    out << std::string(label) << ": requested " << elementCount
        << " elements but buffer has " << maxElements;
    return setError(error, "validation", out.str());
  }
  return true;
}

bool ElementwisePrimitives::validateAlias(
    const std::string_view label,
    const viennaps::vulkan::runtime::HostVisibleBuffer &input,
    const viennaps::vulkan::runtime::HostVisibleBuffer &output,
    const bool allowInPlace, std::string &error) const {
  if (!allowInPlace && input.handle() == output.handle()) {
    std::ostringstream out;
    out << std::string(label)
        << ": input and output buffers are the same handle but in-place is "
           "not allowed";
    return setError(error, "validation", out.str());
  }
  return true;
}

bool ElementwisePrimitives::ensureMapped(
    viennaps::vulkan::runtime::HostVisibleBuffer &buffer,
    const std::string_view label, std::string &error) const {
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

bool ElementwisePrimitives::readOutputIfNeeded(
    viennaps::vulkan::runtime::HostVisibleBuffer &buffer,
    std::string &error) const {
  if (!ensureMapped(buffer, "execution", error)) {
    return false;
  }
  if (!buffer.invalidate(error)) {
    return setError(error, "execution",
                    "failed to invalidate output buffer for CPU readback");
  }
  return true;
}

bool ElementwisePrimitives::dispatch(const OperationContext &context,
                                     std::string &error) const {
  if (!isReady(error)) {
    return false;
  }
  if (context.elementCount == 0u) {
    return true;
  }
  if (!validateLength("execution input", *context.input, context.elementCount,
                      error) ||
      !validateLength("execution output", *context.output, context.elementCount,
                      error)) {
    return false;
  }
  if (context.operation == ElementwiseOperation::copy &&
      !validateAlias("copy", *context.input, *context.output,
                     context.allowInPlace, error)) {
    return false;
  }
  if (context.operation == ElementwiseOperation::affine &&
      !validateAlias("affine", *context.input, *context.output,
                     context.allowInPlace, error)) {
    return false;
  }

  if (!ensureMapped(*context.input, "execution", error) ||
      !ensureMapped(*context.output, "execution", error)) {
    return false;
  }
  if (!context.input->flush(error) ||
      (context.input->handle() != context.output->handle() &&
       !context.output->flush(error))) {
    return setError(error, "execution", "failed to flush host writes");
  }

  if (context.elementCount >
      std::numeric_limits<VkDeviceSize>::max() / sizeof(float)) {
    return setError(error, "dispatch",
                    "elementCount exceeds the dispatch byte limit");
  }

  const auto bytesToProcess =
      static_cast<VkDeviceSize>(context.elementCount * sizeof(float));
  const auto dispatchGroups = context.elementCount / kWorkgroupSize +
                              (context.elementCount % kWorkgroupSize != 0u);
  const auto maxDispatchGroups =
      device_.selection().properties.limits.maxComputeWorkGroupCount[0];
  if (dispatchGroups > maxDispatchGroups) {
    return setError(error, "dispatch",
                    "dispatch group count exceeds the device limit");
  }

  VkBufferMemoryBarrier preBarriers[2u]{};
  preBarriers[0].sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
  preBarriers[0].srcAccessMask = VK_ACCESS_HOST_WRITE_BIT;
  const bool aliases = context.input->handle() == context.output->handle();
  preBarriers[0].dstAccessMask =
      aliases ? VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT
              : VK_ACCESS_SHADER_READ_BIT;
  preBarriers[0].buffer = context.input->handle();
  preBarriers[0].offset = 0u;
  preBarriers[0].size = bytesToProcess;
  preBarriers[1].sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
  preBarriers[1].srcAccessMask = VK_ACCESS_HOST_WRITE_BIT;
  preBarriers[1].dstAccessMask =
      VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
  preBarriers[1].buffer = context.output->handle();
  preBarriers[1].offset = 0u;
  preBarriers[1].size = bytesToProcess;
  const std::uint32_t preBarrierCount = aliases ? 1u : 2u;

  VkDescriptorBufferInfo inputInfo{};
  inputInfo.buffer = context.input->handle();
  inputInfo.offset = 0u;
  inputInfo.range = bytesToProcess;
  VkDescriptorBufferInfo outputInfo{};
  outputInfo.buffer = context.output->handle();
  outputInfo.offset = 0u;
  outputInfo.range = bytesToProcess;
  std::array<VkWriteDescriptorSet, 2u> writes{};
  writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
  writes[0].dstSet = descriptorSet_;
  writes[0].dstBinding = 0u;
  writes[0].descriptorCount = 1u;
  writes[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  writes[0].pBufferInfo = &inputInfo;
  writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
  writes[1].dstSet = descriptorSet_;
  writes[1].dstBinding = 1u;
  writes[1].descriptorCount = 1u;
  writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  writes[1].pBufferInfo = &outputInfo;
  vkUpdateDescriptorSets(device_.get(),
                         static_cast<std::uint32_t>(writes.size()),
                         writes.data(), 0u, nullptr);

  if (context.operation != ElementwiseOperation::fill &&
      context.operation != ElementwiseOperation::copy &&
      context.operation != ElementwiseOperation::affine) {
    return setError(error, "dispatch", "unsupported operation");
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
                       preBarrierCount, preBarriers, 0u, nullptr);

  VkPipeline pipeline = VK_NULL_HANDLE;
  switch (context.operation) {
  case ElementwiseOperation::fill:
    pipeline = fillPipeline_.get();
    break;
  case ElementwiseOperation::copy:
    pipeline = copyPipeline_.get();
    break;
  case ElementwiseOperation::affine:
    pipeline = affinePipeline_.get();
    break;
  default:
    return setError(error, "dispatch", "unsupported operation");
  }

  vkCmdBindPipeline(commandBuffer_, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
  vkCmdBindDescriptorSets(commandBuffer_, VK_PIPELINE_BIND_POINT_COMPUTE,
                          pipelineLayout_.get(), 0u, 1u, &descriptorSet_, 0u,
                          nullptr);

  const PushConstants constants{
      static_cast<std::uint32_t>(context.elementCount), context.scalarA,
      context.scalarB};
  vkCmdPushConstants(commandBuffer_, pipelineLayout_.get(),
                     VK_SHADER_STAGE_COMPUTE_BIT, 0u, sizeof(PushConstants),
                     &constants);
  vkCmdDispatch(commandBuffer_, static_cast<std::uint32_t>(dispatchGroups), 1u,
                1u);

  VkBufferMemoryBarrier postBarrier{};
  postBarrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
  postBarrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
  postBarrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
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

  if (!readOutputIfNeeded(*context.output, error)) {
    return false;
  }
  return true;
}

bool ElementwisePrimitives::fill(runtime::HostVisibleBuffer &output,
                                 const std::size_t elementCount,
                                 const float value, std::string &error) const {
  if (!isReady(error)) {
    return false;
  }
  if (!validateLength("fill", output, elementCount, error)) {
    return false;
  }
  if (elementCount == 0u) {
    return true;
  }
  return dispatch({ElementwiseOperation::fill, &output, &output, elementCount,
                   value, kDefaultFillValue, false},
                  error);
}

bool ElementwisePrimitives::copy(
    runtime::HostVisibleBuffer &output, const std::size_t outputElementCount,
    runtime::HostVisibleBuffer &input, const std::size_t inputElementCount,
    std::string &error, const ElementwisePrimitivesOptions options) const {
  if (!isReady(error)) {
    return false;
  }
  if (outputElementCount != inputElementCount) {
    return setError(error, "validation",
                    "copy requires matching input and output element counts");
  }
  if (!validateAlias("copy", input, output, options.allowInPlaceCopy, error)) {
    return false;
  }
  if (!validateLength("copy output", output, outputElementCount, error) ||
      !validateLength("copy input", input, inputElementCount, error)) {
    return false;
  }
  if (outputElementCount == 0u) {
    return true;
  }
  return dispatch({ElementwiseOperation::copy, &input, &output,
                   outputElementCount, kDefaultFillValue, 0.0F,
                   options.allowInPlaceCopy},
                  error);
}

bool ElementwisePrimitives::affineTransform(
    runtime::HostVisibleBuffer &output, const std::size_t outputElementCount,
    runtime::HostVisibleBuffer &input, const std::size_t inputElementCount,
    const float scale, const float offset, std::string &error,
    const ElementwisePrimitivesOptions options) const {
  if (!isReady(error)) {
    return false;
  }
  if (outputElementCount != inputElementCount) {
    return setError(error, "validation",
                    "affineTransform requires matching input and output "
                    "element counts");
  }
  if (!validateAlias("affine", input, output, options.allowInPlaceAffine,
                     error)) {
    return false;
  }
  if (!validateLength("affine output", output, outputElementCount, error) ||
      !validateLength("affine input", input, inputElementCount, error)) {
    return false;
  }
  if (outputElementCount == 0u) {
    return true;
  }
  return dispatch({ElementwiseOperation::affine, &input, &output,
                   outputElementCount, scale, offset,
                   options.allowInPlaceAffine},
                  error);
}

const runtime::VulkanDevice &ElementwisePrimitives::device() const {
  return device_;
}

} // namespace viennaps::vulkan::primitives
