// Copyright 2026 ViennaPS
// SPDX-License-Identifier: MIT

#include "hrle_rebuild_classification.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <span>
#include <string_view>
#include <vector>

namespace {

namespace classification = viennaps::levelset;
namespace runtime = viennaps::vulkan::runtime;

constexpr std::uint32_t kLocalSize = 256U;
constexpr std::uint32_t kBindingCount = 4U;
constexpr std::size_t kNeighborSlots = 6U;
constexpr VkBufferUsageFlags kBufferUsage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                            VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                                            VK_BUFFER_USAGE_TRANSFER_DST_BIT;
constexpr VkMemoryPropertyFlags kMemoryProperties =
    VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT;

struct alignas(16) GpuCenter {
  float value;
  float definedValue;
  std::uint32_t pointId;
  std::uint32_t padding;
};

struct alignas(16) GpuNeighbor {
  float value;
  float definedValue;
  std::uint32_t pointId;
  std::uint32_t padding;
};

struct alignas(16) GpuDecision {
  float value;
  std::uint32_t sourcePointId;
  std::uint32_t action;
  std::uint32_t padding;
};

struct PushConstants {
  std::uint32_t elementCount;
  std::uint32_t dimensions;
  float cutoff;
  std::uint32_t padding;
};

static_assert(sizeof(GpuCenter) == 16U);
static_assert(sizeof(GpuNeighbor) == 16U);
static_assert(sizeof(GpuDecision) == 16U);
static_assert(sizeof(PushConstants) == 16U);

[[nodiscard]] bool fail(std::string &error, const std::string_view phase,
                        const std::string_view message) {
  error = std::string(phase) + ": " + std::string(message);
  return false;
}

[[nodiscard]] bool validateInput(
    const std::span<const classification::HrleRebuildCandidateFp32> candidates,
    const std::uint32_t dimensions, const float cutoff, std::string &error) {
  if (dimensions != 2U && dimensions != 3U)
    return fail(error, "validation", "dimensions must be two or three");
  if (!std::isfinite(cutoff) || cutoff < 0.0F)
    return fail(error, "validation", "cutoff must be finite and non-negative");
  if (candidates.size() > std::numeric_limits<std::uint32_t>::max())
    return fail(error, "validation", "candidate count exceeds uint32 range");

  const std::size_t neighborCount = 2U * dimensions;
  for (const auto &candidate : candidates) {
    if (!std::isfinite(candidate.centerValue) ||
        !std::isfinite(candidate.centerDefinedValue)) {
      return fail(error, "validation", "center values must be finite");
    }
    for (std::size_t neighbor = 0U; neighbor < neighborCount; ++neighbor) {
      if (!std::isfinite(candidate.neighborValues[neighbor]) ||
          !std::isfinite(candidate.neighborDefinedValues[neighbor])) {
        return fail(error, "validation", "neighbor values must be finite");
      }
    }
  }
  return true;
}

template <class T>
[[nodiscard]] bool createAndWrite(runtime::VulkanDevice &device,
                                  runtime::HostVisibleBuffer &buffer,
                                  const std::span<const T> values,
                                  std::string &error) {
  const std::size_t allocatedCount = std::max<std::size_t>(values.size(), 1U);
  if (allocatedCount > std::numeric_limits<std::size_t>::max() / sizeof(T))
    return fail(error, "buffer", "allocation size overflow");
  const VkDeviceSize bytes = allocatedCount * sizeof(T);
  if (!buffer.create(device, bytes, kBufferUsage, kMemoryProperties, error) ||
      !buffer.map(error)) {
    return false;
  }
  return values.empty() ||
         buffer.write(values.data(), values.size_bytes(), 0U, error);
}

[[nodiscard]] VkDescriptorSetLayoutBinding
makeBinding(const std::uint32_t binding) {
  VkDescriptorSetLayoutBinding result{};
  result.binding = binding;
  result.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  result.descriptorCount = 1U;
  result.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
  return result;
}

[[nodiscard]] const char *statusMessage(const std::uint32_t status) {
  switch (status) {
  case 1U:
    return "defined output referenced an invalid source point ID";
  case 2U:
    return "shader produced a non-finite output value";
  default:
    return "shader reported an unknown failure";
  }
}

} // namespace

