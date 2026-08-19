// Copyright 2026 ViennaPS
// SPDX-License-Identifier: MIT
#include "multibounce_event.hpp"

#include <array>
#include <bit>
#include <cmath>
#include <limits>

namespace viennaps::vulkan::ray {
namespace {
bool fail(std::string &error, const char *message) {
  error = message;
  return false;
}
bool validFloat(float value) {
  return std::isfinite(value) && (value == 0.0F || std::isnormal(value));
}
bool validEvent(const MultibounceEvent &event) {
  for (float value : event.origin)
    if (!validFloat(value)) return false;
  for (float value : event.direction)
    if (!validFloat(value)) return false;
  return validFloat(event.weight) && validFloat(event.nextWeight) &&
         event.activeFlag <= 1U;
}
bool validDecision(const MultibounceDecision &decision) {
  if (decision.action >
      static_cast<std::uint32_t>(MultibounceAction::rouletteReject))
    return false;
  if (!validFloat(decision.weight) || !validFloat(decision.nextWeight) ||
      !validFloat(decision.contribution))
    return false;
  for (float value : decision.successorOrigin)
    if (!validFloat(value)) return false;
  for (float value : decision.successorDirection)
    if (!validFloat(value)) return false;
  return true;
}
} // namespace

DeviceMultibounceSlice::~DeviceMultibounceSlice() { reset(); }

bool DeviceMultibounceSlice::initialize(runtime::ComputeSession &session,
                                        const std::string_view spirv,
                                        std::string &error) {
  error.clear();
  reset();
  if (!session.isValid()) return fail(error, "multibounce session is invalid");
  runtime::SpirvProgram program{};
  if (!runtime::readSpirv(spirv, program, error) ||
      !shader_.create(session.device(), program, error)) return false;
  session_ = &session;
  std::array<VkDescriptorSetLayoutBinding, 5U> bindings{};
  for (std::uint32_t i = 0; i < bindings.size(); ++i)
    bindings[i] = {i, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1U,
                   VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
  const VkPushConstantRange push{VK_SHADER_STAGE_COMPUTE_BIT, 0U,
                                 4U * sizeof(std::uint32_t)};
  if (!layout_.create(session.device(), bindings, error) ||
      !pipelineLayout_.create(session.device(), layout_.get(),
                              std::span<const VkPushConstantRange>(&push, 1),
                              error) ||
      !pool_.create(session.device(), 2U, 10U,
                    VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, error) ||
      !pool_.allocate(layout_.get(), descriptors_[0], error) ||
      !pool_.allocate(layout_.get(), descriptors_[1], error) ||
      !pipeline_.create(session.device(), shader_, pipelineLayout_,
                        runtime::ComputePipelineOptions{}, error) ||
      !session.commandContext().allocatePrimary(commandBuffer_, error) ||
      !fence_.create(session.device(), error)) {
    reset();
    return false;
  }
  return true;
}

void DeviceMultibounceSlice::reset() {
  if (session_ != nullptr && session_->isValid())
    vkDeviceWaitIdle(session_->deviceHandle());
  fence_.destroy();
  status_.reset();
  accumulation_.reset();
  decisions_.reset();
  events_[0].reset();
  events_[1].reset();
  if (session_ != nullptr && session_->isValid())
    vkFreeCommandBuffers(session_->deviceHandle(), session_->commandContext().pool(),
                         1U, &commandBuffer_);
  commandBuffer_ = VK_NULL_HANDLE;
  descriptors_[0] = VK_NULL_HANDLE;
  descriptors_[1] = VK_NULL_HANDLE;
  pipeline_.reset();
  pool_.reset();
  pipelineLayout_.reset();
  layout_.reset();
  shader_.reset();
  session_ = nullptr;
}

bool DeviceMultibounceSlice::isInitialized() const {
  return session_ != nullptr && session_->isValid() &&
         shader_.get() != VK_NULL_HANDLE && pipeline_.get() != VK_NULL_HANDLE &&
         descriptors_[0] != VK_NULL_HANDLE && descriptors_[1] != VK_NULL_HANDLE &&
         fence_.get() != VK_NULL_HANDLE;
}

bool DeviceMultibounceSlice::run(
    const std::span<const MultibounceEvent> input,
    const std::span<const MultibounceDecision> decisions,
    const std::uint32_t decisionStride, const std::uint32_t maxReflections,
    MultibounceSliceResult &output, std::string &error) {
  error.clear();
  if (!isInitialized()) return fail(error, "multibounce slice is not initialized");
  if (input.empty() || decisionStride < 2U || input.size() > 1024U ||
      input.size() > std::numeric_limits<std::uint32_t>::max() ||
      decisions.size() != input.size() * decisionStride)
    return fail(error, "multibounce input or decision shape is invalid");
  for (const auto &event : input)
    if (!validEvent(event)) return fail(error, "multibounce input event is invalid");
  for (const auto &decision : decisions)
    if (!validDecision(decision)) return fail(error, "multibounce decision is invalid");

  const auto laneCount = input.size();
  const auto eventBytes = static_cast<VkDeviceSize>(laneCount * sizeof(MultibounceEvent));
  const auto decisionBytes = static_cast<VkDeviceSize>(decisions.size() * sizeof(MultibounceDecision));
  const auto accumBytes = static_cast<VkDeviceSize>(laneCount * 2U * sizeof(MultibounceAccumulation));
  if (!events_[0].create(*session_, eventBytes, error) ||
      !events_[1].create(*session_, eventBytes, error) ||
      !decisions_.create(*session_, decisionBytes, error) ||
      !accumulation_.create(*session_, accumBytes, error) ||
      !status_.create(*session_, sizeof(std::uint32_t), error) ||
      !events_[0].upload(*session_, input.data(), eventBytes, 0U, error) ||
      !decisions_.upload(*session_, decisions.data(), decisionBytes, 0U, error))
    return false;
  const std::uint32_t zero = 0U;
  if (!status_.upload(*session_, &zero, sizeof(zero), 0U, error)) return false;
  if (vkResetCommandBuffer(commandBuffer_, 0U) != VK_SUCCESS)
    return fail(error, "failed to reset multibounce command buffer");
  VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  if (vkBeginCommandBuffer(commandBuffer_, &begin) != VK_SUCCESS)
    return fail(error, "failed to begin multibounce command buffer");
  for (std::uint32_t pass = 0U; pass < 2U; ++pass) {
    const std::array<VkBuffer, 5U> handles{events_[pass & 1U].handle(),
        decisions_.handle(), events_[(pass + 1U) & 1U].handle(),
        accumulation_.handle(), status_.handle()};
    std::array<VkDescriptorBufferInfo, 5U> infos{};
    std::array<VkWriteDescriptorSet, 5U> writes{};
    for (std::uint32_t i = 0; i < handles.size(); ++i) {
      infos[i] = {handles[i], 0U, VK_WHOLE_SIZE};
      writes[i] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr,
                   descriptors_[pass], i, 0U, 1U, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                   nullptr, &infos[i], nullptr};
    }
    vkUpdateDescriptorSets(session_->deviceHandle(), 5U, writes.data(), 0U,
                           nullptr);
    vkCmdBindPipeline(commandBuffer_, VK_PIPELINE_BIND_POINT_COMPUTE,
                      pipeline_.get());
    vkCmdBindDescriptorSets(commandBuffer_, VK_PIPELINE_BIND_POINT_COMPUTE,
                            pipelineLayout_.get(), 0U, 1U, &descriptors_[pass], 0U,
                            nullptr);
    const std::array<std::uint32_t, 4U> push{
        static_cast<std::uint32_t>(laneCount), decisionStride, maxReflections,
        pass};
    vkCmdPushConstants(commandBuffer_, pipelineLayout_.get(),
                       VK_SHADER_STAGE_COMPUTE_BIT, 0U, sizeof(push), push.data());
    vkCmdDispatch(commandBuffer_, static_cast<std::uint32_t>((laneCount + 63U) / 64U), 1U, 1U);
    VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER, nullptr,
                            VK_ACCESS_SHADER_WRITE_BIT,
                            VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT};
    vkCmdPipelineBarrier(commandBuffer_, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0U, 1U, &barrier,
                         0U, nullptr, 0U, nullptr);
  }
  if (vkEndCommandBuffer(commandBuffer_) != VK_SUCCESS)
    return fail(error, "failed to end multibounce command buffer");
  VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
  submit.commandBufferCount = 1U;
  submit.pCommandBuffers = &commandBuffer_;
  if (vkQueueSubmit(session_->device().computeQueue(), 1U, &submit,
                    fence_.get()) != VK_SUCCESS ||
      !fence_.wait(10'000'000'000ULL, error)) return false;
  fence_.reset();
  std::uint32_t status = 0U;
  if (!status_.download(*session_, &status, sizeof(status), 0U, error)) return false;
  if (status != 0U) {
    error = "multibounce device rejected event or decision status=" +
            std::to_string(status);
    return false;
  }
  MultibounceSliceResult staged;
  staged.events.resize(laneCount);
  staged.accumulation.resize(laneCount * 2U);
  if (!events_[0].download(*session_, staged.events.data(), eventBytes, 0U, error) ||
      !accumulation_.download(*session_, staged.accumulation.data(), accumBytes, 0U, error)) return false;
  output = std::move(staged);
  return true;
}

} // namespace viennaps::vulkan::ray
