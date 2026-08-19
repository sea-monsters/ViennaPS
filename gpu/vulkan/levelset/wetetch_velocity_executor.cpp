#include "wetetch_velocity_executor.hpp"

#include <materials/psBuiltInMaterial.hpp>

#include <algorithm>
#include <array>
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

using Work = viennaps::WetEtchVelocityWork<float>;
using Rate = viennaps::WetEtchMaterialRate<float>;
using Buffer = runtime::HostVisibleBuffer;

constexpr std::size_t kMaxMaterialRates = 8U;
constexpr std::size_t kWorkgroupSize = 256U;
constexpr std::string_view kEntryPoint{"main"};

struct PushConstants {
  std::array<float, 4> direction100{};
  std::array<float, 4> direction010{};
  std::array<float, 4> rates{};
  std::uint32_t elementCount = 0U;
  std::uint32_t materialRateCount = 0U;
};
static_assert(sizeof(PushConstants) == 56U);

[[nodiscard]] bool fail(std::string &error, const std::string_view message) {
  error.assign(message);
  return false;
}

[[nodiscard]] bool isNormalOrZero(const float value) {
  return value == 0.0F || std::isnormal(value);
}

[[nodiscard]] bool validFloat(const float value) {
  return std::isfinite(value) && isNormalOrZero(value);
}

