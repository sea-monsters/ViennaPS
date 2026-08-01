// Copyright 2026 ViennaPS
// SPDX-License-Identifier: MIT
//
// Minimal compute smoke using the reusable ComputeSession RAII wrapper.
//
// Expected output:
//   output[i] = input[i] * 2 + 1, for i = 0..15

#include "compute_session.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <limits>
#include <span>
#include <sstream>
#include <string>
#include <vector>

namespace {

template <typename T>
[[nodiscard]] constexpr std::size_t elementCount(const T &array) {
  return array.size();
}

struct PushConstants {
  float bias = 0.0F;
};

} // namespace

int main() {
  using namespace viennaps::vulkan::runtime;

  std::string error;
  ComputeSession session{};
  if (!session.initialize(error)) {
    std::cerr << error << '\n';
    return EXIT_FAILURE;
  }

  const auto fail = [](const std::string_view message) {
    std::cerr << "[SessionSmoke] " << message << '\n';
    return EXIT_FAILURE;
  };
  if (!session.isValid() || session.instanceHandle() == VK_NULL_HANDLE ||
      session.deviceHandle() == VK_NULL_HANDLE ||
      session.commandContext().pool() == VK_NULL_HANDLE) {
    return fail("initialized session does not own all required handles");
  }
  if (!session.initialize(error)) {
    return fail("same-option initialization was not idempotent");
  }

  ComputeSession conflictSession{};
  ComputeSessionOptions conflictOptions{};
  conflictOptions.manualDeviceIndex = 0;
  conflictOptions.manualDeviceName = session.selection().properties.deviceName;
  if (conflictSession.initialize(error, conflictOptions) ||
      conflictSession.isValid()) {
    return fail("conflicting manual selectors were not rejected");
  }

  ComputeSession invalidSession{};
  ComputeSessionOptions invalidOptions{};
  invalidOptions.manualDeviceName = "__viennaps_missing_device__";
  if (invalidSession.initialize(error, invalidOptions) ||
      invalidSession.isValid()) {
    return fail("missing manual device name was not rejected cleanly");
  }
  invalidOptions.manualDeviceName = {};
  invalidOptions.manualDeviceUuid = "not-a-vulkan-uuid";
  if (invalidSession.initialize(error, invalidOptions) ||
      invalidSession.isValid()) {
    return fail("malformed manual device UUID was not rejected cleanly");
  }
  invalidOptions.manualDeviceUuid = {};
  invalidOptions.manualDeviceIndex =
      std::numeric_limits<std::uint32_t>::max() - 1U;
  if (invalidSession.initialize(error, invalidOptions) ||
      invalidSession.isValid()) {
    return fail("out-of-range manual device index was not rejected cleanly");
  }

  std::uint32_t physicalDeviceCount = 0;
  if (vkEnumeratePhysicalDevices(session.instanceHandle(), &physicalDeviceCount,
                                 nullptr) != VK_SUCCESS ||
      physicalDeviceCount == 0) {
    return fail("could not enumerate the initialized physical device");
  }
  std::vector<VkPhysicalDevice> physicalDevices(physicalDeviceCount);
  if (vkEnumeratePhysicalDevices(session.instanceHandle(), &physicalDeviceCount,
                                 physicalDevices.data()) != VK_SUCCESS) {
    return fail("could not read the physical-device enumeration");
  }
  const auto selectedDevice = session.selection().handle;
  const auto selectedIt =
      std::find(physicalDevices.begin(), physicalDevices.end(), selectedDevice);
  if (selectedIt == physicalDevices.end()) {
    return fail("selected device is absent from physical-device enumeration");
  }
  const auto selectedIndex =
      static_cast<std::uint32_t>(selectedIt - physicalDevices.begin());

  VkPhysicalDeviceIDProperties idProperties{};
  idProperties.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES;
  VkPhysicalDeviceProperties2 properties2{};
  properties2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
  properties2.pNext = &idProperties;
  vkGetPhysicalDeviceProperties2(selectedDevice, &properties2);
  std::ostringstream uuidStream;
  uuidStream << std::hex << std::setfill('0');
  for (const std::uint8_t byte : idProperties.deviceUUID) {
    uuidStream << std::setw(2) << static_cast<unsigned int>(byte);
  }
  const std::string selectedUuid = uuidStream.str();
  const std::string selectedName = session.selection().properties.deviceName;

  session.reset();
  if (session.isValid() || session.instanceHandle() != VK_NULL_HANDLE ||
      session.deviceHandle() != VK_NULL_HANDLE ||
      session.commandContext().pool() != VK_NULL_HANDLE) {
    return fail("reset did not release the complete session");
  }
  ComputeSessionOptions indexOptions{};
  indexOptions.manualDeviceIndex = selectedIndex;
  if (!session.initialize(error, indexOptions) ||
      !session.initialize(error, indexOptions)) {
    return fail("manual index selection or its idempotent reuse failed");
  }
  ComputeSessionOptions nameOptions{};
  nameOptions.manualDeviceName = selectedName;
  if (session.initialize(error, nameOptions)) {
    return fail("live session accepted different device options without reset");
  }
  session.reset();
  if (!session.initialize(error, nameOptions)) {
    return fail("manual name selection failed");
  }
  session.reset();
  ComputeSessionOptions uuidOptions{};
  uuidOptions.manualDeviceUuid = selectedUuid;
  if (!session.initialize(error, uuidOptions)) {
    return fail("manual UUID selection failed");
  }

  ComputeSession movedSession(std::move(session));
  if (session.isValid() || !movedSession.isValid()) {
    return fail("move construction did not transfer session ownership");
  }
  session = std::move(movedSession);
  if (!session.isValid() || movedSession.isValid()) {
    return fail("move assignment did not transfer session ownership");
  }

  const ComputeDeviceSelection &selection = session.selection();
  std::cout << "[SessionSmoke] device: " << selection.properties.deviceName
            << '\n';
  std::cout << "[SessionSmoke] device type: "
            << vkDeviceTypeName(selection.properties.deviceType) << '\n';
  std::cout << "[SessionSmoke] vendorId: 0x" << std::hex
            << selection.properties.vendorID << ", deviceId: 0x"
            << selection.properties.deviceID << std::dec << '\n';
  std::cout << "[SessionSmoke] queue family: " << selection.queue.familyIndex
            << ", dedicated: "
            << (selection.queue.dedicatedQueue ? "yes" : "no") << '\n';

  SpirvProgram program{};
  if (!readSpirv(VIENNAPS_RUNTIME_SMOKE_SPV_PATH, program, error)) {
    std::cerr << error << '\n';
    return EXIT_FAILURE;
  }

  constexpr std::array<float, 16> inputData = {
      0.0F, 1.0F, 2.0F,  3.0F,  4.0F,  5.0F,  6.0F,  7.0F,
      8.0F, 9.0F, 10.0F, 11.0F, 12.0F, 13.0F, 14.0F, 15.0F};
  std::array<float, 16> expected{};
  std::array<float, 16> output{};
  for (std::size_t i = 0; i < elementCount(expected); ++i) {
    expected[i] = inputData[i] * 2.0F + 1.0F;
  }

  auto &device = session.device();
  HostVisibleBuffer inputBuffer{};
  HostVisibleBuffer outputBuffer{};
  const VkBufferUsageFlags inUsage =
      VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
  const VkBufferUsageFlags outUsage =
      VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
  const VkMemoryPropertyFlags requiredMemory =
      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT;

  if (!inputBuffer.create(device, sizeof(inputData), inUsage, requiredMemory,
                          error) ||
      !outputBuffer.create(device, sizeof(output), outUsage, requiredMemory,
                           error)) {
    std::cerr << error << '\n';
    return EXIT_FAILURE;
  }

  if (!inputBuffer.map(error) || !outputBuffer.map(error)) {
    std::cerr << error << '\n';
    return EXIT_FAILURE;
  }
  if (!inputBuffer.write(inputData.data(), sizeof(inputData), 0, error)) {
    std::cerr << error << '\n';
    return EXIT_FAILURE;
  }

  ShaderModule module{};
  if (!module.create(device, program, error)) {
    std::cerr << error << '\n';
    return EXIT_FAILURE;
  }

  std::array<VkDescriptorSetLayoutBinding, 2> bindings{};
  bindings[0].binding = 0;
  bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  bindings[0].descriptorCount = 1;
  bindings[0].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
  bindings[1].binding = 1;
  bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  bindings[1].descriptorCount = 1;
  bindings[1].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

  DescriptorSetLayout descriptorSetLayout{};
  if (!descriptorSetLayout.create(device, bindings, error)) {
    std::cerr << error << '\n';
    return EXIT_FAILURE;
  }

  PipelineLayout pipelineLayout{};
  const VkPushConstantRange pushConstantRange{VK_SHADER_STAGE_COMPUTE_BIT, 0,
                                              sizeof(PushConstants)};
  if (!pipelineLayout.create(device, descriptorSetLayout.get(),
                             std::span(&pushConstantRange, 1), error)) {
    std::cerr << error << '\n';
    return EXIT_FAILURE;
  }

  ComputePipeline pipeline{};
  constexpr float multiplier = 2.0F;
  const VkSpecializationMapEntry multiplierEntry{0, 0, sizeof(multiplier)};
  const ComputePipelineOptions pipelineOptions{
      "main", std::span(&multiplierEntry, 1), &multiplier, sizeof(multiplier)};
  if (!pipeline.create(device, module, pipelineLayout, pipelineOptions,
                       error)) {
    std::cerr << error << '\n';
    return EXIT_FAILURE;
  }

  DescriptorPool descriptorPool{};
  if (!descriptorPool.create(device, 1u, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                             error)) {
    std::cerr << error << '\n';
    return EXIT_FAILURE;
  }
  VkDescriptorSet descriptorSet = VK_NULL_HANDLE;
  if (!descriptorPool.allocate(descriptorSetLayout.get(), descriptorSet,
                               error)) {
    std::cerr << error << '\n';
    return EXIT_FAILURE;
  }

  VkDescriptorBufferInfo inBindingInfo{};
  inBindingInfo.buffer = inputBuffer.handle();
  inBindingInfo.offset = 0;
  inBindingInfo.range = sizeof(inputData);
  VkDescriptorBufferInfo outBindingInfo{};
  outBindingInfo.buffer = outputBuffer.handle();
  outBindingInfo.offset = 0;
  outBindingInfo.range = sizeof(output);
  std::array<VkWriteDescriptorSet, 2> writes{};
  writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
  writes[0].dstSet = descriptorSet;
  writes[0].dstBinding = 0;
  writes[0].descriptorCount = 1;
  writes[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  writes[0].pBufferInfo = &inBindingInfo;
  writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
  writes[1].dstSet = descriptorSet;
  writes[1].dstBinding = 1;
  writes[1].descriptorCount = 1;
  writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  writes[1].pBufferInfo = &outBindingInfo;
  vkUpdateDescriptorSets(device.get(),
                         static_cast<std::uint32_t>(writes.size()),
                         writes.data(), 0, nullptr);

  auto &commandContext = session.commandContext();
  VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
  if (!commandContext.allocatePrimary(commandBuffer, error)) {
    std::cerr << error << '\n';
    return EXIT_FAILURE;
  }

  VkCommandBufferBeginInfo beginInfo{};
  beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
  beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  if (vkBeginCommandBuffer(commandBuffer, &beginInfo) != VK_SUCCESS) {
    std::cerr << "vkBeginCommandBuffer failed." << '\n';
    return EXIT_FAILURE;
  }

  std::array<VkBufferMemoryBarrier, 2> preBarriers{};
  preBarriers[0].sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
  preBarriers[0].srcAccessMask = VK_ACCESS_HOST_WRITE_BIT;
  preBarriers[0].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
  preBarriers[0].buffer = inputBuffer.handle();
  preBarriers[0].offset = 0;
  preBarriers[0].size = sizeof(inputData);
  preBarriers[1].sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
  preBarriers[1].srcAccessMask = VK_ACCESS_HOST_WRITE_BIT;
  preBarriers[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
  preBarriers[1].buffer = outputBuffer.handle();
  preBarriers[1].offset = 0;
  preBarriers[1].size = sizeof(output);

  vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_HOST_BIT,
                       VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 2,
                       preBarriers.data(), 0, nullptr);
  vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE,
                    pipeline.get());
  vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE,
                          pipelineLayout.get(), 0, 1, &descriptorSet, 0,
                          nullptr);
  constexpr PushConstants pushConstants{1.0F};
  vkCmdPushConstants(commandBuffer, pipelineLayout.get(),
                     VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pushConstants),
                     &pushConstants);
  vkCmdDispatch(commandBuffer, 1, 1, 1);

  VkBufferMemoryBarrier postBarrier{};
  postBarrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
  postBarrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
  postBarrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
  postBarrier.buffer = outputBuffer.handle();
  postBarrier.offset = 0;
  postBarrier.size = sizeof(output);
  vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                       VK_PIPELINE_STAGE_HOST_BIT, 0, 0, nullptr, 1,
                       &postBarrier, 0, nullptr);

  if (vkEndCommandBuffer(commandBuffer) != VK_SUCCESS) {
    std::cerr << "vkEndCommandBuffer failed." << '\n';
    return EXIT_FAILURE;
  }

  Fence fence{};
  if (!fence.create(device, error)) {
    std::cerr << error << '\n';
    return EXIT_FAILURE;
  }

  VkSubmitInfo submit{};
  submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
  submit.commandBufferCount = 1;
  submit.pCommandBuffers = &commandBuffer;
  if (vkQueueSubmit(device.computeQueue(), 1, &submit, fence.get()) !=
      VK_SUCCESS) {
    std::cerr << "vkQueueSubmit failed." << '\n';
    return EXIT_FAILURE;
  }
  if (!fence.wait(10'000'000'000ULL, error)) {
    std::cerr << error << '\n';
    return EXIT_FAILURE;
  }

  if (!outputBuffer.read(output.data(), sizeof(output), 0, error)) {
    std::cerr << error << '\n';
    return EXIT_FAILURE;
  }
  bool pass = true;
  for (std::size_t i = 0; i < output.size(); ++i) {
    const bool isMatch = exactlyEqualFloat(output[i], expected[i]);
    const std::uint32_t outBits = std::bit_cast<std::uint32_t>(output[i]);
    const std::uint32_t expectedBits =
        std::bit_cast<std::uint32_t>(expected[i]);
    std::cout << std::fixed << std::setprecision(7)
              << "[SessionSmoke] idx=" << i << " expected=" << expected[i]
              << " actual=" << output[i]
              << " exact=" << (expectedBits == outBits ? "yes" : "no")
              << " ok=" << (isMatch ? "yes" : "no") << '\n';
    pass = pass && isMatch;
  }
  std::cout << "[SessionSmoke] " << (pass ? "PASS" : "FAIL") << '\n';
  return pass ? EXIT_SUCCESS : EXIT_FAILURE;
}
