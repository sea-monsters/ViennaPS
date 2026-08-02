// Probe utility for Vulkan hardware capability collection.
//
// This utility is intentionally small and conservative:
// - with Vulkan headers/libs present: enumerate devices and core capabilities
// - without Vulkan headers/libs: emit a diagnostic profile that allows CPU-only
//   paths to continue safely
//
// Output is stable JSON text suitable for persistence and later diffing.

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <csignal>
#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#include <compute/probeProfileAdapter.hpp>

#ifdef VIENNAPS_VULKAN_ENABLED
#include <vulkan/vulkan.h>
#ifdef VIENNAPS_VULKAN_LEVELSET_SUITE_AVAILABLE
#include "levelset/hrle_rebuild_classification.hpp"
#include "levelset/hrle_rebuild_compaction.hpp"
#include "levelset/hrle_rebuild_pipeline.hpp"
#include "levelset/levelset_update.hpp"
#include "primitives/reduction_scan_primitives.hpp"
#include "runtime/compute_session.hpp"

#include <hrleSparseStarIterator.hpp>
#include <levelset/psHrleSparseReconstruction.hpp>
#include <lsDomain.hpp>
#include <lsExpand.hpp>
#include <lsMakeGeometry.hpp>
#endif
#endif

namespace {

[[nodiscard]] std::string escapeJson(std::string_view input);

struct StrictFp32SmokeResult {
  viennaps::compute::VulkanFp32NumericalSmokeEvidence evidence{};
  std::string json;
  std::string deviceUuid;
};

[[nodiscard]] std::string strictFp32EvidenceJson(
    const viennaps::compute::VulkanFp32NumericalSmokeEvidence &evidence,
    const std::string_view deviceUuid = {}) {
  const auto status = [&] {
    switch (evidence.status) {
    case viennaps::compute::VulkanNumericalSmokeStatus::PASS:
      return "PASS";
    case viennaps::compute::VulkanNumericalSmokeStatus::FAIL:
      return "FAIL";
    case viennaps::compute::VulkanNumericalSmokeStatus::NOT_RUN:
    default:
      return "NOT_RUN";
    }
  }();
  return std::string("{\"status\":\"") + status + "\",\"contractId\":\"" +
         escapeJson(evidence.contractId) +
         "\",\"caseCount\":" + std::to_string(evidence.caseCount) +
         ",\"mismatchCount\":" + std::to_string(evidence.mismatchCount) +
         ",\"maxUlp\":" + std::to_string(evidence.maxUlp) +
         ",\"watchdogMs\":" + std::to_string(evidence.watchdogMs) +
         ",\"elapsedMs\":" + std::to_string(evidence.elapsedMs) +
         ",\"failureDiagnostic\":\"" + escapeJson(evidence.failureDiagnostic) +
         "\",\"deviceUuid\":\"" + escapeJson(deviceUuid) + "\"}";
}

[[nodiscard]] StrictFp32SmokeResult
makeStrictFp32Result(const viennaps::compute::VulkanNumericalSmokeStatus status,
                     std::string diagnostic, const std::uint32_t cases = 0U,
                     const std::uint32_t mismatches = 0U,
                     const std::uint32_t maxUlp = 0U,
                     const std::uint64_t elapsed = 0ULL) {
  StrictFp32SmokeResult result;
  result.evidence.status = status;
  result.evidence.contractId =
      std::string(viennaps::compute::kVulkanFp32NumericalSmokeContract);
  result.evidence.caseCount = cases;
  result.evidence.mismatchCount = mismatches;
  result.evidence.maxUlp = maxUlp;
  result.evidence.watchdogMs =
      viennaps::compute::kVulkanFp32NumericalSmokeWatchdogMs;
  result.evidence.elapsedMs = elapsed;
  result.evidence.failureDiagnostic = std::move(diagnostic);
  result.json = strictFp32EvidenceJson(result.evidence);
  return result;
}

[[nodiscard]] bool writeStrictFp32Result(
    const std::string &path,
    const viennaps::compute::VulkanFp32NumericalSmokeEvidence &evidence,
    const std::string_view deviceUuid = {}) {
  if (path.empty())
    return false;
  const auto payload = strictFp32EvidenceJson(evidence, deviceUuid) + "\n";
#if defined(_WIN32)
  if (payload.size() > std::numeric_limits<DWORD>::max())
    return false;
  const auto outputPath = std::filesystem::path(path).wstring();
  const auto handle =
      CreateFileW(outputPath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                  FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_WRITE_THROUGH, nullptr);
  if (handle == INVALID_HANDLE_VALUE)
    return false;
  DWORD written = 0U;
  const auto completed =
      WriteFile(handle, payload.data(), static_cast<DWORD>(payload.size()),
                &written, nullptr) != FALSE &&
      written == payload.size();
  CloseHandle(handle);
  if (!completed)
    DeleteFileW(outputPath.c_str());
  return completed;
#else
  const auto descriptor =
      open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW,
           S_IRUSR | S_IWUSR);
  if (descriptor < 0)
    return false;
  std::size_t offset = 0U;
  bool completed = true;
  while (offset < payload.size()) {
    const auto written =
        write(descriptor, payload.data() + offset, payload.size() - offset);
    if (written <= 0) {
      completed = false;
      break;
    }
    offset += static_cast<std::size_t>(written);
  }
  close(descriptor);
  if (!completed)
    unlink(path.c_str());
  return completed;
#endif
}

