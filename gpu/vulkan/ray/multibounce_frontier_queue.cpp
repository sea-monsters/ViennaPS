// Copyright 2026 ViennaPS
// SPDX-License-Identifier: MIT
#include "multibounce_frontier_queue.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

namespace viennaps::vulkan::ray {
namespace {
bool fail(std::string &error, const char *message) {
  error = message;
  return false;
}
bool valid(float value) {
  return std::isfinite(value) &&
         (value == 0.0F || std::isnormal(value));
}
bool validEvent(const MultibounceEvent &event) {
  for (float value : event.origin)
    if (!valid(value)) return false;
  for (float value : event.direction)
    if (!valid(value)) return false;
  return valid(event.weight) && valid(event.nextWeight) &&
         event.activeFlag <= 1U;
}
bool validDecision(const MultibounceDecision &decision) {
  if (decision.action >
      static_cast<std::uint32_t>(MultibounceAction::rouletteReject))
    return false;
  if (!valid(decision.weight) || !valid(decision.nextWeight) ||
      !valid(decision.contribution))
    return false;
  for (float value : decision.successorOrigin)
    if (!valid(value)) return false;
  for (float value : decision.successorDirection)
    if (!valid(value)) return false;
  return true;
}
bool lessEvent(const MultibounceEvent &a, const MultibounceEvent &b) {
  if (a.particle != b.particle) return a.particle < b.particle;
  if (a.bounce != b.bounce) return a.bounce < b.bounce;
  return a.sequence < b.sequence;
}
} // namespace

MultibounceFrontierQueue::~MultibounceFrontierQueue() { reset(); }

bool MultibounceFrontierQueue::initialize(runtime::ComputeSession &session,
                                          const std::string_view spirv,
                                          std::string &error) {
  error.clear();
  reset();
  if (!session.isValid()) return fail(error, "frontier queue session is invalid");
  runtime::SpirvProgram program{};
  if (!runtime::readSpirv(spirv, program, error) ||
      !shader_.create(session.device(), program, error))
    return false;
  session_ = &session;
  sessionGeneration_ = session.generation();
  std::array<VkDescriptorSetLayoutBinding, 5U> bindings{};
  for (std::uint32_t i = 0U; i < bindings.size(); ++i)
    bindings[i] = {i, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1U,
                   VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
  const VkPushConstantRange push{VK_SHADER_STAGE_COMPUTE_BIT, 0U,
                                 4U * sizeof(std::uint32_t)};
  if (!layout_.create(session.device(), bindings, error) ||
      !pipelineLayout_.create(session.device(), layout_.get(),
                              std::span<const VkPushConstantRange>(&push, 1U),
                              error) ||
      !pool_.create(session.device(), 1U, 5U,
                    VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, error) ||
      !pool_.allocate(layout_.get(), descriptor_, error) ||
      !pipeline_.create(session.device(), shader_, pipelineLayout_,
                        runtime::ComputePipelineOptions{}, error) ||
      !session.commandContext().allocatePrimary(commandBuffer_, error) ||
      !fence_.create(session.device(), error)) {
    reset();
    return false;
  }
  return true;
}

void MultibounceFrontierQueue::reset() {
  const bool hasLiveSession =
      session_ != nullptr && sessionGeneration_ != 0U && session_->isValid() &&
      session_->generation() == sessionGeneration_;
  if (hasLiveSession)
    vkDeviceWaitIdle(session_->deviceHandle());

  if (hasLiveSession) {
    fence_.destroy();
  } else {
    fence_.abandon();
  }
  status_.reset();
  accumulation_.reset();
  decisions_.reset();
  eventsOut_.reset();
  eventsIn_.reset();
  if (hasLiveSession && commandBuffer_ != VK_NULL_HANDLE)
    vkFreeCommandBuffers(session_->deviceHandle(),
                         session_->commandContext().pool(), 1U,
                         &commandBuffer_);
  commandBuffer_ = VK_NULL_HANDLE;
  descriptor_ = VK_NULL_HANDLE;
  if (hasLiveSession) {
    pipeline_.reset();
    pool_.reset();
    pipelineLayout_.reset();
    layout_.reset();
    shader_.reset();
  } else {
    pipeline_.abandon();
    pool_.abandon();
    pipelineLayout_.abandon();
    layout_.abandon();
    shader_.abandon();
  }
  session_ = nullptr;
  sessionGeneration_ = 0U;
}

bool MultibounceFrontierQueue::isInitialized() const {
  return session_ != nullptr && sessionGeneration_ != 0U &&
         session_->isValid() && session_->generation() == sessionGeneration_ &&
         shader_.get() != VK_NULL_HANDLE && pipeline_.get() != VK_NULL_HANDLE &&
         descriptor_ != VK_NULL_HANDLE && fence_.get() != VK_NULL_HANDLE;
}

bool MultibounceFrontierQueue::run(
    const std::span<const MultibounceFrontier> frontiers,
    const MultibounceFrontierLimits &limits,
                         MultibounceFrontierResult &output, std::string &error) {
  error.clear();
  if (session_ != nullptr && sessionGeneration_ != 0U &&
      (!session_->isValid() || session_->generation() != sessionGeneration_))
    return fail(error, "frontier queue belongs to a stale compute session generation");
  if (!isInitialized()) return fail(error, "frontier queue is not initialized");
  if (frontiers.empty() || limits.maxTotalEvents == 0U ||
      limits.maxFrontierEvents == 0U || limits.maxRounds == 0U ||
      frontiers.size() > limits.maxRounds)
    return fail(error, "frontier limits are invalid");
  MultibounceFrontierResult staged;
  std::size_t totalEvents = 0U;
  for (const auto &frontier : frontiers) {
    if (frontier.events.size() >
        std::numeric_limits<std::size_t>::max() /
            std::max<std::size_t>(1U, frontier.decisionStride))
      return fail(error, "frontier decision size overflows");
    if (frontier.events.empty() ||
        frontier.events.size() > limits.maxFrontierEvents ||
        frontier.decisionStride == 0U ||
        frontier.decisionStride > 1024U ||
        frontier.decisions.size() !=
            frontier.events.size() * frontier.decisionStride)
      return fail(error, "frontier shape or capacity is invalid");
    if (frontier.events.size() > limits.maxTotalEvents ||
        totalEvents > limits.maxTotalEvents - frontier.events.size())
      return fail(error, "frontier total event capacity exceeded");
    totalEvents += frontier.events.size();
    for (const auto &event : frontier.events)
      if (!validEvent(event)) return fail(error, "frontier event is invalid");
    for (const auto &decision : frontier.decisions)
      if (!validDecision(decision))
        return fail(error, "frontier decision is invalid");
  }

  for (const auto &frontier : frontiers) {
    const auto laneCount = frontier.events.size();
    const auto eventBytes = static_cast<VkDeviceSize>(
        laneCount * sizeof(MultibounceEvent));
    const auto decisionBytes = static_cast<VkDeviceSize>(
        frontier.decisions.size() * sizeof(MultibounceDecision));
    const auto accumulationBytes = static_cast<VkDeviceSize>(
        laneCount * sizeof(MultibounceAccumulation));
    if (!eventsIn_.create(*session_, eventBytes, error) ||
        !eventsOut_.create(*session_, eventBytes, error) ||
        !decisions_.create(*session_, decisionBytes, error) ||
        !accumulation_.create(*session_, accumulationBytes, error) ||
        !status_.create(*session_, sizeof(std::uint32_t), error) ||
        !eventsIn_.upload(*session_, frontier.events.data(), eventBytes, 0U,
                          error) ||
        !decisions_.upload(*session_, frontier.decisions.data(), decisionBytes,
                           0U, error))
      return false;
    const std::uint32_t zero = 0U;
    if (!status_.upload(*session_, &zero, sizeof(zero), 0U, error)) return false;
    if (vkResetCommandBuffer(commandBuffer_, 0U) != VK_SUCCESS)
      return fail(error, "failed to reset frontier command buffer");
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (vkBeginCommandBuffer(commandBuffer_, &begin) != VK_SUCCESS)
      return fail(error, "failed to begin frontier command buffer");
    const std::array<VkBuffer, 5U> handles{eventsIn_.handle(), decisions_.handle(),
        eventsOut_.handle(), accumulation_.handle(), status_.handle()};
    std::array<VkDescriptorBufferInfo, 5U> infos{};
    std::array<VkWriteDescriptorSet, 5U> writes{};
    for (std::uint32_t i = 0U; i < handles.size(); ++i) {
      infos[i] = {handles[i], 0U, VK_WHOLE_SIZE};
      writes[i] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, descriptor_,
                   i, 0U, 1U, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, nullptr,
                   &infos[i], nullptr};
    }
    vkUpdateDescriptorSets(session_->deviceHandle(), 5U, writes.data(), 0U,
                           nullptr);
    vkCmdBindPipeline(commandBuffer_, VK_PIPELINE_BIND_POINT_COMPUTE,
                      pipeline_.get());
    vkCmdBindDescriptorSets(commandBuffer_, VK_PIPELINE_BIND_POINT_COMPUTE,
                            pipelineLayout_.get(), 0U, 1U, &descriptor_, 0U,
                            nullptr);
    const std::array<std::uint32_t, 4U> push{
        static_cast<std::uint32_t>(laneCount), frontier.decisionStride,
        limits.maxReflections, 0U};
    vkCmdPushConstants(commandBuffer_, pipelineLayout_.get(),
                       VK_SHADER_STAGE_COMPUTE_BIT, 0U, sizeof(push), push.data());
    vkCmdDispatch(commandBuffer_,
                  static_cast<std::uint32_t>((laneCount + 63U) / 64U), 1U, 1U);
    VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER, nullptr,
                            VK_ACCESS_SHADER_WRITE_BIT,
                            VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT};
    vkCmdPipelineBarrier(commandBuffer_, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0U, 1U, &barrier,
                         0U, nullptr, 0U, nullptr);
    if (vkEndCommandBuffer(commandBuffer_) != VK_SUCCESS)
      return fail(error, "failed to end frontier command buffer");
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.commandBufferCount = 1U;
    submit.pCommandBuffers = &commandBuffer_;
    if (vkQueueSubmit(session_->device().computeQueue(), 1U, &submit,
                      fence_.get()) != VK_SUCCESS ||
        !fence_.wait(10'000'000'000ULL, error))
      return false;
    fence_.reset();
    std::uint32_t status = 0U;
    if (!status_.download(*session_, &status, sizeof(status), 0U, error))
      return false;
    if (status != 0U)
      return fail(error, "frontier device status rejected the batch");
    std::vector<MultibounceEvent> events(laneCount);
    std::vector<MultibounceAccumulation> accumulation(laneCount);
    if (!eventsOut_.download(*session_, events.data(), eventBytes, 0U, error) ||
        !accumulation_.download(*session_, accumulation.data(), accumulationBytes,
                                0U, error))
      return false;
    std::sort(events.begin(), events.end(), lessEvent);
    staged.accumulation.insert(staged.accumulation.end(), accumulation.begin(),
                               accumulation.end());
    staged.terminalEvents = std::move(events);
    ++staged.rounds;
    status_.reset();
    accumulation_.reset();
    decisions_.reset();
    eventsOut_.reset();
    eventsIn_.reset();
  }
  if (session_->generation() != sessionGeneration_)
    return fail(error, "frontier queue session generation changed before publish");
  staged.totalEvents = static_cast<std::uint32_t>(totalEvents);
  output = std::move(staged);
  return true;
}

} // namespace viennaps::vulkan::ray