[[nodiscard]] bool validMaterialId(const std::int32_t id) {
  return id >= 0 && id <= static_cast<std::int32_t>(kBuiltInMaterialMaxId) &&
         isValidBuiltInMaterialId(static_cast<std::uint16_t>(id));
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

struct VulkanWetEtchVelocityExecutor::State {
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

    std::array<VkDescriptorSetLayoutBinding, 6U> bindings{};
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
        !descriptorPool.create(session->device(), 1U, 6U,
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
           descriptorSet != VK_NULL_HANDLE &&
           commandBuffer != VK_NULL_HANDLE && fence.get() != VK_NULL_HANDLE;
  }

  [[nodiscard]] bool invoke(Work &work, std::string &error) {
    std::lock_guard lock(mutex);
    error.clear();
    work.writtenCount = 0U;
    work.complete = false;
    if (!liveGeneration(session, sessionGeneration) ||
        shaderModule.get() == VK_NULL_HANDLE || pipeline.get() == VK_NULL_HANDLE)
      return fail(error, "wet-etch velocity session is not initialized");

    const std::size_t count = work.normals.size();
    if (work.coordinates.size() != count || work.materialIds.size() != count ||
        work.cpuOracle.size() != count || work.output.size() != count)
      return fail(error, "wet-etch velocity spans must have equal lengths");
    if (count > std::numeric_limits<std::uint32_t>::max())
      return fail(error, "wet-etch velocity length exceeds the Vulkan ABI");
    const auto &parameters = work.parameters;
    if (parameters.materialRates.empty() ||
        parameters.materialRates.size() > kMaxMaterialRates)
      return fail(error, "wet-etch material-rate table is outside its ABI");

    for (const float value : parameters.direction100)
      if (!validFloat(value))
        return fail(error, "direction100 is outside strict FP32");
    for (const float value : parameters.direction010)
      if (!validFloat(value))
        return fail(error, "direction010 is outside strict FP32");
    for (const float value : {parameters.r100, parameters.r110,
                              parameters.r111, parameters.r311}) {
      if (!validFloat(value) || value < 0.0F)
        return fail(error, "wet-etch rate is outside the admitted domain");
    }

    const auto nonZeroDirection = [](const std::array<float, 3> &direction) {
      return direction[0] != 0.0F || direction[1] != 0.0F ||
             direction[2] != 0.0F;
    };
    if (!nonZeroDirection(parameters.direction100) ||
        !nonZeroDirection(parameters.direction010))
      return fail(error, "crystal direction must be non-zero");

    std::array<std::int32_t, kMaxMaterialRates> rateIds{};
    std::array<float, kMaxMaterialRates> rateValues{};
    for (std::size_t index = 0U; index < parameters.materialRates.size();
         ++index) {
      const auto &entry = parameters.materialRates[index];
      if (!validMaterialId(entry.materialId) || !validFloat(entry.rate) ||
          entry.rate < 0.0F)
        return fail(error, "material-rate table contains an invalid entry");
      for (std::size_t prior = 0U; prior < index; ++prior) {
        if (rateIds[prior] == entry.materialId)
          return fail(error, "material-rate table contains duplicate IDs");
      }
      rateIds[index] = entry.materialId;
      rateValues[index] = entry.rate;
    }
    for (std::size_t index = 0U; index < count; ++index) {
      for (const float value : work.coordinates[index])
        if (!validFloat(value))
          return fail(error, "coordinate input is outside strict FP32");
      for (const float value : work.normals[index])
        if (!validFloat(value))
          return fail(error, "normal input is outside strict FP32");
      if (!validMaterialId(work.materialIds[index]))
        return fail(error, "material input is outside the built-in domain");
      if (!validFloat(work.cpuOracle[index]))
        return fail(error, "CPU oracle is outside strict FP32");
    }
    if (count == 0U) {
      work.writtenCount = 0U;
      work.complete = true;
      return true;
    }

    std::size_t coordBytes = 0U;
    std::size_t normalBytes = 0U;
    std::size_t scalarBytes = 0U;
    if (!checkedBytes(count, sizeof(std::array<float, 4>), coordBytes) ||
        !checkedBytes(count, sizeof(std::array<float, 4>), normalBytes) ||
        !checkedBytes(count, sizeof(std::int32_t), scalarBytes))
      return fail(error, "wet-etch buffer size overflows");
    const auto tableBytes = parameters.materialRates.size() * sizeof(float);

    Buffer coordinatesBuffer{};
    Buffer normalsBuffer{};
    Buffer materialBuffer{};
    Buffer outputBuffer{};
    Buffer rateIdBuffer{};
    Buffer rateValueBuffer{};
    const VkBufferUsageFlags usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                     VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                                     VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    const VkMemoryPropertyFlags memory = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT;
    auto create = [&](Buffer &buffer, const std::size_t bytes,
                      const char *name) {
      if (!buffer.create(session->device(),
                         static_cast<VkDeviceSize>(bytes == 0U ? 4U : bytes),
                         usage, memory, error)) {
        if (error.empty())
          error = std::string("failed to allocate ") + name + " buffer";
        return false;
      }
      return true;
    };
    if (!create(coordinatesBuffer, coordBytes, "coordinate") ||
        !create(normalsBuffer, normalBytes, "normal") ||
        !create(materialBuffer, scalarBytes, "material") ||
        !create(outputBuffer, scalarBytes, "output") ||
        !create(rateIdBuffer, tableBytes, "material-rate ID") ||
        !create(rateValueBuffer, tableBytes, "material-rate value"))
      return false;

    std::vector<std::array<float, 4>> coordinates(count);
    std::vector<std::array<float, 4>> normals(count);
    for (std::size_t index = 0U; index < count; ++index) {
      coordinates[index] = {work.coordinates[index][0], work.coordinates[index][1],
                            work.coordinates[index][2], 0.0F};
      normals[index] = {work.normals[index][0], work.normals[index][1],
                        work.normals[index][2], 0.0F};
    }

    if (!coordinatesBuffer.write(coordinates.data(), coordBytes, 0U, error) ||
        !normalsBuffer.write(normals.data(), normalBytes, 0U, error) ||
        !materialBuffer.write(work.materialIds.data(), scalarBytes, 0U,
                              error) ||
        !outputBuffer.write(work.cpuOracle.data(), scalarBytes, 0U, error) ||
        !rateIdBuffer.write(rateIds.data(), tableBytes, 0U, error) ||
        !rateValueBuffer.write(rateValues.data(), tableBytes, 0U, error) ||
        !coordinatesBuffer.flush(error) || !normalsBuffer.flush(error) ||
        !materialBuffer.flush(error) || !outputBuffer.flush(error) ||
        !rateIdBuffer.flush(error) || !rateValueBuffer.flush(error))
      return false;

    const std::array<VkBuffer, 6U> buffers{
        coordinatesBuffer.handle(), normalsBuffer.handle(), materialBuffer.handle(),
        outputBuffer.handle(), rateIdBuffer.handle(), rateValueBuffer.handle()};
    std::array<VkDescriptorBufferInfo, 6U> infos{};
    std::array<VkWriteDescriptorSet, 6U> writes{};
    const std::array<VkDeviceSize, 6U> ranges{
        static_cast<VkDeviceSize>(coordBytes),
        static_cast<VkDeviceSize>(normalBytes), static_cast<VkDeviceSize>(scalarBytes),
        static_cast<VkDeviceSize>(scalarBytes), static_cast<VkDeviceSize>(tableBytes),
        static_cast<VkDeviceSize>(tableBytes)};
    for (std::uint32_t binding = 0U; binding < writes.size(); ++binding) {
      infos[binding].buffer = buffers[binding];
      infos[binding].range = ranges[binding];
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

    std::array<VkBufferMemoryBarrier, 6U> preBarriers{};
    for (std::uint32_t index = 0U; index < preBarriers.size(); ++index) {
      preBarriers[index].sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
      preBarriers[index].srcAccessMask = VK_ACCESS_HOST_WRITE_BIT;
      preBarriers[index].dstAccessMask = index == 3U
                                             ? VK_ACCESS_SHADER_WRITE_BIT
                                             : VK_ACCESS_SHADER_READ_BIT;
      preBarriers[index].buffer = buffers[index];
      preBarriers[index].size = ranges[index];
    }
    VkCommandBufferBeginInfo beginInfo{
        VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (vkResetCommandBuffer(commandBuffer, 0U) != VK_SUCCESS ||
        vkBeginCommandBuffer(commandBuffer, &beginInfo) != VK_SUCCESS)
      return fail(error, "failed to begin wet-etch command buffer");
    vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_HOST_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0U, 0U, nullptr,
                         static_cast<std::uint32_t>(preBarriers.size()),
                         preBarriers.data(), 0U, nullptr);
    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE,
                      pipeline.get());
    vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE,
                            pipelineLayout.get(), 0U, 1U, &descriptorSet, 0U,
                            nullptr);
    PushConstants push{};
    push.direction100 = {parameters.direction100[0], parameters.direction100[1],
                         parameters.direction100[2], 0.0F};
    push.direction010 = {parameters.direction010[0], parameters.direction010[1],
                         parameters.direction010[2], 0.0F};
    push.rates = {parameters.r100, parameters.r110, parameters.r111,
                  parameters.r311};
    push.elementCount = static_cast<std::uint32_t>(count);
    push.materialRateCount =
        static_cast<std::uint32_t>(parameters.materialRates.size());
    vkCmdPushConstants(commandBuffer, pipelineLayout.get(),
                       VK_SHADER_STAGE_COMPUTE_BIT, 0U, sizeof(push), &push);
    const auto dispatchGroups =
        count / kWorkgroupSize + (count % kWorkgroupSize != 0U);
    if (dispatchGroups >
        session->selection().properties.limits.maxComputeWorkGroupCount[0])
      return fail(error, "wet-etch dispatch exceeds device limits");
    vkCmdDispatch(commandBuffer, static_cast<std::uint32_t>(dispatchGroups), 1U,
                  1U);

    VkBufferMemoryBarrier postBarrier{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
    postBarrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    postBarrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    postBarrier.buffer = outputBuffer.handle();
    postBarrier.size = static_cast<VkDeviceSize>(scalarBytes);
    vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_HOST_BIT, 0U, 0U, nullptr, 1U,
                         &postBarrier, 0U, nullptr);
    if (vkEndCommandBuffer(commandBuffer) != VK_SUCCESS)
      return fail(error, "failed to end wet-etch command buffer");
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.commandBufferCount = 1U;
    submit.pCommandBuffers = &commandBuffer;
    if (vkQueueSubmit(session->device().computeQueue(), 1U, &submit,
                      fence.get()) != VK_SUCCESS)
      return fail(error, "failed to submit wet-etch command buffer");
    if (!fence.wait(10'000'000'000ULL, error))
      return false;
    fence.reset();
    if (!outputBuffer.invalidate(error))
      return fail(error, "failed to invalidate wet-etch output");

    std::vector<float> candidate(count);
    if (!outputBuffer.read(candidate.data(), scalarBytes, 0U, error))
      return false;
    constexpr std::uint32_t kMaxUlp = 32U;
    for (std::size_t index = 0U; index < count; ++index) {
      if (!validFloat(candidate[index]) ||
          ulpDistance(candidate[index], work.cpuOracle[index]) > kMaxUlp) {
        std::ostringstream message;
        message << "Vulkan wet-etch velocity differs from CPU oracle at index "
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

VulkanWetEtchVelocityExecutor::VulkanWetEtchVelocityExecutor()
    : state_(std::make_shared<State>()) {}

VulkanWetEtchVelocityExecutor::~VulkanWetEtchVelocityExecutor() = default;

bool VulkanWetEtchVelocityExecutor::initialize(const std::string_view spirvPath,
                                               std::string &error) {
  return state_->setup(spirvPath, nullptr, error);
}

bool VulkanWetEtchVelocityExecutor::initialize(runtime::ComputeSession &session,
                                               const std::string_view spirvPath,
                                               std::string &error) {
  return state_->setup(spirvPath, &session, error);
}

void VulkanWetEtchVelocityExecutor::reset() {
  std::lock_guard lock(state_->mutex);
  state_->resetUnlocked();
}

bool VulkanWetEtchVelocityExecutor::isInitialized() const {
  return state_->initialized();
}

VulkanWetEtchVelocityExecutor::Executor
VulkanWetEtchVelocityExecutor::makeExecutor() const {
  const auto state = state_;
  return [state](Work &work, std::string &error) {
    try {
      return state->invoke(work, error);
    } catch (const std::exception &exception) {
      error = exception.what();
      work.writtenCount = 0U;
      work.complete = false;
      return false;
    } catch (...) {
      error = "unknown Vulkan wet-etch velocity executor failure";
      work.writtenCount = 0U;
      work.complete = false;
      return false;
    }
  };
}

} // namespace viennaps::vulkan::levelset