[[nodiscard]] StrictFp32SmokeResult
runStrictFp32Child(const bool forceFailure,
                   const std::uint32_t requestedDeviceIndex = 0U) {
  const auto start = std::chrono::steady_clock::now();
#if !defined(VIENNAPS_VULKAN_ENABLED) ||                                       \
    !defined(VIENNAPS_VULKAN_FP32_BASELINE_SPV_PATH)
  (void)requestedDeviceIndex;
#endif
  if (forceFailure) {
    return makeStrictFp32Result(
        viennaps::compute::VulkanNumericalSmokeStatus::FAIL,
        "forced strict-FP32 child failure", 2U, 1U, 1U);
  }
  // These CPU cases are deliberately ordered and bitwise, including signed
  // zero. They guard the oracle independently of device advertised features.
  std::uint32_t cpuMismatches = 0U;
#if defined(VIENNAPS_VULKAN_ENABLED) &&                                        \
    defined(VIENNAPS_VULKAN_FP32_BASELINE_SPV_PATH)
  std::uint32_t deviceMismatches = 0U;
  std::ostringstream deviceDiagnostic;
#endif
  const float negativeZero = std::bit_cast<float>(0x80000000U);
  if (std::bit_cast<std::uint32_t>(negativeZero) != 0x80000000U)
    ++cpuMismatches;
  volatile float left = 1.0e20F;
  volatile float middle = -1.0e20F;
  volatile float right = 3.0F;
  const float ordered = (left + middle) + right;
  const float orderedExpected = 3.0F;
  if (std::bit_cast<std::uint32_t>(ordered) !=
      std::bit_cast<std::uint32_t>(orderedExpected))
    ++cpuMismatches;

  std::string selectedUuid;
#if defined(VIENNAPS_VULKAN_ENABLED) &&                                        \
    defined(VIENNAPS_VULKAN_FP32_BASELINE_SPV_PATH)
  // Dispatch the dedicated non-experimental FP32 numerical contract and
  // compare every returned FP32 word with the host oracle.
  auto readSpirv = [](const char *path) {
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input.good())
      return std::vector<std::uint32_t>{};
    const auto size = input.tellg();
    if (size <= 0 ||
        size % static_cast<std::streamoff>(sizeof(std::uint32_t)) != 0)
      return std::vector<std::uint32_t>{};
    std::vector<std::uint32_t> code(static_cast<std::size_t>(size) /
                                    sizeof(std::uint32_t));
    input.seekg(0);
    input.read(reinterpret_cast<char *>(code.data()),
               static_cast<std::streamsize>(size));
    return input.good() ? code : std::vector<std::uint32_t>{};
  };
  const auto spirv = readSpirv(VIENNAPS_VULKAN_FP32_BASELINE_SPV_PATH);
  if (spirv.empty())
    return makeStrictFp32Result(
        viennaps::compute::VulkanNumericalSmokeStatus::FAIL,
        "strict FP32 SPIR-V artifact unavailable", 18U, 1U, 1U);
  VkApplicationInfo appInfo{};
  appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
  appInfo.pApplicationName = "ViennaPS strict FP32 numerical smoke";
  appInfo.apiVersion = VK_API_VERSION_1_2;
  VkInstanceCreateInfo instanceInfo{};
  instanceInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
  instanceInfo.pApplicationInfo = &appInfo;
  VkInstance instance = VK_NULL_HANDLE;
  if (vkCreateInstance(&instanceInfo, nullptr, &instance) != VK_SUCCESS)
    return makeStrictFp32Result(
        viennaps::compute::VulkanNumericalSmokeStatus::FAIL,
        "vkCreateInstance failed", 2U, 1U, 1U);
  std::uint32_t deviceCount = 0U;
  if (vkEnumeratePhysicalDevices(instance, &deviceCount, nullptr) !=
          VK_SUCCESS ||
      deviceCount == 0U) {
    vkDestroyInstance(instance, nullptr);
    return makeStrictFp32Result(
        viennaps::compute::VulkanNumericalSmokeStatus::FAIL,
        "no Vulkan physical device found", 2U, 1U, 1U);
  }
  std::vector<VkPhysicalDevice> devices(deviceCount);
  if (vkEnumeratePhysicalDevices(instance, &deviceCount, devices.data()) !=
          VK_SUCCESS ||
      deviceCount == 0U) {
    vkDestroyInstance(instance, nullptr);
    return makeStrictFp32Result(
        viennaps::compute::VulkanNumericalSmokeStatus::FAIL,
        "Vulkan physical-device enumeration changed during strict smoke", 18U,
        1U, 1U);
  }
  devices.resize(deviceCount);
  VkPhysicalDevice physical = VK_NULL_HANDLE;
  std::uint32_t queueFamily = 0U;
  if (requestedDeviceIndex >= devices.size()) {
    vkDestroyInstance(instance, nullptr);
    return makeStrictFp32Result(
        viennaps::compute::VulkanNumericalSmokeStatus::FAIL,
        "requested Vulkan device index is unavailable", 18U, 1U, 1U);
  }
  for (std::uint32_t deviceIndex = 0U; deviceIndex < devices.size();
       ++deviceIndex) {
    if (deviceIndex != requestedDeviceIndex)
      continue;
    const auto candidate = devices[deviceIndex];
    std::uint32_t count = 0U;
    vkGetPhysicalDeviceQueueFamilyProperties(candidate, &count, nullptr);
    std::vector<VkQueueFamilyProperties> queues(count);
    vkGetPhysicalDeviceQueueFamilyProperties(candidate, &count, queues.data());
    for (std::uint32_t i = 0U; i < count; ++i) {
      if ((queues[i].queueFlags & VK_QUEUE_COMPUTE_BIT) != 0U) {
        physical = candidate;
        queueFamily = i;
        break;
      }
    }
    if (physical != VK_NULL_HANDLE)
      break;
  }
  if (physical == VK_NULL_HANDLE) {
    vkDestroyInstance(instance, nullptr);
    return makeStrictFp32Result(
        viennaps::compute::VulkanNumericalSmokeStatus::FAIL,
        "no compute queue available", 18U, 1U, 1U);
  }
  {
    VkPhysicalDeviceProperties2 properties2{};
    VkPhysicalDeviceIDProperties idProperties{};
    properties2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
    idProperties.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES;
    properties2.pNext = &idProperties;
    vkGetPhysicalDeviceProperties2(physical, &properties2);
    std::ostringstream uuid;
    uuid << std::hex << std::setfill('0');
    for (const auto byte : idProperties.deviceUUID)
      uuid << std::setw(2) << static_cast<unsigned>(byte);
    selectedUuid = uuid.str();
  }
  float priority = 1.0F;
  VkDeviceQueueCreateInfo queueInfo{};
  queueInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
  queueInfo.queueFamilyIndex = queueFamily;
  queueInfo.queueCount = 1U;
  queueInfo.pQueuePriorities = &priority;
  VkDeviceCreateInfo deviceInfo{};
  deviceInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
  deviceInfo.queueCreateInfoCount = 1U;
  deviceInfo.pQueueCreateInfos = &queueInfo;
  VkDevice device = VK_NULL_HANDLE;
  if (vkCreateDevice(physical, &deviceInfo, nullptr, &device) != VK_SUCCESS) {
    vkDestroyInstance(instance, nullptr);
    return makeStrictFp32Result(
        viennaps::compute::VulkanNumericalSmokeStatus::FAIL,
        "vkCreateDevice failed", 18U, 1U, 1U);
  }
  VkQueue queue = VK_NULL_HANDLE;
  vkGetDeviceQueue(device, queueFamily, 0U, &queue);
  VkPhysicalDeviceMemoryProperties memories{};
  vkGetPhysicalDeviceMemoryProperties(physical, &memories);
  const auto memoryType = [&](const VkMemoryRequirements &requirements) {
    for (std::uint32_t i = 0U; i < memories.memoryTypeCount; ++i)
      if ((requirements.memoryTypeBits & (1U << i)) != 0U &&
          (memories.memoryTypes[i].propertyFlags &
           (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
            VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) ==
              (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
               VK_MEMORY_PROPERTY_HOST_COHERENT_BIT))
        return i;
    return std::numeric_limits<std::uint32_t>::max();
  };
  constexpr std::size_t kCount = 16U;
  std::array<float, kCount> input{};
  std::array<float, kCount> expected{};
  std::array<std::uint32_t, kCount> output{};
  input[0] = std::bit_cast<float>(0x80000000U);
  input[1] = 1.0e20F;
  input[2] = -1.0e20F;
  input[3] = 3.0F;
  for (std::size_t i = 4U; i < kCount; ++i)
    input[i] = static_cast<float>(i) - 4.0F;
  expected[0] = input[0];
  expected[1] = (input[1] + input[2]) + input[3];
  for (std::size_t i = 2U; i < kCount; ++i)
    expected[i] = input[i] * 2.0F + 1.0F;
  VkBuffer inputBuffer = VK_NULL_HANDLE, outputBuffer = VK_NULL_HANDLE;
  VkDeviceMemory inputMemory = VK_NULL_HANDLE, outputMemory = VK_NULL_HANDLE;
  VkShaderModule shader = VK_NULL_HANDLE;
  VkDescriptorSetLayout setLayout = VK_NULL_HANDLE;
  VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
  VkPipeline pipeline = VK_NULL_HANDLE;
  VkDescriptorPool descriptorPool = VK_NULL_HANDLE;
  VkDescriptorSet descriptorSet = VK_NULL_HANDLE;
  VkCommandPool commandPool = VK_NULL_HANDLE;
  VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
  VkFence fence = VK_NULL_HANDLE;
  auto cleanup = [&] {
    if (device != VK_NULL_HANDLE)
      vkDeviceWaitIdle(device);
    if (fence != VK_NULL_HANDLE)
      vkDestroyFence(device, fence, nullptr);
    if (commandPool != VK_NULL_HANDLE)
      vkDestroyCommandPool(device, commandPool, nullptr);
    if (descriptorPool != VK_NULL_HANDLE)
      vkDestroyDescriptorPool(device, descriptorPool, nullptr);
    if (pipeline != VK_NULL_HANDLE)
      vkDestroyPipeline(device, pipeline, nullptr);
    if (pipelineLayout != VK_NULL_HANDLE)
      vkDestroyPipelineLayout(device, pipelineLayout, nullptr);
    if (setLayout != VK_NULL_HANDLE)
      vkDestroyDescriptorSetLayout(device, setLayout, nullptr);
    if (shader != VK_NULL_HANDLE)
      vkDestroyShaderModule(device, shader, nullptr);
    if (inputBuffer != VK_NULL_HANDLE)
      vkDestroyBuffer(device, inputBuffer, nullptr);
    if (outputBuffer != VK_NULL_HANDLE)
      vkDestroyBuffer(device, outputBuffer, nullptr);
    if (inputMemory != VK_NULL_HANDLE)
      vkFreeMemory(device, inputMemory, nullptr);
    if (outputMemory != VK_NULL_HANDLE)
      vkFreeMemory(device, outputMemory, nullptr);
    if (device != VK_NULL_HANDLE)
      vkDestroyDevice(device, nullptr);
    if (instance != VK_NULL_HANDLE)
      vkDestroyInstance(instance, nullptr);
  };
  auto makeBuffer = [&](VkBufferUsageFlags usage, VkBuffer &buffer,
                        VkDeviceMemory &memory) {
    VkBufferCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    info.size = sizeof(input);
    info.usage = usage;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateBuffer(device, &info, nullptr, &buffer) != VK_SUCCESS)
      return false;
    VkMemoryRequirements requirements{};
    vkGetBufferMemoryRequirements(device, buffer, &requirements);
    const auto type = memoryType(requirements);
    if (type == std::numeric_limits<std::uint32_t>::max())
      return false;
    VkMemoryAllocateInfo allocation{};
    allocation.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocation.allocationSize = requirements.size;
    allocation.memoryTypeIndex = type;
    if (vkAllocateMemory(device, &allocation, nullptr, &memory) != VK_SUCCESS)
      return false;
    return vkBindBufferMemory(device, buffer, memory, 0U) == VK_SUCCESS;
  };
  if (!makeBuffer(VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, inputBuffer,
                  inputMemory) ||
      !makeBuffer(VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, outputBuffer,
                  outputMemory)) {
    cleanup();
    return makeStrictFp32Result(
        viennaps::compute::VulkanNumericalSmokeStatus::FAIL,
        "baseline buffer allocation failed", 18U, 1U, 1U);
  }
  void *inputPtr = nullptr;
  void *outputPtr = nullptr;
  if (vkMapMemory(device, inputMemory, 0U, sizeof(input), 0U, &inputPtr) !=
          VK_SUCCESS ||
      vkMapMemory(device, outputMemory, 0U, sizeof(output), 0U, &outputPtr) !=
          VK_SUCCESS) {
    cleanup();
    return makeStrictFp32Result(
        viennaps::compute::VulkanNumericalSmokeStatus::FAIL,
        "baseline memory map failed", 18U, 1U, 1U);
  }
  std::memcpy(inputPtr, input.data(), sizeof(input));
  std::memset(outputPtr, 0, sizeof(output));
  vkUnmapMemory(device, inputMemory);
  vkUnmapMemory(device, outputMemory);
  VkShaderModuleCreateInfo shaderInfo{};
  shaderInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
  shaderInfo.codeSize = spirv.size() * sizeof(std::uint32_t);
  shaderInfo.pCode = spirv.data();
  if (vkCreateShaderModule(device, &shaderInfo, nullptr, &shader) !=
      VK_SUCCESS) {
    cleanup();
    return makeStrictFp32Result(
        viennaps::compute::VulkanNumericalSmokeStatus::FAIL,
        "baseline shader module creation failed", 18U, 1U, 1U);
  }
  VkDescriptorSetLayoutBinding bindings[2]{};
  for (std::uint32_t i = 0U; i < 2U; ++i) {
    bindings[i].binding = i;
    bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    bindings[i].descriptorCount = 1U;
    bindings[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
  }
  VkDescriptorSetLayoutCreateInfo layoutInfo{};
  layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
  layoutInfo.bindingCount = 2U;
  layoutInfo.pBindings = bindings;
  if (vkCreateDescriptorSetLayout(device, &layoutInfo, nullptr, &setLayout) !=
      VK_SUCCESS) {
    cleanup();
    return makeStrictFp32Result(
        viennaps::compute::VulkanNumericalSmokeStatus::FAIL,
        "baseline descriptor layout failed", 18U, 1U, 1U);
  }
  VkPipelineLayoutCreateInfo pipelineLayoutInfo{};
  pipelineLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
  pipelineLayoutInfo.setLayoutCount = 1U;
  pipelineLayoutInfo.pSetLayouts = &setLayout;
  if (vkCreatePipelineLayout(device, &pipelineLayoutInfo, nullptr,
                             &pipelineLayout) != VK_SUCCESS) {
    cleanup();
    return makeStrictFp32Result(
        viennaps::compute::VulkanNumericalSmokeStatus::FAIL,
        "baseline pipeline layout failed", 18U, 1U, 1U);
  }
  VkPipelineShaderStageCreateInfo stage{};
  stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
  stage.module = shader;
  stage.pName = "main";
  VkComputePipelineCreateInfo pipelineInfo{};
  pipelineInfo.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
  pipelineInfo.stage = stage;
  pipelineInfo.layout = pipelineLayout;
  if (vkCreateComputePipelines(device, VK_NULL_HANDLE, 1U, &pipelineInfo,
                               nullptr, &pipeline) != VK_SUCCESS) {
    cleanup();
    return makeStrictFp32Result(
        viennaps::compute::VulkanNumericalSmokeStatus::FAIL,
        "baseline compute pipeline failed", 18U, 1U, 1U);
  }
  VkDescriptorPoolSize poolSize{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2U};
  VkDescriptorPoolCreateInfo poolInfo{};
  poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
  poolInfo.poolSizeCount = 1U;
  poolInfo.pPoolSizes = &poolSize;
  poolInfo.maxSets = 1U;
  if (vkCreateDescriptorPool(device, &poolInfo, nullptr, &descriptorPool) !=
      VK_SUCCESS) {
    cleanup();
    return makeStrictFp32Result(
        viennaps::compute::VulkanNumericalSmokeStatus::FAIL,
        "baseline descriptor pool failed", 18U, 1U, 1U);
  }
  VkDescriptorSetAllocateInfo setAlloc{};
  setAlloc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
  setAlloc.descriptorPool = descriptorPool;
  setAlloc.descriptorSetCount = 1U;
  setAlloc.pSetLayouts = &setLayout;
  if (vkAllocateDescriptorSets(device, &setAlloc, &descriptorSet) !=
      VK_SUCCESS) {
    cleanup();
    return makeStrictFp32Result(
        viennaps::compute::VulkanNumericalSmokeStatus::FAIL,
        "baseline descriptor allocation failed", 18U, 1U, 1U);
  }
  VkDescriptorBufferInfo inInfo{inputBuffer, 0U, sizeof(input)};
  VkDescriptorBufferInfo outInfo{outputBuffer, 0U, sizeof(output)};
  VkWriteDescriptorSet writes[2]{};
  for (std::uint32_t i = 0U; i < 2U; ++i) {
    writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[i].dstSet = descriptorSet;
    writes[i].dstBinding = i;
    writes[i].descriptorCount = 1U;
    writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    writes[i].pBufferInfo = i == 0U ? &inInfo : &outInfo;
  }
  vkUpdateDescriptorSets(device, 2U, writes, 0U, nullptr);
  VkCommandPoolCreateInfo commandPoolInfo{};
  commandPoolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
  commandPoolInfo.queueFamilyIndex = queueFamily;
  if (vkCreateCommandPool(device, &commandPoolInfo, nullptr, &commandPool) !=
      VK_SUCCESS) {
    cleanup();
    return makeStrictFp32Result(
        viennaps::compute::VulkanNumericalSmokeStatus::FAIL,
        "baseline command pool failed", 18U, 1U, 1U);
  }
  VkCommandBufferAllocateInfo commandAlloc{};
  commandAlloc.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
  commandAlloc.commandPool = commandPool;
  commandAlloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  commandAlloc.commandBufferCount = 1U;
  if (vkAllocateCommandBuffers(device, &commandAlloc, &commandBuffer) !=
      VK_SUCCESS) {
    cleanup();
    return makeStrictFp32Result(
        viennaps::compute::VulkanNumericalSmokeStatus::FAIL,
        "baseline command buffer failed", 18U, 1U, 1U);
  }
  VkCommandBufferBeginInfo begin{};
  begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
  begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  vkBeginCommandBuffer(commandBuffer, &begin);
  VkBufferMemoryBarrier hostToDevice[2]{};
  for (std::uint32_t i = 0U; i < 2U; ++i) {
    hostToDevice[i].sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
    hostToDevice[i].srcAccessMask = VK_ACCESS_HOST_WRITE_BIT;
    hostToDevice[i].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    hostToDevice[i].buffer = i == 0U ? inputBuffer : outputBuffer;
    hostToDevice[i].size = sizeof(input);
  }
  vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_HOST_BIT,
                       VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0U, 0U, nullptr,
                       2U, hostToDevice, 0U, nullptr);
  vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
  vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE,
                          pipelineLayout, 0U, 1U, &descriptorSet, 0U, nullptr);
  vkCmdDispatch(commandBuffer, 1U, 1U, 1U);
  VkBufferMemoryBarrier deviceToHost{};
  deviceToHost.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
  deviceToHost.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
  deviceToHost.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
  deviceToHost.buffer = outputBuffer;
  deviceToHost.size = sizeof(output);
  vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                       VK_PIPELINE_STAGE_HOST_BIT, 0U, 0U, nullptr, 1U,
                       &deviceToHost, 0U, nullptr);
  vkEndCommandBuffer(commandBuffer);
  VkFenceCreateInfo fenceInfo{};
  fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
  VkSubmitInfo submitInfo{};
  submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
  submitInfo.commandBufferCount = 1U;
  submitInfo.pCommandBuffers = &commandBuffer;
  if (vkCreateFence(device, &fenceInfo, nullptr, &fence) != VK_SUCCESS ||
      vkQueueSubmit(queue, 1U, &submitInfo, fence) != VK_SUCCESS ||
      vkWaitForFences(device, 1U, &fence, VK_TRUE, 10'000'000'000ULL) !=
          VK_SUCCESS) {
    cleanup();
    return makeStrictFp32Result(
        viennaps::compute::VulkanNumericalSmokeStatus::FAIL,
        "baseline queue submission/fence failed", 18U, 1U, 1U);
  }
  if (vkMapMemory(device, outputMemory, 0U, sizeof(output), 0U, &outputPtr) !=
      VK_SUCCESS) {
    cleanup();
    return makeStrictFp32Result(
        viennaps::compute::VulkanNumericalSmokeStatus::FAIL,
        "baseline output map failed", 18U, 1U, 1U);
  }
  std::memcpy(output.data(), outputPtr, sizeof(output));
  vkUnmapMemory(device, outputMemory);
  for (std::size_t i = 0U; i < kCount; ++i) {
    if (output[i] == std::bit_cast<std::uint32_t>(expected[i]))
      continue;
    if (deviceMismatches++ == 0U)
      deviceDiagnostic << "Vulkan bitwise mismatch at case";
    deviceDiagnostic << ' ' << i;
  }
  cleanup();
#else
  const auto elapsed = static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::steady_clock::now() - start)
          .count());
  return makeStrictFp32Result(
      viennaps::compute::VulkanNumericalSmokeStatus::FAIL,
      "Vulkan strict FP32 smoke unavailable: strict shader artifacts are not "
      "built",
      2U, 1U, 1U, elapsed);
