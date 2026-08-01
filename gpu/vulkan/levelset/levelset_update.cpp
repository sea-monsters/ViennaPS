// Copyright 2026 ViennaPS
// SPDX-License-Identifier: MIT

#include "levelset_update.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <string_view>

namespace {

constexpr std::uint32_t kLocalSize = 256U;
constexpr std::uint32_t kBindingCount = 4U;
constexpr VkBufferUsageFlags kBufferUsage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                            VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                                            VK_BUFFER_USAGE_TRANSFER_DST_BIT;
constexpr VkMemoryPropertyFlags kMemoryProperties =
    VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT;

struct PushConstants {
  std::uint32_t pointCount = 0;
  float timeStep = 0.0F;
  float integrationCutoff = 0.0F;
  std::uint32_t checkDissipation = 0;
};

static_assert(sizeof(PushConstants) == 16U);

struct GpuRateEntry {
  float gradient = 0.0F;
  float dissipation = 0.0F;
  float stopValue = 0.0F;
  float padding = 0.0F;
};

static_assert(sizeof(GpuRateEntry) == 16U);

[[nodiscard]] bool fail(std::string &error, const std::string_view phase,
                        const std::string_view message) {
  error = std::string(phase) + ": " + std::string(message);
  return false;
}

[[nodiscard]] bool
validateInput(const viennaps::vulkan::levelset::LevelSetUpdateInput &input,
              std::string &error) {
  const std::size_t pointCount = input.values.size();
  if (pointCount > std::numeric_limits<std::uint32_t>::max()) {
    return fail(error, "validation", "point count exceeds uint32 range");
  }
  if (input.rateOffsets.size() != pointCount + 1U) {
    return fail(error, "validation",
                "rateOffsets must contain pointCount + 1 entries");
  }
  if (input.rateOffsets.empty() || input.rateOffsets.front() != 0U) {
    return fail(error, "validation", "rateOffsets must start at zero");
  }
  const std::size_t rateCount = input.gradients.size();
  if (rateCount != input.dissipations.size() ||
      rateCount != input.stopValues.size()) {
    return fail(error, "validation", "rate arrays must have equal lengths");
  }
  if (rateCount > std::numeric_limits<std::uint32_t>::max() ||
      input.rateOffsets.back() != rateCount) {
    return fail(error, "validation",
                "final rate offset must equal the rate-array length");
  }
  if (!std::isfinite(input.timeStep) || input.timeStep < 0.0F) {
    return fail(error, "validation",
                "timeStep must be finite and non-negative");
  }
  if (!std::isfinite(input.integrationCutoff) ||
      input.integrationCutoff < 0.0F) {
    return fail(error, "validation",
                "integrationCutoff must be finite and non-negative");
  }

  for (std::size_t point = 0; point < pointCount; ++point) {
    const std::uint32_t begin = input.rateOffsets[point];
    const std::uint32_t end = input.rateOffsets[point + 1U];
    if (begin >= end || end > rateCount) {
      return fail(error, "validation",
                  "each point must own a non-empty monotonic rate range");
    }
    if (std::abs(input.stopValues[end - 1U]) !=
        std::numeric_limits<float>::max()) {
      return fail(error, "validation",
                  "each point's final stop value must be the float sentinel");
    }
  }

  for (const float value : input.values) {
    if (!std::isfinite(value)) {
      return fail(error, "validation", "level-set values must be finite");
    }
  }
  for (std::size_t rate = 0; rate < rateCount; ++rate) {
    if (!std::isfinite(input.gradients[rate]) ||
        !std::isfinite(input.dissipations[rate]) ||
        !std::isfinite(input.stopValues[rate])) {
      return fail(error, "validation", "rate data must be finite");
    }
    const float velocity = input.gradients[rate] - input.dissipations[rate];
    if (!std::isfinite(velocity) || !std::isfinite(input.timeStep * velocity)) {
      return fail(error, "validation",
                  "rate velocity or time-scaled rate is not finite");
    }
  }
  return true;
}

[[nodiscard]] VkDescriptorSetLayoutBinding
makeBinding(const std::uint32_t binding) {
  VkDescriptorSetLayoutBinding result{};
  result.binding = binding;
  result.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  result.descriptorCount = 1;
  result.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
  return result;
}

template <class T>
[[nodiscard]] bool
createAndWrite(viennaps::vulkan::runtime::VulkanDevice &device,
               viennaps::vulkan::runtime::HostVisibleBuffer &buffer,
               const std::span<const T> values, std::string &error) {
  const std::size_t allocatedCount = std::max<std::size_t>(values.size(), 1U);
  if (allocatedCount > std::numeric_limits<std::size_t>::max() / sizeof(T)) {
    return fail(error, "buffer", "allocation size overflow");
  }
  const VkDeviceSize bytes = allocatedCount * sizeof(T);
  if (!buffer.create(device, bytes, kBufferUsage, kMemoryProperties, error) ||
      !buffer.map(error)) {
    return false;
  }
  return values.empty() ||
         buffer.write(values.data(), values.size_bytes(), 0, error);
}

[[nodiscard]] const char *statusMessage(const std::uint32_t status) {
  switch (status) {
  case 1U:
    return "material transition encountered zero velocity";
  case 2U:
    return "material transition exhausted its CSR rate range";
  case 3U:
    return "shader produced a non-finite result";
  default:
    return "shader reported an unknown failure";
  }
}

} // namespace

