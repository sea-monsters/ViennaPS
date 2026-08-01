// Minimal Vulkan compute smoke test.
//
// This program builds one small compute dispatch and validates output against a
// CPU oracle:
//   y[i] = 2 * x[i] + 1, for 16 float values.
//
// Exit code:
//   0 -> PASS
//   1 -> any failure

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include <vulkan/vulkan.h>

#ifndef VIENNAPS_COMPUTE_SMOKE_SPV_PATH
#error                                                                         \
    "VIENNAPS_COMPUTE_SMOKE_SPV_PATH must be defined when building viennaps-compute-smoke."
#endif

namespace {

constexpr std::string_view kShaderSpvPath = VIENNAPS_COMPUTE_SMOKE_SPV_PATH;

[[nodiscard]] const char *vkResultToString(const VkResult result) {
  switch (result) {
  case VK_SUCCESS:
    return "VK_SUCCESS";
  case VK_NOT_READY:
    return "VK_NOT_READY";
  case VK_TIMEOUT:
    return "VK_TIMEOUT";
  case VK_EVENT_SET:
    return "VK_EVENT_SET";
  case VK_EVENT_RESET:
    return "VK_EVENT_RESET";
  case VK_INCOMPLETE:
    return "VK_INCOMPLETE";
  case VK_ERROR_OUT_OF_HOST_MEMORY:
    return "VK_ERROR_OUT_OF_HOST_MEMORY";
  case VK_ERROR_OUT_OF_DEVICE_MEMORY:
    return "VK_ERROR_OUT_OF_DEVICE_MEMORY";
  case VK_ERROR_INITIALIZATION_FAILED:
    return "VK_ERROR_INITIALIZATION_FAILED";
  case VK_ERROR_DEVICE_LOST:
    return "VK_ERROR_DEVICE_LOST";
  case VK_ERROR_MEMORY_MAP_FAILED:
    return "VK_ERROR_MEMORY_MAP_FAILED";
  case VK_ERROR_LAYER_NOT_PRESENT:
    return "VK_ERROR_LAYER_NOT_PRESENT";
  case VK_ERROR_EXTENSION_NOT_PRESENT:
    return "VK_ERROR_EXTENSION_NOT_PRESENT";
  case VK_ERROR_FEATURE_NOT_PRESENT:
    return "VK_ERROR_FEATURE_NOT_PRESENT";
  case VK_ERROR_INCOMPATIBLE_DRIVER:
    return "VK_ERROR_INCOMPATIBLE_DRIVER";
  case VK_ERROR_TOO_MANY_OBJECTS:
    return "VK_ERROR_TOO_MANY_OBJECTS";
  case VK_ERROR_FORMAT_NOT_SUPPORTED:
    return "VK_ERROR_FORMAT_NOT_SUPPORTED";
  case VK_ERROR_SURFACE_LOST_KHR:
    return "VK_ERROR_SURFACE_LOST_KHR";
  case VK_ERROR_OUT_OF_POOL_MEMORY:
    return "VK_ERROR_OUT_OF_POOL_MEMORY";
  default:
    return "VK_UNKNOWN_ERROR";
  }
}

[[nodiscard]] std::string_view
vkDeviceTypeName(const VkPhysicalDeviceType type) {
  switch (type) {
  case VK_PHYSICAL_DEVICE_TYPE_OTHER:
    return "other";
  case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU:
    return "integrated-gpu";
  case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU:
    return "discrete-gpu";
  case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU:
    return "virtual-gpu";
  case VK_PHYSICAL_DEVICE_TYPE_CPU:
    return "cpu";
  default:
    return "unknown";
  }
}

[[nodiscard]] std::vector<std::uint8_t>
readBinaryFile(const std::string_view path) {
  std::ifstream file(std::string(path), std::ios::binary | std::ios::ate);
  if (!file) {
    return {};
  }
  const auto size = static_cast<std::size_t>(file.tellg());
  if (size == 0) {
    return {};
  }
  file.seekg(0, std::ios::beg);
  std::vector<std::uint8_t> bytes(size);
  file.read(reinterpret_cast<char *>(bytes.data()),
            static_cast<std::streamsize>(size));
  return file ? bytes : std::vector<std::uint8_t>{};
}

[[nodiscard]] std::vector<std::uint32_t>
readSpirv(const std::string_view path) {
  auto bytes = readBinaryFile(path);
  if (bytes.empty() || bytes.size() % sizeof(std::uint32_t) != 0) {
    return {};
  }
  const std::size_t words = bytes.size() / sizeof(std::uint32_t);
  const auto *raw = reinterpret_cast<const std::uint32_t *>(bytes.data());
  return std::vector<std::uint32_t>(raw, raw + words);
}

[[nodiscard]] bool hasComputeQueueFamily(VkPhysicalDevice device,
                                         std::uint32_t &queueFamilyIndex,
                                         bool &hasDedicatedQueue) {
  std::uint32_t queueFamilyCount = 0;
  vkGetPhysicalDeviceQueueFamilyProperties(device, &queueFamilyCount, nullptr);
  if (queueFamilyCount == 0) {
    return false;
  }
  std::vector<VkQueueFamilyProperties> families(queueFamilyCount);
  vkGetPhysicalDeviceQueueFamilyProperties(device, &queueFamilyCount,
                                           families.data());

  queueFamilyIndex = 0;
  hasDedicatedQueue = false;
  bool foundDedicated = false;
  bool hasAnyCompute = false;
  for (std::uint32_t i = 0; i < queueFamilyCount; ++i) {
    if ((families[i].queueFlags & VK_QUEUE_COMPUTE_BIT) != 0u) {
      if (!hasAnyCompute) {
        queueFamilyIndex = i;
        hasAnyCompute = true;
      }
      if ((families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) == 0u) {
        queueFamilyIndex = i;
        hasDedicatedQueue = true;
        foundDedicated = true;
      }
    }
  }
  if (!hasAnyCompute) {
    return false;
  }
  if (!foundDedicated) {
    hasDedicatedQueue = false;
  }
  return true;
}

[[nodiscard]] std::uint32_t
selectMemoryType(const VkPhysicalDevice device,
                 const VkMemoryRequirements &requirements,
                 const VkMemoryPropertyFlags requiredFlags) {
  VkPhysicalDeviceMemoryProperties memProps{};
  vkGetPhysicalDeviceMemoryProperties(device, &memProps);

  std::uint32_t best = std::numeric_limits<std::uint32_t>::max();
  for (std::uint32_t i = 0; i < memProps.memoryTypeCount; ++i) {
    if ((requirements.memoryTypeBits & (1u << i)) == 0u) {
      continue;
    }
    const auto candidateFlags = memProps.memoryTypes[i].propertyFlags;
    if ((candidateFlags & requiredFlags) != requiredFlags) {
      continue;
    }
    best = i;
    if ((candidateFlags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0u) {
      return i;
    }
  }
  return best;
}

[[nodiscard]] std::uint32_t orderedFloatBits(const float value) {
  const std::uint32_t bits = std::bit_cast<std::uint32_t>(value);
  return (bits & 0x80000000u) ? ~bits : (bits ^ 0x80000000u);
}

[[nodiscard]] bool nearlyEqual(const float lhs, const float rhs) {
  if (lhs == rhs) {
    return true;
  }
  const float diff = std::abs(lhs - rhs);
  const float absMax = std::max(std::abs(lhs), std::abs(rhs));
  const float relative = absMax > 0.0f ? diff / absMax : diff;
  const std::uint32_t ulpL = orderedFloatBits(lhs);
  const std::uint32_t ulpR = orderedFloatBits(rhs);
  const std::uint32_t ulpDiff = ulpL > ulpR ? (ulpL - ulpR) : (ulpR - ulpL);
  return diff <= 1e-6f && relative <= 1e-6f && ulpDiff <= 8u;
}

[[nodiscard]] std::string driverSummary(const VkPhysicalDevice physicalDevice) {
  VkPhysicalDeviceProperties2 props2{};
  props2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
#ifdef VK_KHR_DRIVER_PROPERTIES_EXTENSION_NAME
#ifdef VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES_KHR
  VkPhysicalDeviceDriverPropertiesKHR driverProperties{};
  driverProperties.sType =
      VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES_KHR;
  props2.pNext = &driverProperties;
#endif
#endif

  vkGetPhysicalDeviceProperties2(physicalDevice, &props2);

  std::ostringstream out;
  out << "driver unavailable";
#ifdef VK_KHR_DRIVER_PROPERTIES_EXTENSION_NAME
#ifdef VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES_KHR
  if (props2.pNext == &driverProperties) {
    out << " (name: " << driverProperties.driverName
        << ", info: " << driverProperties.driverInfo
        << ", id: " << static_cast<int>(driverProperties.driverID) << ')';
  }
#endif
#endif
  return out.str();
}

bool runComputeSmoke() {
  VkApplicationInfo appInfo{};
  appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
  appInfo.pApplicationName = "ViennaPS Vulkan Compute Smoke";
  appInfo.applicationVersion = VK_MAKE_VERSION(4, 6, 2);
  appInfo.pEngineName = "ViennaPS";
  appInfo.engineVersion = VK_MAKE_VERSION(4, 6, 2);
  appInfo.apiVersion = VK_API_VERSION_1_2;

  VkInstanceCreateInfo instanceInfo{};
  instanceInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
  instanceInfo.pApplicationInfo = &appInfo;

  VkInstance instance{};
  VkResult result = vkCreateInstance(&instanceInfo, nullptr, &instance);
  if (result != VK_SUCCESS) {
    std::cerr << "vkCreateInstance failed: " << vkResultToString(result)
              << '\n';
    return false;
  }

  std::uint32_t deviceCount = 0;
  result = vkEnumeratePhysicalDevices(instance, &deviceCount, nullptr);
  if (result != VK_SUCCESS || deviceCount == 0) {
    std::cerr << "No Vulkan physical device found." << '\n';
    vkDestroyInstance(instance, nullptr);
    return false;
  }

  std::vector<VkPhysicalDevice> devices(deviceCount);
  vkEnumeratePhysicalDevices(instance, &deviceCount, devices.data());

  VkPhysicalDevice selected = VK_NULL_HANDLE;
  VkPhysicalDeviceProperties selectedProps{};
  std::uint32_t selectedQueueFamily = 0;
  bool selectedDedicatedQueue = false;
  for (const auto device : devices) {
    std::uint32_t qIndex = 0;
    bool hasDedicated = false;
    if (!hasComputeQueueFamily(device, qIndex, hasDedicated)) {
      continue;
    }
    vkGetPhysicalDeviceProperties(device, &selectedProps);
    selected = device;
    selectedQueueFamily = qIndex;
    selectedDedicatedQueue = hasDedicated;
    break;
  }

  if (selected == VK_NULL_HANDLE) {
    std::cerr << "No compute-capable physical device found." << '\n';
    vkDestroyInstance(instance, nullptr);
    return false;
  }

  std::cout << "[Smoke] device: " << selectedProps.deviceName << '\n';
  std::cout << "[Smoke] device type: "
            << vkDeviceTypeName(selectedProps.deviceType) << '\n';
  std::cout << "[Smoke] vendorId: 0x" << std::hex << selectedProps.vendorID
            << ", deviceId: 0x" << selectedProps.deviceID << std::dec << '\n';
  std::cout << "[Smoke] driverVersion: " << selectedProps.driverVersion << '\n';
  std::cout << "[Smoke] queue family: " << selectedQueueFamily
            << ", dedicated: " << (selectedDedicatedQueue ? "yes" : "no")
            << '\n';
  std::cout << "[Smoke] queue info: " << driverSummary(selected) << '\n';

  const float inputData[16] = {0.0F,  1.0F,  2.0F,  3.0F, 4.0F,  5.0F,
                               6.0F,  7.0F,  8.0F,  9.0F, 10.0F, 11.0F,
                               12.0F, 13.0F, 14.0F, 15.0F};
  std::array<float, 16> expected{};
  std::array<float, 16> output{};
  for (std::size_t i = 0; i < expected.size(); ++i) {
    expected[i] = inputData[i] * 2.0F + 1.0F;
  }

  const std::vector<std::uint32_t> spirv = readSpirv(kShaderSpvPath);
  if (spirv.empty()) {
    std::cerr << "Failed to load SPIR-V module: " << kShaderSpvPath << '\n';
    vkDestroyInstance(instance, nullptr);
    return false;
  }

  float queuePriority = 1.0F;
  VkDeviceQueueCreateInfo queueInfo{};
  queueInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
  queueInfo.queueFamilyIndex = selectedQueueFamily;
  queueInfo.queueCount = 1;
  queueInfo.pQueuePriorities = &queuePriority;

  VkDeviceCreateInfo deviceInfo{};
  deviceInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
  deviceInfo.queueCreateInfoCount = 1;
  deviceInfo.pQueueCreateInfos = &queueInfo;

  VkDevice device{};
  result = vkCreateDevice(selected, &deviceInfo, nullptr, &device);
  if (result != VK_SUCCESS) {
    std::cerr << "vkCreateDevice failed: " << vkResultToString(result) << '\n';
    vkDestroyInstance(instance, nullptr);
    return false;
  }

  VkQueue queue{};
  vkGetDeviceQueue(device, selectedQueueFamily, 0, &queue);

  auto cleanupQueue =
      [&](VkCommandBuffer cmdBuf, VkCommandPool cmdPool,
          VkDescriptorPool dsetPool, VkPipeline pipeline,
          VkPipelineLayout pipelineLayout, VkDescriptorSetLayout setLayout,
          VkShaderModule module, VkBuffer inBuffer, VkDeviceMemory inMemory,
          VkBuffer outBuffer, VkDeviceMemory outMemory, VkFence fence) {
        if (cmdBuf != VK_NULL_HANDLE) {
          (void)cmdBuf;
        }
        if (fence != VK_NULL_HANDLE) {
          vkDestroyFence(device, fence, nullptr);
        }
        if (outMemory != VK_NULL_HANDLE) {
          vkUnmapMemory(device, outMemory);
        }
        if (inMemory != VK_NULL_HANDLE) {
          vkUnmapMemory(device, inMemory);
        }
        if (outBuffer != VK_NULL_HANDLE) {
          vkDestroyBuffer(device, outBuffer, nullptr);
        }
        if (inBuffer != VK_NULL_HANDLE) {
          vkDestroyBuffer(device, inBuffer, nullptr);
        }
        if (pipeline != VK_NULL_HANDLE) {
          vkDestroyPipeline(device, pipeline, nullptr);
        }
        if (pipelineLayout != VK_NULL_HANDLE) {
          vkDestroyPipelineLayout(device, pipelineLayout, nullptr);
        }
        if (setLayout != VK_NULL_HANDLE) {
          vkDestroyDescriptorSetLayout(device, setLayout, nullptr);
        }
        if (dsetPool != VK_NULL_HANDLE) {
          vkDestroyDescriptorPool(device, dsetPool, nullptr);
        }
        if (cmdPool != VK_NULL_HANDLE) {
          vkDestroyCommandPool(device, cmdPool, nullptr);
        }
        if (module != VK_NULL_HANDLE) {
          vkDestroyShaderModule(device, module, nullptr);
        }
        if (device != VK_NULL_HANDLE) {
          vkDestroyDevice(device, nullptr);
        }
        if (instance != VK_NULL_HANDLE) {
          vkDestroyInstance(instance, nullptr);
        }
      };

  VkBuffer inputBuffer{};
  VkDeviceMemory inputMemory{};
  VkBuffer outputBuffer{};
  VkDeviceMemory outputMemory{};
  bool ok = false;
  VkFence fence = VK_NULL_HANDLE;
  VkCommandPool commandPool = VK_NULL_HANDLE;
  VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
  VkDescriptorSet descriptorSet = VK_NULL_HANDLE;
  VkDescriptorPool descriptorPool = VK_NULL_HANDLE;
  VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
  VkDescriptorSetLayout descriptorSetLayout = VK_NULL_HANDLE;
  VkPipeline pipeline = VK_NULL_HANDLE;
  VkShaderModule shaderModule = VK_NULL_HANDLE;

  VkBufferCreateInfo bufferInfo{};
  bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
  bufferInfo.size = sizeof(inputData);
  bufferInfo.usage =
      VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
  bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  VkResult inBufferResult =
      vkCreateBuffer(device, &bufferInfo, nullptr, &inputBuffer);
  if (inBufferResult != VK_SUCCESS) {
    std::cerr << "vkCreateBuffer(input) failed: "
              << vkResultToString(inBufferResult) << '\n';
    cleanupQueue(commandBuffer, commandPool, descriptorPool, pipeline,
                 pipelineLayout, descriptorSetLayout, shaderModule, inputBuffer,
                 inputMemory, outputBuffer, outputMemory, fence);
    return false;
  }

  VkMemoryRequirements memReq{};
  vkGetBufferMemoryRequirements(device, inputBuffer, &memReq);
  const auto inputMemType =
      selectMemoryType(selected, memReq, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT);
  if (inputMemType == std::numeric_limits<std::uint32_t>::max()) {
    std::cerr << "No host-visible memory type for input buffer." << '\n';
    cleanupQueue(commandBuffer, commandPool, descriptorPool, pipeline,
                 pipelineLayout, descriptorSetLayout, shaderModule, inputBuffer,
                 inputMemory, outputBuffer, outputMemory, fence);
    return false;
  }
  VkMemoryAllocateInfo allocInfo{};
  allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
  allocInfo.allocationSize = memReq.size;
  allocInfo.memoryTypeIndex = inputMemType;
  VkResult allocInput =
      vkAllocateMemory(device, &allocInfo, nullptr, &inputMemory);
  if (allocInput != VK_SUCCESS) {
    std::cerr << "vkAllocateMemory(input) failed: "
              << vkResultToString(allocInput) << '\n';
    cleanupQueue(commandBuffer, commandPool, descriptorPool, pipeline,
                 pipelineLayout, descriptorSetLayout, shaderModule, inputBuffer,
                 inputMemory, outputBuffer, outputMemory, fence);
    return false;
  }
  result = vkBindBufferMemory(device, inputBuffer, inputMemory, 0);
  if (result != VK_SUCCESS) {
    std::cerr << "vkBindBufferMemory(input) failed: "
              << vkResultToString(result) << '\n';
    cleanupQueue(commandBuffer, commandPool, descriptorPool, pipeline,
                 pipelineLayout, descriptorSetLayout, shaderModule, inputBuffer,
                 inputMemory, outputBuffer, outputMemory, fence);
    return false;
  }

  bufferInfo.size = sizeof(output);
  bufferInfo.usage =
      VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
  VkResult outBufferResult =
      vkCreateBuffer(device, &bufferInfo, nullptr, &outputBuffer);
  if (outBufferResult != VK_SUCCESS) {
    std::cerr << "vkCreateBuffer(output) failed: "
              << vkResultToString(outBufferResult) << '\n';
    cleanupQueue(commandBuffer, commandPool, descriptorPool, pipeline,
                 pipelineLayout, descriptorSetLayout, shaderModule, inputBuffer,
                 inputMemory, outputBuffer, outputMemory, fence);
    return false;
  }

  vkGetBufferMemoryRequirements(device, outputBuffer, &memReq);
  const auto outputMemType =
      selectMemoryType(selected, memReq, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT);
  if (outputMemType == std::numeric_limits<std::uint32_t>::max()) {
    std::cerr << "No host-visible memory type for output buffer." << '\n';
    cleanupQueue(commandBuffer, commandPool, descriptorPool, pipeline,
                 pipelineLayout, descriptorSetLayout, shaderModule, inputBuffer,
                 inputMemory, outputBuffer, outputMemory, fence);
    return false;
  }
  allocInfo.allocationSize = memReq.size;
  allocInfo.memoryTypeIndex = outputMemType;
  VkResult allocOutput =
      vkAllocateMemory(device, &allocInfo, nullptr, &outputMemory);
  if (allocOutput != VK_SUCCESS) {
    std::cerr << "vkAllocateMemory(output) failed: "
              << vkResultToString(allocOutput) << '\n';
    cleanupQueue(commandBuffer, commandPool, descriptorPool, pipeline,
                 pipelineLayout, descriptorSetLayout, shaderModule, inputBuffer,
                 inputMemory, outputBuffer, outputMemory, fence);
    return false;
  }
  result = vkBindBufferMemory(device, outputBuffer, outputMemory, 0);
  if (result != VK_SUCCESS) {
    std::cerr << "vkBindBufferMemory(output) failed: "
              << vkResultToString(result) << '\n';
    cleanupQueue(commandBuffer, commandPool, descriptorPool, pipeline,
                 pipelineLayout, descriptorSetLayout, shaderModule, inputBuffer,
                 inputMemory, outputBuffer, outputMemory, fence);
    return false;
  }

  VkMemoryPropertyFlags inputProps{};
  VkMemoryPropertyFlags outputProps{};
  VkPhysicalDeviceMemoryProperties memProps{};
  vkGetPhysicalDeviceMemoryProperties(selected, &memProps);
  inputProps = memProps.memoryTypes[inputMemType].propertyFlags;
  outputProps = memProps.memoryTypes[outputMemType].propertyFlags;

  void *inputPtr = nullptr;
  void *outputPtr = nullptr;
  result = vkMapMemory(device, inputMemory, 0, bufferInfo.size, 0, &inputPtr);
  if (result != VK_SUCCESS) {
    std::cerr << "vkMapMemory(input) failed: " << vkResultToString(result)
              << '\n';
    cleanupQueue(commandBuffer, commandPool, descriptorPool, pipeline,
                 pipelineLayout, descriptorSetLayout, shaderModule, inputBuffer,
                 inputMemory, outputBuffer, outputMemory, fence);
    return false;
  }
  result = vkMapMemory(device, outputMemory, 0, bufferInfo.size, 0, &outputPtr);
  if (result != VK_SUCCESS) {
    std::cerr << "vkMapMemory(output) failed: " << vkResultToString(result)
              << '\n';
    cleanupQueue(commandBuffer, commandPool, descriptorPool, pipeline,
                 pipelineLayout, descriptorSetLayout, shaderModule, inputBuffer,
                 inputMemory, outputBuffer, outputMemory, fence);
    return false;
  }
  std::memcpy(inputPtr, inputData, sizeof(inputData));
  if ((inputProps & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) == 0u) {
    VkMappedMemoryRange range{};
    range.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
    range.memory = inputMemory;
    range.offset = 0;
    range.size = VK_WHOLE_SIZE;
    vkFlushMappedMemoryRanges(device, 1, &range);
  }

  VkShaderModuleCreateInfo shaderInfo{};
  shaderInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
  shaderInfo.codeSize = spirv.size() * sizeof(std::uint32_t);
  shaderInfo.pCode = spirv.data();
  result = vkCreateShaderModule(device, &shaderInfo, nullptr, &shaderModule);
  if (result != VK_SUCCESS) {
    std::cerr << "vkCreateShaderModule failed: " << vkResultToString(result)
              << '\n';
    cleanupQueue(commandBuffer, commandPool, descriptorPool, pipeline,
                 pipelineLayout, descriptorSetLayout, shaderModule, inputBuffer,
                 inputMemory, outputBuffer, outputMemory, fence);
    return false;
  }

  VkDescriptorSetLayoutBinding bindings[2]{};
  bindings[0].binding = 0;
  bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  bindings[0].descriptorCount = 1;
  bindings[0].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
  bindings[1].binding = 1;
  bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  bindings[1].descriptorCount = 1;
  bindings[1].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

  VkDescriptorSetLayoutCreateInfo dsetLayoutInfo{};
  dsetLayoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
  dsetLayoutInfo.bindingCount = 2;
  dsetLayoutInfo.pBindings = bindings;
  result = vkCreateDescriptorSetLayout(device, &dsetLayoutInfo, nullptr,
                                       &descriptorSetLayout);
  if (result != VK_SUCCESS) {
    std::cerr << "vkCreateDescriptorSetLayout failed: "
              << vkResultToString(result) << '\n';
    cleanupQueue(commandBuffer, commandPool, descriptorPool, pipeline,
                 pipelineLayout, descriptorSetLayout, shaderModule, inputBuffer,
                 inputMemory, outputBuffer, outputMemory, fence);
    return false;
  }

  VkPipelineLayoutCreateInfo pipelineLayoutInfo{};
  pipelineLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
  pipelineLayoutInfo.setLayoutCount = 1;
  pipelineLayoutInfo.pSetLayouts = &descriptorSetLayout;
  result = vkCreatePipelineLayout(device, &pipelineLayoutInfo, nullptr,
                                  &pipelineLayout);
  if (result != VK_SUCCESS) {
    std::cerr << "vkCreatePipelineLayout failed: " << vkResultToString(result)
              << '\n';
    cleanupQueue(commandBuffer, commandPool, descriptorPool, pipeline,
                 pipelineLayout, descriptorSetLayout, shaderModule, inputBuffer,
                 inputMemory, outputBuffer, outputMemory, fence);
    return false;
  }

  VkComputePipelineCreateInfo pipelineInfo{};
  VkPipelineShaderStageCreateInfo stageInfo{};
  stageInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  stageInfo.stage = VK_SHADER_STAGE_COMPUTE_BIT;
  stageInfo.module = shaderModule;
  stageInfo.pName = "main";
  pipelineInfo.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
  pipelineInfo.stage = stageInfo;
  pipelineInfo.layout = pipelineLayout;
  result = vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &pipelineInfo,
                                    nullptr, &pipeline);
  if (result != VK_SUCCESS) {
    std::cerr << "vkCreateComputePipelines failed: " << vkResultToString(result)
              << '\n';
    cleanupQueue(commandBuffer, commandPool, descriptorPool, pipeline,
                 pipelineLayout, descriptorSetLayout, shaderModule, inputBuffer,
                 inputMemory, outputBuffer, outputMemory, fence);
    return false;
  }

  VkDescriptorPoolSize poolSize{};
  poolSize.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  poolSize.descriptorCount = 2;

  VkDescriptorPoolCreateInfo poolInfo{};
  poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
  poolInfo.poolSizeCount = 1;
  poolInfo.pPoolSizes = &poolSize;
  poolInfo.maxSets = 1;
  result = vkCreateDescriptorPool(device, &poolInfo, nullptr, &descriptorPool);
  if (result != VK_SUCCESS) {
    std::cerr << "vkCreateDescriptorPool failed: " << vkResultToString(result)
              << '\n';
    cleanupQueue(commandBuffer, commandPool, descriptorPool, pipeline,
                 pipelineLayout, descriptorSetLayout, shaderModule, inputBuffer,
                 inputMemory, outputBuffer, outputMemory, fence);
    return false;
  }

  VkDescriptorSetAllocateInfo dsetAlloc{};
  dsetAlloc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
  dsetAlloc.descriptorPool = descriptorPool;
  dsetAlloc.descriptorSetCount = 1;
  dsetAlloc.pSetLayouts = &descriptorSetLayout;
  result = vkAllocateDescriptorSets(device, &dsetAlloc, &descriptorSet);
  if (result != VK_SUCCESS) {
    std::cerr << "vkAllocateDescriptorSets failed: " << vkResultToString(result)
              << '\n';
    cleanupQueue(commandBuffer, commandPool, descriptorPool, pipeline,
                 pipelineLayout, descriptorSetLayout, shaderModule, inputBuffer,
                 inputMemory, outputBuffer, outputMemory, fence);
    return false;
  }

  VkDescriptorBufferInfo inputBindingInfo{};
  inputBindingInfo.buffer = inputBuffer;
  inputBindingInfo.offset = 0;
  inputBindingInfo.range = sizeof(inputData);
  VkDescriptorBufferInfo outputBindingInfo{};
  outputBindingInfo.buffer = outputBuffer;
  outputBindingInfo.offset = 0;
  outputBindingInfo.range = sizeof(output);
  VkWriteDescriptorSet writes[2]{};
  writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
  writes[0].dstSet = descriptorSet;
  writes[0].dstBinding = 0;
  writes[0].descriptorCount = 1;
  writes[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  writes[0].pBufferInfo = &inputBindingInfo;
  writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
  writes[1].dstSet = descriptorSet;
  writes[1].dstBinding = 1;
  writes[1].descriptorCount = 1;
  writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  writes[1].pBufferInfo = &outputBindingInfo;
  vkUpdateDescriptorSets(device, 2, writes, 0, nullptr);

  VkCommandPoolCreateInfo cmdPoolInfo{};
  cmdPoolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
  cmdPoolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
  cmdPoolInfo.queueFamilyIndex = selectedQueueFamily;
  result = vkCreateCommandPool(device, &cmdPoolInfo, nullptr, &commandPool);
  if (result != VK_SUCCESS) {
    std::cerr << "vkCreateCommandPool failed: " << vkResultToString(result)
              << '\n';
    cleanupQueue(commandBuffer, commandPool, descriptorPool, pipeline,
                 pipelineLayout, descriptorSetLayout, shaderModule, inputBuffer,
                 inputMemory, outputBuffer, outputMemory, fence);
    return false;
  }

  VkCommandBufferAllocateInfo cmdAlloc{};
  cmdAlloc.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
  cmdAlloc.commandPool = commandPool;
  cmdAlloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  cmdAlloc.commandBufferCount = 1;
  result = vkAllocateCommandBuffers(device, &cmdAlloc, &commandBuffer);
  if (result != VK_SUCCESS) {
    std::cerr << "vkAllocateCommandBuffers failed: " << vkResultToString(result)
              << '\n';
    cleanupQueue(commandBuffer, commandPool, descriptorPool, pipeline,
                 pipelineLayout, descriptorSetLayout, shaderModule, inputBuffer,
                 inputMemory, outputBuffer, outputMemory, fence);
    return false;
  }

  VkCommandBufferBeginInfo beginInfo{};
  beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
  beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  result = vkBeginCommandBuffer(commandBuffer, &beginInfo);
  if (result != VK_SUCCESS) {
    std::cerr << "vkBeginCommandBuffer failed: " << vkResultToString(result)
              << '\n';
    cleanupQueue(commandBuffer, commandPool, descriptorPool, pipeline,
                 pipelineLayout, descriptorSetLayout, shaderModule, inputBuffer,
                 inputMemory, outputBuffer, outputMemory, fence);
    return false;
  }

  VkBufferMemoryBarrier hostToDevice[2]{};
  hostToDevice[0].sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
  hostToDevice[0].srcAccessMask = VK_ACCESS_HOST_WRITE_BIT;
  hostToDevice[0].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
  hostToDevice[0].buffer = inputBuffer;
  hostToDevice[0].offset = 0;
  hostToDevice[0].size = sizeof(inputData);
  hostToDevice[1].sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
  hostToDevice[1].srcAccessMask = VK_ACCESS_HOST_WRITE_BIT;
  hostToDevice[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
  hostToDevice[1].buffer = outputBuffer;
  hostToDevice[1].offset = 0;
  hostToDevice[1].size = sizeof(output);
  vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_HOST_BIT,
                       VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 2,
                       hostToDevice, 0, nullptr);
  vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
  vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE,
                          pipelineLayout, 0, 1, &descriptorSet, 0, nullptr);
  vkCmdDispatch(commandBuffer, 1, 1, 1);

  VkBufferMemoryBarrier deviceToHost{};
  deviceToHost.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
  deviceToHost.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
  deviceToHost.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
  deviceToHost.buffer = outputBuffer;
  deviceToHost.offset = 0;
  deviceToHost.size = sizeof(output);
  vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                       VK_PIPELINE_STAGE_HOST_BIT, 0, 0, nullptr, 1,
                       &deviceToHost, 0, nullptr);
  result = vkEndCommandBuffer(commandBuffer);
  if (result != VK_SUCCESS) {
    std::cerr << "vkEndCommandBuffer failed: " << vkResultToString(result)
              << '\n';
    cleanupQueue(commandBuffer, commandPool, descriptorPool, pipeline,
                 pipelineLayout, descriptorSetLayout, shaderModule, inputBuffer,
                 inputMemory, outputBuffer, outputMemory, fence);
    return false;
  }