#endif

#if defined(VIENNAPS_VULKAN_ENABLED) &&                                        \
    defined(VIENNAPS_VULKAN_FP32_BASELINE_SPV_PATH)
  const auto elapsed = static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::steady_clock::now() - start)
          .count());
  const auto mismatches = cpuMismatches + deviceMismatches;
  std::string diagnostic;
  if (cpuMismatches != 0U)
    diagnostic = "CPU bitwise oracle mismatch";
  else if (deviceMismatches != 0U)
    diagnostic = deviceDiagnostic.str();
  auto result = makeStrictFp32Result(
      mismatches == 0U ? viennaps::compute::VulkanNumericalSmokeStatus::PASS
                       : viennaps::compute::VulkanNumericalSmokeStatus::FAIL,
      std::move(diagnostic), 18U, mismatches, mismatches == 0U ? 0U : 1U,
      elapsed);
  result.deviceUuid = selectedUuid;
  result.json = strictFp32EvidenceJson(result.evidence, result.deviceUuid);
  return result;
#endif
}

[[nodiscard]] bool
parseStrictFp32Result(const std::string &json,
                      viennaps::compute::VulkanFp32NumericalSmokeEvidence &out,
                      std::string &deviceUuid) {
  std::size_t first = 0U;
  while (first < json.size() && (json[first] == ' ' || json[first] == '\t' ||
                                 json[first] == '\r' || json[first] == '\n'))
    ++first;
  std::size_t last = json.size();
  while (last > first && (json[last - 1U] == ' ' || json[last - 1U] == '\t' ||
                          json[last - 1U] == '\r' || json[last - 1U] == '\n'))
    --last;
  if (last - first < 2U || json[first] != '{' || json[last - 1U] != '}')
    return false;
  const std::string normalized = json.substr(first, last - first);
  const std::string &inputJson = normalized;
  const auto isKnownKey = [](const std::string_view key) {
    for (const auto candidate :
         {"status", "contractId", "caseCount", "mismatchCount", "maxUlp",
          "watchdogMs", "elapsedMs", "failureDiagnostic", "deviceUuid"})
      if (key == candidate)
        return true;
    return false;
  };
  for (std::size_t cursor = 0U; cursor < inputJson.size();) {
    if (inputJson[cursor] != '"') {
      ++cursor;
      continue;
    }
    const auto keyStart = ++cursor;
    while (cursor < inputJson.size() && inputJson[cursor] != '"') {
      if (inputJson[cursor] == '\\' && cursor + 1U < inputJson.size())
        cursor += 2U;
      else
        ++cursor;
    }
    if (cursor >= inputJson.size())
      return false;
    const auto key = inputJson.substr(keyStart, cursor - keyStart);
    ++cursor;
    while (cursor < inputJson.size() &&
           (inputJson[cursor] == ' ' || inputJson[cursor] == '\t' ||
            inputJson[cursor] == '\r' || inputJson[cursor] == '\n'))
      ++cursor;
    if (cursor < inputJson.size() && inputJson[cursor] == ':' &&
        !isKnownKey(key))
      return false;
  }
  const auto countKey = [&](const std::string_view key) {
    const auto marker = std::string("\"") + std::string(key) + "\":";
    std::size_t count = 0U;
    std::size_t offset = 0U;
    while ((offset = inputJson.find(marker, offset)) != std::string::npos) {
      ++count;
      offset += marker.size();
    }
    return count;
  };
  for (const auto key :
       {"status", "contractId", "caseCount", "mismatchCount", "maxUlp",
        "watchdogMs", "elapsedMs", "failureDiagnostic"})
    if (countKey(key) != 1U)
      return false;
  if (countKey("deviceUuid") != 1U)
    return false;
  const auto findString = [&](const std::string_view key,
                              std::string &value) -> bool {
    const auto marker = std::string("\"") + std::string(key) + "\":\"";
    const auto begin = inputJson.find(marker);
    if (begin == std::string::npos)
      return false;
    const auto start = begin + marker.size();
    const auto end = inputJson.find('"', start);
    if (end == std::string::npos)
      return false;
    value = inputJson.substr(start, end - start);
    return true;
  };
  const auto findUint = [&](const std::string_view key,
                            std::uint64_t &value) -> bool {
    const auto marker = std::string("\"") + std::string(key) + "\":";
    const auto begin = inputJson.find(marker);
    if (begin == std::string::npos)
      return false;
    const auto start = begin + marker.size();
    std::size_t end = start;
    while (end < inputJson.size() && inputJson[end] >= '0' &&
           inputJson[end] <= '9')
      ++end;
    if (end == start || (end < inputJson.size() && inputJson[end] != ',' &&
                         inputJson[end] != '}'))
      return false;
    try {
      std::size_t consumed = 0U;
      value = std::stoull(inputJson.substr(start, end - start), &consumed);
      if (consumed != end - start)
        return false;
    } catch (...) {
      return false;
    }
    return true;
  };
  std::string status;
  if (!findString("status", status) ||
      (status != "PASS" && status != "FAIL" && status != "NOT_RUN") ||
      !findString("contractId", out.contractId) ||
      out.contractId != viennaps::compute::kVulkanFp32NumericalSmokeContract ||
      !findString("failureDiagnostic", out.failureDiagnostic))
    return false;
  if (!findString("deviceUuid", deviceUuid) || deviceUuid.empty())
    return false;
  std::uint64_t value = 0ULL;
  if (!findUint("caseCount", value) ||
      value > std::numeric_limits<std::uint32_t>::max())
    return false;
  out.caseCount = static_cast<std::uint32_t>(value);
  if (!findUint("mismatchCount", value) ||
      value > std::numeric_limits<std::uint32_t>::max())
    return false;
  out.mismatchCount = static_cast<std::uint32_t>(value);
  if (!findUint("maxUlp", value) ||
      value > std::numeric_limits<std::uint32_t>::max())
    return false;
  out.maxUlp = static_cast<std::uint32_t>(value);
  if (!findUint("watchdogMs", out.watchdogMs) ||
      !findUint("elapsedMs", out.elapsedMs))
    return false;
  out.status = status == "PASS"
                   ? viennaps::compute::VulkanNumericalSmokeStatus::PASS
               : status == "FAIL"
                   ? viennaps::compute::VulkanNumericalSmokeStatus::FAIL
                   : viennaps::compute::VulkanNumericalSmokeStatus::NOT_RUN;
  return true;
}

