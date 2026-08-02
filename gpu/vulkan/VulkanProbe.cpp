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
#include <ctime>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

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

using viennaps::compute::CapabilityProfileRecord;

struct ArgView {
  bool writeProfile = false;
  bool writeDeploymentProfile = false;
  bool validateProfile = false;
  std::string profilePath;
  std::string deploymentProfilePath;
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
  vkEnumeratePhysicalDevices(instance, &deviceCount, devices.data());

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
                       const std::vector<CapabilityProfileRecord> &profiles) {
  if (profiles.empty()) {
    std::cerr << "No generated Vulkan deployment profile available to write.\n";
    return false;
  }
  std::string error;
  if (!viennaps::compute::writeCapabilityProfileRecordToFile(
          path, profiles.front(), &error)) {
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
  const ProbeResult profile = probeSummary();
  std::cout << profile.rawSummary << '\n';
  bool success = true;
  bool deploymentProfileWritten = false;

  if (args.writeProfile && !args.profilePath.empty()) {
    writeProfile(args.profilePath, profile.rawSummary);
  }

  if (args.writeDeploymentProfile && !args.deploymentProfilePath.empty()) {
    deploymentProfileWritten = writeDeploymentProfile(
        args.deploymentProfilePath, profile.deploymentProfiles);
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
