// Minimal compute smoke using reusable Vulkan runtime wrappers.
//
// Expected output:
//   output[i] = input[i] * 2 + 1, for i = 0..15

#include "vulkan_compute_runtime.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <span>

namespace {

template <typename T>
[[nodiscard]] constexpr std::size_t elementCount(const T &array) {
  return array.size();
}

} // namespace

int main() {
  using namespace viennaps::vulkan::runtime;

  std::string error;
  VulkanInstance instance;
  if (!instance.create(error)) {
    std::cerr << error << '\n';
    return EXIT_FAILURE;
  }

  ComputeDeviceSelection selection{};
  if (!pickFirstComputeDevice(instance.get(), selection, error)) {
    std::cerr << error << '\n';
    return EXIT_FAILURE;
  }
  std::cout << "[RuntimeSmoke] device: " << selection.properties.deviceName
            << '\n';
  std::cout << "[RuntimeSmoke] device type: "
            << vkDeviceTypeName(selection.properties.deviceType) << '\n';
  std::cout << "[RuntimeSmoke] vendorId: 0x" << std::hex
            << selection.properties.vendorID << ", deviceId: 0x"
            << selection.properties.deviceID << std::dec << '\n';
  std::cout << "[RuntimeSmoke] queue family: " << selection.queue.familyIndex
            << ", dedicated: "
            << (selection.queue.dedicatedQueue ? "yes" : "no") << '\n';
  std::cout << "[RuntimeSmoke] dedicatedQueuePreferred: "
            << (selection.queue.dedicatedQueue ? "true" : "false") << '\n';

  VulkanDevice device;
  if (!device.create(selection, error)) {
    std::cerr << error << '\n';
    return EXIT_FAILURE;
  }

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
  if (!pipelineLayout.create(device, descriptorSetLayout.get(), error)) {
    std::cerr << error << '\n';
    return EXIT_FAILURE;
  }

  ComputePipeline pipeline{};
  if (!pipeline.create(device, module, pipelineLayout, error)) {
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

  CommandContext commandContext{};
  if (!commandContext.create(device, device.computeQueueFamily(), error)) {
    std::cerr << error << '\n';
    return EXIT_FAILURE;
  }
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
              << "[RuntimeSmoke] idx=" << i << " expected=" << expected[i]
              << " actual=" << output[i]
              << " exact=" << (expectedBits == outBits ? "yes" : "no")
              << " ok=" << (isMatch ? "yes" : "no") << '\n';
    pass = pass && isMatch;
  }
  std::cout << "[RuntimeSmoke] " << (pass ? "PASS" : "FAIL") << '\n';
  return pass ? EXIT_SUCCESS : EXIT_FAILURE;
}