using viennaps::compute::CapabilityProfileRecord;

struct ArgView {
  bool writeProfile = false;
  bool writeDeploymentProfile = false;
  bool validateProfile = false;
  std::string profilePath;
  std::string deploymentProfilePath;
  bool strictFp32Smoke = false;
  bool strictFp32Child = false;
  bool strictFp32ForceFailure = false;
  bool strictFp32DeviceIndexValid = true;
  std::uint32_t strictFp32DeviceIndex = 0U;
  std::string strictFp32ChildOutput;
};

struct ProbeResult {
  std::string rawSummary;
  std::vector<CapabilityProfileRecord> deploymentProfiles;
};

[[nodiscard]] std::string escapeJson(std::string_view input) {
  std::string out;
  out.reserve(input.size() + 16);
  for (char ch : input) {
    switch (ch) {
    case '\"':
      out += "\\\"";
      break;
    case '\\':
      out += "\\\\";
      break;
    case '\b':
      out += "\\b";
      break;
    case '\f':
      out += "\\f";
      break;
    case '\n':
      out += "\\n";
      break;
    case '\r':
      out += "\\r";
      break;
    case '\t':
      out += "\\t";
      break;
    default:
      if (static_cast<unsigned char>(ch) < 0x20) {
        std::ostringstream s;
        s << "\\u" << std::hex << std::setw(4) << std::setfill('0')
          << static_cast<int>(static_cast<unsigned char>(ch));
        out += s.str();
      } else {
        out += ch;
      }
    }
  }
  return out;
}

[[nodiscard]] std::string nowUtcTimestamp() {
  const auto now = std::chrono::system_clock::now();
  const auto utc_time = std::chrono::floor<std::chrono::seconds>(now);
  const auto now_c = std::chrono::system_clock::to_time_t(utc_time);
  std::tm utc_tm{};
#if defined(_WIN32)
  gmtime_s(&utc_tm, &now_c);
#else
  gmtime_r(&now_c, &utc_tm);
#endif

  std::ostringstream out;
  out << std::put_time(&utc_tm, "%Y-%m-%dT%H:%M:%SZ");
  return out.str();
}

[[nodiscard]] std::string boolToJson(const bool value) {
  return value ? "true" : "false";
}

[[nodiscard]] std::string jsonSize3(const std::array<std::uint32_t, 3> &value) {
  return "[" + std::to_string(value[0]) + "," + std::to_string(value[1]) + "," +
         std::to_string(value[2]) + "]";
}

[[nodiscard]] std::string
jsonNumberList(const std::vector<std::uint32_t> &value) {
  if (value.empty())
    return "[]";

  std::ostringstream out;
  out << '[';
  for (std::size_t i = 0; i < value.size(); ++i) {
    if (i != 0)
      out << ',';
    out << value[i];
  }
  out << ']';
  return out.str();
}

[[nodiscard]] std::string bytesToHex(const std::uint8_t *data,
                                     const std::size_t size) {
  std::ostringstream out;
  out << std::hex << std::setfill('0');
  for (std::size_t i = 0; i < size; ++i) {
    out << std::setw(2) << static_cast<int>(data[i]);
  }
  return out.str();
}

[[nodiscard]] bool hasExtension(const std::vector<std::string> &extensions,
                                std::string_view candidate) {
  return std::find(extensions.begin(), extensions.end(),
                   std::string(candidate)) != extensions.end();
}

struct ValidationSuiteResult {
  bool pass = false;
  std::string reason;
};

#ifdef VIENNAPS_VULKAN_LEVELSET_SUITE_AVAILABLE

using ProbeCandidate = viennaps::levelset::HrleRebuildCandidateFp32;
using ProbeDecision = viennaps::levelset::HrleRebuildDecisionFp32;

[[nodiscard]] std::vector<float>
cpuUpdate(const viennaps::vulkan::levelset::LevelSetUpdateInput &input) {
  std::vector<float> output(input.values.begin(), input.values.end());
  for (std::size_t point = 0; point < output.size(); ++point) {
    if (std::abs(output[point]) > input.integrationCutoff) {
      continue;
    }
    std::uint32_t rateIndex = input.rateOffsets[point];
    float remainingTime = input.timeStep;
    float gradient = input.gradients[rateIndex];
    float velocity = gradient - input.dissipations[rateIndex];
    if ((input.checkDissipation && gradient < 0.0F && velocity > 0.0F) ||
        (gradient > 0.0F && velocity < 0.0F)) {
      velocity = 0.0F;
    }
    float rate = remainingTime * velocity;
    while (std::abs(input.stopValues[rateIndex] - output[point]) <
           std::abs(rate)) {
      remainingTime -=
          std::abs((input.stopValues[rateIndex] - output[point]) / velocity);
      output[point] = input.stopValues[rateIndex++];
      gradient = input.gradients[rateIndex];
      velocity = gradient - input.dissipations[rateIndex];
      if ((input.checkDissipation && gradient < 0.0F && velocity > 0.0F) ||
          (gradient > 0.0F && velocity < 0.0F)) {
        velocity = 0.0F;
      }
      rate = remainingTime * velocity;
    }
    output[point] -= rate;
  }
  return output;
}

template <class Iterator>
[[nodiscard]] ProbeCandidate makeProbeCandidate(const Iterator &iterator) {
  ProbeCandidate candidate{};
  const auto &center = iterator.getCenter();
  candidate.centerValue = center.getValue();
  candidate.centerDefinedValue =
      center.isDefined() ? center.getDefinedValue() : center.getValue();
  candidate.centerPointId =
      center.isDefined() ? static_cast<std::uint32_t>(center.getPointId())
                         : viennaps::levelset::kInvalidHrlePointId;
  for (std::size_t neighbor = 0U; neighbor < 4U; ++neighbor) {
    const auto &value =
        iterator.getNeighbor(static_cast<unsigned int>(neighbor));
    candidate.neighborValues[neighbor] = value.getValue();
    candidate.neighborDefinedValues[neighbor] =
        value.isDefined() ? value.getDefinedValue() : value.getValue();
    candidate.neighborPointIds[neighbor] =
        value.isDefined() ? static_cast<std::uint32_t>(value.getPointId())
                          : viennaps::levelset::kInvalidHrlePointId;
  }
  return candidate;
}

[[nodiscard]] viennals::SmartPointer<viennals::Domain<float, 2>>
makeProbeDomain() {
  constexpr viennahrle::CoordType extent = 8.0;
  constexpr viennahrle::CoordType gridDelta = 0.5;
  viennahrle::CoordType bounds[4] = {-extent, extent, -extent, extent};
  viennals::BoundaryConditionEnum boundaries[2] = {
      viennals::BoundaryConditionEnum::REFLECTIVE_BOUNDARY,
      viennals::BoundaryConditionEnum::REFLECTIVE_BOUNDARY};
  auto domain = viennals::Domain<float, 2>::New(bounds, boundaries, gridDelta);
  float origin[2] = {0.0F, 0.0F};
  viennals::MakeGeometry<float, 2>(
      domain,
      viennals::SmartPointer<viennals::Sphere<float, 2>>::New(origin, 3.0F))
      .apply();
  viennals::Expand<float, 2>(domain, 2).apply();
  return domain;
}

[[nodiscard]] bool exactFloatVectors(const std::span<const float> actual,
                                     const std::span<const float> expected) {
  if (actual.size() != expected.size()) {
    return false;
  }
  for (std::size_t index = 0; index < actual.size(); ++index) {
    if (std::bit_cast<std::uint32_t>(actual[index]) !=
        std::bit_cast<std::uint32_t>(expected[index])) {
      return false;
    }
  }
  return true;
}

