// Copyright 2026 ViennaPS
// SPDX-License-Identifier: MIT

#include "neutral_transport_surface.hpp"

#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <sstream>
#include <string>

namespace {

[[nodiscard]] bool setError(std::string &error, const std::string_view phase,
                            const std::string_view message) {
  error = std::string(phase);
  if (!message.empty()) {
    error += ": ";
    error += message;
  }
  return false;
}

constexpr std::string_view kEntryPoint{"main"};

[[nodiscard]] bool isNormalOrZero(const float value) {
  return value == 0.0F || std::isnormal(value);
}

} // namespace

namespace viennaps::vulkan::surface {

NeutralTransportSurfaceModelFp32::~NeutralTransportSurfaceModelFp32() {
  reset();
}

bool NeutralTransportSurfaceModelFp32::initialize(
    const std::string_view spirvPath, std::string &error) {
  error.clear();
  if (isInitialized()) {
    return true;
  }
  if (spirvPath.empty()) {
    return setError(error, "initialization", "SPIR-V path is empty");
  }

  reset();
  const auto failInitialization = [this, &error]() {
    const auto detail = error;
    reset();
    return setError(error, "initialization", detail);
  };

  if (!session_.initialize(error)) {
    return failInitialization();
  }

  runtime::SpirvProgram program{};
  if (!runtime::readSpirv(spirvPath, program, error) ||
      !shaderModule_.create(session_.device(), program, error)) {
    return failInitialization();
  }

  std::array<VkDescriptorSetLayoutBinding, 3U> bindings{};
  for (std::uint32_t binding = 0U; binding < bindings.size(); ++binding) {
    bindings[binding].binding = binding;
    bindings[binding].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    bindings[binding].descriptorCount = 1U;
    bindings[binding].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
  }
  if (!descriptorSetLayout_.create(
          session_.device(),
          std::span<const VkDescriptorSetLayoutBinding>(bindings.data(),
                                                        bindings.size()),
          error)) {
    return failInitialization();
  }

  const VkPushConstantRange pushRange{VK_SHADER_STAGE_COMPUTE_BIT, 0U,
                                      sizeof(PushConstants)};
  if (!pipelineLayout_.create(session_.device(), descriptorSetLayout_.get(),
                              std::span(&pushRange, 1U), error)) {
    return failInitialization();
  }
  if (!descriptorPool_.create(session_.device(), 1U, 3U,
                              VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, error) ||
      !descriptorPool_.allocate(descriptorSetLayout_.get(), descriptorSet_,
                                error)) {
    return failInitialization();
  }
  if (!session_.commandContext().allocatePrimary(commandBuffer_, error) ||
      !fence_.create(session_.device(), error)) {
    return failInitialization();
  }

  const runtime::ComputePipelineOptions options{kEntryPoint, {}, nullptr, 0U};
  if (!pipeline_.create(session_.device(), shaderModule_, pipelineLayout_,
                        options, error)) {
    return failInitialization();
  }
  return true;
}

void NeutralTransportSurfaceModelFp32::reset() {
  fence_.destroy();
  pipeline_.reset();
  descriptorPool_.reset();
  pipelineLayout_.reset();
  descriptorSetLayout_.reset();
  shaderModule_.reset();
  commandBuffer_ = VK_NULL_HANDLE;
  descriptorSet_ = VK_NULL_HANDLE;
  session_.reset();
}

bool NeutralTransportSurfaceModelFp32::isInitialized() const {
  return session_.isValid() && shaderModule_.get() != VK_NULL_HANDLE &&
         descriptorSetLayout_.get() != VK_NULL_HANDLE &&
         pipelineLayout_.get() != VK_NULL_HANDLE &&
         pipeline_.get() != VK_NULL_HANDLE &&
         descriptorSet_ != VK_NULL_HANDLE && commandBuffer_ != VK_NULL_HANDLE &&
         fence_.get() != VK_NULL_HANDLE;
}

bool NeutralTransportSurfaceModelFp32::isReady(std::string &error) const {
  if (!isInitialized()) {
    return setError(error, "execution", "surface model is not initialized");
  }
  return true;
}

bool NeutralTransportSurfaceModelFp32::createFloatBuffer(
    const std::size_t elementCount, runtime::HostVisibleBuffer &buffer,
    std::string &error) {
  if (!isReady(error)) {
    return false;
  }
  const auto allocatedElements = elementCount == 0U ? 1U : elementCount;
  if (allocatedElements >
      std::numeric_limits<std::size_t>::max() / sizeof(float)) {
    return setError(error, "createFloatBuffer", "element count overflows");
  }
  return buffer.create(
      session_.device(),
      static_cast<VkDeviceSize>(allocatedElements * sizeof(float)),
      kFloatBufferUsage, kFloatMemoryFlags, error);
}

bool NeutralTransportSurfaceModelFp32::evaluate(
    runtime::HostVisibleBuffer &coverage, const std::size_t coverageCount,
    runtime::HostVisibleBuffer &materialIds, const std::size_t materialCount,
    runtime::HostVisibleBuffer &velocity, const std::size_t velocityCount,
    const NeutralTransportSurfaceParamsFp32 &params, std::string &error) const {
  error.clear();
  if (!isReady(error)) {
    return false;
  }
  if (coverageCount != materialCount || coverageCount != velocityCount) {
    return setError(error, "validation", "all SoA lengths must match");
  }
  const auto validateLength = [&error](const std::string_view label,
                                       const runtime::HostVisibleBuffer &buffer,
                                       const std::size_t count) {
    if (!buffer.isValid()) {
      return setError(error, "validation",
                      std::string(label) + ": buffer is not initialized");
    }
    if (count > static_cast<std::size_t>(buffer.size() / sizeof(float))) {
      std::ostringstream out;
      out << label << ": element count exceeds buffer capacity";
      return setError(error, "validation", out.str());
    }
    return true;
  };
  if (!validateLength("coverage", coverage, coverageCount) ||
      !validateLength("materialIds", materialIds, materialCount) ||
      !validateLength("velocity", velocity, velocityCount)) {
    return false;
  }
  if (coverage.handle() == materialIds.handle() ||
      coverage.handle() == velocity.handle() ||
      materialIds.handle() == velocity.handle()) {
    return setError(error, "validation", "SoA buffers must not alias");
  }
  if (!std::isfinite(params.kEtch) ||
      !std::isfinite(params.surfaceSiteDensity) ||
      !std::isfinite(params.siliconDensity) ||
      !std::isfinite(params.timeToSecond) ||
      !std::isfinite(params.lengthToMeter) || params.lengthToMeter == 0.0F) {
    return setError(error, "validation", "surface parameters are invalid");
  }
  if (!isNormalOrZero(params.kEtch) ||
      !isNormalOrZero(params.surfaceSiteDensity) ||
      !isNormalOrZero(params.siliconDensity) ||
      !isNormalOrZero(params.timeToSecond) ||
      !isNormalOrZero(params.lengthToMeter)) {
    return setError(error, "validation",
                    "surface parameters must be zero or normal finite FP32");
  }
  if (coverageCount == 0U) {
    return true;
  }
  if (!coverage.mappedPtr() && !coverage.map(error)) {
    return false;
  }
  if (!materialIds.mappedPtr() && !materialIds.map(error)) {
    return false;
  }
  if (!velocity.mappedPtr() && !velocity.map(error)) {
    return false;
  }
  if (!coverage.flush(error) || !materialIds.flush(error) ||
      !velocity.flush(error)) {
    return setError(error, "execution", "failed to flush host writes");
  }

  const auto *coverageValues = static_cast<const float *>(coverage.mappedPtr());
  const auto *materialValues =
      static_cast<const float *>(materialIds.mappedPtr());
  for (std::size_t index = 0U; index < coverageCount; ++index) {
    if (!isNormalOrZero(coverageValues[index])) {
      return setError(error, "validation",
                      "coverage values must be zero or normal finite FP32");
    }
    if (materialValues[index] !=
        static_cast<float>(params.etchFrontMaterialId)) {
      continue;
    }

    volatile float product =
        params.kEtch * params.surfaceSiteDensity * coverageValues[index];
    if (!isNormalOrZero(product)) {
      return setError(error, "validation",
                      "surface product leaves the strict FP32 domain");
    }
    if (params.siliconDensity <= 0.0F) {
      continue;
    }
    volatile float etchVelocity = product / params.siliconDensity;
    if (!isNormalOrZero(etchVelocity)) {
      return setError(error, "validation",
                      "surface velocity leaves the strict FP32 domain");
    }
    volatile float signedVelocity = etchVelocity * params.timeToSecond;
    if (!isNormalOrZero(signedVelocity)) {
      return setError(error, "validation",
                      "surface time scaling leaves the strict FP32 domain");
    }
    volatile float finalVelocity = signedVelocity / params.lengthToMeter;
    if (!isNormalOrZero(finalVelocity)) {
      return setError(error, "validation",
                      "surface length scaling leaves the strict FP32 domain");
    }
  }

  if (coverageCount > std::numeric_limits<std::uint32_t>::max()) {
    return setError(error, "dispatch", "element count exceeds shader ABI");
  }
  const auto dispatchGroups =
      coverageCount / kWorkgroupSize + (coverageCount % kWorkgroupSize != 0U);
  if (dispatchGroups >
      session_.selection().properties.limits.maxComputeWorkGroupCount[0]) {
    return setError(error, "dispatch",
                    "dispatch group count exceeds device limit");
  }
  const auto bytes = static_cast<VkDeviceSize>(coverageCount * sizeof(float));

  std::array<VkDescriptorBufferInfo, 3U> infos{};
  const std::array<VkBuffer, 3U> buffers{
      coverage.handle(), materialIds.handle(), velocity.handle()};
  std::array<VkWriteDescriptorSet, 3U> writes{};
  for (std::uint32_t binding = 0U; binding < writes.size(); ++binding) {
    infos[binding].buffer = buffers[binding];
    infos[binding].range = bytes;
    writes[binding].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[binding].dstSet = descriptorSet_;
    writes[binding].dstBinding = binding;
    writes[binding].descriptorCount = 1U;
    writes[binding].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    writes[binding].pBufferInfo = &infos[binding];
  }
  vkUpdateDescriptorSets(session_.device().get(),
                         static_cast<std::uint32_t>(writes.size()),
                         writes.data(), 0U, nullptr);

  std::array<VkBufferMemoryBarrier, 3U> preBarriers{};
  for (std::uint32_t index = 0U; index < preBarriers.size(); ++index) {
    preBarriers[index].sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
    preBarriers[index].srcAccessMask = VK_ACCESS_HOST_WRITE_BIT;
    preBarriers[index].dstAccessMask =
        index == 2U ? VK_ACCESS_SHADER_WRITE_BIT : VK_ACCESS_SHADER_READ_BIT;
    preBarriers[index].buffer = buffers[index];
    preBarriers[index].size = bytes;
  }
  VkCommandBufferBeginInfo beginInfo{
      VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  if (vkResetCommandBuffer(commandBuffer_, 0U) != VK_SUCCESS ||
      vkBeginCommandBuffer(commandBuffer_, &beginInfo) != VK_SUCCESS) {
    return setError(error, "dispatch", "failed to begin command buffer");
  }
  vkCmdPipelineBarrier(commandBuffer_, VK_PIPELINE_STAGE_HOST_BIT,
                       VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0U, 0U, nullptr,
                       static_cast<std::uint32_t>(preBarriers.size()),
                       preBarriers.data(), 0U, nullptr);
  vkCmdBindPipeline(commandBuffer_, VK_PIPELINE_BIND_POINT_COMPUTE,
                    pipeline_.get());
  vkCmdBindDescriptorSets(commandBuffer_, VK_PIPELINE_BIND_POINT_COMPUTE,
                          pipelineLayout_.get(), 0U, 1U, &descriptorSet_, 0U,
                          nullptr);
  const PushConstants pushConstants{static_cast<std::uint32_t>(coverageCount),
                                    params.etchFrontMaterialId,
                                    params.kEtch,
                                    params.surfaceSiteDensity,
                                    params.siliconDensity,
                                    params.timeToSecond,
                                    params.lengthToMeter};
  vkCmdPushConstants(commandBuffer_, pipelineLayout_.get(),
                     VK_SHADER_STAGE_COMPUTE_BIT, 0U, sizeof(PushConstants),
                     &pushConstants);
  vkCmdDispatch(commandBuffer_, static_cast<std::uint32_t>(dispatchGroups), 1U,
                1U);

  VkBufferMemoryBarrier postBarrier{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
  postBarrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
  postBarrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
  postBarrier.buffer = velocity.handle();
  postBarrier.size = bytes;
  vkCmdPipelineBarrier(commandBuffer_, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                       VK_PIPELINE_STAGE_HOST_BIT, 0U, 0U, nullptr, 1U,
                       &postBarrier, 0U, nullptr);
  if (vkEndCommandBuffer(commandBuffer_) != VK_SUCCESS) {
    return setError(error, "dispatch", "failed to end command buffer");
  }
  VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
  submit.commandBufferCount = 1U;
  submit.pCommandBuffers = &commandBuffer_;
  if (vkQueueSubmit(session_.device().computeQueue(), 1U, &submit,
                    fence_.get()) != VK_SUCCESS) {
    return setError(error, "dispatch", "failed to submit command buffer");
  }
  if (!fence_.wait(10'000'000'000ULL, error)) {
    return false;
  }
  fence_.reset();
  if (!velocity.invalidate(error)) {
    return setError(error, "execution", "failed to invalidate velocity output");
  }
  return true;
}

const runtime::VulkanDevice &NeutralTransportSurfaceModelFp32::device() const {
  return session_.device();
}

float NeutralTransportSurfaceModelFp32::cpuVelocity(
    const float coverage, const float materialId,
    const NeutralTransportSurfaceParamsFp32 &params) {
  if (materialId != static_cast<float>(params.etchFrontMaterialId)) {
    return 0.0F;
  }
  if (params.siliconDensity <= 0.0F) {
    return 0.0F;
  }
  volatile float product = params.kEtch * params.surfaceSiteDensity;
  product = product * coverage;
  volatile float etchVelocity = product / params.siliconDensity;
  volatile float signedVelocity = -etchVelocity;
  signedVelocity = signedVelocity * params.timeToSecond;
  signedVelocity = signedVelocity / params.lengthToMeter;
  return signedVelocity;
}

} // namespace viennaps::vulkan::surface