namespace viennaps::vulkan::levelset {

bool classifyHrleRebuildFp32(
    runtime::ComputeSession &session, const runtime::SpirvProgram &program,
    const std::span<const viennaps::levelset::HrleRebuildCandidateFp32>
        candidates,
    const std::uint32_t dimensions, const float cutoff,
    std::vector<viennaps::levelset::HrleRebuildDecisionFp32> &output,
    std::string &error) {
  error.clear();
  if (!validateInput(candidates, dimensions, cutoff, error))
    return false;
  if (!session.isValid())
    return fail(error, "session", "compute session is not initialized");
  if (program.words.empty())
    return fail(error, "shader", "SPIR-V program is empty");
  if (candidates.empty()) {
    output.clear();
    return true;
  }

  const std::uint64_t groupCount =
      (static_cast<std::uint64_t>(candidates.size()) + kLocalSize - 1U) /
      kLocalSize;
  const auto &limits = session.selection().properties.limits;
  if (groupCount > limits.maxComputeWorkGroupCount[0])
    return fail(error, "dispatch", "workgroup count exceeds device limit");
  if (candidates.size() >
      std::numeric_limits<std::size_t>::max() / kNeighborSlots) {
    return fail(error, "dispatch", "neighbor count overflows size_t");
  }

  std::vector<GpuCenter> centers(candidates.size());
  std::vector<GpuNeighbor> neighbors(candidates.size() * kNeighborSlots);
  for (std::size_t index = 0U; index < candidates.size(); ++index) {
    const auto &candidate = candidates[index];
    centers[index] = {candidate.centerValue, candidate.centerDefinedValue,
                      candidate.centerPointId, 0U};
    for (std::size_t neighbor = 0U; neighbor < kNeighborSlots; ++neighbor) {
      neighbors[index * kNeighborSlots + neighbor] = {
          candidate.neighborValues[neighbor],
          candidate.neighborDefinedValues[neighbor],
          candidate.neighborPointIds[neighbor], 0U};
    }
  }
  std::vector<GpuDecision> decisions(candidates.size());
  const std::uint32_t initialStatus = 0U;

  const std::array<std::uint64_t, kBindingCount> bindingBytes = {
      centers.size() * sizeof(GpuCenter),
      neighbors.size() * sizeof(GpuNeighbor),
      decisions.size() * sizeof(GpuDecision), sizeof(initialStatus)};
  for (const auto bytes : bindingBytes) {
    if (bytes > limits.maxStorageBufferRange)
      return fail(error, "dispatch", "storage buffer exceeds device limit");
  }
  if (limits.maxPerStageDescriptorStorageBuffers < kBindingCount ||
      limits.maxPushConstantsSize < sizeof(PushConstants)) {
    return fail(error, "dispatch",
                "device descriptor or push-constant limits are insufficient");
  }

  auto &device = session.device();
  std::array<runtime::HostVisibleBuffer, kBindingCount> buffers{};
  if (!createAndWrite(device, buffers[0], std::span<const GpuCenter>(centers),
                      error) ||
      !createAndWrite(device, buffers[1],
                      std::span<const GpuNeighbor>(neighbors), error) ||
      !createAndWrite(device, buffers[2],
                      std::span<const GpuDecision>(decisions), error) ||
      !createAndWrite(device, buffers[3],
                      std::span<const std::uint32_t>(&initialStatus, 1U),
                      error)) {
    return false;
  }

  runtime::ShaderModule shader{};
  if (!shader.create(device, program, error))
    return false;
  std::array<VkDescriptorSetLayoutBinding, kBindingCount> bindings{};
  for (std::uint32_t binding = 0U; binding < kBindingCount; ++binding)
    bindings[binding] = makeBinding(binding);
  runtime::DescriptorSetLayout descriptorLayout{};
  if (!descriptorLayout.create(device, bindings, error))
    return false;
  const VkPushConstantRange pushRange{VK_SHADER_STAGE_COMPUTE_BIT, 0U,
                                      sizeof(PushConstants)};
  runtime::PipelineLayout pipelineLayout{};
  if (!pipelineLayout.create(device, descriptorLayout.get(),
                             std::span(&pushRange, 1U), error)) {
    return false;
  }
  runtime::ComputePipeline pipeline{};
  if (!pipeline.create(device, shader, pipelineLayout, error))
    return false;
  runtime::DescriptorPool descriptorPool{};
  if (!descriptorPool.create(device, 1U, kBindingCount,
                             VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, error)) {
    return false;
  }
  VkDescriptorSet descriptorSet = VK_NULL_HANDLE;
  if (!descriptorPool.allocate(descriptorLayout.get(), descriptorSet, error))
    return false;

  std::array<VkDescriptorBufferInfo, kBindingCount> bufferInfos{};
  std::array<VkWriteDescriptorSet, kBindingCount> descriptorWrites{};
  for (std::uint32_t binding = 0U; binding < kBindingCount; ++binding) {
    bufferInfos[binding] = {buffers[binding].handle(), 0U,
                            bindingBytes[binding]};
    descriptorWrites[binding].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    descriptorWrites[binding].dstSet = descriptorSet;
    descriptorWrites[binding].dstBinding = binding;
    descriptorWrites[binding].descriptorCount = 1U;
    descriptorWrites[binding].descriptorType =
        VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    descriptorWrites[binding].pBufferInfo = &bufferInfos[binding];
  }
  vkUpdateDescriptorSets(device.get(), kBindingCount, descriptorWrites.data(),
                         0U, nullptr);

  VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
  auto &commandContext = session.commandContext();
  if (!commandContext.allocatePrimary(commandBuffer, error))
    return false;
  const auto freeCommandBuffer = [&]() {
    if (commandBuffer != VK_NULL_HANDLE) {
      vkFreeCommandBuffers(device.get(), commandContext.pool(), 1U,
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
  for (std::uint32_t binding = 0U; binding < kBindingCount; ++binding) {
    auto &barrier = preBarriers[binding];
    barrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
    barrier.srcAccessMask = VK_ACCESS_HOST_WRITE_BIT;
    barrier.dstAccessMask =
        VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.buffer = buffers[binding].handle();
    barrier.offset = 0U;
    barrier.size = bindingBytes[binding];
  }
  vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_HOST_BIT,
                       VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0U, 0U, nullptr,
                       kBindingCount, preBarriers.data(), 0U, nullptr);
  vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE,
                    pipeline.get());
  vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE,
                          pipelineLayout.get(), 0U, 1U, &descriptorSet, 0U,
                          nullptr);
  const PushConstants pushConstants{
      static_cast<std::uint32_t>(candidates.size()), dimensions, cutoff, 0U};
  vkCmdPushConstants(commandBuffer, pipelineLayout.get(),
                     VK_SHADER_STAGE_COMPUTE_BIT, 0U, sizeof(pushConstants),
                     &pushConstants);
  vkCmdDispatch(commandBuffer, static_cast<std::uint32_t>(groupCount), 1U, 1U);

  std::array<VkBufferMemoryBarrier, 2U> postBarriers{};
  constexpr std::array<std::uint32_t, 2U> outputBindings = {2U, 3U};
  for (std::size_t index = 0U; index < outputBindings.size(); ++index) {
    const std::uint32_t binding = outputBindings[index];
    auto &barrier = postBarriers[index];
    barrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
    barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.buffer = buffers[binding].handle();
    barrier.offset = 0U;
    barrier.size = bindingBytes[binding];
  }
  vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                       VK_PIPELINE_STAGE_HOST_BIT, 0U, 0U, nullptr,
                       static_cast<std::uint32_t>(postBarriers.size()),
                       postBarriers.data(), 0U, nullptr);
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
  submitInfo.commandBufferCount = 1U;
  submitInfo.pCommandBuffers = &commandBuffer;
  if (vkQueueSubmit(device.computeQueue(), 1U, &submitInfo, fence.get()) !=
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
  if (!buffers[3].read(&status, sizeof(status), 0U, error))
    return false;
  if (status != 0U)
    return fail(error, "shader", statusMessage(status));
  if (!buffers[2].read(decisions.data(), decisions.size() * sizeof(GpuDecision),
                       0U, error)) {
    return false;
  }

  std::vector<viennaps::levelset::HrleRebuildDecisionFp32> candidateOutput;
  candidateOutput.reserve(decisions.size());
  for (const auto &decision : decisions) {
    if (decision.action > static_cast<std::uint32_t>(
                              viennaps::levelset::HrleRebuildAction::DEFINED) ||
        !std::isfinite(decision.value)) {
      return fail(error, "output", "shader returned an invalid decision");
    }
    candidateOutput.push_back(
        {decision.value, decision.sourcePointId,
         static_cast<viennaps::levelset::HrleRebuildAction>(decision.action)});
  }
  output = std::move(candidateOutput);
  return true;
}

} // namespace viennaps::vulkan::levelset