  VkSubmitInfo submitInfo{};
  submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
  submitInfo.commandBufferCount = 1;
  submitInfo.pCommandBuffers = &commandBuffer;
  VkFenceCreateInfo fenceInfo{};
  fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
  result = vkCreateFence(device, &fenceInfo, nullptr, &fence);
  if (result != VK_SUCCESS) {
    std::cerr << "vkCreateFence failed: " << vkResultToString(result) << '\n';
    cleanupQueue(commandBuffer, commandPool, descriptorPool, pipeline,
                 pipelineLayout, descriptorSetLayout, shaderModule, inputBuffer,
                 inputMemory, outputBuffer, outputMemory, fence);
    return false;
  }

  result = vkQueueSubmit(queue, 1, &submitInfo, fence);
  if (result != VK_SUCCESS) {
    std::cerr << "vkQueueSubmit failed: " << vkResultToString(result) << '\n';
    cleanupQueue(commandBuffer, commandPool, descriptorPool, pipeline,
                 pipelineLayout, descriptorSetLayout, shaderModule, inputBuffer,
                 inputMemory, outputBuffer, outputMemory, fence);
    return false;
  }

  result = vkWaitForFences(device, 1, &fence, VK_TRUE,
                           10'000'000'000ULL); // 10s
  if (result != VK_SUCCESS) {
    std::cerr << "vkWaitForFences failed: " << vkResultToString(result) << '\n';
    cleanupQueue(commandBuffer, commandPool, descriptorPool, pipeline,
                 pipelineLayout, descriptorSetLayout, shaderModule, inputBuffer,
                 inputMemory, outputBuffer, outputMemory, fence);
    return false;
  }

  if ((outputProps & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) == 0u) {
    VkMappedMemoryRange range{};
    range.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
    range.memory = outputMemory;
    range.offset = 0;
    range.size = VK_WHOLE_SIZE;
    result = vkInvalidateMappedMemoryRanges(device, 1, &range);
    if (result != VK_SUCCESS) {
      std::cerr << "vkInvalidateMappedMemoryRanges failed: "
                << vkResultToString(result) << '\n';
    }
  }
  std::memcpy(output.data(), outputPtr, sizeof(output));

  bool match = true;
  for (std::size_t i = 0; i < expected.size(); ++i) {
    const std::uint32_t expectedBits =
        std::bit_cast<std::uint32_t>(expected[i]);
    const std::uint32_t actualBits = std::bit_cast<std::uint32_t>(output[i]);
    const bool isMatch = nearlyEqual(expected[i], output[i]);
    std::cout
        << std::fixed << std::setprecision(7) << "[Smoke] idx=" << i
        << " expected=" << expected[i] << " actual=" << output[i] << " ulp="
        << static_cast<long long>(
               orderedFloatBits(expected[i]) > orderedFloatBits(output[i])
                   ? static_cast<long long>(orderedFloatBits(expected[i]) -
                                            orderedFloatBits(output[i]))
                   : static_cast<long long>(orderedFloatBits(output[i]) -
                                            orderedFloatBits(expected[i])))
        << " exact=" << (expectedBits == actualBits ? "yes" : "no")
        << " ok=" << (isMatch ? "yes" : "no") << '\n';
    if (!isMatch) {
      match = false;
    }
  }

  ok = match;
  std::cout << (ok ? "[Smoke] PASS\n" : "[Smoke] FAIL\n");

  cleanupQueue(commandBuffer, commandPool, descriptorPool, pipeline,
               pipelineLayout, descriptorSetLayout, shaderModule, inputBuffer,
               inputMemory, outputBuffer, outputMemory, fence);
  return ok;
}

} // namespace

int main() { return runComputeSmoke() ? 0 : 1; }