[[nodiscard]] ValidationSuiteResult
runLevelSetValidationSuite(const std::uint32_t deviceIndex) {
  using namespace viennaps::vulkan;
  using namespace viennaps::vulkan::runtime;
  std::string error;
  if (std::getenv("VIENNAPS_VULKAN_PROBE_FORCE_SUITE_FAIL") != nullptr) {
    return {false, "validation suite forced to fail by deployment test hook"};
  }
  ComputeSession session{};
  ComputeSessionOptions options{};
  options.manualDeviceIndex = deviceIndex;
  if (!session.initialize(error, options)) {
    return {false, "compute session initialization failed: " + error};
  }

  SpirvProgram updateProgram{};
  SpirvProgram classificationProgram{};
  SpirvProgram actionFlagsProgram{};
  SpirvProgram compactProgram{};
  if (!readSpirv(VIENNAPS_LEVELSET_UPDATE_SPV_PATH, updateProgram, error) ||
      !readSpirv(VIENNAPS_HRLE_CLASSIFICATION_SPV_PATH, classificationProgram,
                 error) ||
      !readSpirv(VIENNAPS_HRLE_ACTION_FLAGS_SPV_PATH, actionFlagsProgram,
                 error) ||
      !readSpirv(VIENNAPS_HRLE_COMPACT_SPV_PATH, compactProgram, error)) {
    return {false, "level-set validation shader load failed: " + error};
  }

  const std::array<float, 4> values{0.25F, 2.0F, -0.2F, 0.0F};
  const std::array<std::uint32_t, 5> offsets{0U, 1U, 2U, 3U, 5U};
  const std::array<float, 5> gradients{0.5F, 0.5F, 0.2F, 1.0F, 2.0F};
  const std::array<float, 5> dissipations{0.1F, 0.0F, 0.5F, 0.0F, 0.0F};
  const float sentinel = std::numeric_limits<float>::max();
  const std::array<float, 5> stops{sentinel, sentinel, sentinel, -0.25F,
                                   sentinel};
  const viennaps::vulkan::levelset::LevelSetUpdateInput updateInput{
      values, offsets, gradients, dissipations, stops, 1.0F, 1.0F, false};
  const auto expectedUpdate = cpuUpdate(updateInput);
  std::vector<float> actualUpdate;
  if (!viennaps::vulkan::levelset::updateLevelSetFp32(
          session, updateProgram, updateInput, actualUpdate, error) ||
      !exactFloatVectors(actualUpdate, expectedUpdate)) {
    return {false, "level-set update CPU differential failed" +
                       (error.empty() ? std::string(": output differs from "
                                                    "CPU oracle")
                                      : ": " + error)};
  }

  const auto levelSet = makeProbeDomain();
  std::vector<ProbeCandidate> candidates;
  std::vector<viennahrle::Index<2>> candidateIndices;
  const auto &grid = levelSet->getGrid();
  const auto &domain = levelSet->getDomain();
  for (unsigned segment = 0U; segment < domain.getNumberOfSegments();
       ++segment) {
    const auto start = segment == 0U ? grid.getMinGridPoint()
                                     : domain.getSegmentation()[segment - 1U];
    const auto end = segment + 1U < domain.getNumberOfSegments()
                         ? domain.getSegmentation()[segment]
                         : grid.incrementIndices(grid.getMaxGridPoint());
    for (viennahrle::ConstSparseStarIterator<
             typename viennals::Domain<float, 2>::DomainType, 1>
             iterator(domain, start);
         iterator.getIndices() < end; ++iterator) {
      candidates.push_back(makeProbeCandidate(iterator));
      candidateIndices.push_back(iterator.getIndices());
    }
  }
  std::vector<ProbeDecision> cpuDecisions;
  if (!viennaps::levelset::classifyHrleRebuildCpu(candidates, 2U, 1.0F,
                                                  cpuDecisions, error)) {
    return {false, "CPU HRLE oracle failed: " + error};
  }
  viennaps::levelset::HrleRebuildCompactionResultFp32 cpuCompaction;
  if (!viennaps::levelset::compactHrleRebuildDecisionsCpu(
          cpuDecisions, cpuCompaction, error)) {
    return {false, "CPU HRLE compaction oracle failed: " + error};
  }

  viennaps::vulkan::primitives::ReductionScanPrimitives primitives;
  if (!primitives.initialize(session, VIENNAPS_REDUCTION_SCAN_SPV_PATH,
                             error)) {
    return {false, "reduction/scan initialization failed: " + error};
  }
  viennahrle::Domain<float, 2> actualDomain;
  std::vector<std::uint32_t> actualIds;
  if (!viennaps::vulkan::levelset::rebuildHrleRebuildFp32DeviceToCpu<2>(
          session, classificationProgram, actionFlagsProgram, compactProgram,
          primitives, candidates, 2U, 1.0F, candidateIndices,
          levelSet->getDomain().getNumberOfPoints(), levelSet->getGrid(),
          actualDomain, actualIds, error)) {
    return {false, "Vulkan HRLE rebuild transaction failed: " + error};
  }
  viennahrle::Domain<float, 2> expectedDomain;
  std::vector<std::uint32_t> expectedIds;
  if (!viennaps::levelset::reconstructHrleRebuildCpu<2>(
          cpuCompaction, candidateIndices,
          levelSet->getDomain().getNumberOfPoints(), levelSet->getGrid(),
          expectedDomain, expectedIds, error) ||
      actualIds != expectedIds) {
    return {false, "HRLE source-point mapping differs from CPU oracle"};
  }
  for (const auto &index : candidateIndices) {
    viennahrle::ConstSparseIterator<viennahrle::Domain<float, 2>> actual(
        actualDomain, index);
    viennahrle::ConstSparseIterator<viennahrle::Domain<float, 2>> expected(
        expectedDomain, index);
    if (actual.isDefined() != expected.isDefined() ||
        std::bit_cast<std::uint32_t>(actual.getValue()) !=
            std::bit_cast<std::uint32_t>(expected.getValue()) ||
        (actual.isDefined() &&
         std::bit_cast<std::uint32_t>(actual.getDefinedValue()) !=
             std::bit_cast<std::uint32_t>(expected.getDefinedValue()))) {
      return {false, "HRLE rebuild structure differs from CPU oracle"};
    }
  }
  return {true, "level-set update and HRLE rebuild CPU differential passed"};
}

#endif

#ifdef VIENNAPS_VULKAN_ENABLED

