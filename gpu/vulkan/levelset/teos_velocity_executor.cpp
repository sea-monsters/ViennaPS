#include "teos_velocity_executor.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <exception>
#include <limits>
#include <mutex>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace viennaps::vulkan::levelset {

namespace {

using Work = viennaps::TEOSVelocityWork<float>;
using Buffer = runtime::HostVisibleBuffer;

constexpr std::size_t kWorkgroupSize = 256U;
constexpr std::string_view kEntryPoint{"main"};

struct PushConstants {
  float depositionRate = 0.0F;
  float reactionOrder = 0.0F;
  std::uint32_t elementCount = 0U;
};
static_assert(sizeof(PushConstants) == 12U);

[[nodiscard]] bool fail(std::string &error, const std::string_view message) {
  error.assign(message);
  return false;
}

[[nodiscard]] bool validFloat(const float value) {
  return std::isfinite(value) &&
         (value == 0.0F || std::isnormal(value));
}

[[nodiscard]] bool checkedBytes(const std::size_t count,
                                const std::size_t elementBytes,
                                std::size_t &bytes) {
  if (count > std::numeric_limits<std::size_t>::max() / elementBytes)
    return false;
  bytes = count * elementBytes;
  return true;
}

[[nodiscard]] std::uint32_t orderedBits(const float value) {
  const auto bits = std::bit_cast<std::uint32_t>(value);
  return (bits & 0x80000000U) != 0U ? ~bits : bits ^ 0x80000000U;
}

[[nodiscard]] std::uint32_t ulpDistance(const float lhs, const float rhs) {
  const auto a = orderedBits(lhs);
  const auto b = orderedBits(rhs);
  return a >= b ? a - b : b - a;
}

[[nodiscard]] bool liveGeneration(const runtime::ComputeSession *session,
                                  const std::uint64_t generation) {
  return session != nullptr && session->isValid() &&
         session->generation() == generation &&
         runtime::isLiveComputeSessionGeneration(generation);
}

} // namespace

struct VulkanTEOSVelocityExecutor::State {
  runtime::ComputeSession ownedSession{};
  runtime::ComputeSession *session = nullptr;
  std::uint64_t sessionGeneration = 0U;
  runtime::ShaderModule shaderModule{};
  runtime::DescriptorSetLayout descriptorSetLayout{};
  runtime::PipelineLayout pipelineLayout{};
  runtime::ComputePipeline pipeline{};
  runtime::DescriptorPool descriptorPool{};
  runtime::Fence fence{};
  VkDescriptorSet descriptorSet = VK_NULL_HANDLE;
  VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
  mutable std::mutex mutex{};

  ~State() { resetUnlocked(); }

  void resetUnlocked() {
    const bool live = liveGeneration(session, sessionGeneration);
    if (live) {
      fence.destroy();
      pipeline.reset();
      descriptorPool.reset();
      pipelineLayout.reset();
      descriptorSetLayout.reset();
      shaderModule.reset();
    } else {
      fence.abandon();
      pipeline.abandon();
      descriptorPool.abandon();
      pipelineLayout.abandon();
      descriptorSetLayout.abandon();
      shaderModule.abandon();
    }
    descriptorSet = VK_NULL_HANDLE;
    commandBuffer = VK_NULL_HANDLE;
    if (session == &ownedSession)
      ownedSession.reset();
    session = nullptr;
    sessionGeneration = 0U;
  }

  bool setup(const std::string_view spirvPath,
             runtime::ComputeSession *externalSession, std::string &error) {
    std::lock_guard lock(mutex);
    error.clear();
    resetUnlocked();
    if (spirvPath.empty())
      return fail(error, "SPIR-V path is empty");

    if (externalSession != nullptr) {
      if (!externalSession->isValid())
        return fail(error, "external compute session is not initialized");
      session = externalSession;
    } else {
      if (!ownedSession.initialize(error))
        return false;
      session = &ownedSession;
    }
    sessionGeneration = session->generation();

    runtime::SpirvProgram program{};
    if (!runtime::readSpirv(spirvPath, program, error) ||
        !shaderModule.create(session->device(), program, error)) {
      resetUnlocked();
      return false;
    }

    std::array<VkDescriptorSetLayoutBinding, 2U> bindings{};
    for (std::uint32_t binding = 0U; binding < bindings.size(); ++binding) {
      bindings[binding].binding = binding;
      bindings[binding].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
      bindings[binding].descriptorCount = 1U;
      bindings[binding].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    }
    if (!descriptorSetLayout.create(
            session->device(),
            std::span<const VkDescriptorSetLayoutBinding>(bindings.data(),
                                                           bindings.size()),
            error)) {
      resetUnlocked();
      return false;
    }

    const VkPushConstantRange pushRange{VK_SHADER_STAGE_COMPUTE_BIT, 0U,
                                        sizeof(PushConstants)};
    if (!pipelineLayout.create(
            session->device(), descriptorSetLayout.get(),
            std::span<const VkPushConstantRange>(&pushRange, 1U), error) ||
        !descriptorPool.create(session->device(), 1U, 2U,
                               VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, error) ||
        !descriptorPool.allocate(descriptorSetLayout.get(), descriptorSet,
                                 error) ||
        !session->commandContext().allocatePrimary(commandBuffer, error) ||
        !fence.create(session->device(), error)) {
      resetUnlocked();
      return false;
    }

    const runtime::ComputePipelineOptions options{kEntryPoint, {}, nullptr,
                                                   0U};
    if (!pipeline.create(session->device(), shaderModule, pipelineLayout,
                         options, error)) {
      resetUnlocked();
      return false;
    }
    return true;
  }

