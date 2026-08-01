// Minimal Vulkan primitives verification slice.
//
// The program validates these GPU kernels on a real Vulkan device:
// - fill (uniform scalar fill)
// - copy (buffer copy)
// - transform (y = 2x + 1)
// - reduction (sum/min/max)
// - exclusive scan (int32)
//
// Exit code:
//   0 -> all tested cases pass
//   1 -> one or more failures

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
#include <numeric>
#include <string>
#include <string_view>
#include <vector>

#include <vulkan/vulkan.h>

#ifndef VIENNAPS_VULKAN_PRIMITIVES_SPV_PATH
#error "VIENNAPS_VULKAN_PRIMITIVES_SPV_PATH must be defined when building "
"viennaps-vulkan-primitives-smoke."
#endif

    namespace {

  constexpr std::string_view kShaderSpvPath =
      VIENNAPS_VULKAN_PRIMITIVES_SPV_PATH;
  constexpr std::uint32_t kWorkgroupSize = 256u;
  constexpr std::array<std::size_t, 5> kTestLengths = {0, 1, 16, 257, 65'535};
  constexpr float kFloatSentinel = 12.345678f;
  constexpr int kIntSentinel = 0x5A5A5A5A;
  constexpr float kFillValue = 3.75f;

  struct PushConstants {
    std::uint32_t length;
    float scalar;
  };

  struct GpuBuffer {
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    void *mapped = nullptr;
    VkDeviceSize size = 0;
    bool hostCoherent = false;
  };

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
    default:
      return "VK_UNKNOWN_ERROR";
    }
  }

  [[nodiscard]] std::string_view vkDeviceTypeName(
      const VkPhysicalDeviceType type) {
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

  [[nodiscard]] std::vector<std::uint8_t> readBinaryFile(
      const std::string_view path) {
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

  [[nodiscard]] std::vector<std::uint32_t> readSpirv(
      const std::string_view path) {
    const auto bytes = readBinaryFile(path);
    if (bytes.empty() || (bytes.size() % sizeof(std::uint32_t) != 0u)) {
      return {};
    }
    const auto words = bytes.size() / sizeof(std::uint32_t);
    const auto *raw = reinterpret_cast<const std::uint32_t *>(bytes.data());
    return std::vector<std::uint32_t>(raw, raw + words);
  }

  [[nodiscard]] bool hasComputeQueueFamily(const VkPhysicalDevice device,
                                           std::uint32_t &queueFamilyIndex,
                                           bool &hasDedicatedQueue) {
    std::uint32_t queueFamilyCount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(device, &queueFamilyCount,
                                             nullptr);
    if (queueFamilyCount == 0) {
      return false;
    }
    std::vector<VkQueueFamilyProperties> families(queueFamilyCount);
    vkGetPhysicalDeviceQueueFamilyProperties(device, &queueFamilyCount,
                                             families.data());

    queueFamilyIndex = 0;
    hasDedicatedQueue = false;
    bool found = false;
    bool dedicated = false;
    for (std::uint32_t i = 0; i < queueFamilyCount; ++i) {
      const auto &family = families[i];
      if ((family.queueFlags & VK_QUEUE_COMPUTE_BIT) == 0u) {
        continue;
      }
      if (!found) {
        queueFamilyIndex = i;
        found = true;
      }
      if ((family.queueFlags & VK_QUEUE_GRAPHICS_BIT) == 0u) {
        queueFamilyIndex = i;
        dedicated = true;
        hasDedicatedQueue = true;
        break;
      }
    }
    if (!found) {
      return false;
    }
    if (!dedicated) {
      hasDedicatedQueue = false;
    }
    return true;
  }

  [[nodiscard]] std::uint32_t selectMemoryType(
      const VkPhysicalDevice device, const VkMemoryRequirements &requirements,
      const VkMemoryPropertyFlags requiredFlags) {
    VkPhysicalDeviceMemoryProperties memProps{};
    vkGetPhysicalDeviceMemoryProperties(device, &memProps);
    std::uint32_t selected = std::numeric_limits<std::uint32_t>::max();
    for (std::uint32_t i = 0; i < memProps.memoryTypeCount; ++i) {
      if ((requirements.memoryTypeBits & (1u << i)) == 0u) {
        continue;
      }
      if ((memProps.memoryTypes[i].propertyFlags & requiredFlags) !=
          requiredFlags) {
        continue;
      }
      selected = i;
      if ((memProps.memoryTypes[i].propertyFlags &
           VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0u) {
        return i;
      }
    }
    return selected;
  }

  [[nodiscard]] std::uint32_t orderedFloatBits(const float value) {
    const auto bits = std::bit_cast<std::uint32_t>(value);
    return (bits & 0x80000000u) ? ~bits : (bits ^ 0x80000000u);
  }

  [[nodiscard]] std::uint32_t floatUlpDistance(const float lhs,
                                               const float rhs) {
    const std::uint32_t lhsBits = orderedFloatBits(lhs);
    const std::uint32_t rhsBits = orderedFloatBits(rhs);
    return lhsBits > rhsBits ? lhsBits - rhsBits : rhsBits - lhsBits;
  }

  [[nodiscard]] bool nearlyEqual(const float lhs, const float rhs) {
    if (lhs == rhs) {
      return true;
    }
    const float diff = std::fabs(lhs - rhs);
    const float absMax = std::max(std::fabs(lhs), std::fabs(rhs));
    const float relative = absMax > 0.0f ? diff / absMax : diff;
    const std::uint32_t ulpDiff = floatUlpDistance(lhs, rhs);
    return diff <= 1.0e-6f || relative <= 1.0e-6f || ulpDiff <= 16u;
  }

  [[nodiscard]] bool flushMappedRange(const VkDevice device,
                                      const GpuBuffer &buffer) {
    if (buffer.hostCoherent) {
      return true;
    }
    VkMappedMemoryRange range{};
    range.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
    range.memory = buffer.memory;
    range.offset = 0;
    range.size = buffer.size;
    const VkResult result = vkFlushMappedMemoryRanges(device, 1, &range);
    return result == VK_SUCCESS;
  }

  [[nodiscard]] bool invalidateMappedRange(const VkDevice device,
                                           const GpuBuffer &buffer) {
    if (buffer.hostCoherent) {
      return true;
    }
    VkMappedMemoryRange range{};
    range.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
    range.memory = buffer.memory;
    range.offset = 0;
    range.size = buffer.size;
    const VkResult result = vkInvalidateMappedMemoryRanges(device, 1, &range);
    return result == VK_SUCCESS;
  }

  bool createHostBuffer(const VkPhysicalDevice physicalDevice,
                        const VkDevice device, const VkDeviceSize size,
                        const VkBufferUsageFlags usage, GpuBuffer &buffer) {
    buffer.size = size;
    if (buffer.size == 0) {
      buffer.size = 1;
    }

    VkBufferCreateInfo bufferInfo{};
    bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufferInfo.size = buffer.size;
    bufferInfo.usage = usage;
    bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VkResult result =
        vkCreateBuffer(device, &bufferInfo, nullptr, &buffer.buffer);
    if (result != VK_SUCCESS) {
      std::cerr << "vkCreateBuffer failed: " << vkResultToString(result)
                << '\n';
      return false;
    }

    VkMemoryRequirements requirements{};
    vkGetBufferMemoryRequirements(device, buffer.buffer, &requirements);
    const auto memoryType =
        selectMemoryType(physicalDevice, requirements,
                         VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                             VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    const auto fallbackMemoryType = selectMemoryType(
        physicalDevice, requirements, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT);
    std::uint32_t selectedMemoryType = memoryType;
    if (selectedMemoryType == std::numeric_limits<std::uint32_t>::max()) {
      selectedMemoryType = fallbackMemoryType;
    }
    if (selectedMemoryType == std::numeric_limits<std::uint32_t>::max()) {
      std::cerr << "No host-visible memory type found." << '\n';
      return false;
    }

    VkMemoryAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.allocationSize = requirements.size;
    allocInfo.memoryTypeIndex = selectedMemoryType;
    result = vkAllocateMemory(device, &allocInfo, nullptr, &buffer.memory);
    if (result != VK_SUCCESS) {
      std::cerr << "vkAllocateMemory failed: " << vkResultToString(result)
                << '\n';
      return false;
    }
    result = vkBindBufferMemory(device, buffer.buffer, buffer.memory, 0);
    if (result != VK_SUCCESS) {
      std::cerr << "vkBindBufferMemory failed: " << vkResultToString(result)
                << '\n';
      return false;
    }

    VkMemoryPropertyFlags chosenFlags{};
    VkPhysicalDeviceMemoryProperties memProps{};
    vkGetPhysicalDeviceMemoryProperties(physicalDevice, &memProps);
    chosenFlags = memProps.memoryTypes[selectedMemoryType].propertyFlags;
    buffer.hostCoherent =
        (chosenFlags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0u;
    result =
        vkMapMemory(device, buffer.memory, 0, buffer.size, 0, &buffer.mapped);
    if (result != VK_SUCCESS) {
      std::cerr << "vkMapMemory failed: " << vkResultToString(result) << '\n';
      return false;
    }
    return true;
  }

  bool writeBufferBytes(const VkDevice device, const GpuBuffer &buffer,
                        const void *data, const std::size_t bytes) {
    if (bytes > static_cast<std::size_t>(buffer.size)) {
      return false;
    }
    std::memcpy(buffer.mapped, data, bytes);
    std::fill_n(static_cast<char *>(buffer.mapped) + bytes,
                static_cast<std::size_t>(buffer.size) - bytes, '\0');
    return flushMappedRange(device, buffer);
  }

  void readBufferBytes(const GpuBuffer &buffer, void *out,
                       const std::size_t bytes) {
    std::memcpy(out, buffer.mapped, bytes);
  }

  bool dispatchKernel(
      const VkDevice device, const VkQueue queue,
      const VkCommandBuffer commandBuffer, const VkPipeline pipeline,
      const VkPipelineLayout pipelineLayout,
      const VkDescriptorSet descriptorSet, const PushConstants params,
      const std::uint32_t dispatchX, const VkBuffer inFloatBuffer,
      const VkBuffer outFloatBuffer, const VkBuffer inIntBuffer,
      const VkBuffer outIntBuffer, const VkBuffer reductionBuffer) {
    vkResetCommandBuffer(commandBuffer, 0);
    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    VkResult result = vkBeginCommandBuffer(commandBuffer, &beginInfo);
    if (result != VK_SUCCESS) {
      std::cerr << "vkBeginCommandBuffer failed: " << vkResultToString(result)
                << '\n';
      return false;
    }

    std::array<VkBufferMemoryBarrier, 4> toCompute{};
    toCompute[0].sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
    toCompute[0].srcAccessMask = VK_ACCESS_HOST_WRITE_BIT;
    toCompute[0].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    toCompute[0].buffer = inFloatBuffer;
    toCompute[0].offset = 0;
    toCompute[0].size = VK_WHOLE_SIZE;
    toCompute[1].sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
    toCompute[1].srcAccessMask = VK_ACCESS_HOST_WRITE_BIT;
    toCompute[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    toCompute[1].buffer = inIntBuffer;
    toCompute[1].offset = 0;
    toCompute[1].size = VK_WHOLE_SIZE;
    toCompute[2].sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
    toCompute[2].srcAccessMask =
        VK_ACCESS_HOST_WRITE_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    toCompute[2].dstAccessMask =
        VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    toCompute[2].buffer = outFloatBuffer;
    toCompute[2].offset = 0;
    toCompute[2].size = VK_WHOLE_SIZE;
    toCompute[3].sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
    toCompute[3].srcAccessMask =
        VK_ACCESS_HOST_WRITE_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    toCompute[3].dstAccessMask =
        VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    toCompute[3].buffer = outIntBuffer;
    toCompute[3].offset = 0;
    toCompute[3].size = VK_WHOLE_SIZE;
    vkCmdPipelineBarrier(commandBuffer,
                         VK_PIPELINE_STAGE_HOST_BIT |
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 4,
                         toCompute.data(), 0, nullptr);

    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
    vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE,
                            pipelineLayout, 0, 1, &descriptorSet, 0, nullptr);
    vkCmdPushConstants(commandBuffer, pipelineLayout,
                       VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(PushConstants),
                       &params);
    vkCmdDispatch(commandBuffer, dispatchX, 1, 1);

    std::array<VkBufferMemoryBarrier, 3> toHost{};
    toHost[0].sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
    toHost[0].srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    toHost[0].dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    toHost[0].buffer = outFloatBuffer;
    toHost[0].offset = 0;
    toHost[0].size = VK_WHOLE_SIZE;
    toHost[1].sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
    toHost[1].srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    toHost[1].dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    toHost[1].buffer = outIntBuffer;
    toHost[1].offset = 0;
    toHost[1].size = VK_WHOLE_SIZE;
    toHost[2].sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
    toHost[2].srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    toHost[2].dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    toHost[2].buffer = reductionBuffer;
    toHost[2].offset = 0;
    toHost[2].size = VK_WHOLE_SIZE;
    vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_HOST_BIT, 0, 0, nullptr, 3,
                         toHost.data(), 0, nullptr);

    result = vkEndCommandBuffer(commandBuffer);
    if (result != VK_SUCCESS) {
      std::cerr << "vkEndCommandBuffer failed: " << vkResultToString(result)
                << '\n';
      return false;
    }

    VkFence fence{};
    VkFenceCreateInfo fenceInfo{};
    fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    result = vkCreateFence(device, &fenceInfo, nullptr, &fence);
    if (result != VK_SUCCESS) {
      std::cerr << "vkCreateFence failed: " << vkResultToString(result) << '\n';
      return false;
    }
    VkSubmitInfo submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &commandBuffer;
    result = vkQueueSubmit(queue, 1, &submit, fence);
    if (result != VK_SUCCESS) {
      std::cerr << "vkQueueSubmit failed: " << vkResultToString(result) << '\n';
      vkDestroyFence(device, fence, nullptr);
      return false;
    }
    result = vkWaitForFences(device, 1, &fence, VK_TRUE, 10'000'000'000ULL);
    vkDestroyFence(device, fence, nullptr);
    if (result != VK_SUCCESS) {
      std::cerr << "vkWaitForFences failed: " << vkResultToString(result)
                << '\n';
      return false;
    }
    return true;
  }

  bool compareFloatArray(const std::string_view label, const float *actual,
                         const float *expected, const std::size_t activeLength,
                         const std::size_t totalLength, const float sentinel,
                         std::size_t &mismatchCount, float &maxAbsErr,
                         float &maxRelErr, std::uint32_t &maxUlp) {
    mismatchCount = 0;
    maxAbsErr = 0.0f;
    maxRelErr = 0.0f;
    maxUlp = 0u;
    for (std::size_t i = 0; i < activeLength; ++i) {
      const float lhs = actual[i];
      const float rhs = expected[i];
      const float diff = std::fabs(lhs - rhs);
      const float absMax = std::max(std::fabs(lhs), std::fabs(rhs));
      const float rel = absMax > 0.0f ? diff / absMax : diff;
      const std::uint32_t ulp = floatUlpDistance(lhs, rhs);
      if (!nearlyEqual(lhs, rhs)) {
        ++mismatchCount;
      }
      maxAbsErr = std::max(maxAbsErr, diff);
      maxRelErr = std::max(maxRelErr, rel);
      maxUlp = std::max(maxUlp, ulp);
    }
    for (std::size_t i = activeLength; i < totalLength; ++i) {
      if (!nearlyEqual(actual[i], sentinel)) {
        ++mismatchCount;
        const float lhs = actual[i];
        const float rhs = sentinel;
        const float diff = std::fabs(lhs - rhs);
        const float absMax = std::max(std::fabs(lhs), std::fabs(rhs));
        const float rel = absMax > 0.0f ? diff / absMax : diff;
        const std::uint32_t ulp = floatUlpDistance(lhs, rhs);
        maxAbsErr = std::max(maxAbsErr, diff);
        maxRelErr = std::max(maxRelErr, rel);
        maxUlp = std::max(maxUlp, ulp);
      }
    }
    std::cout << "[PrimitiveSmoke] " << label << " [active=" << activeLength
              << ", total=" << totalLength << "] mismatch=" << mismatchCount
              << " max_abs=" << maxAbsErr << " max_rel=" << maxRelErr
              << " max_ulp=" << maxUlp << '\n';
    return mismatchCount == 0;
  }

  bool compareIntArray(const std::string_view label, const int *actual,
                       const int *expected, const std::size_t activeLength,
                       const std::size_t totalLength, const int sentinel,
                       std::size_t &mismatchCount) {
    mismatchCount = 0;
    for (std::size_t i = 0; i < activeLength; ++i) {
      if (actual[i] != expected[i]) {
        ++mismatchCount;
      }
    }
    for (std::size_t i = activeLength; i < totalLength; ++i) {
      if (actual[i] != sentinel) {
        ++mismatchCount;
      }
    }
    std::cout << "[PrimitiveSmoke] " << label << " mismatch=" << mismatchCount
              << '\n';
    return mismatchCount == 0;
  }

  bool compareScalar(const std::string_view label, const float actual,
                     const float expected, float &maxAbsErr, float &maxRelErr,
                     std::uint32_t &maxUlp) {
    const float diff = std::fabs(actual - expected);
    const float absMax = std::max(std::fabs(actual), std::fabs(expected));
    maxAbsErr = diff;
    maxRelErr = absMax > 0.0f ? diff / absMax : diff;
    maxUlp = floatUlpDistance(actual, expected);
    const bool ok = nearlyEqual(actual, expected);
    std::cout << "[PrimitiveSmoke] " << label << " actual=" << actual
              << " expected=" << expected << " diff=" << diff
              << " rel=" << maxRelErr << " ulp=" << maxUlp
              << " ok=" << (ok ? "yes" : "no") << '\n';
    return ok;
  }

  bool runPrimitiveSmoke() {
    const auto shaderSpv = readSpirv(kShaderSpvPath);
    if (shaderSpv.empty()) {
      std::cerr << "Failed to load SPIR-V module: " << kShaderSpvPath << '\n';
      return false;
    }

    VkApplicationInfo appInfo{};
    appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    appInfo.pApplicationName = "ViennaPS Vulkan Primitives Smoke";
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

    VkInstance localInstance = instance;
    auto destroyInstance = [&localInstance]() {
      if (localInstance != VK_NULL_HANDLE) {
        vkDestroyInstance(localInstance, nullptr);
        localInstance = VK_NULL_HANDLE;
      }
    };

    std::uint32_t deviceCount = 0;
    result = vkEnumeratePhysicalDevices(instance, &deviceCount, nullptr);
    if (result != VK_SUCCESS || deviceCount == 0) {
      std::cerr << "No Vulkan physical devices found." << '\n';
      destroyInstance();
      return false;
    }

    std::vector<VkPhysicalDevice> devices(deviceCount);
    result = vkEnumeratePhysicalDevices(instance, &deviceCount, devices.data());
    if (result != VK_SUCCESS) {
      std::cerr << "Failed to enumerate Vulkan devices." << '\n';
      destroyInstance();
      return false;
    }

    VkPhysicalDevice selectedDevice = VK_NULL_HANDLE;
    VkPhysicalDeviceProperties selectedProps{};
    std::uint32_t selectedQueueFamily = 0;
    bool selectedHasDedicatedQueue = false;
    for (const auto device : devices) {
      std::uint32_t queueFamily = 0;
      bool dedicated = false;
      if (!hasComputeQueueFamily(device, queueFamily, dedicated)) {
        continue;
      }
      vkGetPhysicalDeviceProperties(device, &selectedProps);
      selectedDevice = device;
      selectedQueueFamily = queueFamily;
      selectedHasDedicatedQueue = dedicated;
      break;
    }
    if (selectedDevice == VK_NULL_HANDLE) {
      std::cerr << "No compute-capable Vulkan physical device found." << '\n';
      destroyInstance();
      return false;
    }

    std::cout << "[PrimitiveSmoke] device: " << selectedProps.deviceName
              << '\n';
    std::cout << "[PrimitiveSmoke] device type: "
              << vkDeviceTypeName(selectedProps.deviceType) << '\n';
    std::cout << "[PrimitiveSmoke] queue family: " << selectedQueueFamily
              << ", dedicated compute: "
              << (selectedHasDedicatedQueue ? "yes" : "no") << '\n';

    const float queuePriority = 1.0f;
    VkDeviceQueueCreateInfo queueCreateInfo{};
    queueCreateInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    queueCreateInfo.queueFamilyIndex = selectedQueueFamily;
    queueCreateInfo.queueCount = 1;
    queueCreateInfo.pQueuePriorities = &queuePriority;
    VkDeviceCreateInfo deviceInfo{};
    deviceInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    deviceInfo.queueCreateInfoCount = 1;
    deviceInfo.pQueueCreateInfos = &queueCreateInfo;

    VkDevice device{};
    result = vkCreateDevice(selectedDevice, &deviceInfo, nullptr, &device);
    if (result != VK_SUCCESS) {
      std::cerr << "vkCreateDevice failed: " << vkResultToString(result)
                << '\n';
      destroyInstance();
      return false;
    }

    VkDevice localDevice = device;
    auto destroyDevice = [&localDevice]() {
      if (localDevice != VK_NULL_HANDLE) {
        vkDestroyDevice(localDevice, nullptr);
        localDevice = VK_NULL_HANDLE;
      }
    };

    VkQueue queue{};
    vkGetDeviceQueue(device, selectedQueueFamily, 0, &queue);

    std::array<std::byte, 4> _pad{};

    VkShaderModule shaderModule{};
    VkShaderModuleCreateInfo shaderInfo{};
    shaderInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    shaderInfo.codeSize = shaderSpv.size() * sizeof(std::uint32_t);
    shaderInfo.pCode = shaderSpv.data();
    result = vkCreateShaderModule(device, &shaderInfo, nullptr, &shaderModule);
    if (result != VK_SUCCESS) {
      std::cerr << "vkCreateShaderModule failed: " << vkResultToString(result)
                << '\n';
      destroyDevice();
      destroyInstance();
      return false;
    }

    auto destroyShaderModule = [&device, &shaderModule]() {
      if (shaderModule != VK_NULL_HANDLE) {
        vkDestroyShaderModule(device, shaderModule, nullptr);
        shaderModule = VK_NULL_HANDLE;
      }
    };

    VkDescriptorSetLayoutBinding bindings[5]{};
    bindings[0].binding = 0;
    bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    bindings[0].descriptorCount = 1;
    bindings[0].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    bindings[1].binding = 1;
    bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    bindings[1].descriptorCount = 1;
    bindings[1].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    bindings[2].binding = 2;
    bindings[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    bindings[2].descriptorCount = 1;
    bindings[2].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    bindings[3].binding = 3;
    bindings[3].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    bindings[3].descriptorCount = 1;
    bindings[3].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    bindings[4].binding = 4;
    bindings[4].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    bindings[4].descriptorCount = 1;
    bindings[4].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

    VkDescriptorSetLayout descriptorSetLayout{};
    VkDescriptorSetLayoutCreateInfo dsetLayoutInfo{};
    dsetLayoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    dsetLayoutInfo.bindingCount = 5;
    dsetLayoutInfo.pBindings = bindings;
    result = vkCreateDescriptorSetLayout(device, &dsetLayoutInfo, nullptr,
                                         &descriptorSetLayout);
    if (result != VK_SUCCESS) {
      std::cerr << "vkCreateDescriptorSetLayout failed: "
                << vkResultToString(result) << '\n';
      destroyShaderModule();
      destroyDevice();
      destroyInstance();
      return false;
    }

    VkPipelineLayout pipelineLayout{};
    VkPipelineLayoutCreateInfo layoutInfo{};
    VkPushConstantRange pcRange{};
    pcRange.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    pcRange.offset = 0;
    pcRange.size = sizeof(PushConstants);
    layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layoutInfo.setLayoutCount = 1;
    layoutInfo.pSetLayouts = &descriptorSetLayout;
    layoutInfo.pushConstantRangeCount = 1;
    layoutInfo.pPushConstantRanges = &pcRange;
    result =
        vkCreatePipelineLayout(device, &layoutInfo, nullptr, &pipelineLayout);
    if (result != VK_SUCCESS) {
      std::cerr << "vkCreatePipelineLayout failed: " << vkResultToString(result)
                << '\n';
      vkDestroyDescriptorSetLayout(device, descriptorSetLayout, nullptr);
      destroyShaderModule();
      destroyDevice();
      destroyInstance();
      return false;
    }

    auto destroyPipelineLayout = [&device, &pipelineLayout]() {
      if (pipelineLayout != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(device, pipelineLayout, nullptr);
        pipelineLayout = VK_NULL_HANDLE;
      }
    };
    auto destroyDescriptorSetLayout = [&device, &descriptorSetLayout]() {
      if (descriptorSetLayout != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(device, descriptorSetLayout, nullptr);
        descriptorSetLayout = VK_NULL_HANDLE;
      }
    };

    auto createPipeline = [&](const std::uint32_t operation) -> VkPipeline {
      VkPipeline pipeline{};
      VkComputePipelineCreateInfo pipelineInfo{};
      VkPipelineShaderStageCreateInfo stageInfo{};
      const VkSpecializationMapEntry operationEntry{0, 0, sizeof(operation)};
      const VkSpecializationInfo specializationInfo{
          1, &operationEntry, sizeof(operation), &operation};
      stageInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
      stageInfo.stage = VK_SHADER_STAGE_COMPUTE_BIT;
      stageInfo.module = shaderModule;
      stageInfo.pName = "main";
      stageInfo.pSpecializationInfo = &specializationInfo;
      pipelineInfo.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
      pipelineInfo.stage = stageInfo;
      pipelineInfo.layout = pipelineLayout;
      const VkResult pipelineResult = vkCreateComputePipelines(
          device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &pipeline);
      if (pipelineResult != VK_SUCCESS) {
        std::cerr << "vkCreateComputePipelines failed for operation "
                  << operation << ": " << vkResultToString(pipelineResult)
                  << '\n';
        return VK_NULL_HANDLE;
      }
      return pipeline;
    };

    const VkPipeline fillPipeline = createPipeline(0);
    const VkPipeline copyPipeline = createPipeline(1);
    const VkPipeline transformPipeline = createPipeline(2);
    const VkPipeline reducePipeline = createPipeline(3);
    const VkPipeline scanPipeline = createPipeline(4);
    const VkPipeline reduceFinalizePipeline = createPipeline(5);
    const VkPipeline scanBlockSumsPipeline = createPipeline(6);
    const VkPipeline scanAddOffsetsPipeline = createPipeline(7);
    const std::array primitivePipelines = {
        fillPipeline,           copyPipeline,          transformPipeline,
        reducePipeline,         scanPipeline,          reduceFinalizePipeline,
        scanBlockSumsPipeline,  scanAddOffsetsPipeline};
    auto destroyPrimitivePipelines = [&]() {
      for (const VkPipeline pipeline : primitivePipelines) {
        if (pipeline != VK_NULL_HANDLE) {
          vkDestroyPipeline(device, pipeline, nullptr);
        }
      }
    };
    if (fillPipeline == VK_NULL_HANDLE || copyPipeline == VK_NULL_HANDLE ||
        transformPipeline == VK_NULL_HANDLE ||
        reducePipeline == VK_NULL_HANDLE || scanPipeline == VK_NULL_HANDLE ||
        reduceFinalizePipeline == VK_NULL_HANDLE ||
        scanBlockSumsPipeline == VK_NULL_HANDLE ||
        scanAddOffsetsPipeline == VK_NULL_HANDLE) {
      destroyPrimitivePipelines();
      destroyPipelineLayout();
      destroyDescriptorSetLayout();
      destroyShaderModule();
      destroyDevice();
      destroyInstance();
      return false;
    }

    VkDescriptorPoolSize poolSizes{};
    poolSizes.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    poolSizes.descriptorCount = 10;
    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.poolSizeCount = 1;
    poolInfo.pPoolSizes = &poolSizes;
    poolInfo.maxSets = 1;
    VkDescriptorPool descriptorPool{};
    result =
        vkCreateDescriptorPool(device, &poolInfo, nullptr, &descriptorPool);
    if (result != VK_SUCCESS) {
      std::cerr << "vkCreateDescriptorPool failed: " << vkResultToString(result)
                << '\n';
      destroyPrimitivePipelines();
      destroyPipelineLayout();
      destroyDescriptorSetLayout();
      destroyShaderModule();
      destroyDevice();
      destroyInstance();
      return false;
    }

    VkDescriptorSetAllocateInfo dsetAlloc{};
    dsetAlloc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    dsetAlloc.descriptorPool = descriptorPool;
    dsetAlloc.descriptorSetCount = 1;
    dsetAlloc.pSetLayouts = &descriptorSetLayout;
    VkDescriptorSet descriptorSet{};
    result = vkAllocateDescriptorSets(device, &dsetAlloc, &descriptorSet);
    if (result != VK_SUCCESS) {
      std::cerr << "vkAllocateDescriptorSets failed: "
                << vkResultToString(result) << '\n';
      vkDestroyDescriptorPool(device, descriptorPool, nullptr);
      destroyPrimitivePipelines();
      destroyPipelineLayout();
      destroyDescriptorSetLayout();
      destroyShaderModule();
      destroyDevice();
      destroyInstance();
      return false;
    }

    const std::size_t maxLength = kTestLengths.back();
    const std::uint32_t maxDispatch =
        std::max(1u, static_cast<std::uint32_t>(
                         (maxLength + kWorkgroupSize - 1u) / kWorkgroupSize));
    const std::size_t storageCount =
        static_cast<std::size_t>(maxDispatch) * kWorkgroupSize;
    const VkDeviceSize floatBytes = storageCount * sizeof(float);
    const VkDeviceSize intBytes = storageCount * sizeof(int);
    const VkDeviceSize reductionBytes = 4 * sizeof(float);

    const VkBufferUsageFlags bufferUsage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                           VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                                           VK_BUFFER_USAGE_TRANSFER_DST_BIT;

    GpuBuffer inputFloat{};
    GpuBuffer outputFloat{};
    GpuBuffer inputInt{};
    GpuBuffer outputInt{};
    GpuBuffer reduction{};

    auto cleanupGpuBuffers = [&]() {
      if (inputFloat.mapped != nullptr) {
        vkUnmapMemory(device, inputFloat.memory);
      }
      if (outputFloat.mapped != nullptr) {
        vkUnmapMemory(device, outputFloat.memory);
      }
      if (inputInt.mapped != nullptr) {
        vkUnmapMemory(device, inputInt.memory);
      }
      if (outputInt.mapped != nullptr) {
        vkUnmapMemory(device, outputInt.memory);
      }
      if (reduction.mapped != nullptr) {
        vkUnmapMemory(device, reduction.memory);
      }
      if (inputFloat.buffer != VK_NULL_HANDLE) {
        vkDestroyBuffer(device, inputFloat.buffer, nullptr);
      }
      if (outputFloat.buffer != VK_NULL_HANDLE) {
        vkDestroyBuffer(device, outputFloat.buffer, nullptr);
      }
      if (inputInt.buffer != VK_NULL_HANDLE) {
        vkDestroyBuffer(device, inputInt.buffer, nullptr);
      }
      if (outputInt.buffer != VK_NULL_HANDLE) {
        vkDestroyBuffer(device, outputInt.buffer, nullptr);
      }
      if (reduction.buffer != VK_NULL_HANDLE) {
        vkDestroyBuffer(device, reduction.buffer, nullptr);
      }
      if (inputFloat.memory != VK_NULL_HANDLE) {
        vkFreeMemory(device, inputFloat.memory, nullptr);
      }
      if (outputFloat.memory != VK_NULL_HANDLE) {
        vkFreeMemory(device, outputFloat.memory, nullptr);
      }
      if (inputInt.memory != VK_NULL_HANDLE) {
        vkFreeMemory(device, inputInt.memory, nullptr);
      }
      if (outputInt.memory != VK_NULL_HANDLE) {
        vkFreeMemory(device, outputInt.memory, nullptr);
      }
      if (reduction.memory != VK_NULL_HANDLE) {
        vkFreeMemory(device, reduction.memory, nullptr);
      }
    };

    if (!createHostBuffer(selectedDevice, device, floatBytes, bufferUsage,
                          inputFloat) ||
        !createHostBuffer(selectedDevice, device, floatBytes, bufferUsage,
                          outputFloat) ||
        !createHostBuffer(selectedDevice, device, intBytes, bufferUsage,
                          inputInt) ||
        !createHostBuffer(selectedDevice, device, intBytes, bufferUsage,
                          outputInt) ||
        !createHostBuffer(selectedDevice, device, reductionBytes, bufferUsage,
                          reduction)) {
      cleanupGpuBuffers();
      vkDestroyDescriptorPool(device, descriptorPool, nullptr);
      destroyPrimitivePipelines();
      destroyPipelineLayout();
      destroyDescriptorSetLayout();
      destroyShaderModule();
      destroyDevice();
      destroyInstance();
      return false;
    }

    VkDescriptorBufferInfo inFloatInfo{};
    inFloatInfo.buffer = inputFloat.buffer;
    inFloatInfo.offset = 0;
    inFloatInfo.range = VK_WHOLE_SIZE;
    VkDescriptorBufferInfo outFloatInfo{};
    outFloatInfo.buffer = outputFloat.buffer;
    outFloatInfo.offset = 0;
    outFloatInfo.range = VK_WHOLE_SIZE;
    VkDescriptorBufferInfo inIntInfo{};
    inIntInfo.buffer = inputInt.buffer;
    inIntInfo.offset = 0;
    inIntInfo.range = VK_WHOLE_SIZE;
    VkDescriptorBufferInfo outIntInfo{};
    outIntInfo.buffer = outputInt.buffer;
    outIntInfo.offset = 0;
    outIntInfo.range = VK_WHOLE_SIZE;
    VkDescriptorBufferInfo reductionInfo{};
    reductionInfo.buffer = reduction.buffer;
    reductionInfo.offset = 0;
    reductionInfo.range = reductionBytes;

    std::array<VkWriteDescriptorSet, 5> writes{};
    writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[0].dstSet = descriptorSet;
    writes[0].dstBinding = 0;
    writes[0].descriptorCount = 1;
    writes[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    writes[0].pBufferInfo = &inFloatInfo;
    writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[1].dstSet = descriptorSet;
    writes[1].dstBinding = 1;
    writes[1].descriptorCount = 1;
    writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    writes[1].pBufferInfo = &outFloatInfo;
    writes[2].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[2].dstSet = descriptorSet;
    writes[2].dstBinding = 2;
    writes[2].descriptorCount = 1;
    writes[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    writes[2].pBufferInfo = &inIntInfo;
    writes[3].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[3].dstSet = descriptorSet;
    writes[3].dstBinding = 3;
    writes[3].descriptorCount = 1;
    writes[3].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    writes[3].pBufferInfo = &outIntInfo;
    writes[4].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[4].dstSet = descriptorSet;
    writes[4].dstBinding = 4;
    writes[4].descriptorCount = 1;
    writes[4].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    writes[4].pBufferInfo = &reductionInfo;
    vkUpdateDescriptorSets(device, 5, writes.data(), 0, nullptr);

    VkCommandPoolCreateInfo cmdPoolInfo{};
    cmdPoolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    cmdPoolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    cmdPoolInfo.queueFamilyIndex = selectedQueueFamily;
    VkCommandPool commandPool{};
    result = vkCreateCommandPool(device, &cmdPoolInfo, nullptr, &commandPool);
    if (result != VK_SUCCESS) {
      std::cerr << "vkCreateCommandPool failed: " << vkResultToString(result)
                << '\n';
      cleanupGpuBuffers();
      vkDestroyDescriptorPool(device, descriptorPool, nullptr);
      destroyPrimitivePipelines();
      destroyPipelineLayout();
      destroyDescriptorSetLayout();
      destroyShaderModule();
      destroyDevice();
      destroyInstance();
      return false;
    }
    VkCommandBuffer commandBuffer{};
    VkCommandBufferAllocateInfo cmdAlloc{};
    cmdAlloc.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cmdAlloc.commandPool = commandPool;
    cmdAlloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cmdAlloc.commandBufferCount = 1;
    result = vkAllocateCommandBuffers(device, &cmdAlloc, &commandBuffer);
    if (result != VK_SUCCESS) {
      std::cerr << "vkAllocateCommandBuffers failed: "
                << vkResultToString(result) << '\n';
      vkDestroyCommandPool(device, commandPool, nullptr);
      cleanupGpuBuffers();
      vkDestroyDescriptorPool(device, descriptorPool, nullptr);
      destroyPrimitivePipelines();
      destroyPipelineLayout();
      destroyDescriptorSetLayout();
      destroyShaderModule();
      destroyDevice();
      destroyInstance();
      return false;
    }

    bool allPass = true;
    auto runDispatchExact = [&](const VkPipeline pipeline,
                                const std::size_t elementCount,
                                const float scalar,
                                const std::uint32_t dispatchX) -> bool {
      const PushConstants params{static_cast<std::uint32_t>(elementCount),
                                 scalar};
      return dispatchKernel(
          device, queue, commandBuffer, pipeline, pipelineLayout, descriptorSet,
          params, dispatchX, inputFloat.buffer, outputFloat.buffer,
          inputInt.buffer, outputInt.buffer, reduction.buffer);
    };
    auto runDispatch = [&](const VkPipeline pipeline, const std::size_t length,
                           const float scalar) -> bool {
      const std::uint32_t dispatchX =
          std::max(1u, static_cast<std::uint32_t>(
                           (length + kWorkgroupSize - 1u) / kWorkgroupSize));
      return runDispatchExact(pipeline, length, scalar, dispatchX);
    };
    auto runReduction = [&](const std::size_t length) -> bool {
      const std::uint32_t partialCount =
          std::max(1u, static_cast<std::uint32_t>(
                           (length + kWorkgroupSize - 1u) / kWorkgroupSize));
      return partialCount <= kWorkgroupSize &&
             runDispatchExact(reducePipeline, length, 0.0f, partialCount) &&
             runDispatchExact(reduceFinalizePipeline, partialCount,
                              static_cast<float>(length), 1u);
    };
    auto runScan = [&](const std::size_t length) -> bool {
      const std::uint32_t blockCount =
          std::max(1u, static_cast<std::uint32_t>(
                           (length + kWorkgroupSize - 1u) / kWorkgroupSize));
      return blockCount <= kWorkgroupSize &&
             runDispatchExact(scanPipeline, length, 0.0f, blockCount) &&
             runDispatchExact(scanBlockSumsPipeline, blockCount, 0.0f, 1u) &&
             runDispatchExact(scanAddOffsetsPipeline, length, 0.0f,
                              blockCount);
    };

    auto fillFloat = [&](const std::size_t length,
                         const std::vector<float> &values) {
      if (values.size() < length) {
        return false;
      }
      return writeBufferBytes(device, inputFloat, values.data(),
                              length * sizeof(float));
    };

    auto fillInt = [&](const std::size_t length,
                       const std::vector<int> &values) {
      if (values.size() < length) {
        return false;
      }
      return writeBufferBytes(device, inputInt, values.data(),
                              length * sizeof(int));
    };

    auto fillSentinel = [&]() {
      std::vector<float> floatSentinel(storageCount, kFloatSentinel);
      if (!writeBufferBytes(device, outputFloat, floatSentinel.data(),
                            floatSentinel.size() * sizeof(float))) {
        return false;
      }
      std::vector<int> intSentinel(storageCount, kIntSentinel);
      if (!writeBufferBytes(device, outputInt, intSentinel.data(),
                            intSentinel.size() * sizeof(int))) {
        return false;
      }
      std::vector<float> reductionSentinel(
          {kFloatSentinel, kFloatSentinel, kFloatSentinel, kFloatSentinel});
      return writeBufferBytes(device, reduction, reductionSentinel.data(),
                              reductionSentinel.size() * sizeof(float));
    };

    auto readFloatOut = [&](std::vector<float> &out) {
      return invalidateMappedRange(device, outputFloat) &&
             (std::memcpy(out.data(), outputFloat.mapped,
                          out.size() * sizeof(float)),
              true);
    };
    auto readIntOut = [&](std::vector<int> &out) {
      return invalidateMappedRange(device, outputInt) &&
             (std::memcpy(out.data(), outputInt.mapped,
                          out.size() * sizeof(int)),
              true);
    };
    auto readReductionOut = [&](std::array<float, 4> &out) {
      return invalidateMappedRange(device, reduction) &&
             (std::memcpy(out.data(), reduction.mapped,
                          out.size() * sizeof(float)),
              true);
    };

    auto readAllHost = [&]() {
      auto okFloat = invalidateMappedRange(device, inputFloat);
      auto okOutFloat = invalidateMappedRange(device, outputFloat);
      auto okInt = invalidateMappedRange(device, inputInt);
      auto okOutInt = invalidateMappedRange(device, outputInt);
      auto okReduce = invalidateMappedRange(device, reduction);
      return okFloat && okOutFloat && okInt && okOutInt && okReduce;
    };
    (void)readAllHost;

    bool runPass = true;
    for (const auto length : kTestLengths) {
      const auto dispatchCount =
          std::max(1u, static_cast<std::uint32_t>(
                           (length + kWorkgroupSize - 1u) / kWorkgroupSize));
      (void)dispatchCount;
      std::vector<float> inFloat(storageCount, kFloatSentinel);
      std::vector<float> outFloat(storageCount, kFloatSentinel);
      for (std::size_t i = 0; i < length; ++i) {
        inFloat[i] = 0.5f + static_cast<float>(i);
      }
      if (!fillInt(length, std::vector<int>(length > 0 ? length : 1, 0))) {
        runPass = false;
        break;
      }
      if (!fillFloat(length, inFloat)) {
        runPass = false;
        break;
      }
      if (!fillSentinel()) {
        runPass = false;
        break;
      }
      if (!runDispatch(fillPipeline, length, kFillValue)) {
        runPass = false;
        break;
      }
      if (!invalidateMappedRange(device, outputFloat)) {
        runPass = false;
        break;
      }
      for (std::size_t i = 0; i < length; ++i) {
        outFloat[i] = kFillValue;
      }
      std::size_t fillMismatch = 0;
      float fillMaxAbs = 0.0f;
      float fillMaxRel = 0.0f;
      std::uint32_t fillMaxUlp = 0;
      const bool fillOk = compareFloatArray(
          "fill", static_cast<const float *>(outputFloat.mapped),
          outFloat.data(), length, storageCount, kFloatSentinel, fillMismatch,
          fillMaxAbs, fillMaxRel, fillMaxUlp);
      const bool fillStatus = fillOk;
      std::cout << "[PrimitiveSmoke] fill length=" << length
                << (fillStatus ? " PASS" : " FAIL") << '\n';
      allPass = allPass && fillStatus;
    }

    for (const auto length : kTestLengths) {
      std::vector<float> inFloat(storageCount, kFloatSentinel);
      std::vector<float> outFloat(storageCount, kFloatSentinel);
      for (std::size_t i = 0; i < length; ++i) {
        inFloat[i] = 0.75f + static_cast<float>(i);
      }
      if (!fillFloat(length, inFloat) || !fillSentinel() ||
          !runDispatch(copyPipeline, length, 0.0f)) {
        runPass = false;
        break;
      }
      if (!invalidateMappedRange(device, outputFloat)) {
        runPass = false;
        break;
      }
      std::copy_n(inFloat.data(), storageCount, outFloat.data());
      std::size_t copyMismatch = 0;
      float copyMaxAbs = 0.0f;
      float copyMaxRel = 0.0f;
      std::uint32_t copyMaxUlp = 0;
      const bool copyOk = compareFloatArray(
          "copy", static_cast<const float *>(outputFloat.mapped),
          outFloat.data(), length, storageCount, kFloatSentinel, copyMismatch,
          copyMaxAbs, copyMaxRel, copyMaxUlp);
      const bool copyStatus = copyOk;
      std::cout << "[PrimitiveSmoke] copy length=" << length
                << (copyStatus ? " PASS" : " FAIL") << '\n';
      allPass = allPass && copyStatus;
    }

    for (const auto length : kTestLengths) {
      std::vector<float> inFloat(storageCount, kFloatSentinel);
      std::vector<float> outFloat(storageCount, kFloatSentinel);
      for (std::size_t i = 0; i < length; ++i) {
        inFloat[i] = 1.25f + static_cast<float>(i);
      }
      if (!fillFloat(length, inFloat) || !fillSentinel() ||
          !runDispatch(transformPipeline, length, 0.0f)) {
        runPass = false;
        break;
      }
      if (!invalidateMappedRange(device, outputFloat)) {
        runPass = false;
        break;
      }
      for (std::size_t i = 0; i < length; ++i) {
        outFloat[i] = inFloat[i] * 2.0f + 1.0f;
      }
      std::size_t transformMismatch = 0;
      float transformMaxAbs = 0.0f;
      float transformMaxRel = 0.0f;
      std::uint32_t transformMaxUlp = 0;
      const bool transformOk = compareFloatArray(
          "transform", static_cast<const float *>(outputFloat.mapped),
          outFloat.data(), length, storageCount, kFloatSentinel,
          transformMismatch, transformMaxAbs, transformMaxRel, transformMaxUlp);
      const bool transformStatus = transformOk;
      std::cout << "[PrimitiveSmoke] transform length=" << length
                << (transformStatus ? " PASS" : " FAIL") << '\n';
      allPass = allPass && transformStatus;
    }

    for (const auto length : kTestLengths) {
      std::vector<float> inFloat(storageCount, kFloatSentinel);
      std::vector<float> outFloat(storageCount, kFloatSentinel);
      std::array<float, 4> reductionExpected = {kFloatSentinel, kFloatSentinel,
                                                kFloatSentinel, kFloatSentinel};
      std::vector<int> outInt(storageCount, kIntSentinel);
      for (std::size_t i = 0; i < length; ++i) {
        inFloat[i] = -1.25f + static_cast<float>(i) * 0.5f;
      }
      if (!fillInt(length, std::vector<int>(length, 0))) {
        runPass = false;
        break;
      }
      if (!fillFloat(length, inFloat) || !fillSentinel() ||
          !runReduction(length)) {
        runPass = false;
        break;
      }
      if (!invalidateMappedRange(device, reduction) ||
          !invalidateMappedRange(device, outputFloat) ||
          !invalidateMappedRange(device, outputInt)) {
        runPass = false;
        break;
      }
      if (length == 0) {
        reductionExpected = {0.0f, 0.0f, 0.0f, kFloatSentinel};
      } else {
        long double sum = 0.0L;
        float minv = inFloat[0];
        float maxv = inFloat[0];
        for (std::size_t i = 0; i < length; ++i) {
          sum += static_cast<long double>(inFloat[i]);
          minv = std::min(minv, inFloat[i]);
          maxv = std::max(maxv, inFloat[i]);
        }
        reductionExpected = {static_cast<float>(sum), minv, maxv,
                             kFloatSentinel};
      }
      const std::size_t partialCount =
          std::max<std::size_t>(1, (length + kWorkgroupSize - 1u) /
                                       kWorkgroupSize);
      for (std::size_t block = 0; block < partialCount; ++block) {
        const std::size_t begin = block * kWorkgroupSize;
        const std::size_t end = std::min(length, begin + kWorkgroupSize);
        const std::size_t base = block * 3;
        if (begin == end) {
          outFloat[base] = 0.0f;
          outFloat[base + 1] = std::numeric_limits<float>::infinity();
          outFloat[base + 2] = -std::numeric_limits<float>::infinity();
          continue;
        }
        long double blockSum = 0.0L;
        float blockMin = inFloat[begin];
        float blockMax = inFloat[begin];
        for (std::size_t i = begin; i < end; ++i) {
          blockSum += static_cast<long double>(inFloat[i]);
          blockMin = std::min(blockMin, inFloat[i]);
          blockMax = std::max(blockMax, inFloat[i]);
        }
        outFloat[base] = static_cast<float>(blockSum);
        outFloat[base + 1] = blockMin;
        outFloat[base + 2] = blockMax;
      }
      const float *actualReduction =
          static_cast<const float *>(reduction.mapped);
      float redAbs = 0.0f;
      float redRel = 0.0f;
      std::uint32_t redUlp = 0;
      bool redOk = true;
      for (std::size_t i = 0; i < 4; ++i) {
        const bool scalarOk = compareScalar(
            std::string("reduce scalar ").append(std::to_string(i)),
            actualReduction[i], reductionExpected[i], redAbs, redRel, redUlp);
        redOk = redOk && scalarOk;
      }
      std::copy_n(outFloat.data(), storageCount, outFloat.data());
      std::size_t outMismatch = 0;
      float outAbs = 0.0f;
      float outRel = 0.0f;
      std::uint32_t outUlp = 0;
      const bool outFloatOk = compareFloatArray(
          "reduction partials_and_guard",
          static_cast<const float *>(outputFloat.mapped), outFloat.data(),
          partialCount * 3, storageCount, kFloatSentinel, outMismatch, outAbs,
          outRel, outUlp);
      std::copy_n(outInt.data(), storageCount, outInt.data());
      std::size_t outIntMismatch = 0;
      const bool outIntOk = compareIntArray(
          "reduction output_int_guard",
          static_cast<const int *>(outputInt.mapped), outInt.data(), length,
          storageCount, kIntSentinel, outIntMismatch);
      const bool reductionPass = redOk && outFloatOk && outIntOk &&
                                 (outMismatch == 0) && (outIntMismatch == 0);
      std::cout << "[PrimitiveSmoke] reduce length=" << length
                << (reductionPass ? " PASS" : " FAIL") << '\n';
      allPass = allPass && reductionPass;
    }

    for (const auto length : kTestLengths) {
      std::vector<int> inInt(storageCount, kIntSentinel);
      std::vector<int> outInt(storageCount, kIntSentinel);
      std::vector<int> expectInt(storageCount, kIntSentinel);
      for (std::size_t i = 0; i < length; ++i) {
        inInt[i] = static_cast<int>(i) - 3;
      }
      std::vector<float> guardFloat(storageCount, kFloatSentinel);
      std::array<float, 4> reductionSentinel = {kFloatSentinel, kFloatSentinel,
                                                kFloatSentinel, kFloatSentinel};
      if (!fillInt(length, inInt) || !fillSentinel() ||
          !fillFloat(length, guardFloat) ||
          !writeBufferBytes(device, reduction, reductionSentinel.data(),
                            reductionSentinel.size() * sizeof(float)) ||
          !runScan(length)) {
        runPass = false;
        break;
      }
      if (!invalidateMappedRange(device, outputInt) ||
          !invalidateMappedRange(device, outputFloat) ||
          !invalidateMappedRange(device, reduction)) {
        runPass = false;
        break;
      }
      int running = 0;
      for (std::size_t i = 0; i < length; ++i) {
        expectInt[i] = running;
        running += inInt[i];
      }
      std::size_t scanMismatch = 0;
      const bool scanOk = compareIntArray(
          "exclusive_scan_i32", static_cast<const int *>(outputInt.mapped),
          expectInt.data(), length, storageCount, kIntSentinel, scanMismatch);
      const std::size_t blockCount =
          std::max<std::size_t>(1, (length + kWorkgroupSize - 1u) /
                                       kWorkgroupSize);
      std::vector<std::uint32_t> expectedScratchBits(
          storageCount, std::bit_cast<std::uint32_t>(kFloatSentinel));
      int blockOffset = 0;
      for (std::size_t block = 0; block < blockCount; ++block) {
        expectedScratchBits[block] =
            std::bit_cast<std::uint32_t>(blockOffset);
        const std::size_t begin = block * kWorkgroupSize;
        const std::size_t end = std::min(length, begin + kWorkgroupSize);
        for (std::size_t i = begin; i < end; ++i) {
          blockOffset += inInt[i];
        }
      }
      const auto *actualScratch =
          static_cast<const float *>(outputFloat.mapped);
      std::size_t outFloatMismatch = 0;
      for (std::size_t i = 0; i < storageCount; ++i) {
        if (std::bit_cast<std::uint32_t>(actualScratch[i]) !=
            expectedScratchBits[i]) {
          ++outFloatMismatch;
        }
      }
      const bool outFloatOk = outFloatMismatch == 0;
      std::cout << "[PrimitiveSmoke] scan block_offsets_and_guard mismatch="
                << outFloatMismatch << '\n';
      std::array<float, 4> red = {0, 0, 0, kFloatSentinel};
      std::memcpy(red.data(), reduction.mapped, sizeof(red));
      const bool redSentinelOk =
          std::all_of(red.begin(), red.end(), [](const float value) {
            return std::bit_cast<std::uint32_t>(value) ==
                   std::bit_cast<std::uint32_t>(kFloatSentinel);
          });
      const bool scanPass = scanOk && outFloatOk && redSentinelOk &&
                            (scanMismatch == 0) && (outFloatMismatch == 0);
      std::cout << "[PrimitiveSmoke] exclusive_scan_i32 length=" << length
                << (scanPass ? " PASS" : " FAIL") << '\n';
      allPass = allPass && scanPass;
    }

    vkDestroyCommandPool(device, commandPool, nullptr);
    cleanupGpuBuffers();
    vkDestroyDescriptorPool(device, descriptorPool, nullptr);
    destroyPrimitivePipelines();
    destroyPipelineLayout();
    destroyDescriptorSetLayout();
    destroyShaderModule();
    destroyDevice();
    destroyInstance();

    std::cout << "[PrimitiveSmoke] overall " << (allPass ? "PASS" : "FAIL")
              << '\n';
    return allPass;
  }

} // namespace

int main() { return runPrimitiveSmoke() ? 0 : 1; }
