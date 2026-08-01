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
#include <chrono>
#include <cstdint>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#ifdef VIENNAPS_VULKAN_ENABLED
#include <vulkan/vulkan.h>
#endif

namespace {

struct ArgView {
  bool writeProfile = false;
  bool validateProfile = false;
  std::string profilePath;
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

#ifdef VIENNAPS_VULKAN_ENABLED

[[nodiscard]] std::string
makeDeviceProfile(const VkPhysicalDevice physicalDevice,
                  const std::uint32_t deviceIndex) {
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
    VkPhysicalDeviceProperties2 props2{};
    VkPhysicalDeviceRayTracingPipelinePropertiesKHR rtPipelineProps{};
    props2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
    rtPipelineProps.sType =
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_PROPERTIES_KHR;
    props2.pNext = &rtPipelineProps;
    vkGetPhysicalDeviceProperties2(physicalDevice, &props2);
    maxRayRecursionDepth = rtPipelineProps.maxRayRecursionDepth;
  }

  const std::vector<std::uint32_t> subgroupSizes = {subgroupSize};
  const bool hasMemoryBudgetExt =
      hasExtension(extensionNames, "VK_EXT_memory_budget");
  const auto computeWorkGroupCount = jsonSize3(std::to_array(
      {limits.maxComputeWorkGroupCount[0], limits.maxComputeWorkGroupCount[1],
       limits.maxComputeWorkGroupCount[2]}));
  const auto computeWorkGroupSize = jsonSize3(std::to_array(
      {limits.maxComputeWorkGroupSize[0], limits.maxComputeWorkGroupSize[1],
       limits.maxComputeWorkGroupSize[2]}));

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
  json << "\"measuredSafeWorkingSetBytes\":0},";

  json << "\"compute\":{";
  json << "\"queueFamily\":" << selectedComputeQueueFamily << ',';
  json << "\"dedicatedQueue\":" << boolToJson(hasDedicatedComputeQueueFamily)
       << ',';
  json << "\"maxWorkGroupInvocations\":"
       << limits.maxComputeWorkGroupInvocations << ',';
  json << "\"subgroupSizes\":" << jsonNumberList(subgroupSizes) << ',';
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
  json << "\"tessellationShader\":"
       << boolToJson(features2.features.tessellationShader) << ',';
  json << "\"shaderInt16\":" << boolToJson(features2.features.shaderInt16)
       << ',';
  json << "\"shaderFloat16\":" << boolToJson(float16Int8Enabled) << ',';
  json << "\"samplerAnisotropy\":"
       << boolToJson(features2.features.samplerAnisotropy) << ',';
  json << "\"pipelineStatisticsQuery\":"
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
  json << "\"primitiveSuite\":\"not-run\",";
  json << "\"fp32Suite\":\"not-run\",";
  json << "\"fp64Suite\":\"not-run\",";
  json << "\"computeBvhSuite\":\"not-run\",";
  json << "\"hardwareRaySuite\":\"not-run\",";
  json << "\"deviceIndex\":" << deviceIndex << "},";

  json << "\"extensions\":[";
  for (std::size_t i = 0; i < extensionNames.size(); ++i) {
    if (i != 0)
      json << ',';
    json << '"' << escapeJson(extensionNames[i]) << '"';
  }
  json << "],";

  json << "\"computeQueueFamilyIndices\":"
       << jsonNumberList(computeQueueFamilies);
  json << '}';
  return json.str();
}

[[nodiscard]] std::string probeSummary() {
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

  if (vkCreateInstance(&instanceCreateInfo, nullptr, &instance) != VK_SUCCESS) {
    return std::string("{\"schemaVersion\":1,\"status\":\"error\",\"reason\":"
                       "\"vkCreateInstance failed.\"}");
  }

  std::uint32_t deviceCount = 0;
  if (vkEnumeratePhysicalDevices(instance, &deviceCount, nullptr) !=
          VK_SUCCESS ||
      deviceCount == 0) {
    vkDestroyInstance(instance, nullptr);
    return std::string("{\"schemaVersion\":1,\"status\":\"disabled\","
                       "\"reason\":\"No Vulkan physical device found.\"}");
  }

  std::vector<VkPhysicalDevice> devices(deviceCount);
  vkEnumeratePhysicalDevices(instance, &deviceCount, devices.data());

  std::ostringstream out;
  out << "{\n";
  out << "\"schemaVersion\":1,\n";
  out << "\"viennaPsVersion\":\"4.6.2+vulkan-probe\",\n";
  out << "\"createdUtc\":\"" << nowUtcTimestamp() << "\",\n";
  out << "\"status\":\"pass\",\n";
  out << "\"featuresProfiled\":true,\n";
  out << "\"devices\":[\n";
  for (std::uint32_t i = 0; i < devices.size(); ++i) {
    if (i != 0)
      out << ",\n";
    out << makeDeviceProfile(devices[i], i);
  }
  out << "]\n";
  out << "}";
  vkDestroyInstance(instance, nullptr);
  return out.str();
}

#else

[[nodiscard]] std::string probeSummary() {
  return std::string("{\n"
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
    } else if (arg == "--validate-profile") {
      args.validateProfile = true;
    } else if (arg == "--help") {
      std::cout << "viennaps-device-probe --write-profile <path> "
                   "[--validate-profile]\n";
    } else {
      std::cerr << "Unknown argument: " << arg << '\n';
    }
  }
  return args;
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

} // namespace

int main(int argc, char **argv) {
  const ArgView args = parseArgs(argc, argv);
  const std::string profile = probeSummary();
  std::cout << profile << '\n';

  if (args.writeProfile && !args.profilePath.empty()) {
    writeProfile(args.profilePath, profile);
  }

  if (args.validateProfile) {
    std::cout << "{\"validation\":\"not-implemented\"}\n";
  }

  return 0;
}