  [[nodiscard]] bool initialized() const {
    std::lock_guard lock(mutex);
    return liveGeneration(session, sessionGeneration) &&
           shaderModule.get() != VK_NULL_HANDLE &&
           descriptorSetLayout.get() != VK_NULL_HANDLE &&
           pipelineLayout.get() != VK_NULL_HANDLE &&
           pipeline.get() != VK_NULL_HANDLE &&
           descriptorSet != VK_NULL_HANDLE && commandBuffer != VK_NULL_HANDLE &&
           fence.get() != VK_NULL_HANDLE;
  }

  [[nodiscard]] bool invoke(Work &work, std::string &error) {
    std::lock_guard lock(mutex);
    error.clear();
    work.writtenCount = 0U;
    work.complete = false;
    if (!liveGeneration(session, sessionGeneration) ||
        shaderModule.get() == VK_NULL_HANDLE || pipeline.get() == VK_NULL_HANDLE)
      return fail(error, "TEOS velocity session is not initialized");

    const std::size_t count = work.particleFlux.size();
    if (work.cpuOracle.size() != count || work.output.size() != count)
      return fail(error, "TEOS velocity spans must have equal lengths");
    if (count > std::numeric_limits<std::uint32_t>::max())
      return fail(error, "TEOS velocity length exceeds the Vulkan ABI");
    const auto parameters = work.parameters;
    if (!validFloat(parameters.depositionRate) ||
        !validFloat(parameters.reactionOrder) ||
        parameters.depositionRate < 0.0F || parameters.reactionOrder < 0.0F)
      return fail(error, "TEOS reaction parameters are outside the admitted domain");
    for (std::size_t index = 0U; index < count; ++index) {
      if (!validFloat(work.particleFlux[index]) ||
          work.particleFlux[index] < 0.0F)
        return fail(error, "TEOS particle flux is outside the admitted domain");
      if (!validFloat(work.cpuOracle[index]))
        return fail(error, "TEOS CPU oracle is outside strict FP32");
    }
    if (count == 0U) {
      work.complete = true;
      return true;
    }

    std::size_t scalarBytes = 0U;
    if (!checkedBytes(count, sizeof(float), scalarBytes))
      return fail(error, "TEOS buffer size overflows");

    Buffer fluxBuffer{};
    Buffer outputBuffer{};
    const VkBufferUsageFlags usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                     VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                                     VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    const VkMemoryPropertyFlags memory = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT;
    if (!fluxBuffer.create(session->device(), static_cast<VkDeviceSize>(
                               scalarBytes == 0U ? sizeof(float) : scalarBytes),
                           usage, memory, error) ||
        !outputBuffer.create(
            session->device(), static_cast<VkDeviceSize>(
                                   scalarBytes == 0U ? sizeof(float) : scalarBytes),
            usage, memory, error))
      return false;
    if (!fluxBuffer.write(work.particleFlux.data(), scalarBytes, 0U, error) ||
        !outputBuffer.write(work.cpuOracle.data(), scalarBytes, 0U, error) ||
        !fluxBuffer.flush(error) || !outputBuffer.flush(error))
      return false;

    const std::array<VkBuffer, 2U> buffers{fluxBuffer.handle(),
                                           outputBuffer.handle()};
    std::array<VkDescriptorBufferInfo, 2U> infos{};
    std::array<VkWriteDescriptorSet, 2U> writes{};
    for (std::uint32_t binding = 0U; binding < writes.size(); ++binding) {
      infos[binding].buffer = buffers[binding];
      infos[binding].range = static_cast<VkDeviceSize>(scalarBytes);
      writes[binding].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
      writes[binding].dstSet = descriptorSet;
      writes[binding].dstBinding = binding;
      writes[binding].descriptorCount = 1U;
      writes[binding].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
      writes[binding].pBufferInfo = &infos[binding];
    }
    vkUpdateDescriptorSets(session->device().get(),
                           static_cast<std::uint32_t>(writes.size()),
                           writes.data(), 0U, nullptr);

    std::array<VkBufferMemoryBarrier, 2U> preBarriers{};
    for (std::uint32_t index = 0U; index < preBarriers.size(); ++index) {
      preBarriers[index].sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
      preBarriers[index].srcAccessMask = VK_ACCESS_HOST_WRITE_BIT;
      preBarriers[index].dstAccessMask = index == 1U
                                             ? VK_ACCESS_SHADER_WRITE_BIT
                                             : VK_ACCESS_SHADER_READ_BIT;
      preBarriers[index].buffer = buffers[index];
      preBarriers[index].size = static_cast<VkDeviceSize>(scalarBytes);
    }
    VkCommandBufferBeginInfo beginInfo{
        VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (vkResetCommandBuffer(commandBuffer, 0U) != VK_SUCCESS ||
        vkBeginCommandBuffer(commandBuffer, &beginInfo) != VK_SUCCESS)
      return fail(error, "failed to begin TEOS command buffer");
    vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_HOST_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0U, 0U, nullptr,
                         static_cast<std::uint32_t>(preBarriers.size()),
                         preBarriers.data(), 0U, nullptr);
    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE,
                      pipeline.get());
    vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE,
                            pipelineLayout.get(), 0U, 1U, &descriptorSet, 0U,
                            nullptr);
    const PushConstants push{parameters.depositionRate, parameters.reactionOrder,
                             static_cast<std::uint32_t>(count)};
    vkCmdPushConstants(commandBuffer, pipelineLayout.get(),
                       VK_SHADER_STAGE_COMPUTE_BIT, 0U, sizeof(push), &push);
    const auto dispatchGroups =
        count / kWorkgroupSize + (count % kWorkgroupSize != 0U);
    if (dispatchGroups >
        session->selection().properties.limits.maxComputeWorkGroupCount[0])
      return fail(error, "TEOS dispatch exceeds device limits");
    vkCmdDispatch(commandBuffer, static_cast<std::uint32_t>(dispatchGroups), 1U,
                  1U);

    VkBufferMemoryBarrier postBarrier{
        VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
    postBarrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    postBarrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    postBarrier.buffer = outputBuffer.handle();
    postBarrier.size = static_cast<VkDeviceSize>(scalarBytes);
    vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_HOST_BIT, 0U, 0U, nullptr, 1U,
                         &postBarrier, 0U, nullptr);
    if (vkEndCommandBuffer(commandBuffer) != VK_SUCCESS)
      return fail(error, "failed to end TEOS command buffer");
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.commandBufferCount = 1U;
    submit.pCommandBuffers = &commandBuffer;
    if (vkQueueSubmit(session->device().computeQueue(), 1U, &submit,
                      fence.get()) != VK_SUCCESS)
      return fail(error, "failed to submit TEOS command buffer");
    if (!fence.wait(10'000'000'000ULL, error))
      return false;
    fence.reset();
    if (!outputBuffer.invalidate(error))
      return fail(error, "failed to invalidate TEOS output");

    std::vector<float> candidate(count);
    if (!outputBuffer.read(candidate.data(), scalarBytes, 0U, error))
      return false;
    constexpr std::uint32_t kMaxUlp = 32U;
    for (std::size_t index = 0U; index < count; ++index) {
      if (!validFloat(candidate[index]) ||
          ulpDistance(candidate[index], work.cpuOracle[index]) > kMaxUlp) {
        std::ostringstream message;
        message << "Vulkan TEOS velocity differs from CPU oracle at index "
                << index << " (max ULP " << kMaxUlp << ")";
        return fail(error, message.str());
      }
    }
    std::copy(candidate.begin(), candidate.end(), work.output.begin());
    work.writtenCount = count;
    work.complete = true;
    return true;
  }
};

VulkanTEOSVelocityExecutor::VulkanTEOSVelocityExecutor()
    : state_(std::make_shared<State>()) {}

VulkanTEOSVelocityExecutor::~VulkanTEOSVelocityExecutor() = default;

bool VulkanTEOSVelocityExecutor::initialize(const std::string_view spirvPath,
                                            std::string &error) {
  return state_->setup(spirvPath, nullptr, error);
}

bool VulkanTEOSVelocityExecutor::initialize(runtime::ComputeSession &session,
                                            const std::string_view spirvPath,
                                            std::string &error) {
  return state_->setup(spirvPath, &session, error);
}

void VulkanTEOSVelocityExecutor::reset() {
  std::lock_guard lock(state_->mutex);
  state_->resetUnlocked();
}

bool VulkanTEOSVelocityExecutor::isInitialized() const {
  return state_->initialized();
}

VulkanTEOSVelocityExecutor::Executor
VulkanTEOSVelocityExecutor::makeExecutor() const {
  const auto state = state_;
  return [state](Work &work, std::string &error) {
    return state != nullptr && state->invoke(work, error);
  };
}

} // namespace viennaps::vulkan::levelset