[[nodiscard]] ProbeResult
makeDeviceProfile(const VkPhysicalDevice physicalDevice,
                  const std::uint32_t deviceIndex) {
  ProbeResult out;

  VkPhysicalDeviceProperties properties{};
  vkGetPhysicalDeviceProperties(physicalDevice, &properties);
  const VkPhysicalDeviceLimits &limits = properties.limits;

  VkPhysicalDeviceMemoryProperties memoryProperties{};
  vkGetPhysicalDeviceMemoryProperties(physicalDevice, &memoryProperties);
  std::uint64_t deviceLocalBytes = 0;
  std::uint64_t hostVisibleBytes = 0;
  for (uint32_t i = 0; i < memoryProperties.memoryHeapCount; ++i) {
    if (memoryProperties.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT)
      deviceLocalBytes += memoryProperties.memoryHeaps[i].size;
    else
      hostVisibleBytes += memoryProperties.memoryHeaps[i].size;
  }

  std::uint32_t extensionCount = 0;
  vkEnumerateDeviceExtensionProperties(physicalDevice, nullptr, &extensionCount,
                                       nullptr);
  std::vector<VkExtensionProperties> extensionProps(extensionCount);
  vkEnumerateDeviceExtensionProperties(physicalDevice, nullptr, &extensionCount,
                                       extensionProps.data());
  std::vector<std::string> extensionNames;
  extensionNames.reserve(extensionProps.size());
  for (const auto &ext : extensionProps) {
    extensionNames.emplace_back(ext.extensionName);
  }

  std::uint32_t queueFamilyCount = 0;
  vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice, &queueFamilyCount,
                                           nullptr);
  std::vector<VkQueueFamilyProperties> queueFamilies(queueFamilyCount);
  vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice, &queueFamilyCount,
                                           queueFamilies.data());
  std::vector<std::uint32_t> computeQueueFamilies;
  computeQueueFamilies.reserve(queueFamilies.size());
  bool hasDedicatedComputeQueueFamily = false;
  int selectedComputeQueueFamily = -1;
  for (std::uint32_t i = 0; i < queueFamilies.size(); ++i) {
    if (queueFamilies[i].queueFlags & VK_QUEUE_COMPUTE_BIT) {
      computeQueueFamilies.push_back(i);
      if (selectedComputeQueueFamily < 0)
        selectedComputeQueueFamily = static_cast<int>(i);
      if (!hasDedicatedComputeQueueFamily &&
          (queueFamilies[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) == 0) {
        hasDedicatedComputeQueueFamily = true;
        selectedComputeQueueFamily = static_cast<int>(i);
      }
    }
  }

  VkPhysicalDeviceProperties2 props2{};
  VkPhysicalDeviceSubgroupProperties subgroupProps{};
  VkPhysicalDeviceFeatures2 features2{};
  std::uint32_t driverId = 0;
  std::string deviceUuid = "n/a";
  std::string driverUuid = "n/a";
  std::string driverName;
  std::string driverInfo;

  props2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
  subgroupProps.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_PROPERTIES;
  subgroupProps.pNext = nullptr;

  VkPhysicalDeviceIDProperties idProperties{};
  idProperties.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES;
  idProperties.pNext = &subgroupProps;

  VkPhysicalDeviceDriverProperties driverProperties{};

  const bool hasDriverPropertiesExt =
      hasExtension(extensionNames, "VK_KHR_driver_properties");
#ifdef VK_KHR_DRIVER_PROPERTIES_EXTENSION_NAME
  if (hasDriverPropertiesExt) {
    driverProperties.sType =
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES;
    driverProperties.pNext = &idProperties;
    props2.pNext = &driverProperties;
  } else
#endif
  {
    props2.pNext = &idProperties;
  }
  vkGetPhysicalDeviceProperties2(physicalDevice, &props2);

  deviceUuid = bytesToHex(idProperties.deviceUUID, VK_UUID_SIZE);
  driverUuid = bytesToHex(idProperties.driverUUID, VK_UUID_SIZE);
#ifdef VK_KHR_DRIVER_PROPERTIES_EXTENSION_NAME
  if (hasDriverPropertiesExt) {
    driverId = static_cast<std::uint32_t>(driverProperties.driverID);
    driverName = driverProperties.driverName;
    driverInfo = driverProperties.driverInfo;
  }
#endif

  const std::string pipelineCacheUuid =
      bytesToHex(properties.pipelineCacheUUID, VK_UUID_SIZE);

  VkPhysicalDeviceBufferDeviceAddressFeatures bufferAddressFeatures{};
  const std::uint32_t subgroupSize =
      subgroupProps.subgroupSize == 0 ? 32 : subgroupProps.subgroupSize;

#ifdef VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME
  VkPhysicalDeviceAccelerationStructureFeaturesKHR asFeatures{};
#endif
#ifdef VK_KHR_RAY_QUERY_EXTENSION_NAME
  VkPhysicalDeviceRayQueryFeaturesKHR rayQueryFeatures{};
#endif
#ifdef VK_KHR_RAY_TRACING_PIPELINE_EXTENSION_NAME
  VkPhysicalDeviceRayTracingPipelineFeaturesKHR rtPipelineFeatures{};
#endif
#ifdef VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FLOAT16_INT8_FEATURES_KHR
  VkPhysicalDeviceFloat16Int8FeaturesKHR float16Int8Features{};
#endif

  void *featureChain = nullptr;

  const bool hasAccelerationStructureExt =
      hasExtension(extensionNames, "VK_KHR_acceleration_structure");
  const bool hasRayQueryExt = hasExtension(extensionNames, "VK_KHR_ray_query");
  const bool hasRtPipelineExt =
      hasExtension(extensionNames, "VK_KHR_ray_tracing_pipeline");

  if (hasRtPipelineExt) {
#ifdef VK_KHR_RAY_TRACING_PIPELINE_EXTENSION_NAME
    rtPipelineFeatures.sType =
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_FEATURES_KHR;
    rtPipelineFeatures.pNext = featureChain;
    featureChain = &rtPipelineFeatures;
#endif
  }
  if (hasRayQueryExt) {
#ifdef VK_KHR_RAY_QUERY_EXTENSION_NAME
    rayQueryFeatures.sType =
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR;
    rayQueryFeatures.pNext = featureChain;
    featureChain = &rayQueryFeatures;
#endif
  }
  if (hasAccelerationStructureExt) {
#ifdef VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME
    asFeatures.sType =
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR;
    asFeatures.pNext = featureChain;
    featureChain = &asFeatures;
#endif
  }
#ifdef VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FLOAT16_INT8_FEATURES_KHR
  const bool hasFloat16Int8Ext =
      hasExtension(extensionNames, "VK_KHR_shader_float16_int8");
  if (hasFloat16Int8Ext) {
    float16Int8Features.sType =
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FLOAT16_INT8_FEATURES_KHR;
    float16Int8Features.pNext = featureChain;
    featureChain = &float16Int8Features;
  }
#endif
  bufferAddressFeatures.sType =
      VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES;
  bufferAddressFeatures.pNext = featureChain;
  featureChain = &bufferAddressFeatures;
  features2.pNext = featureChain;
  vkGetPhysicalDeviceFeatures2(physicalDevice, &features2);

  bool asEnabled = false;
#ifdef VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME
  asEnabled =
      hasAccelerationStructureExt ? asFeatures.accelerationStructure : false;
#endif
  bool rayQueryEnabled = false;
#ifdef VK_KHR_RAY_QUERY_EXTENSION_NAME
  rayQueryEnabled = hasRayQueryExt ? rayQueryFeatures.rayQuery : false;
#endif
  bool rtPipelineEnabled = false;
#ifdef VK_KHR_RAY_TRACING_PIPELINE_EXTENSION_NAME
  rtPipelineEnabled =
      hasRtPipelineExt ? rtPipelineFeatures.rayTracingPipeline : false;
#endif
  bool bdaEnabled = bufferAddressFeatures.bufferDeviceAddress;
  bool float16Int8Enabled = false;
#ifdef VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FLOAT16_INT8_FEATURES_KHR
  float16Int8Enabled =
      hasFloat16Int8Ext ? float16Int8Features.shaderFloat16 : false;
#endif
  std::uint32_t maxRayRecursionDepth = 0;

  if (hasRtPipelineExt) {
    VkPhysicalDeviceProperties2 rayTracingProperties{};
    VkPhysicalDeviceRayTracingPipelinePropertiesKHR rtPipelineProps{};
    rayTracingProperties.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
    rtPipelineProps.sType =
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_PROPERTIES_KHR;
    rayTracingProperties.pNext = &rtPipelineProps;
    vkGetPhysicalDeviceProperties2(physicalDevice, &rayTracingProperties);
    maxRayRecursionDepth = rtPipelineProps.maxRayRecursionDepth;
  }

  std::vector<std::uint64_t> memoryBudgetBytes{};
  const bool hasMemoryBudgetExt =
      hasExtension(extensionNames, "VK_EXT_memory_budget");
#ifdef VK_EXT_MEMORY_BUDGET_SPEC_VERSION
  if (hasMemoryBudgetExt) {
    VkPhysicalDeviceMemoryProperties2 memoryProperties2{};
    VkPhysicalDeviceMemoryBudgetPropertiesEXT memoryBudget{};
    memoryProperties2.sType =
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2;
    memoryBudget.sType =
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT;
    memoryProperties2.pNext = &memoryBudget;
    vkGetPhysicalDeviceMemoryProperties2(physicalDevice, &memoryProperties2);
    const auto &budgetMemoryProperties = memoryProperties2.memoryProperties;
    memoryBudgetBytes.reserve(budgetMemoryProperties.memoryHeapCount);
    for (std::uint32_t i = 0; i < budgetMemoryProperties.memoryHeapCount; ++i) {
      if ((budgetMemoryProperties.memoryHeaps[i].flags &
           VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) == 0u) {
        continue;
      }
      const auto budget = memoryBudget.heapBudget[i];
      const auto usage = memoryBudget.heapUsage[i];
      memoryBudgetBytes.push_back(budget > usage ? budget - usage : 0ULL);
    }
  }
#endif

  const auto computeWorkGroupCount = jsonSize3(std::to_array(
      {limits.maxComputeWorkGroupCount[0], limits.maxComputeWorkGroupCount[1],
       limits.maxComputeWorkGroupCount[2]}));
  const auto computeWorkGroupSize = jsonSize3(std::to_array(
      {limits.maxComputeWorkGroupSize[0], limits.maxComputeWorkGroupSize[1],
       limits.maxComputeWorkGroupSize[2]}));
  const auto safeVulkanWorkingSetBytes =
      viennaps::compute::deriveSafeVulkanWorkingSetBytes(
          memoryBudgetBytes, hasMemoryBudgetExt, nullptr);

  viennaps::compute::VulkanProbeDeviceFacts facts{};
  facts.hardware.deviceUuid = deviceUuid;
  facts.hardware.driverUuid = driverUuid;
  facts.hardware.vendorId = properties.vendorID;
  facts.hardware.deviceId = properties.deviceID;
  facts.hardware.deviceName = properties.deviceName;
  facts.hardware.driverVersion = std::to_string(properties.driverVersion);
  facts.hardware.driverDate = "unknown";
  facts.supportsVulkan = true;
  facts.hasDedicatedComputeQueue = hasDedicatedComputeQueueFamily;
  facts.supportsComputeQueue = !computeQueueFamilies.empty();
  facts.supportsRayQuery = rayQueryEnabled;
  facts.supportsRayTracingPipeline = rtPipelineEnabled;
  facts.supportsShaderFloat64 = features2.features.shaderFloat64;
  facts.memoryBudgetExtensionAvailable = hasMemoryBudgetExt;
  facts.deviceLocalBytes = deviceLocalBytes;
  facts.hostVisibleBytes = hostVisibleBytes;
  facts.memoryBudgetBytes = memoryBudgetBytes;

#ifdef VIENNAPS_VULKAN_LEVELSET_SUITE_AVAILABLE
  const auto validation = runLevelSetValidationSuite(deviceIndex);
  facts.validationEvidence.primitiveSuite =
      validation.pass ? viennaps::compute::VulkanProbeSuiteStatus::PASS
                      : viennaps::compute::VulkanProbeSuiteStatus::FAIL;
  facts.validationEvidence.fp32Suite = facts.validationEvidence.primitiveSuite;
#else
  const ValidationSuiteResult validation{
      false, "required Vulkan level-set suite artifacts unavailable"};
#endif

  std::ostringstream json;
  json << '{';
  json << "\"schemaVersion\":1,";
  json << "\"device\":{";
  json << "\"index\":" << deviceIndex << ",";
  json << "\"uuid\":\"" << deviceUuid << "\",";
  json << "\"driverUUID\":\"" << driverUuid << "\",";
  json << "\"vendorId\":" << properties.vendorID << ",";
  json << "\"deviceId\":" << properties.deviceID << ",";
  json << "\"name\":\"" << escapeJson(properties.deviceName) << "\",";
  json << "\"driverId\":" << driverId << ",";
  json << "\"driverName\":\"" << escapeJson(driverName) << "\",";
  json << "\"driverInfo\":\"" << escapeJson(driverInfo) << "\",";
  json << "\"driverVersion\":\"" << properties.driverVersion << "\",";
  json << "\"apiVersion\":\"" << VK_VERSION_MAJOR(properties.apiVersion) << '.'
       << VK_VERSION_MINOR(properties.apiVersion) << '.'
       << VK_VERSION_PATCH(properties.apiVersion) << "\",";
  json << "\"pipelineCacheUuid\":\"" << pipelineCacheUuid << "\",";
  json << "\"deviceType\":\"" << static_cast<int>(properties.deviceType)
       << "\"},";

  json << "\"memory\":{";
  json << "\"deviceLocalBytes\":" << deviceLocalBytes << ',';
  json << "\"hostVisibleBytes\":" << hostVisibleBytes << ',';
  json << "\"budgetExtension\":" << boolToJson(hasMemoryBudgetExt) << ',';
  json << "\"measuredSafeWorkingSetBytes\":" << safeVulkanWorkingSetBytes
       << "},";

  json << "\"compute\":{";
  json << "\"queueFamily\":" << selectedComputeQueueFamily << ',';
  json << "\"dedicatedQueue\":" << boolToJson(hasDedicatedComputeQueueFamily)
       << ',';
  json << "\"maxWorkGroupInvocations\":"
       << limits.maxComputeWorkGroupInvocations << ',';
  json << "\"subgroupSizes\":"
       << jsonNumberList({static_cast<std::uint32_t>(subgroupSize)}) << ',';
  json << "\"shaderFloat64\":" << boolToJson(features2.features.shaderFloat64)
       << ',';
  json << "\"shaderInt64\":" << boolToJson(features2.features.shaderInt64)
       << ',';
  json << "\"atomicFloat32\":false,";
  json << "\"bufferDeviceAddress\":" << boolToJson(bdaEnabled) << "},";

  json << "\"features\":{";
  json << "\"robustBufferAccess\":"
       << boolToJson(features2.features.robustBufferAccess) << ',';
  json << "\"geometryShader\":" << boolToJson(features2.features.geometryShader)
       << ',';
  json << "\"tessellationShader\"" << ':'
       << boolToJson(features2.features.tessellationShader) << ',';
  json << "\"shaderInt16\":" << boolToJson(features2.features.shaderInt16)
       << ',';
  json << "\"shaderFloat16\":" << boolToJson(float16Int8Enabled) << ',';
  json << "\"samplerAnisotropy\"" << ':'
       << boolToJson(features2.features.samplerAnisotropy) << ',';
  json << "\"pipelineStatisticsQuery\"" << ':'
       << boolToJson(features2.features.pipelineStatisticsQuery) << '}';

  json << ",\"limits\":{";
  json << "\"maxStorageBufferRange\":" << limits.maxStorageBufferRange << ',';
  json << "\"maxPushConstantsSize\":" << limits.maxPushConstantsSize << ',';
  json << "\"maxMemoryAllocationCount\":" << limits.maxMemoryAllocationCount
       << ',';
  json << "\"maxSamplerAllocationCount\":" << limits.maxSamplerAllocationCount
       << ',';
  json << "\"maxComputeWorkGroupInvocations\":"
       << limits.maxComputeWorkGroupInvocations << ',';
  json << "\"maxComputeWorkGroupCount\":" << computeWorkGroupCount << ',';
  json << "\"maxComputeWorkGroupSize\":" << computeWorkGroupSize << "},";

  json << "\"ray\":{";
  json << "\"accelerationStructure\":" << boolToJson(asEnabled) << ',';
  json << "\"rayQuery\":" << boolToJson(rayQueryEnabled) << ',';
  json << "\"rayTracingPipeline\":" << boolToJson(rtPipelineEnabled) << ',';
  json << "\"maxRayRecursionDepth\":" << maxRayRecursionDepth << "},";

  json << "\"validation\":{";
  const auto suiteStatus = [](const auto status) {
    using Status = viennaps::compute::VulkanProbeSuiteStatus;
    switch (status) {
    case Status::PASS:
      return "pass";
    case Status::FAIL:
      return "fail";
    case Status::NOT_RUN:
    default:
      return "not-run";
    }
  };
  json << "\"primitiveSuite\":\""
       << suiteStatus(facts.validationEvidence.primitiveSuite) << "\",";
  json << "\"fp32Suite\":\"" << suiteStatus(facts.validationEvidence.fp32Suite)
       << "\",";
  json << "\"fp64Suite\":\"not-run\",";
  json << "\"computeBvhSuite\":\"not-run\",";
  json << "\"hardwareRaySuite\":\"not-run\",";
  json << "\"levelSetSuiteReason\":\"" << escapeJson(validation.reason)
       << "\",";
  json << "\"deviceIndex\":" << deviceIndex << "},";

  json << "\"extensions\":[";
  for (std::size_t i = 0; i < extensionNames.size(); ++i) {
    if (i != 0) {
      json << ',';
    }
    json << '"' << escapeJson(extensionNames[i]) << '"';
  }
  json << "],";

  const auto adaptedProfile =
      viennaps::compute::adaptVulkanProbeFactsToCapabilityProfile(
          facts, nowUtcTimestamp());
  if (adaptedProfile.ok) {
    out.deploymentProfiles.push_back(adaptedProfile.record);
  }

  json << "\"computeQueueFamilyIndices\":"
       << jsonNumberList(computeQueueFamilies);
  json << '}';
  out.rawSummary = json.str();
  return out;
}