namespace viennaps::vulkan::levelset {

bool updateLevelSetFp32(runtime::ComputeSession &session,
                        const runtime::SpirvProgram &program,
                        const LevelSetUpdateInput &input,
                        std::vector<float> &output, std::string &error) {
  error.clear();
  if (!validateInput(input, error)) {
    return false;
  }
  if (!session.isValid()) {
    return fail(error, "session", "compute session is not initialized");
  }
  if (program.words.empty()) {
    return fail(error, "shader", "SPIR-V program is empty");
  }
  if (input.values.empty()) {
    output.clear();
    return true;
  }

  const std::uint64_t groupCount =
      (static_cast<std::uint64_t>(input.values.size()) + kLocalSize - 1U) /
      kLocalSize;
  if (groupCount >
      session.selection().properties.limits.maxComputeWorkGroupCount[0]) {
    return fail(error, "dispatch", "workgroup count exceeds device limit");
  }
  const auto &limits = session.selection().properties.limits;
  const std::uint64_t maxStorageRange = limits.maxStorageBufferRange;
  const std::array<std::uint64_t, 4> requiredRanges = {
      input.values.size_bytes(), input.rateOffsets.size_bytes(),
      input.gradients.size() * sizeof(GpuRateEntry), sizeof(std::uint32_t)};
  if (std::ranges::any_of(requiredRanges, [maxStorageRange](const auto bytes) {
        return bytes > maxStorageRange;
      })) {
    return fail(error, "dispatch",
                "a storage buffer exceeds maxStorageBufferRange");
  }
  if (limits.maxPerStageDescriptorStorageBuffers < kBindingCount ||
      limits.maxPushConstantsSize < sizeof(PushConstants)) {
    return fail(error, "dispatch",
                "device descriptor or push-constant limits are insufficient");
  }

  auto &device = session.device();
  std::array<runtime::HostVisibleBuffer, kBindingCount> buffers{};
  const std::span<const std::uint32_t> offsets(input.rateOffsets);
  std::vector<GpuRateEntry> rates(input.gradients.size());
  for (std::size_t i = 0; i < rates.size(); ++i) {
    rates[i] = {input.gradients[i], input.dissipations[i], input.stopValues[i],
                0.0F};
  }
  const std::uint32_t initialStatus = 0U;
  if (!createAndWrite(device, buffers[0], input.values, error) ||
      !createAndWrite(device, buffers[1], offsets, error) ||
      !createAndWrite(device, buffers[2], std::span<const GpuRateEntry>(rates),
                      error) ||
      !createAndWrite(device, buffers[3],
                      std::span<const std::uint32_t>(&initialStatus, 1),
                      error)) {
    return false;
  }

  runtime::ShaderModule shader{};
  if (!shader.create(device, program, error)) {
    return false;
  }
  std::array<VkDescriptorSetLayoutBinding, kBindingCount> bindings{};
  for (std::uint32_t i = 0; i < kBindingCount; ++i) {
    bindings[i] = makeBinding(i);
  }
  runtime::DescriptorSetLayout descriptorLayout{};
  if (!descriptorLayout.create(device, bindings, error)) {
    return false;
  }
  const VkPushConstantRange pushRange{VK_SHADER_STAGE_COMPUTE_BIT, 0,
                                      sizeof(PushConstants)};
  runtime::PipelineLayout pipelineLayout{};
  if (!pipelineLayout.create(device, descriptorLayout.get(),
                             std::span(&pushRange, 1), error)) {
    return false;
  }
  runtime::ComputePipeline pipeline{};
  if (!pipeline.create(device, shader, pipelineLayout, error)) {
    return false;
  }
  runtime::DescriptorPool descriptorPool{};
  if (!descriptorPool.create(device, 1U, kBindingCount,
                             VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, error)) {
    return false;
  }
  VkDescriptorSet descriptorSet = VK_NULL_HANDLE;
  if (!descriptorPool.allocate(descriptorLayout.get(), descriptorSet, error)) {
    return false;
  }

  std::array<VkDescriptorBufferInfo, kBindingCount> bufferInfos{};
  std::array<VkWriteDescriptorSet, kBindingCount> descriptorWrites{};
  const std::array<VkDeviceSize, kBindingCount> bindingBytes = {
      input.values.size_bytes(), input.rateOffsets.size_bytes(),
      rates.size() * sizeof(GpuRateEntry), sizeof(std::uint32_t)};
  for (std::uint32_t i = 0; i < kBindingCount; ++i) {
    bufferInfos[i].buffer = buffers[i].handle();
    bufferInfos[i].offset = 0;
    bufferInfos[i].range = bindingBytes[i];
    descriptorWrites[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    descriptorWrites[i].dstSet = descriptorSet;
    descriptorWrites[i].dstBinding = i;
    descriptorWrites[i].descriptorCount = 1;
    descriptorWrites[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    descriptorWrites[i].pBufferInfo = &bufferInfos[i];
  }
  vkUpdateDescriptorSets(device.get(), kBindingCount, descriptorWrites.data(),
                         0, nullptr);

  VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
  auto &commandContext = session.commandContext();
  if (!commandContext.allocatePrimary(commandBuffer, error)) {
    return false;
  }
  const auto freeCommandBuffer = [&]() {
    if (commandBuffer != VK_NULL_HANDLE) {
      vkFreeCommandBuffers(device.get(), commandContext.pool(), 1,
                           &commandBuffer);
      commandBuffer = VK_NULL_HANDLE;
    }
  };

  VkCommandBufferBeginInfo beginInfo{};
  beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
  beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  if (vkBeginCommandBuffer(commandBuffer, &beginInfo) != VK_SUCCESS) {
    freeCommandBuffer();
    return fail(error, "dispatch", "vkBeginCommandBuffer failed");
  }

  std::array<VkBufferMemoryBarrier, kBindingCount> preBarriers{};
  for (std::uint32_t i = 0; i < kBindingCount; ++i) {
    preBarriers[i].sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
    preBarriers[i].srcAccessMask = VK_ACCESS_HOST_WRITE_BIT;
    preBarriers[i].dstAccessMask =
        VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    preBarriers[i].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    preBarriers[i].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    preBarriers[i].buffer = buffers[i].handle();
    preBarriers[i].offset = 0;
    preBarriers[i].size = bindingBytes[i];
  }
  vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_HOST_BIT,
                       VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr,
                       kBindingCount, preBarriers.data(), 0, nullptr);
  vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE,
                    pipeline.get());
  vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE,
                          pipelineLayout.get(), 0, 1, &descriptorSet, 0,
                          nullptr);
  const PushConstants pushConstants{
      static_cast<std::uint32_t>(input.values.size()), input.timeStep,
      input.integrationCutoff, input.checkDissipation ? 1U : 0U};
  vkCmdPushConstants(commandBuffer, pipelineLayout.get(),
                     VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pushConstants),
                     &pushConstants);
  vkCmdDispatch(commandBuffer, static_cast<std::uint32_t>(groupCount), 1, 1);

  std::array<VkBufferMemoryBarrier, 2> postBarriers{};
  for (std::uint32_t i = 0; i < postBarriers.size(); ++i) {
    const std::uint32_t binding = i == 0U ? 0U : 3U;
    postBarriers[i].sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
    postBarriers[i].srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    postBarriers[i].dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    postBarriers[i].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    postBarriers[i].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    postBarriers[i].buffer = buffers[binding].handle();
    postBarriers[i].offset = 0;
    postBarriers[i].size = bindingBytes[binding];
  }
  vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                       VK_PIPELINE_STAGE_HOST_BIT, 0, 0, nullptr,
                       static_cast<std::uint32_t>(postBarriers.size()),
                       postBarriers.data(), 0, nullptr);
  if (vkEndCommandBuffer(commandBuffer) != VK_SUCCESS) {
    freeCommandBuffer();
    return fail(error, "dispatch", "vkEndCommandBuffer failed");
  }

  runtime::Fence fence{};
  if (!fence.create(device, error)) {
    freeCommandBuffer();
    return false;
  }
  VkSubmitInfo submitInfo{};
  submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
  submitInfo.commandBufferCount = 1;
  submitInfo.pCommandBuffers = &commandBuffer;
  if (vkQueueSubmit(device.computeQueue(), 1, &submitInfo, fence.get()) !=
      VK_SUCCESS) {
    freeCommandBuffer();
    return fail(error, "dispatch", "vkQueueSubmit failed");
  }
  if (!fence.wait(10'000'000'000ULL, error)) {
    vkQueueWaitIdle(device.computeQueue());
    freeCommandBuffer();
    return false;
  }
  freeCommandBuffer();

  std::uint32_t status = 0U;
  if (!buffers[3].read(&status, sizeof(status), 0, error)) {
    return false;
  }
  if (status != 0U) {
    return fail(error, "shader", statusMessage(status));
  }
  std::vector<float> candidate(input.values.size());
  if (!buffers[0].read(candidate.data(), candidate.size() * sizeof(float), 0,
                       error)) {
    return false;
  }
  output = std::move(candidate);
  return true;
}

} // namespace viennaps::vulkan::levelset
