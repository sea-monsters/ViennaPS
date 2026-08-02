// Copyright 2026 ViennaPS
// SPDX-License-Identifier: MIT

#include "hrle_rebuild_compaction.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

namespace {

namespace Classification = viennaps::levelset;
namespace runtime = viennaps::vulkan::runtime;

using Compaction = viennaps::levelset::HrleRebuildCompactionResultFp32;
using Decision = viennaps::levelset::HrleRebuildDecisionFp32;
using DecisionIndex = viennaps::levelset::HrleRebuildCompactDecisionFp32;
using Primitive = viennaps::vulkan::primitives::ReductionScanPrimitives;

struct DecisionWire {
  float value;
  std::uint32_t sourcePointId;
  std::uint32_t action;
  std::uint32_t padding;
};
struct CompactWire {
  std::uint32_t candidateIndex;
  float value;
  std::uint32_t sourcePointId;
  std::uint32_t action;
};
static_assert(sizeof(DecisionWire) == 16U);
static_assert(sizeof(CompactWire) == 16U);

[[nodiscard]] bool fail(std::string &error, const std::string_view phase,
                        const std::string_view message) {
  error = std::string(phase) + ": " + std::string(message);
  return false;
}

template <class T>
[[nodiscard]] bool writeBuffer(runtime::HostVisibleBuffer &buffer,
                               const std::span<const T> values,
                               std::string &error) {
  if (values.empty()) {
    return true;
  }
  return buffer.write(values.data(), values.size() * sizeof(T), 0U, error);
}

template <class T>
[[nodiscard]] bool readBuffer(runtime::HostVisibleBuffer &buffer,
                              const std::size_t count, std::vector<T> &values,
                              std::string &error) {
  values.resize(count);
  if (count == 0U) {
    return true;
  }
  return buffer.read(values.data(), values.size() * sizeof(T), 0U, error);
}

[[nodiscard]] bool validDeviceBuffer(const runtime::DeviceBuffer &buffer,
                                     runtime::ComputeSession &session,
                                     const VkDeviceSize bytes,
                                     std::string &error,
                                     const std::string_view label) {
  if (!buffer.isValid() || buffer.ownerDevice() != session.deviceHandle() ||
      buffer.ownerSessionGeneration() != session.generation() ||
      buffer.size() < bytes)
    return fail(error, "validation",
                std::string(label) + " is invalid, stale, or undersized");
  return true;
}

[[nodiscard]] bool dispatchDeviceKernel(
    runtime::ComputeSession &session, const runtime::SpirvProgram &program,
    const std::span<const runtime::DeviceBuffer *const> buffers,
    const std::span<const VkAccessFlags> sourceAccess,
    const std::span<const VkAccessFlags> destinationAccess,
    const std::span<const VkDeviceSize> ranges,
    const std::span<const std::uint32_t> pushConstants,
    const std::uint32_t groupCount, std::string &error) {
  if (program.words.empty() || buffers.empty() ||
      buffers.size() != ranges.size() ||
      buffers.size() != sourceAccess.size() ||
      buffers.size() != destinationAccess.size() || groupCount == 0U)
    return fail(error, "dispatch", "invalid device-kernel arguments");
  auto &device = session.device();
  const auto &limits = device.selection().properties.limits;
  if (limits.maxComputeWorkGroupInvocations < 256U ||
      limits.maxComputeWorkGroupSize[0] < 256U ||
      groupCount > limits.maxComputeWorkGroupCount[0] ||
      buffers.size() > limits.maxPerStageDescriptorStorageBuffers)
    return fail(error, "dispatch", "device compute limits are insufficient");
  for (const auto range : ranges) {
    if (range > limits.maxStorageBufferRange)
      return fail(error, "dispatch", "storage buffer exceeds device limit");
  }
  runtime::ShaderModule shader{};
  runtime::DescriptorSetLayout descriptorLayout{};
  runtime::PipelineLayout pipelineLayout{};
  runtime::ComputePipeline pipeline{};
  runtime::DescriptorPool descriptorPool{};
  if (!shader.create(device, program, error))
    return false;
  std::vector<VkDescriptorSetLayoutBinding> layoutBindings(buffers.size());
  for (std::uint32_t i = 0U; i < layoutBindings.size(); ++i)
    layoutBindings[i] = {i, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1U,
                         VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
  if (!descriptorLayout.create(device, layoutBindings, error))
    return false;
  const auto pushBytes = pushConstants.size() * sizeof(std::uint32_t);
  if (pushBytes == 0U ||
      pushBytes > device.selection().properties.limits.maxPushConstantsSize)
    return fail(error, "dispatch", "invalid push-constant size");
  const VkPushConstantRange pushRange{VK_SHADER_STAGE_COMPUTE_BIT, 0U,
                                      static_cast<std::uint32_t>(pushBytes)};
  if (!pipelineLayout.create(device, descriptorLayout.get(),
                             std::span(&pushRange, 1U), error) ||
      !pipeline.create(device, shader, pipelineLayout, error) ||
      !descriptorPool.create(device, 1U,
                             static_cast<std::uint32_t>(buffers.size()),
                             VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, error))
    return false;
  VkDescriptorSet descriptorSet = VK_NULL_HANDLE;
  if (!descriptorPool.allocate(descriptorLayout.get(), descriptorSet, error))
    return false;
  std::vector<VkDescriptorBufferInfo> infos(buffers.size());
  std::vector<VkWriteDescriptorSet> writes(buffers.size());
  for (std::uint32_t i = 0U; i < buffers.size(); ++i) {
    infos[i] = {buffers[i]->handle(), 0U,
                std::max<VkDeviceSize>(1U, ranges[i])};
    writes[i] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                 nullptr,
                 descriptorSet,
                 i,
                 0U,
                 1U,
                 VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                 nullptr,
                 &infos[i],
                 nullptr};
  }
  vkUpdateDescriptorSets(device.get(),
                         static_cast<std::uint32_t>(writes.size()),
                         writes.data(), 0U, nullptr);
  VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
  auto &context = session.commandContext();
  if (!context.allocatePrimary(commandBuffer, error))
    return false;
  const auto freeCommand = [&]() {
    if (commandBuffer != VK_NULL_HANDLE) {
      vkFreeCommandBuffers(device.get(), context.pool(), 1U, &commandBuffer);
      commandBuffer = VK_NULL_HANDLE;
    }
  };
  VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  if (vkBeginCommandBuffer(commandBuffer, &begin) != VK_SUCCESS) {
    freeCommand();
    return fail(error, "dispatch", "vkBeginCommandBuffer failed");
  }
  std::vector<VkBufferMemoryBarrier> pre(buffers.size());
  for (std::uint32_t i = 0U; i < buffers.size(); ++i)
    pre[i] = {VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
              nullptr,
              sourceAccess[i],
              VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
              VK_QUEUE_FAMILY_IGNORED,
              VK_QUEUE_FAMILY_IGNORED,
              buffers[i]->handle(),
              0U,
              std::max<VkDeviceSize>(1U, ranges[i])};
  vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                       VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0U, 0U, nullptr,
                       static_cast<std::uint32_t>(pre.size()), pre.data(), 0U,
                       nullptr);
  vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE,
                    pipeline.get());
  vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE,
                          pipelineLayout.get(), 0U, 1U, &descriptorSet, 0U,
                          nullptr);
  vkCmdPushConstants(
      commandBuffer, pipelineLayout.get(), VK_SHADER_STAGE_COMPUTE_BIT, 0U,
      static_cast<std::uint32_t>(pushBytes), pushConstants.data());
  vkCmdDispatch(commandBuffer, groupCount, 1U, 1U);
  std::vector<VkBufferMemoryBarrier> post;
  for (std::uint32_t i = 0U; i < buffers.size(); ++i) {
    if (destinationAccess[i] != 0U)
      post.push_back({VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER, nullptr,
                      VK_ACCESS_SHADER_WRITE_BIT, destinationAccess[i],
                      VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED,
                      buffers[i]->handle(), 0U,
                      std::max<VkDeviceSize>(1U, ranges[i])});
  }
  if (!post.empty())
    vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0U, 0U, nullptr,
                         static_cast<std::uint32_t>(post.size()), post.data(),
                         0U, nullptr);
  if (vkEndCommandBuffer(commandBuffer) != VK_SUCCESS) {
    freeCommand();
    return fail(error, "dispatch", "vkEndCommandBuffer failed");
  }
  runtime::Fence fence{};
  if (!fence.create(device, error)) {
    freeCommand();
    return false;
  }
  VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
  submit.commandBufferCount = 1U;
  submit.pCommandBuffers = &commandBuffer;
  if (vkQueueSubmit(device.computeQueue(), 1U, &submit, fence.get()) !=
      VK_SUCCESS) {
    freeCommand();
    return fail(error, "dispatch", "vkQueueSubmit failed");
  }
  if (!fence.wait(std::numeric_limits<std::uint64_t>::max(), error)) {
    freeCommand();
    return false;
  }
  freeCommand();
  return true;
}

} // namespace