[[nodiscard]] ProbeResult probeSummary() {
  VkApplicationInfo appInfo{};
  appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
  appInfo.pApplicationName = "ViennaPS Vulkan Device Probe";
  appInfo.applicationVersion = VK_MAKE_VERSION(4, 6, 2);
  appInfo.pEngineName = "ViennaPS";
  appInfo.engineVersion = VK_MAKE_VERSION(4, 6, 2);
  appInfo.apiVersion = VK_API_VERSION_1_2;

  VkInstanceCreateInfo instanceCreateInfo{};
  instanceCreateInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
  instanceCreateInfo.pApplicationInfo = &appInfo;
  VkInstance instance{};

  const auto createdUtc = nowUtcTimestamp();
  ProbeResult out;
  if (vkCreateInstance(&instanceCreateInfo, nullptr, &instance) != VK_SUCCESS) {
    out.rawSummary =
        std::string("{\"schemaVersion\":1,\"status\":\"error\",\"reason\":"
                    "\"vkCreateInstance failed.\"}");
    return out;
  }

  std::uint32_t deviceCount = 0;
  if (vkEnumeratePhysicalDevices(instance, &deviceCount, nullptr) !=
          VK_SUCCESS ||
      deviceCount == 0) {
    vkDestroyInstance(instance, nullptr);
    out.rawSummary =
        std::string("{\"schemaVersion\":1,\"status\":\"disabled\","
                    "\"reason\":\"No Vulkan physical device found.\"}");
    return out;
  }

  std::vector<VkPhysicalDevice> devices(deviceCount);
  if (vkEnumeratePhysicalDevices(instance, &deviceCount, devices.data()) !=
          VK_SUCCESS ||
      deviceCount == 0U) {
    vkDestroyInstance(instance, nullptr);
    out.rawSummary =
        std::string("{\"schemaVersion\":1,\"status\":\"disabled\",") +
        "\"reason\":\"Vulkan physical-device enumeration changed during "
        "probe.\"}";
    return out;
  }
  devices.resize(deviceCount);

  std::ostringstream outStream;
  outStream << "{\n";
  outStream << "\"schemaVersion\":1,\n";
  outStream << "\"viennaPsVersion\":\"4.6.2+vulkan-probe\",\n";
  outStream << "\"createdUtc\":\"" << createdUtc << "\",\n";
  outStream << "\"status\":\"pass\",\n";
  outStream << "\"featuresProfiled\":true,\n";
  outStream << "\"devices\":[\n";
  for (std::uint32_t i = 0; i < devices.size(); ++i) {
    const auto deviceSummary = makeDeviceProfile(devices[i], i);
    if (i != 0)
      outStream << ",\n";
    outStream << deviceSummary.rawSummary;

    if (!deviceSummary.deploymentProfiles.empty()) {
      out.deploymentProfiles.push_back(
          deviceSummary.deploymentProfiles.front());
    }
  }
  outStream << "]\n";
  outStream << "}";
  out.rawSummary = outStream.str();

  vkDestroyInstance(instance, nullptr);
  return out;
}

#else

[[nodiscard]] ProbeResult probeSummary() {
  ProbeResult out;
  out.rawSummary =
      std::string("{\n"
                  "\"schemaVersion\":1,\n"
                  "\"viennaPsVersion\":\"4.6.2+vulkan-probe\",\n"
                  "\"createdUtc\":\"" +
                  nowUtcTimestamp() +
                  "\",\n"
                  "\"status\":\"disabled\",\n"
                  "\"reason\":\"Vulkan headers/libraries were not available "
                  "at build time.\",\n"
                  "\"devices\":[],\n"
                  "\"source\":\"build-time-fallback\"\n"
                  "}");
  return out;
}

#endif

[[nodiscard]] ArgView parseArgs(int argc, char **argv) {
  ArgView args;
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--json") {
      // Reserved for compatibility; JSON is always the default output.
    } else if ((arg == "--write-profile" || arg == "--write") && i + 1 < argc) {
      args.writeProfile = true;
      args.profilePath = argv[++i];
    } else if (arg == "--write-deployment-profile" && i + 1 < argc) {
      args.writeDeploymentProfile = true;
      args.deploymentProfilePath = argv[++i];
    } else if (arg == "--validate-profile") {
      args.validateProfile = true;
    } else if (arg == "--strict-fp32-smoke") {
      args.strictFp32Smoke = true;
    } else if (arg == "--strict-fp32-child") {
      args.strictFp32Child = true;
    } else if (arg == "--strict-fp32-force-failure") {
      args.strictFp32ForceFailure = true;
    } else if (arg == "--strict-fp32-child-output" && i + 1 < argc) {
      args.strictFp32ChildOutput = argv[++i];
    } else if (arg == "--strict-fp32-device-index" && i + 1 < argc) {
      try {
        args.strictFp32DeviceIndex =
            static_cast<std::uint32_t>(std::stoul(argv[++i]));
      } catch (...) {
        std::cerr << "Invalid strict FP32 device index.\n";
        args.strictFp32DeviceIndexValid = false;
      }
    } else if (arg == "--help") {
      std::cout
          << "viennaps-device-probe [--write-profile <path>|--write <path>] "
             "[--write-deployment-profile <path>] [--validate-profile]\n";
    } else {
      std::cerr << "Unknown argument: " << arg << '\n';
    }
  }
  return args;
}

struct IsolatedStrictSmokeResult {
  bool launched = false;
  bool timedOut = false;
  int exitCode = -1;
  std::string diagnostic;
};

