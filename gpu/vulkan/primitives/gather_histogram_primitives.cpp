// Copyright 2026 ViennaPS
// SPDX-License-Identifier: MIT
//
// Production Vulkan gather/scatter/histogram primitives. This layer composes the
// reusable runtime RAII helpers and keeps per-element operations deterministic
// where possible.

#include "gather_histogram_primitives.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>
#include <limits>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace {

[[nodiscard]] bool setError(std::string &error, const std::string_view phase,
                            const std::string_view message) {
  if (message.empty()) {
    error = std::string(phase) + ": operation failed";
  } else {
    error = std::string(phase) + ": " + std::string(message);
  }
  return false;
}

constexpr std::string_view kPipelineEntryPoint{"main"};

} // namespace

namespace viennaps::vulkan::primitives {

bool GatherHistogramPrimitives::initialize(const std::string_view spirvPath,
                                          std::string &error) {
  if (isInitialized()) {
    return true;
  }
  if (spirvPath.empty()) {
    return setError(error, "initialization", "SPIR-V path is empty");
  }

  reset();
  const auto failInitialization = [this, &error]() {
    const std::string detail = error;
    reset();
    return setError(error, "initialization", detail);
  };

  if (!instance_.create(error)) {
    return failInitialization();
  }
  viennaps::vulkan::runtime::ComputeDeviceSelection selection{};
  if (!viennaps::vulkan::runtime::pickFirstComputeDevice(instance_.get(),
                                                         selection, error)) {
    return failInitialization();
  }
  if (!device_.create(selection, error)) {
    return failInitialization();
  }

  viennaps::vulkan::runtime::SpirvProgram program{};
  if (!viennaps::vulkan::runtime::readSpirv(spirvPath, program, error)) {
    return failInitialization();
  }
  if (!shaderModule_.create(device_, program, error)) {
    return failInitialization();
  }

  std::array<VkDescriptorSetLayoutBinding, 4u> layoutBindings{};
  layoutBindings[0].binding = 0u;
  layoutBindings[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  layoutBindings[0].descriptorCount = 1u;
  layoutBindings[0].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
  layoutBindings[1].binding = 1u;
  layoutBindings[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  layoutBindings[1].descriptorCount = 1u;
  layoutBindings[1].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
  layoutBindings[2].binding = 2u;
  layoutBindings[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  layoutBindings[2].descriptorCount = 1u;
  layoutBindings[2].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
  layoutBindings[3].binding = 3u;
  layoutBindings[3].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  layoutBindings[3].descriptorCount = 1u;
  layoutBindings[3].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

  if (!descriptorSetLayout_.create(
          device_, std::span<const VkDescriptorSetLayoutBinding>(
                       layoutBindings.data(), layoutBindings.size()),
          error)) {
    return failInitialization();
  }

  const VkPushConstantRange pushConstantRange{
      VK_SHADER_STAGE_COMPUTE_BIT, 0u, sizeof(PushConstants)};
  if (!pipelineLayout_.create(device_, descriptorSetLayout_.get(),
                              std::span(&pushConstantRange, 1u), error)) {
    return failInitialization();
  }

  // DescriptorPool currently sizes storage descriptors as two per requested
  // set. Request two sets so this four-binding layout has four descriptors.
  if (!descriptorPool_.create(device_, 2u, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                              error)) {
    return failInitialization();
  }
  if (!descriptorPool_.allocate(descriptorSetLayout_.get(), descriptorSet_,
                               error)) {
    return failInitialization();
  }
  if (!commandContext_.create(device_, device_.computeQueueFamily(), error)) {
    return failInitialization();
  }
  if (!commandContext_.allocatePrimary(commandBuffer_, error)) {
    return failInitialization();
  }
  if (!fence_.create(device_, error)) {
    return failInitialization();
  }

  if (!createPipeline(gatherPipeline_, GatherHistogramOperation::gather, error) ||
      !createPipeline(scatterPipeline_, GatherHistogramOperation::scatter,
                     error) ||
      !createPipeline(histogramPipeline_, GatherHistogramOperation::histogram,
                     error)) {
    return failInitialization();
  }
  if (!ensureDummyBuffers(error)) {
    return failInitialization();
  }
  return true;
}

void GatherHistogramPrimitives::reset() {
  dummyFloatBuffer_.reset();
  dummyUInt32Buffer_.reset();
  dummyBuffersReady_ = false;
  fence_.reset();
  commandContext_.reset();
  descriptorPool_.reset();
  scatterPipeline_.reset();
  gatherPipeline_.reset();
  histogramPipeline_.reset();
  pipelineLayout_.reset();
  descriptorSetLayout_.reset();
  shaderModule_.reset();
  device_.reset();
  instance_.reset();
  descriptorSet_ = VK_NULL_HANDLE;
  commandBuffer_ = VK_NULL_HANDLE;
}

bool GatherHistogramPrimitives::isInitialized() const {
  return instance_.isValid() && device_.isValid() &&
         shaderModule_.get() != VK_NULL_HANDLE &&
         descriptorSetLayout_.get() != VK_NULL_HANDLE &&
         pipelineLayout_.get() != VK_NULL_HANDLE &&
         gatherPipeline_.get() != VK_NULL_HANDLE &&
         scatterPipeline_.get() != VK_NULL_HANDLE &&
         histogramPipeline_.get() != VK_NULL_HANDLE &&
         descriptorPool_.get() != VK_NULL_HANDLE && descriptorSet_ !=
             VK_NULL_HANDLE && commandBuffer_ != VK_NULL_HANDLE &&
         fence_.get() != VK_NULL_HANDLE &&
         commandContext_.pool() != VK_NULL_HANDLE;
}

bool GatherHistogramPrimitives::isReady(std::string &error) const {
  if (!isInitialized()) {
    return setError(error, "execution",
                    "gather/histogram primitives are not initialized");
  }
  if (device_.get() == VK_NULL_HANDLE || instance_.get() == VK_NULL_HANDLE) {
    return setError(error, "execution",
                    "runtime device or instance is missing");
  }
  return true;
}

bool GatherHistogramPrimitives::createPipeline(
    runtime::ComputePipeline &pipeline,
    const GatherHistogramOperation operation, std::string &error) {
  const auto operationValue = static_cast<std::uint32_t>(operation);
  const VkSpecializationMapEntry entry{0u, 0u, sizeof(std::uint32_t)};
  const runtime::ComputePipelineOptions options{
      kPipelineEntryPoint, std::span<const VkSpecializationMapEntry>(&entry, 1u),
      &operationValue, sizeof(operationValue)};
  return pipeline.create(device_, shaderModule_, pipelineLayout_, options, error);
}

bool GatherHistogramPrimitives::ensureDummyBuffers(std::string &error) {
  if (dummyBuffersReady_ && dummyFloatBuffer_.isValid() &&
      dummyUInt32Buffer_.isValid()) {
    return true;
  }
  if (!createFloatBuffer(1u, dummyFloatBuffer_, error)) {
    return false;
  }
  if (!createUInt32Buffer(1u, dummyUInt32Buffer_, error)) {
    return false;
  }
  dummyBuffersReady_ = true;
  return true;
}

bool GatherHistogramPrimitives::createFloatBuffer(
    const std::size_t elementCount,
    viennaps::vulkan::runtime::HostVisibleBuffer &buffer,
    std::string &error) {
  if (!isReady(error)) {
    return false;
  }

  const auto allocatedElements = std::max<std::size_t>(1u, elementCount);
  if (allocatedElements > std::numeric_limits<std::size_t>::max() /
                              sizeof(float)) {
    return setError(error, "createFloatBuffer",
                    "requested element count overflows Vulkan device size");
  }
  const auto bytes =
      static_cast<VkDeviceSize>(allocatedElements * sizeof(float));
  return buffer.create(device_, bytes, kFloatBufferUsage, kHostOnlyMemoryFlags,
                       error);
}

bool GatherHistogramPrimitives::createUInt32Buffer(
    const std::size_t elementCount,
    viennaps::vulkan::runtime::HostVisibleBuffer &buffer,
    std::string &error) {
  if (!isReady(error)) {
    return false;
  }

  const auto allocatedElements = std::max<std::size_t>(1u, elementCount);
  if (allocatedElements > std::numeric_limits<std::size_t>::max() /
                              sizeof(std::uint32_t)) {
    return setError(error, "createUInt32Buffer",
                    "requested element count overflows Vulkan device size");
  }
  const auto bytes =
      static_cast<VkDeviceSize>(allocatedElements * sizeof(std::uint32_t));
  return buffer.create(device_, bytes, kUInt32BufferUsage,
                       kHostOnlyMemoryFlags, error);
}

bool GatherHistogramPrimitives::validateFloatLength(
    const std::string_view label, const std::string_view operation,
    const runtime::HostVisibleBuffer &buffer, const std::size_t elementCount,
    std::string &error) const {
  if (!buffer.isValid()) {
    return setError(error, "validation",
                    std::string(label) + " " + std::string(operation) +
                        ": buffer is not initialized");
  }
  const auto maxElements = static_cast<std::size_t>(buffer.size() / sizeof(float));
  if (elementCount > maxElements) {
    return setError(
        error, "validation",
        std::string(label) + " " + std::string(operation) +
            ": requested " + std::to_string(elementCount) +
            " elements but buffer has " + std::to_string(maxElements));
  }
  return true;
}

bool GatherHistogramPrimitives::validateUInt32Length(
    const std::string_view label, const std::string_view operation,
    const runtime::HostVisibleBuffer &buffer, const std::size_t elementCount,
    std::string &error) const {
  if (!buffer.isValid()) {
    return setError(error, "validation",
                    std::string(label) + " " + std::string(operation) +
                        ": buffer is not initialized");
  }
  const auto maxElements =
      static_cast<std::size_t>(buffer.size() / sizeof(std::uint32_t));
  if (elementCount > maxElements) {
    return setError(
        error, "validation",
        std::string(label) + " " + std::string(operation) +
            ": requested " + std::to_string(elementCount) +
            " elements but buffer has " + std::to_string(maxElements));
  }
  return true;
}

bool GatherHistogramPrimitives::validateAlias(
    const std::string_view label,
    const runtime::HostVisibleBuffer &input,
    const runtime::HostVisibleBuffer &output, const bool allowInPlace,
    std::string &error) const {
  if (!allowInPlace && input.handle() == output.handle()) {
    return setError(
        error, "validation",
        std::string(label) + ": in-place input/output aliasing is disallowed");
  }
  return true;
}

bool GatherHistogramPrimitives::ensureMapped(
    runtime::HostVisibleBuffer &buffer, const std::string_view label,
    std::string &error) const {
  if (!buffer.isValid()) {
    return setError(error, "execution",
                    std::string(label) + " buffer is not initialized");
  }
  if (!buffer.mappedPtr() && !buffer.map(error)) {
    return false;
  }
  return true;
}

bool GatherHistogramPrimitives::readOutputIfNeeded(
    runtime::HostVisibleBuffer &buffer, std::string &error) const {
  if (!ensureMapped(buffer, "output", error)) {
    return false;
  }
  if (!buffer.invalidate(error)) {
    return setError(error, "execution",
                    "failed to invalidate output buffer for CPU readback");
  }
  return true;
}

bool GatherHistogramPrimitives::readFloatBuffer(
    runtime::HostVisibleBuffer &buffer, const std::size_t elementCount,
    std::vector<float> &values, std::string &error) const {
  if (elementCount == 0u) {
    values.clear();
    return true;
  }
  if (!validateFloatLength("read", "buffer", buffer, elementCount, error)) {
    return false;
  }
  if (!ensureMapped(buffer, "read", error)) {
    return false;
  }
  if (!buffer.invalidate(error)) {
    return false;
  }
  values.resize(elementCount);
  std::memcpy(values.data(), buffer.mappedPtr(),
              static_cast<std::size_t>(elementCount * sizeof(float)));
  return true;
}

bool GatherHistogramPrimitives::readUInt32Buffer(
    runtime::HostVisibleBuffer &buffer, const std::size_t elementCount,
    std::vector<std::uint32_t> &values, std::string &error) const {
  if (elementCount == 0u) {
    values.clear();
    return true;
  }
  if (!validateUInt32Length("read", "buffer", buffer, elementCount, error)) {
    return false;
  }
  if (!ensureMapped(buffer, "read", error)) {
    return false;
  }
  if (!buffer.invalidate(error)) {
    return false;
  }
  values.resize(elementCount);
  std::memcpy(values.data(), buffer.mappedPtr(),
              static_cast<std::size_t>(elementCount * sizeof(std::uint32_t)));
  return true;
}

bool GatherHistogramPrimitives::writeFloatBuffer(
    runtime::HostVisibleBuffer &buffer, const std::vector<float> &values,
    const std::size_t elementCount, std::string &error) const {
  if (elementCount == 0u) {
    return true;
  }
  if (values.size() < elementCount) {
    return setError(error, "write", "not enough float values provided");
  }
  if (!validateFloatLength("write", "buffer", buffer, elementCount, error)) {
    return false;
  }
  if (!ensureMapped(buffer, "write", error)) {
    return false;
  }
  std::memcpy(buffer.mappedPtr(), values.data(),
              static_cast<std::size_t>(elementCount * sizeof(float)));
  return buffer.flush(error);
}

bool GatherHistogramPrimitives::writeUInt32Buffer(
    runtime::HostVisibleBuffer &buffer, const std::vector<std::uint32_t> &values,
    const std::size_t elementCount, std::string &error) const {
  if (elementCount == 0u) {
    return true;
  }
  if (values.size() < elementCount) {
    return setError(error, "write", "not enough uint32 values provided");
  }
  if (!validateUInt32Length("write", "buffer", buffer, elementCount, error)) {
    return false;
  }
  if (!ensureMapped(buffer, "write", error)) {
    return false;
  }
  std::memcpy(buffer.mappedPtr(), values.data(),
              static_cast<std::size_t>(elementCount * sizeof(std::uint32_t)));
  return buffer.flush(error);
}

bool GatherHistogramPrimitives::validateAndCheckIndices(
    const std::vector<std::uint32_t> &indices,
    const std::size_t validElementCount, const bool allowDuplicates,
    bool &hasDuplicate, std::string &error) const {
  hasDuplicate = false;
  if (!indices.empty() && validElementCount == 0u) {
    return setError(error, "validation",
                    "index validation requires a non-zero element count");
  }

  std::unordered_set<std::uint32_t> seen;
  seen.reserve(indices.size());
  for (const auto index : indices) {
    if (index >= validElementCount) {
      return setError(error, "validation",
                      "index validation found an index outside valid range");
    }
    if (!seen.insert(index).second) {
      hasDuplicate = true;
      if (!allowDuplicates) {
        return setError(error, "validation",
                        "index validation found duplicate index while duplicates "
                        "are disallowed");
      }
    }
  }
  return true;
}

bool GatherHistogramPrimitives::validateHistogramValues(
    const std::vector<std::uint32_t> &values, const std::size_t binCount,
    std::string &error) const {
  if (binCount == 0u) {
    return setError(error, "validation",
                    "histogram values require a non-zero bin count");
  }
  for (const auto value : values) {
    if (value >= binCount) {
      return setError(error, "validation",
                      "histogram input contains value outside bins");
    }
  }
  return true;
}

bool GatherHistogramPrimitives::validateHistogramBinCount(
    const std::size_t binCount, std::string &error) const {
  if (binCount == 0u) {
    return setError(error, "validation", "histogram bin count is zero");
  }
  if (binCount > kMaxHistogramBinCount) {
    std::ostringstream out;
    out << "histogram bin count " << binCount << " exceeds max "
        << kMaxHistogramBinCount;
    return setError(error, "validation", out.str());
  }
  return true;
}

bool GatherHistogramPrimitives::buildDeterministicScatterInputs(
    const std::vector<std::uint32_t> &indices,
    const std::vector<float> &inputValues,
    const std::size_t outputElementCount,
    std::vector<std::uint32_t> &deduplicatedIndices,
    std::vector<float> &deduplicatedValues, std::string &error) const {
  if (indices.size() != inputValues.size()) {
    return setError(error, "deterministic dedupe",
                    "index count and input count differ");
  }
  if (indices.empty()) {
    deduplicatedIndices.clear();
    deduplicatedValues.clear();
    return true;
  }
  if (outputElementCount == 0u) {
    return setError(error, "deterministic dedupe",
                    "deterministic scatter requires non-zero output length");
  }

  std::unordered_map<std::uint32_t, std::size_t> lastPosition;
  lastPosition.reserve(indices.size());
  for (std::size_t i = 0u; i < indices.size(); ++i) {
    lastPosition[indices[i]] = i;
  }

  deduplicatedIndices.clear();
  deduplicatedValues.clear();
  deduplicatedIndices.reserve(lastPosition.size());
  deduplicatedValues.reserve(lastPosition.size());
  for (const auto &it : lastPosition) {
    deduplicatedIndices.push_back(it.first);
  }
  std::sort(deduplicatedIndices.begin(), deduplicatedIndices.end());
  for (const auto index : deduplicatedIndices) {
    deduplicatedValues.push_back(inputValues[lastPosition[index]]);
  }
  return true;
}

bool GatherHistogramPrimitives::zeroUInt32Buffer(
    runtime::HostVisibleBuffer &buffer, const std::size_t elementCount,
    std::string &error) const {
  if (elementCount == 0u) {
    return true;
  }
  if (!validateUInt32Length("write", "buffer", buffer, elementCount, error)) {
    return false;
  }
  if (!ensureMapped(buffer, "write", error)) {
    return false;
  }
  std::memset(buffer.mappedPtr(), 0,
              static_cast<std::size_t>(elementCount * sizeof(std::uint32_t)));
  return buffer.flush(error);
}

bool GatherHistogramPrimitives::dispatch(const OperationContext &context,
                                         std::string &error) {
  if (!isReady(error)) {
    return false;
  }
  if (context.elementCount == 0u) {
    return true;
  }
  if (!ensureDummyBuffers(error)) {
    return false;
  }

  auto *inputFloat = context.inputFloat;
  auto *inputIndices = context.indices;
  auto *outputFloat = context.outputFloat;
  auto *outputHistogram = context.outputHistogram;
  if (inputFloat == nullptr || inputIndices == nullptr || outputFloat == nullptr ||
      outputHistogram == nullptr) {
    return setError(error, "dispatch", "operation context has null buffers");
  }
  if (!((context.operation == GatherHistogramOperation::gather) ||
        (context.operation == GatherHistogramOperation::scatter) ||
        (context.operation == GatherHistogramOperation::histogram))) {
    return setError(error, "dispatch", "unsupported operation");
  }
  if (context.elementCount > std::numeric_limits<std::uint32_t>::max() ||
      context.outputElementCount > std::numeric_limits<std::uint32_t>::max() ||
      context.binCount > std::numeric_limits<std::uint32_t>::max()) {
    return setError(error, "dispatch",
                    "operation lengths exceed 32-bit push-constant fields");
  }

  std::vector<VkBufferMemoryBarrier> preBarriers;
  std::vector<VkBufferMemoryBarrier> postBarriers;
  preBarriers.reserve(3u);
  postBarriers.reserve(2u);
  const auto addPreBarrier = [&](const VkBuffer bufferHandle,
                                 const VkAccessFlags srcAccess,
                                 const VkAccessFlags dstAccess,
                                 const std::size_t bytes) {
    VkBufferMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
    barrier.srcAccessMask = srcAccess;
    barrier.dstAccessMask = dstAccess;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.buffer = bufferHandle;
    barrier.offset = 0u;
    barrier.size = static_cast<VkDeviceSize>(std::max<std::size_t>(1u, bytes));
    preBarriers.push_back(barrier);
  };
  const auto addPostBarrier = [&](const VkBuffer bufferHandle,
                                  const VkAccessFlags dstAccess,
                                  const std::size_t bytes) {
    VkBufferMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
    barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    barrier.dstAccessMask = dstAccess;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.buffer = bufferHandle;
    barrier.offset = 0u;
    barrier.size = static_cast<VkDeviceSize>(std::max<std::size_t>(1u, bytes));
    postBarriers.push_back(barrier);
  };

  bool useHistogramOutput = false;

  if (!ensureMapped(*inputFloat, "input", error) ||
      !ensureMapped(*inputIndices, "indices", error) ||
      !ensureMapped(*outputFloat, "output", error) ||
      !ensureMapped(*outputHistogram, "histogram", error)) {
    return false;
  }

  const auto dispatchX = (context.elementCount / kWorkgroupSize) +
                        (context.elementCount % kWorkgroupSize != 0u);
  if (dispatchX >
      device_.selection().properties.limits.maxComputeWorkGroupCount[0]) {
    return setError(error, "dispatch",
                    "dispatch group count exceeds the device limit");
  }

  std::size_t inputBytes = 0u;
  std::size_t outputBytes = 0u;
  std::size_t indexBytes = 0u;
  std::size_t histogramBytes = 0u;
  switch (context.operation) {
  case GatherHistogramOperation::gather:
    if (!validateFloatLength("dispatch", "input", *inputFloat,
                            context.outputElementCount, error) ||
        !validateFloatLength("dispatch", "output", *outputFloat,
                            context.elementCount, error) ||
        !validateUInt32Length("dispatch", "indices", *inputIndices,
                              context.elementCount, error) ||
        !validateAlias("dispatch gather", *inputFloat, *outputFloat, false,
                       error)) {
      return false;
    }
    inputBytes = context.outputElementCount * sizeof(float);
    outputBytes = context.elementCount * sizeof(float);
    indexBytes = context.elementCount * sizeof(std::uint32_t);
    addPreBarrier(inputFloat->handle(), VK_ACCESS_HOST_WRITE_BIT,
                  VK_ACCESS_SHADER_READ_BIT, inputBytes);
    addPreBarrier(outputFloat->handle(), VK_ACCESS_HOST_WRITE_BIT,
                  VK_ACCESS_SHADER_WRITE_BIT, outputBytes);
    addPreBarrier(inputIndices->handle(), VK_ACCESS_HOST_WRITE_BIT,
                  VK_ACCESS_SHADER_READ_BIT, indexBytes);
    addPostBarrier(outputFloat->handle(), VK_ACCESS_HOST_READ_BIT, outputBytes);
    break;
  case GatherHistogramOperation::scatter:
    if (!validateAlias("dispatch scatter", *inputFloat, *outputFloat, false,
                       error) ||
        !validateFloatLength("dispatch", "input", *inputFloat, context.elementCount,
                            error) ||
        !validateFloatLength("dispatch", "output", *outputFloat,
                            context.outputElementCount, error) ||
        !validateUInt32Length("dispatch", "indices", *inputIndices,
                              context.elementCount, error)) {
      return false;
    }
    inputBytes = context.elementCount * sizeof(float);
    outputBytes = context.outputElementCount * sizeof(float);
    indexBytes = context.elementCount * sizeof(std::uint32_t);
    addPreBarrier(inputFloat->handle(), VK_ACCESS_HOST_WRITE_BIT,
                  VK_ACCESS_SHADER_READ_BIT, inputBytes);
    addPreBarrier(outputFloat->handle(), VK_ACCESS_HOST_WRITE_BIT,
                  VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
                  outputBytes);
    addPreBarrier(inputIndices->handle(), VK_ACCESS_HOST_WRITE_BIT,
                  VK_ACCESS_SHADER_READ_BIT, indexBytes);
    addPostBarrier(outputFloat->handle(), VK_ACCESS_HOST_READ_BIT, outputBytes);
    useHistogramOutput = false;
    break;
  case GatherHistogramOperation::histogram:
    if (!validateAlias("dispatch", *inputFloat, *outputHistogram,
                       false, error) ||
        !validateUInt32Length("dispatch", "input", *inputFloat,
                             context.elementCount, error) ||
        !validateUInt32Length("dispatch", "output histogram", *outputHistogram,
                             context.outputElementCount, error) ||
        !validateUInt32Length("dispatch", "bin count", dummyUInt32Buffer_,
                             1u, error)) {
      return false;
    }
    inputBytes = context.elementCount * sizeof(float);
    outputBytes = 0u;
    indexBytes = 1u * sizeof(std::uint32_t);
    histogramBytes = context.outputElementCount * sizeof(std::uint32_t);
    addPreBarrier(inputFloat->handle(), VK_ACCESS_HOST_WRITE_BIT,
                  VK_ACCESS_SHADER_READ_BIT, inputBytes);
    addPreBarrier(outputHistogram->handle(), VK_ACCESS_HOST_WRITE_BIT,
                  VK_ACCESS_SHADER_WRITE_BIT, histogramBytes);
    addPostBarrier(outputHistogram->handle(), VK_ACCESS_HOST_READ_BIT,
                   histogramBytes);
    useHistogramOutput = true;
    break;
  default:
    return setError(error, "dispatch", "unsupported operation");
  }

  if (!inputFloat->flush(error) || !inputIndices->flush(error) ||
      !outputFloat->flush(error) || !outputHistogram->flush(error)) {
    return false;
  }

  VkDescriptorBufferInfo inputInfo{};
  inputInfo.buffer = inputFloat->handle();
  inputInfo.offset = 0u;
  inputInfo.range = static_cast<VkDeviceSize>(inputBytes);
  VkDescriptorBufferInfo outputInfo{};
  outputInfo.buffer = outputFloat->handle();
  outputInfo.offset = 0u;
  outputInfo.range = static_cast<VkDeviceSize>(
      context.operation == GatherHistogramOperation::histogram
          ? sizeof(float)
          : std::max<std::size_t>(sizeof(float), outputBytes));
  VkDescriptorBufferInfo indexInfo{};
  indexInfo.buffer = inputIndices->handle();
  indexInfo.offset = 0u;
  indexInfo.range = static_cast<VkDeviceSize>(std::max<std::size_t>(1u, indexBytes));
  VkDescriptorBufferInfo histogramInfo{};
  histogramInfo.buffer = outputHistogram->handle();
  histogramInfo.offset = 0u;
  histogramInfo.range = static_cast<VkDeviceSize>(
      useHistogramOutput
          ? std::max<std::size_t>(sizeof(std::uint32_t), histogramBytes)
          : sizeof(std::uint32_t));

  std::array<VkWriteDescriptorSet, 4u> writes{};
  writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
  writes[0].dstSet = descriptorSet_;
  writes[0].dstBinding = 0u;
  writes[0].descriptorCount = 1u;
  writes[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  writes[0].pBufferInfo = &inputInfo;
  writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
  writes[1].dstSet = descriptorSet_;
  writes[1].dstBinding = 1u;
  writes[1].descriptorCount = 1u;
  writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  writes[1].pBufferInfo = &outputInfo;
  writes[2].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
  writes[2].dstSet = descriptorSet_;
  writes[2].dstBinding = 2u;
  writes[2].descriptorCount = 1u;
  writes[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  writes[2].pBufferInfo = &indexInfo;
  writes[3].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
  writes[3].dstSet = descriptorSet_;
  writes[3].dstBinding = 3u;
  writes[3].descriptorCount = 1u;
  writes[3].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  writes[3].pBufferInfo = &histogramInfo;
  vkUpdateDescriptorSets(device_.get(), static_cast<std::uint32_t>(writes.size()),
                        writes.data(), 0u, nullptr);

  VkCommandBufferBeginInfo beginInfo{};
  beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
  beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  if (vkResetCommandBuffer(commandBuffer_, 0u) != VK_SUCCESS) {
    return setError(error, "dispatch", "vkResetCommandBuffer failed");
  }
  if (vkBeginCommandBuffer(commandBuffer_, &beginInfo) != VK_SUCCESS) {
    return setError(error, "dispatch", "vkBeginCommandBuffer failed");
  }

  vkCmdPipelineBarrier(commandBuffer_,
                       VK_PIPELINE_STAGE_HOST_BIT,
                       VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0u, 0u, nullptr,
                       static_cast<std::uint32_t>(preBarriers.size()),
                       preBarriers.data(), 0u, nullptr);

  VkPipeline pipeline = VK_NULL_HANDLE;
  switch (context.operation) {
  case GatherHistogramOperation::gather:
    pipeline = gatherPipeline_.get();
    break;
  case GatherHistogramOperation::scatter:
    pipeline = scatterPipeline_.get();
    break;
  case GatherHistogramOperation::histogram:
    pipeline = histogramPipeline_.get();
    break;
  default:
    return setError(error, "dispatch", "unsupported operation");
  }

  vkCmdBindPipeline(commandBuffer_, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
  vkCmdBindDescriptorSets(commandBuffer_, VK_PIPELINE_BIND_POINT_COMPUTE,
                          pipelineLayout_.get(), 0u, 1u, &descriptorSet_, 0u,
                          nullptr);
  const PushConstants constants{
      static_cast<std::uint32_t>(context.elementCount),
      static_cast<std::uint32_t>(context.outputElementCount),
      static_cast<std::uint32_t>(context.binCount)};
  vkCmdPushConstants(commandBuffer_, pipelineLayout_.get(),
                     VK_SHADER_STAGE_COMPUTE_BIT, 0u, sizeof(PushConstants),
                     &constants);
  vkCmdDispatch(commandBuffer_, static_cast<std::uint32_t>(dispatchX), 1u, 1u);

  vkCmdPipelineBarrier(commandBuffer_, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                       VK_PIPELINE_STAGE_HOST_BIT, 0u, 0u, nullptr,
                       static_cast<std::uint32_t>(postBarriers.size()),
                       postBarriers.data(), 0u, nullptr);

  if (vkEndCommandBuffer(commandBuffer_) != VK_SUCCESS) {
    return setError(error, "dispatch", "vkEndCommandBuffer failed");
  }

  VkSubmitInfo submit{};
  submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
  submit.commandBufferCount = 1u;
  submit.pCommandBuffers = &commandBuffer_;
  if (vkQueueSubmit(device_.computeQueue(), 1u, &submit, fence_.get()) !=
      VK_SUCCESS) {
    return setError(error, "dispatch", "vkQueueSubmit failed");
  }
  if (!fence_.wait(10'000'000'000ULL, error)) {
    return false;
  }
  fence_.reset();

  if (context.operation == GatherHistogramOperation::histogram) {
    return readOutputIfNeeded(*outputHistogram, error);
  }
  return readOutputIfNeeded(*outputFloat, error);
}

bool GatherHistogramPrimitives::gather(
    runtime::HostVisibleBuffer &output, const std::size_t outputElementCount,
    runtime::HostVisibleBuffer &input, const std::size_t inputElementCount,
    runtime::HostVisibleBuffer &indices, const std::size_t indexElementCount,
    std::string &error) {
  if (!isReady(error)) {
    return false;
  }
  if (outputElementCount != indexElementCount) {
    return setError(error, "validation",
                    "gather requires output and index lengths to match");
  }
  if (!validateFloatLength("gather", "input", input, inputElementCount, error) ||
      !validateFloatLength("gather", "output", output, outputElementCount, error) ||
      !validateUInt32Length("gather", "indices", indices, indexElementCount,
                           error)) {
    return false;
  }
  if (!validateAlias("gather", input, output, false, error)) {
    return false;
  }
  if (outputElementCount == 0u) {
    return true;
  }

  std::vector<std::uint32_t> indexValues;
  if (!readUInt32Buffer(indices, indexElementCount, indexValues, error)) {
    return false;
  }
  bool hasDuplicate = false;
  if (!validateAndCheckIndices(indexValues, inputElementCount, true, hasDuplicate,
                               error)) {
    return false;
  }

  return dispatch({GatherHistogramOperation::gather, &input, &indices, &output,
                   &dummyUInt32Buffer_, indexElementCount, inputElementCount,
                   0u},
                  error);
}

bool GatherHistogramPrimitives::scatter(
    runtime::HostVisibleBuffer &output, const std::size_t outputElementCount,
    runtime::HostVisibleBuffer &input, const std::size_t inputElementCount,
    runtime::HostVisibleBuffer &indices, const std::size_t indexElementCount,
    std::string &error, const ScatterOptions options) {
  if (!isReady(error)) {
    return false;
  }
  if (!validateFloatLength("scatter", "input", input, inputElementCount, error) ||
      !validateFloatLength("scatter", "output", output, outputElementCount, error) ||
      !validateUInt32Length("scatter", "indices", indices, indexElementCount,
                           error) ||
      !validateAlias("scatter", input, output, false, error)) {
    return false;
  }
  if (inputElementCount != indexElementCount) {
    return setError(error, "validation",
                    "scatter requires input and index lengths to match");
  }
  if (indexElementCount == 0u) {
    return true;
  }

  std::vector<std::uint32_t> indexValues;
  std::vector<float> inputValues;
  if (!readUInt32Buffer(indices, indexElementCount, indexValues, error) ||
      !readFloatBuffer(input, inputElementCount, inputValues, error)) {
    return false;
  }

  bool hasDuplicate = false;
  if (!validateAndCheckIndices(indexValues, outputElementCount,
                               options.enableDeterministicDuplicatePolicy,
                               hasDuplicate, error)) {
    return false;
  }

  std::size_t effectiveCount = indexElementCount;
  runtime::HostVisibleBuffer *dispatchInput = &input;
  runtime::HostVisibleBuffer *dispatchIndices = &indices;
  runtime::HostVisibleBuffer inputScratch{};
  runtime::HostVisibleBuffer indexScratch{};

  if (hasDuplicate && options.enableDeterministicDuplicatePolicy) {
    std::vector<std::uint32_t> deduplicatedIndices;
    std::vector<float> deduplicatedValues;
    if (!buildDeterministicScatterInputs(indexValues, inputValues,
                                        outputElementCount,
                                        deduplicatedIndices, deduplicatedValues,
                                        error)) {
      return false;
    }
    effectiveCount = deduplicatedIndices.size();
    if (effectiveCount == 0u) {
      return true;
    }
    if (!createFloatBuffer(effectiveCount, inputScratch, error) ||
        !createUInt32Buffer(effectiveCount, indexScratch, error) ||
        !writeFloatBuffer(inputScratch, deduplicatedValues, effectiveCount,
                          error) ||
        !writeUInt32Buffer(indexScratch, deduplicatedIndices, effectiveCount,
                           error)) {
      return false;
    }
    dispatchInput = &inputScratch;
    dispatchIndices = &indexScratch;
  }

  return dispatch(
      {GatherHistogramOperation::scatter, dispatchInput, dispatchIndices,
       &output, &dummyUInt32Buffer_, effectiveCount, outputElementCount, 0u},
      error);
}

bool GatherHistogramPrimitives::histogram(
    runtime::HostVisibleBuffer &histogram, const std::size_t binCount,
    runtime::HostVisibleBuffer &input, const std::size_t inputElementCount,
    std::string &error) {
  if (!isReady(error)) {
    return false;
  }
  if (!validateHistogramBinCount(binCount, error) ||
      !validateUInt32Length("histogram", "input", input, inputElementCount, error) ||
      !validateUInt32Length("histogram", "output", histogram, binCount, error)) {
    return false;
  }
  if (!zeroUInt32Buffer(histogram, binCount, error)) {
    return false;
  }
  if (inputElementCount == 0u) {
    return true;
  }

  std::vector<std::uint32_t> values;
  if (!readUInt32Buffer(input, inputElementCount, values, error)) {
    return false;
  }
  if (!validateHistogramValues(values, binCount, error)) {
    return false;
  }

  return dispatch({GatherHistogramOperation::histogram, &input, &dummyUInt32Buffer_,
                   &dummyFloatBuffer_, &histogram, inputElementCount, binCount,
                   binCount},
                  error);
}

const runtime::VulkanDevice &GatherHistogramPrimitives::device() const {
  return device_;
}

} // namespace viennaps::vulkan::primitives