namespace viennaps::vulkan::levelset {

bool compactHrleRebuildDecisionsFp32(Primitive &primitives,
                                     const std::span<const Decision> decisions,
                                     Compaction &output, std::string &error) {
  const std::size_t decisionCount = decisions.size();
  error.clear();
  if (!primitives.isInitialized()) {
    return fail(error, "execution",
                "reduction/scan primitives are not initialized");
  }
  if (decisionCount > std::numeric_limits<std::uint32_t>::max()) {
    return fail(error, "validation",
                "HRLE rebuild decisions must fit in uint32_t index space.");
  }

  std::vector<Classification::HrleRebuildAction> localCandidateActions;
  std::vector<DecisionIndex> localDefinedPoints;
  std::vector<std::uint32_t> localDefinedMask;
  std::vector<std::uint32_t> localCompactFlags;
  std::vector<std::uint32_t> localCompactIndices;

  localCandidateActions.reserve(decisionCount);
  localDefinedMask.reserve(decisionCount);
  localCompactFlags.reserve(decisionCount);
  localCompactIndices.reserve(decisionCount);

  for (std::size_t candidateIndex = 0U; candidateIndex < decisionCount;
       ++candidateIndex) {
    const auto &decision = decisions[candidateIndex];
    if (!Classification::detail::isValidHrleRebuildAction(decision.action)) {
      return fail(error, "validation",
                  "HRLE rebuild decisions contain an invalid action.");
    }
    if (!std::isfinite(decision.value)) {
      return fail(error, "validation",
                  "HRLE rebuild decisions require finite values.");
    }
    if (decision.action == Classification::HrleRebuildAction::DEFINED &&
        decision.sourcePointId == Classification::kInvalidHrlePointId) {
      return fail(error, "validation",
                  "Defined HRLE rebuild decision requires a valid source point "
                  "ID.");
    }

    localCandidateActions.push_back(decision.action);
    const bool isDefined =
        decision.action == Classification::HrleRebuildAction::DEFINED;
    localDefinedMask.push_back(isDefined ? 1U : 0U);
    localCompactFlags.push_back(isDefined ? 1U : 0U);
    localCompactIndices.push_back(static_cast<std::uint32_t>(candidateIndex));
  }

  if (decisionCount == 0U) {
    Compaction localResult;
    localResult.candidateActions = std::move(localCandidateActions);
    localResult.definedPoints = {};
    localResult.definedMask = std::move(localDefinedMask);
    localResult.definedExclusiveOffsets = {0U};
    output = std::move(localResult);
    return true;
  }

  runtime::HostVisibleBuffer indexInput{};
  runtime::HostVisibleBuffer compactIndexOutput{};
  runtime::HostVisibleBuffer flagsInput{};
  runtime::HostVisibleBuffer offsets{};

  std::size_t outputCount = 0U;
  if (!primitives.createIntBuffer(decisionCount, indexInput, error) ||
      !primitives.createIntBuffer(decisionCount, compactIndexOutput, error) ||
      !primitives.createIntBuffer(decisionCount, flagsInput, error) ||
      !primitives.createIntBuffer(decisionCount, offsets, error) ||
      !writeBuffer(indexInput,
                   std::span<const std::uint32_t>(localCompactIndices),
                   error) ||
      !writeBuffer(flagsInput,
                   std::span<const std::uint32_t>(localCompactFlags), error) ||
      !primitives.exclusiveScanInt(flagsInput, decisionCount, offsets,
                                   decisionCount, error) ||
      !primitives.stableCompactUInt32(indexInput, decisionCount, flagsInput,
                                      decisionCount, compactIndexOutput,
                                      decisionCount, outputCount, error)) {
    return false;
  }

  std::vector<std::uint32_t> localExclusiveOffsets{};
  if (!readBuffer(offsets, decisionCount, localExclusiveOffsets, error)) {
    return false;
  }

  if (outputCount > decisionCount ||
      outputCount > std::numeric_limits<std::uint32_t>::max()) {
    return fail(error, "gpu result",
                "GPU compaction returned an invalid selected count.");
  }

  std::vector<std::uint32_t> compactedIndices{};
  if (!readBuffer(compactIndexOutput, outputCount, compactedIndices, error)) {
    return false;
  }

  localExclusiveOffsets.push_back(static_cast<std::uint32_t>(outputCount));

  std::uint32_t expectedOffset = 0U;
  for (std::size_t index = 0U; index < decisionCount; ++index) {
    if (localExclusiveOffsets[index] != expectedOffset) {
      return fail(error, "gpu result",
                  "GPU scan returned an invalid exclusive offset.");
    }
    expectedOffset += localDefinedMask[index];
  }
  if (expectedOffset != outputCount ||
      localExclusiveOffsets.back() != expectedOffset) {
    return fail(error, "gpu result",
                "GPU scan and compaction selected counts disagree.");
  }

  localDefinedPoints.reserve(outputCount);
  for (std::size_t compactedIndex = 0U; compactedIndex < outputCount;
       ++compactedIndex) {
    const auto candidateIndex = compactedIndices[compactedIndex];
    if (candidateIndex >= decisions.size()) {
      return fail(error, "gpu result",
                  "GPU compaction returned an out-of-range candidate index.");
    }
    if (compactedIndex > 0U &&
        candidateIndex <= compactedIndices[compactedIndex - 1U]) {
      return fail(error, "gpu result",
                  "GPU compaction did not preserve stable candidate order.");
    }
    const auto &decision = decisions[candidateIndex];
    if (decision.action != Classification::HrleRebuildAction::DEFINED) {
      return fail(error, "gpu result",
                  "GPU compaction selected an undefined candidate.");
    }
    localDefinedPoints.push_back({candidateIndex, decision.value,
                                  decision.sourcePointId,
                                  Classification::HrleRebuildAction::DEFINED});
  }

  Compaction localResult{};
  localResult.candidateActions = std::move(localCandidateActions);
  localResult.definedPoints = std::move(localDefinedPoints);
  localResult.definedMask = std::move(localDefinedMask);
  localResult.definedExclusiveOffsets = std::move(localExclusiveOffsets);
  output = std::move(localResult);
  return true;
}

bool compactHrleRebuildDecisionsFp32Device(
    runtime::ComputeSession &session, const runtime::SpirvProgram &flagsProgram,
    const runtime::SpirvProgram &compactProgram, Primitive &primitives,
    const HrleRebuildClassificationDeviceFp32 &classification,
    HrleRebuildCompactionDeviceFp32 &output, std::string &error) {
  error.clear();
  if (!session.isValid())
    return fail(error, "session", "compute session is not initialized");
  if (!primitives.isInitialized() ||
      primitives.device().get() != session.device().get() ||
      primitives.boundSessionGeneration() != session.generation())
    return fail(error, "session",
                "reduction/scan primitives use another device");
  if (classification.sessionGeneration != session.generation())
    return fail(error, "session",
                "classification result belongs to another generation");
  const auto count = static_cast<std::size_t>(classification.candidateCount);
  if (count == 0U) {
    HrleRebuildCompactionDeviceFp32 empty{};
    empty.sessionGeneration = session.generation();
    output = std::move(empty);
    return true;
  }
  if (count > std::numeric_limits<std::size_t>::max() / sizeof(DecisionWire))
    return fail(error, "validation",
                "candidate count overflows device buffers");
  if (!validDeviceBuffer(
          classification.decisions, session,
          static_cast<VkDeviceSize>(count * sizeof(DecisionWire)), error,
          "classification decisions") ||
      !validDeviceBuffer(classification.status, session, sizeof(std::uint32_t),
                         error, "classification status"))
    return false;
  const auto bytes = static_cast<VkDeviceSize>(count * sizeof(std::uint32_t));
  runtime::DeviceBuffer flags{};
  runtime::DeviceBuffer offsets{};
  runtime::DeviceBuffer compacted{};
  runtime::DeviceBuffer selectedCount{};
  if (!flags.create(session, bytes, error) ||
      !offsets.create(session, bytes, error) ||
      !compacted.create(session,
                        static_cast<VkDeviceSize>(count * sizeof(CompactWire)),
                        error) ||
      !selectedCount.create(session, sizeof(std::uint32_t), error))
    return false;

  const std::array<const runtime::DeviceBuffer *, 2U> flagBuffers = {
      &classification.decisions, &flags};
  const std::array<VkAccessFlags, 2U> flagSources = {VK_ACCESS_SHADER_WRITE_BIT,
                                                     0U};
  const std::array<VkAccessFlags, 2U> flagDestinations = {
      0U, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT};
  const std::array<VkDeviceSize, 2U> flagRanges = {
      static_cast<VkDeviceSize>(count * sizeof(DecisionWire)), bytes};
  const std::array<std::uint32_t, 1U> flagPush = {
      static_cast<std::uint32_t>(count)};
  const auto groups = static_cast<std::uint32_t>(
      (static_cast<std::uint64_t>(count) + 255U) / 256U);
  if (!dispatchDeviceKernel(session, flagsProgram,
                            std::span<const runtime::DeviceBuffer *const>(
                                flagBuffers.data(), flagBuffers.size()),
                            flagSources, flagDestinations, flagRanges, flagPush,
                            groups, error))
    return false;
  if (!primitives.exclusiveScanInt(flags, count, offsets, count, error) ||
      !primitives.writeCompactionCount(flags, offsets, count, selectedCount,
                                       error))
    return false;

  const std::array<const runtime::DeviceBuffer *, 3U> compactBuffers = {
      &classification.decisions, &offsets, &compacted};
  const std::array<VkAccessFlags, 3U> compactSources = {
      VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_WRITE_BIT, 0U};
  const std::array<VkAccessFlags, 3U> compactDestinations = {
      0U, 0U, VK_ACCESS_SHADER_READ_BIT};
  const std::array<VkDeviceSize, 3U> compactRanges = {
      static_cast<VkDeviceSize>(count * sizeof(DecisionWire)), bytes,
      static_cast<VkDeviceSize>(count * sizeof(CompactWire))};
  const std::array<std::uint32_t, 2U> compactPush = {
      static_cast<std::uint32_t>(count), static_cast<std::uint32_t>(count)};
  if (!dispatchDeviceKernel(session, compactProgram,
                            std::span<const runtime::DeviceBuffer *const>(
                                compactBuffers.data(), compactBuffers.size()),
                            compactSources, compactDestinations, compactRanges,
                            compactPush, groups, error))
    return false;
  HrleRebuildCompactionDeviceFp32 local{};
  local.flags = std::move(flags);
  local.offsets = std::move(offsets);
  local.compacted = std::move(compacted);
  local.count = std::move(selectedCount);
  local.candidateCount = classification.candidateCount;
  local.sessionGeneration = session.generation();
  output = std::move(local);
  return true;
}

bool materializeHrleRebuildCompactionFp32Device(
    runtime::ComputeSession &session,
    const HrleRebuildClassificationDeviceFp32 &classification,
    const HrleRebuildCompactionDeviceFp32 &deviceOutput, Compaction &output,
    std::string &error) {
  error.clear();
  if (!session.isValid())
    return fail(error, "session", "compute session is not initialized");
  if (classification.sessionGeneration != session.generation() ||
      deviceOutput.sessionGeneration != session.generation())
    return fail(error, "session",
                "device result belongs to another generation");
  if (deviceOutput.candidateCount != classification.candidateCount)
    return fail(error, "validation",
                "device compaction count metadata disagrees");
  const std::size_t count = classification.candidateCount;
  if (count == 0U) {
    Compaction empty{};
    empty.definedExclusiveOffsets = {0U};
    output = std::move(empty);
    return true;
  }
  if (count > std::numeric_limits<std::size_t>::max() / sizeof(DecisionWire))
    return fail(error, "validation", "candidate count overflows host buffers");
  if (!validDeviceBuffer(
          classification.decisions, session,
          static_cast<VkDeviceSize>(count * sizeof(DecisionWire)), error,
          "classification decisions") ||
      !validDeviceBuffer(classification.status, session, sizeof(std::uint32_t),
                         error, "classification status") ||
      !validDeviceBuffer(
          deviceOutput.flags, session,
          static_cast<VkDeviceSize>(count * sizeof(std::uint32_t)), error,
          "compaction flags") ||
      !validDeviceBuffer(
          deviceOutput.offsets, session,
          static_cast<VkDeviceSize>(count * sizeof(std::uint32_t)), error,
          "compaction offsets") ||
      !validDeviceBuffer(deviceOutput.count, session, sizeof(std::uint32_t),
                         error, "compaction count"))
    return false;
  std::uint32_t status = 0U;
  std::uint32_t selected = 0U;
  if (!classification.status.download(session, &status, sizeof(status), 0U,
                                      error) ||
      !deviceOutput.count.download(session, &selected, sizeof(selected), 0U,
                                   error))
    return false;
  if (status != 0U)
    return fail(error, "shader", "classification shader reported failure");
  if (selected > count)
    return fail(error, "gpu result", "GPU compaction count exceeds candidates");
  std::vector<DecisionWire> decisions(count);
  std::vector<std::uint32_t> offsets(count);
  if (!classification.decisions.download(
          session, decisions.data(), decisions.size() * sizeof(DecisionWire),
          0U, error) ||
      !deviceOutput.offsets.download(session, offsets.data(),
                                     offsets.size() * sizeof(std::uint32_t), 0U,
                                     error))
    return false;
  std::vector<CompactWire> compacted(selected);
  if (selected > 0U && !validDeviceBuffer(deviceOutput.compacted, session,
                                          static_cast<VkDeviceSize>(
                                              selected * sizeof(CompactWire)),
                                          error, "compacted output"))
    return false;
  if (selected > 0U && !deviceOutput.compacted.download(
                           session, compacted.data(),
                           compacted.size() * sizeof(CompactWire), 0U, error))
    return false;

  std::vector<Classification::HrleRebuildAction> actions;
  std::vector<std::uint32_t> definedMask;
  std::vector<DecisionIndex> definedPoints;
  std::vector<std::uint32_t> exclusiveOffsets = offsets;
  actions.reserve(count);
  definedMask.reserve(count);
  std::uint32_t expected = 0U;
  for (std::size_t i = 0U; i < count; ++i) {
    const auto &decision = decisions[i];
    if (decision.action > static_cast<std::uint32_t>(
                              Classification::HrleRebuildAction::DEFINED) ||
        !std::isfinite(decision.value))
      return fail(error, "output", "shader returned an invalid decision");
    if (decision.action == static_cast<std::uint32_t>(
                               Classification::HrleRebuildAction::DEFINED) &&
        decision.sourcePointId == Classification::kInvalidHrlePointId)
      return fail(error, "output",
                  "defined decision has invalid source point ID");
    const auto defined =
        decision.action ==
        static_cast<std::uint32_t>(Classification::HrleRebuildAction::DEFINED);
    actions.push_back(
        static_cast<Classification::HrleRebuildAction>(decision.action));
    definedMask.push_back(defined ? 1U : 0U);
    if (offsets[i] != expected)
      return fail(error, "gpu result", "GPU scan returned an invalid offset");
    expected += defined ? 1U : 0U;
  }
  if (expected != selected)
    return fail(error, "gpu result", "GPU scan and compaction counts disagree");
  exclusiveOffsets.push_back(selected);
  definedPoints.reserve(selected);
  for (std::size_t i = 0U; i < compacted.size(); ++i) {
    const auto &record = compacted[i];
    if (record.candidateIndex >= count ||
        (i > 0U && record.candidateIndex <= compacted[i - 1U].candidateIndex) ||
        record.action != static_cast<std::uint32_t>(
                             Classification::HrleRebuildAction::DEFINED))
      return fail(error, "gpu result",
                  "GPU compaction returned invalid or unstable records");
    const auto &decision = decisions[record.candidateIndex];
    if (decision.action != record.action ||
        decision.sourcePointId != record.sourcePointId ||
        std::bit_cast<std::uint32_t>(decision.value) !=
            std::bit_cast<std::uint32_t>(record.value) ||
        offsets[record.candidateIndex] != i)
      return fail(error, "gpu result",
                  "GPU compacted record disagrees with decisions");
    definedPoints.push_back({record.candidateIndex, record.value,
                             record.sourcePointId,
                             Classification::HrleRebuildAction::DEFINED});
  }
  Compaction local{};
  local.candidateActions = std::move(actions);
  local.definedMask = std::move(definedMask);
  local.definedExclusiveOffsets = std::move(exclusiveOffsets);
  local.definedPoints = std::move(definedPoints);
  output = std::move(local);
  return true;
}

} // namespace viennaps::vulkan::levelset