[[nodiscard]] IsolatedStrictSmokeResult runIsolatedStrictFp32Child(
    const std::string &executable, const std::string &outputPath,
    const bool forceFailure, const std::uint32_t deviceIndex) {
  IsolatedStrictSmokeResult result;
#if defined(_WIN32)
  std::wstring command =
      L"\"" + std::filesystem::path(executable).wstring() +
      L"\" --strict-fp32-child --strict-fp32-child-output \"" +
      std::filesystem::path(outputPath).wstring() +
      L"\" --strict-fp32-device-index " + std::to_wstring(deviceIndex);
  if (forceFailure)
    command += L" --strict-fp32-force-failure";
  std::vector<wchar_t> commandLine(command.begin(), command.end());
  commandLine.push_back(L'\0');
  STARTUPINFOW startup{};
  startup.cb = sizeof(startup);
  PROCESS_INFORMATION process{};
  if (!CreateProcessW(nullptr, commandLine.data(), nullptr, nullptr, FALSE, 0,
                      nullptr, nullptr, &startup, &process)) {
    result.diagnostic = "strict FP32 child launch failed";
    return result;
  }
  result.launched = true;
  const DWORD waitResult = WaitForSingleObject(
      process.hProcess,
      static_cast<DWORD>(
          viennaps::compute::kVulkanFp32NumericalSmokeWatchdogMs));
  if (waitResult == WAIT_TIMEOUT) {
    result.timedOut = true;
    TerminateProcess(process.hProcess, 124U);
    WaitForSingleObject(process.hProcess, INFINITE);
    result.diagnostic = "strict FP32 child watchdog timeout";
  } else if (waitResult != WAIT_OBJECT_0) {
    result.diagnostic = "strict FP32 child wait failed";
  }
  DWORD exitCode = 1U;
  GetExitCodeProcess(process.hProcess, &exitCode);
  result.exitCode = static_cast<int>(exitCode);
  CloseHandle(process.hThread);
  CloseHandle(process.hProcess);
#else
  const pid_t child = fork();
  if (child < 0) {
    result.diagnostic = "strict FP32 child fork failed";
    return result;
  }
  if (child == 0) {
    const std::string deviceIndexText = std::to_string(deviceIndex);
    char *childArgv[] = {const_cast<char *>(executable.c_str()),
                         const_cast<char *>("--strict-fp32-child"),
                         const_cast<char *>("--strict-fp32-child-output"),
                         const_cast<char *>(outputPath.c_str()),
                         const_cast<char *>("--strict-fp32-device-index"),
                         const_cast<char *>(deviceIndexText.c_str()),
                         forceFailure
                             ? const_cast<char *>("--strict-fp32-force-failure")
                             : nullptr,
                         nullptr};
    execvp(executable.c_str(), childArgv);
    _exit(127);
  }
  result.launched = true;
  const auto deadline =
      std::chrono::steady_clock::now() +
      std::chrono::milliseconds(
          viennaps::compute::kVulkanFp32NumericalSmokeWatchdogMs);
  int status = 0;
  while (std::chrono::steady_clock::now() < deadline) {
    const pid_t waited = waitpid(child, &status, WNOHANG);
    if (waited == child)
      break;
    if (waited < 0) {
      result.diagnostic = "strict FP32 child wait failed";
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  if (waitpid(child, &status, WNOHANG) == 0) {
    result.timedOut = true;
    kill(child, SIGKILL);
    waitpid(child, &status, 0);
    result.diagnostic = "strict FP32 child watchdog timeout";
  }
  if (WIFEXITED(status))
    result.exitCode = WEXITSTATUS(status);
  else if (WIFSIGNALED(status))
    result.exitCode = 128 + WTERMSIG(status);
#endif
  if (!result.timedOut && result.exitCode != 0 && result.diagnostic.empty())
    result.diagnostic = "strict FP32 child exited nonzero";
  return result;
}

[[nodiscard]] StrictFp32SmokeResult
runIsolatedStrictFp32Smoke(const std::string &executable,
                           const bool forceFailure,
                           const std::uint32_t deviceIndex) {
  const auto unique = std::to_string(
#if defined(_WIN32)
      static_cast<unsigned long long>(GetCurrentProcessId())
#else
      static_cast<unsigned long long>(getpid())
#endif
  );
  const auto outputPath =
      std::filesystem::temp_directory_path() /
      ("viennaps-strict-fp32-" + unique + "-" +
       std::to_string(static_cast<unsigned long long>(
           std::chrono::steady_clock::now().time_since_epoch().count())) +
       ".json");
  const auto child = runIsolatedStrictFp32Child(executable, outputPath.string(),
                                                forceFailure, deviceIndex);
  if (!child.launched || child.timedOut || child.exitCode != 0) {
    std::error_code removeError;
    std::filesystem::remove(outputPath, removeError);
    return makeStrictFp32Result(
        viennaps::compute::VulkanNumericalSmokeStatus::FAIL,
        child.diagnostic.empty() ? "strict FP32 child failed"
                                 : child.diagnostic);
  }
  std::ifstream input(outputPath, std::ios::binary);
  std::string content((std::istreambuf_iterator<char>(input)),
                      std::istreambuf_iterator<char>());
  std::error_code removeError;
  std::filesystem::remove(outputPath, removeError);
  viennaps::compute::VulkanFp32NumericalSmokeEvidence evidence{};
  std::string deviceUuid;
  if (content.empty() ||
      !parseStrictFp32Result(content, evidence, deviceUuid)) {
    return makeStrictFp32Result(
        viennaps::compute::VulkanNumericalSmokeStatus::FAIL,
        "strict FP32 child evidence malformed");
  }
  if (evidence.status == viennaps::compute::VulkanNumericalSmokeStatus::PASS &&
      (evidence.contractId !=
           viennaps::compute::kVulkanFp32NumericalSmokeContract ||
       evidence.caseCount == 0U || evidence.mismatchCount != 0U ||
       evidence.maxUlp != 0U ||
       evidence.watchdogMs !=
           viennaps::compute::kVulkanFp32NumericalSmokeWatchdogMs ||
       evidence.elapsedMs > evidence.watchdogMs ||
       !evidence.failureDiagnostic.empty())) {
    return makeStrictFp32Result(
        viennaps::compute::VulkanNumericalSmokeStatus::FAIL,
        "strict FP32 child PASS evidence violates contract invariants");
  }
  StrictFp32SmokeResult result;
  result.evidence = evidence;
  result.deviceUuid = deviceUuid;
  result.json = strictFp32EvidenceJson(evidence, result.deviceUuid);
  return result;
}

void writeProfile(const std::string &path, const std::string &content) {
  std::ofstream output(path);
  if (!output.good()) {
    std::cerr << "Failed to open output path: " << path << '\n';
    return;
  }
  output << content;
  if (!output.good()) {
    std::cerr << "Failed to write profile to " << path << '\n';
    return;
  }
  std::cout << "Profile written to " << path << '\n';
}

[[nodiscard]] bool
writeDeploymentProfile(const std::string &path,
                       const std::vector<CapabilityProfileRecord> &profiles,
                       const std::string_view selectedUuid = {}) {
  if (profiles.empty()) {
    std::cerr << "No generated Vulkan deployment profile available to write.\n";
    return false;
  }
  std::size_t selected = 0U;
  if (!selectedUuid.empty()) {
    std::size_t matches = 0U;
    for (std::size_t index = 0U; index < profiles.size(); ++index) {
      if (profiles[index].hardware.deviceUuid == selectedUuid) {
        selected = index;
        ++matches;
      }
    }
    if (matches != 1U) {
      std::cerr
          << "Strict FP32 device UUID did not match exactly one profile.\n";
      return false;
    }
  }
  std::string error;
  if (!viennaps::compute::writeCapabilityProfileRecordToFile(
          path, profiles[selected], &error)) {
    std::cerr << "Failed to write deployment profile to " << path << ": "
              << error << '\n';
    return false;
  }
  std::cout << "Deployment profile written to " << path << '\n';
  return true;
}

} // namespace

int main(int argc, char **argv) {
  const ArgView args = parseArgs(argc, argv);
  if (!args.strictFp32DeviceIndexValid)
    return 2;
  if (args.strictFp32Child) {
    const auto childResult = runStrictFp32Child(args.strictFp32ForceFailure,
                                                args.strictFp32DeviceIndex);
    if (!writeStrictFp32Result(args.strictFp32ChildOutput, childResult.evidence,
                               childResult.deviceUuid)) {
      std::cerr << "Failed to write strict FP32 child evidence.\n";
      return 2;
    }
    std::cout << childResult.json << '\n';
    const bool passed = childResult.evidence.status ==
                        viennaps::compute::VulkanNumericalSmokeStatus::PASS;
    if (!passed) {
      std::error_code removeError;
      std::filesystem::remove(args.strictFp32ChildOutput, removeError);
    }
    return passed ? 0 : 1;
  }
  const ProbeResult profile = probeSummary();
  std::cout << profile.rawSummary << '\n';
  bool success = true;
  bool deploymentProfileWritten = false;
  std::string strictDeviceUuid;

  ProbeResult mutableProfile = profile;
  if (args.strictFp32Smoke) {
    const auto strictResult = runIsolatedStrictFp32Smoke(
        argv[0], args.strictFp32ForceFailure, args.strictFp32DeviceIndex);
    std::cout << strictResult.json << '\n';
    strictDeviceUuid = strictResult.deviceUuid;
    std::size_t matches = 0U;
    std::size_t matchedIndex = 0U;
    for (std::size_t index = 0U;
         index < mutableProfile.deploymentProfiles.size(); ++index) {
      if (mutableProfile.deploymentProfiles[index].hardware.deviceUuid ==
          strictDeviceUuid) {
        matchedIndex = index;
        ++matches;
      }
    }
    if (strictDeviceUuid.empty() || matches != 1U) {
      success = false;
    } else {
      mutableProfile.deploymentProfiles[matchedIndex]
          .capabilityProfile.vulkanFp32NumericalSmoke = strictResult.evidence;
    }
    if (strictResult.evidence.status !=
        viennaps::compute::VulkanNumericalSmokeStatus::PASS) {
      success = false;
    }
  }

  if (args.writeProfile && !args.profilePath.empty()) {
    writeProfile(args.profilePath, profile.rawSummary);
  }

  if (args.writeDeploymentProfile && !args.deploymentProfilePath.empty()) {
    deploymentProfileWritten = writeDeploymentProfile(
        args.deploymentProfilePath, mutableProfile.deploymentProfiles,
        strictDeviceUuid);
    success = deploymentProfileWritten && success;
  }

  if (args.validateProfile) {
    if (!deploymentProfileWritten || args.deploymentProfilePath.empty()) {
      std::cerr << "Deployment profile validation requires a successful "
                   "--write-deployment-profile operation.\n";
      success = false;
    } else {
      const auto loaded =
          viennaps::compute::loadCapabilityProfileRecordFromFile(
              args.deploymentProfilePath);
      if (!loaded.ok) {
        std::cerr << "Deployment profile validation failed: " << loaded.message
                  << '\n';
        success = false;
      } else {
        std::cout << "{\"deploymentProfileValidation\":\"pass\"}\n";
      }
    }
  }

  return success ? 0 : 1;
}
