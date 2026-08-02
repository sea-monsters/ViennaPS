// CPU-differential smoke for recursive Vulkan reduction and exclusive scan.

#include "reduction_scan_primitives.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <limits>
#include <string>
#include <utility>
#include <vector>

#ifndef VIENNAPS_VULKAN_REDUCTION_SCAN_SPV_PATH
#error "VIENNAPS_VULKAN_REDUCTION_SCAN_SPV_PATH must be defined."
#endif

namespace {

using viennaps::vulkan::primitives::ReductionScanOptions;
using viennaps::vulkan::primitives::ReductionScanPrimitives;
using viennaps::vulkan::primitives::ReductionScanStats;
namespace runtime = viennaps::vulkan::runtime;

[[maybe_unused]] bool recordApiSurfaceProbe(ReductionScanPrimitives &primitives,
                                            VkCommandBuffer commandBuffer,
                                            runtime::DeviceBuffer &input,
                                            runtime::DeviceBuffer &output,
                                            runtime::DeviceBuffer &count,
                                            std::string &error) {
  ReductionScanPrimitives::DeviceScanScratch scratch{};
  return primitives.recordExclusiveScanInt(commandBuffer, input, 1u, output, 1u,
                                           scratch, error) &&
         primitives.recordWriteCompactionCount(commandBuffer, input, output, 1u,
                                               count, error);
}

constexpr std::size_t kWorkgroupSize = 256u;
constexpr std::array<std::size_t, 6u> kLengths = {0u,   1u,      16u,
                                                  257u, 65'535u, 1'000'003u};

template <typename T>
[[nodiscard]] bool writeBuffer(runtime::HostVisibleBuffer &buffer,
                               const std::vector<T> &values,
                               std::string &error) {
  if (values.empty()) {
    return true;
  }
  return buffer.write(values.data(),
                      static_cast<VkDeviceSize>(values.size() * sizeof(T)), 0u,
                      error);
}

template <typename T>
[[nodiscard]] bool readBuffer(runtime::HostVisibleBuffer &buffer,
                              const std::size_t count, std::vector<T> &values,
                              std::string &error) {
  values.resize(count);
  if (count == 0u) {
    return true;
  }
  return buffer.read(values.data(),
                     static_cast<VkDeviceSize>(count * sizeof(T)), 0u, error);
}

[[nodiscard]] bool sameFloat(const float left, const float right) {
  return std::bit_cast<std::uint32_t>(left) ==
         std::bit_cast<std::uint32_t>(right);
}

[[nodiscard]] ReductionScanStats
cpuReductionOracle(const std::vector<float> &values,
                   const std::size_t elementCount) {
  if (elementCount == 0u) {
    return {};
  }
  std::vector<ReductionScanStats> records;
  bool firstPass = true;
  std::size_t currentCount = elementCount;
  while (true) {
    const auto blockCount =
        currentCount / kWorkgroupSize + (currentCount % kWorkgroupSize != 0u);
    std::vector<ReductionScanStats> next(blockCount);
    for (std::size_t block = 0u; block < blockCount; ++block) {
      std::array<float, kWorkgroupSize> sums{};
      std::array<float, kWorkgroupSize> mins{};
      std::array<float, kWorkgroupSize> maxs{};
      mins.fill(std::numeric_limits<float>::infinity());
      maxs.fill(-std::numeric_limits<float>::infinity());
      for (std::size_t local = 0u; local < kWorkgroupSize; ++local) {
        const auto index = block * kWorkgroupSize + local;
        if (index >= currentCount) {
          continue;
        }
        if (firstPass) {
          sums[local] = values[index];
          mins[local] = values[index];
          maxs[local] = values[index];
        } else {
          sums[local] = records[index].sum;
          mins[local] = records[index].minValue;
          maxs[local] = records[index].maxValue;
        }
      }
      for (std::size_t stride = kWorkgroupSize / 2u; stride > 0u;
           stride /= 2u) {
        for (std::size_t local = 0u; local < stride; ++local) {
          sums[local] += sums[local + stride];
          mins[local] = std::min(mins[local], mins[local + stride]);
          maxs[local] = std::max(maxs[local], maxs[local + stride]);
        }
      }
      next[block] = {sums[0], mins[0], maxs[0]};
    }
    if (blockCount == 1u) {
      return next[0];
    }
    records = std::move(next);
    currentCount = blockCount;
    firstPass = false;
  }
}

[[nodiscard]] bool runReductionCases(ReductionScanPrimitives &primitives,
                                     std::string &error) {
  constexpr float kGuard = -12'345.5F;
  const auto capacity = kLengths.back() + 1u;
  runtime::HostVisibleBuffer input{};
  if (!primitives.createFloatBuffer(capacity, input, error)) {
    return false;
  }

  for (const auto length : kLengths) {
    std::vector<float> values(capacity, kGuard);
    for (std::size_t index = 0u; index < length; ++index) {
      const auto centered = static_cast<std::int32_t>(index % 31u) - 15;
      values[index] = static_cast<float>(centered) * 0.125F;
    }
    if (!writeBuffer(input, values, error)) {
      return false;
    }
    ReductionScanStats actual{};
    const auto expected = cpuReductionOracle(values, length);
    if (!primitives.reduceSumMinMax(input, length, actual, error)) {
      return false;
    }
    if (!sameFloat(actual.sum, expected.sum) ||
        !sameFloat(actual.minValue, expected.minValue) ||
        !sameFloat(actual.maxValue, expected.maxValue)) {
      error = "reduction mismatch at length " + std::to_string(length);
      return false;
    }
    std::vector<float> after;
    if (!readBuffer(input, capacity, after, error) || after != values) {
      error = "reduction modified its input or guard region";
      return false;
    }
  }
  return true;
}

[[nodiscard]] std::vector<std::int32_t>
scanOracle(const std::vector<std::int32_t> &input,
           const std::size_t elementCount, const std::int32_t guard) {
  std::vector<std::int32_t> output(input.size(), guard);
  std::uint32_t running = 0u;
  for (std::size_t index = 0u; index < elementCount; ++index) {
    output[index] = std::bit_cast<std::int32_t>(running);
    running += std::bit_cast<std::uint32_t>(input[index]);
  }
  return output;
}

[[nodiscard]] bool runScanWrapCase(ReductionScanPrimitives &primitives,
                                   std::string &error) {
  const std::vector<std::int32_t> values = {
      std::numeric_limits<std::int32_t>::max(), 1, 2, -7};
  const auto expected = scanOracle(values, values.size(), 0);
  runtime::HostVisibleBuffer input{};
  runtime::HostVisibleBuffer output{};
  if (!primitives.createIntBuffer(values.size(), input, error) ||
      !primitives.createIntBuffer(values.size(), output, error) ||
      !writeBuffer(input, values, error) ||
      !primitives.exclusiveScanInt(input, values.size(), output, values.size(),
                                   error)) {
    return false;
  }
  std::vector<std::int32_t> actual;
  if (!readBuffer(output, values.size(), actual, error) || actual != expected) {
    error = "scan modulo-2^32 wrap mismatch";
    return false;
  }
  return true;
}

[[nodiscard]] bool runScanCases(ReductionScanPrimitives &primitives,
                                std::string &error) {
  constexpr std::int32_t kGuard = 0x4a5b6c7d;
  const auto capacity = kLengths.back() + 1u;
  runtime::HostVisibleBuffer input{};
  runtime::HostVisibleBuffer output{};
  if (!primitives.createIntBuffer(capacity, input, error) ||
      !primitives.createIntBuffer(capacity, output, error)) {
    return false;
  }

  for (const auto length : kLengths) {
    std::vector<std::int32_t> values(capacity, 0);
    std::vector<std::int32_t> sentinels(capacity, kGuard);
    for (std::size_t index = 0u; index < length; ++index) {
      values[index] = static_cast<std::int32_t>(index % 7u) - 3;
    }
    const auto expected = scanOracle(values, length, kGuard);
    if (!writeBuffer(input, values, error) ||
        !writeBuffer(output, sentinels, error) ||
        !primitives.exclusiveScanInt(input, length, output, length, error)) {
      return false;
    }
    std::vector<std::int32_t> actual;
    if (!readBuffer(output, capacity, actual, error) || actual != expected) {
      error = "scan mismatch or guard overwrite at length " +
              std::to_string(length);
      return false;
    }
  }
  return true;
}

[[nodiscard]] bool runInPlaceCases(ReductionScanPrimitives &primitives,
                                   std::string &error) {
  constexpr std::size_t kLength = 65'535u;
  constexpr std::int32_t kGuard = 0x12345678;
  runtime::HostVisibleBuffer buffer{};
  if (!primitives.createIntBuffer(kLength + 1u, buffer, error)) {
    return false;
  }
  std::vector<std::int32_t> values(kLength + 1u, kGuard);
  for (std::size_t index = 0u; index < kLength; ++index) {
    values[index] = static_cast<std::int32_t>(index % 5u) - 2;
  }
  if (!writeBuffer(buffer, values, error)) {
    return false;
  }
  if (primitives.exclusiveScanInt(buffer, kLength, buffer, kLength, error)) {
    error = "in-place scan succeeded without explicit opt-in";
    return false;
  }
  error.clear();
  const auto expected = scanOracle(values, kLength, kGuard);
  if (!primitives.exclusiveScanInt(
          buffer, kLength, buffer, kLength, error,
          ReductionScanOptions{.allowInPlaceScan = true})) {
    return false;
  }
  std::vector<std::int32_t> actual;
  if (!readBuffer(buffer, kLength + 1u, actual, error) || actual != expected) {
    error = "opt-in in-place scan mismatch";
    return false;
  }
  return true;
}

[[nodiscard]] bool runValidationCases(ReductionScanPrimitives &primitives,
                                      std::string &error) {
  runtime::HostVisibleBuffer floatInput{};
  runtime::HostVisibleBuffer intInput{};
  runtime::HostVisibleBuffer intOutput{};
  if (!primitives.createFloatBuffer(4u, floatInput, error) ||
      !primitives.createIntBuffer(4u, intInput, error) ||
      !primitives.createIntBuffer(4u, intOutput, error)) {
    return false;
  }
  ReductionScanStats stats{};
  if (primitives.reduceSumMinMax(floatInput, 5u, stats, error)) {
    error = "oversized reduction succeeded unexpectedly";
    return false;
  }
  error.clear();
  if (primitives.exclusiveScanInt(intInput, 4u, intOutput, 3u, error)) {
    error = "mismatched scan lengths succeeded unexpectedly";
    return false;
  }
  error.clear();
  return true;
}

[[nodiscard]] bool runDeviceScanCases(runtime::ComputeSession &session,
                                      ReductionScanPrimitives &primitives,
                                      std::string &error) {
  constexpr std::array<std::size_t, 6u> lengths = {0u,   1u,   255u,
                                                   256u, 257u, 513u};
  constexpr std::int32_t guard = 0x13579bdf;
  for (const auto length : lengths) {
    const auto capacity = std::max<std::size_t>(1u, length);
    runtime::DeviceBuffer input{};
    runtime::DeviceBuffer output{};
    if (!input.create(session, capacity * sizeof(std::int32_t), error) ||
        !output.create(session, capacity * sizeof(std::int32_t), error)) {
      return false;
    }
    std::vector<std::int32_t> values(capacity, 0);
    std::vector<std::int32_t> expected(capacity, guard);
    for (std::size_t index = 0u; index < length; ++index) {
      values[index] = static_cast<std::int32_t>(index % 5u) - 2;
      expected[index] = scanOracle(values, length, guard)[index];
    }
    if (!output.upload(session, expected.data(),
                       expected.size() * sizeof(std::int32_t), 0u, error) ||
        !input.upload(session, values.data(),
                      values.size() * sizeof(std::int32_t), 0u, error) ||
        !primitives.exclusiveScanInt(input, length, output, length, error)) {
      return false;
    }
    std::vector<std::int32_t> actual(capacity, 0);
    if (!output.download(session, actual.data(),
                         actual.size() * sizeof(std::int32_t), 0u, error) ||
        actual != expected) {
      error = "device scan mismatch at length " + std::to_string(length);
      return false;
    }
  }

  runtime::DeviceBuffer flags{};
  runtime::DeviceBuffer offsets{};
  runtime::DeviceBuffer count{};
  constexpr std::size_t countLength = 513u;
  if (!flags.create(session, countLength * sizeof(std::int32_t), error) ||
      !offsets.create(session, countLength * sizeof(std::int32_t), error) ||
      !count.create(session, sizeof(std::int32_t), error)) {
    return false;
  }
  std::vector<std::int32_t> flagValues(countLength, 0);
  for (std::size_t index = 0u; index < countLength; ++index) {
    flagValues[index] = (index % 3u) == 0u ? 1 : 0;
  }
  if (!flags.upload(session, flagValues.data(),
                    flagValues.size() * sizeof(std::int32_t), 0u, error) ||
      !primitives.exclusiveScanInt(flags, countLength, offsets, countLength,
                                   error) ||
      !primitives.writeCompactionCount(flags, offsets, countLength, count,
                                       error)) {
    return false;
  }
  std::int32_t actualCount = -1;
  if (!count.download(session, &actualCount, sizeof(actualCount), 0u, error) ||
      actualCount != static_cast<std::int32_t>((countLength + 2u) / 3u)) {
    error = "device compaction count mismatch";
    return false;
  }

  // Record-only scan/count must share one externally-owned command buffer and
  // leave all scratch alive until this terminal submission completes.
  runtime::DeviceBuffer recordedOffsets{};
  runtime::DeviceBuffer recordedCount{};
  if (!recordedOffsets.create(session, countLength * sizeof(std::int32_t),
                              error) ||
      !recordedCount.create(session, sizeof(std::int32_t), error)) {
    return false;
  }
  ReductionScanPrimitives::DeviceScanScratch recordScratch{};
  VkCommandBuffer recordCommand = VK_NULL_HANDLE;
  if (!session.commandContext().allocatePrimary(recordCommand, error))
    return false;
  VkCommandBufferBeginInfo recordBegin{
      VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  recordBegin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  if (vkBeginCommandBuffer(recordCommand, &recordBegin) != VK_SUCCESS ||
      !primitives.recordExclusiveScanInt(recordCommand, flags, countLength,
                                         recordedOffsets, countLength,
                                         recordScratch, error) ||
      !primitives.recordWriteCompactionCount(recordCommand, flags,
                                             recordedOffsets, countLength,
                                             recordedCount, error) ||
      vkEndCommandBuffer(recordCommand) != VK_SUCCESS) {
    return false;
  }
  runtime::Fence recordFence{};
  if (!recordFence.create(session.device(), error))
    return false;
  VkSubmitInfo recordSubmit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
  recordSubmit.commandBufferCount = 1u;
  recordSubmit.pCommandBuffers = &recordCommand;
  if (vkQueueSubmit(session.device().computeQueue(), 1u, &recordSubmit,
                    recordFence.get()) != VK_SUCCESS ||
      !recordFence.wait(std::numeric_limits<std::uint64_t>::max(), error)) {
    return false;
  }
  recordFence.reset();
  std::int32_t recordedActual = -1;
  if (!recordedCount.download(session, &recordedActual, sizeof(recordedActual),
                              0u, error) ||
      recordedActual != actualCount) {
    error = "record-only compaction count mismatch";
    return false;
  }
  vkFreeCommandBuffers(session.device().get(), session.commandContext().pool(),
                       1u, &recordCommand);
  runtime::ComputeSession foreignSession{};
  if (!foreignSession.initialize(error)) {
    return false;
  }
  runtime::DeviceBuffer foreign{};
  if (!foreign.create(foreignSession, sizeof(std::int32_t), error)) {
    return false;
  }
  constexpr std::int32_t sentinel = 0x2468ace;
  if (!offsets.upload(session, &sentinel, sizeof(sentinel), 0u, error)) {
    return false;
  }
  error.clear();
  if (primitives.exclusiveScanInt(foreign, 1u, offsets, 1u, error)) {
    error = "cross-session device scan succeeded unexpectedly";
    return false;
  }
  std::int32_t actualSentinel = 0;
  if (!offsets.download(session, &actualSentinel, sizeof(actualSentinel), 0u,
                        error) ||
      actualSentinel != sentinel) {
    error = "cross-session rejection changed device output";
    return false;
  }
  error.clear();
  if (primitives.exclusiveScanInt(offsets, 1u, offsets, 1u, error)) {
    error = "device in-place scan succeeded unexpectedly";
    return false;
  }
  error.clear();
  if (primitives.exclusiveScanInt(flags, countLength, offsets, countLength - 1u,
                                  error)) {
    error = "device length mismatch succeeded unexpectedly";
    return false;
  }
  return true;
}

[[nodiscard]] bool
runGenerationRecoveryCases(runtime::ComputeSession &session,
                           ReductionScanPrimitives &primitives,
                           std::string &error) {
  runtime::DeviceBuffer staleInput{};
  runtime::DeviceBuffer staleOutput{};
  if (!staleInput.create(session, 257u * sizeof(std::int32_t), error) ||
      !staleOutput.create(session, 257u * sizeof(std::int32_t), error)) {
    return false;
  }

  const auto boundGeneration = primitives.boundSessionGeneration();
  runtime::ComputeSession movedSession = std::move(session);
  if (movedSession.generation() != boundGeneration || session.isValid()) {
    error = "compute session move did not preserve generation ownership";
    return false;
  }
  error.clear();
  runtime::HostVisibleBuffer staleHost{};
  if (primitives.createIntBuffer(1u, staleHost, error) ||
      error.find("stale compute session generation") == std::string::npos) {
    error = "stale reduction/scan primitive accepted a host operation";
    return false;
  }
  error.clear();
  if (primitives.exclusiveScanInt(staleInput, 1u, staleOutput, 1u, error) ||
      error.find("stale compute session generation") == std::string::npos) {
    error = "stale reduction/scan primitive accepted a device operation";
    return false;
  }
  error.clear();
  if (primitives.initialize(movedSession,
                            VIENNAPS_VULKAN_REDUCTION_SCAN_SPV_PATH, error) ||
      error.find("bound compute session is stale") == std::string::npos) {
    error = "stale reduction/scan primitive accepted reinitialization";
    return false;
  }
  error.clear();

  // A primitive must be torn down while its original Vulkan device is still
  // alive; session reset/reinitialization is not concurrent with primitive
  // resource destruction.
  staleInput.reset();
  staleOutput.reset();
  primitives.reset();
  if (!primitives.initialize(movedSession,
                             VIENNAPS_VULKAN_REDUCTION_SCAN_SPV_PATH, error)) {
    return false;
  }

  constexpr std::array<std::size_t, 2u> lengths = {1u, 257u};
  for (const auto length : lengths) {
    runtime::DeviceBuffer input{};
    runtime::DeviceBuffer output{};
    runtime::DeviceBuffer flags{};
    runtime::DeviceBuffer offsets{};
    runtime::DeviceBuffer count{};
    if (!input.create(movedSession, length * sizeof(std::int32_t), error) ||
        !output.create(movedSession, length * sizeof(std::int32_t), error) ||
        !flags.create(movedSession, length * sizeof(std::int32_t), error) ||
        !offsets.create(movedSession, length * sizeof(std::int32_t), error) ||
        !count.create(movedSession, sizeof(std::int32_t), error)) {
      return false;
    }
    std::vector<std::int32_t> values(length);
    std::vector<std::int32_t> expected(length);
    std::vector<std::int32_t> flagValues(length);
    for (std::size_t index = 0u; index < length; ++index) {
      values[index] = static_cast<std::int32_t>(index % 7u) - 3;
      expected[index] = scanOracle(values, length, 0)[index];
      flagValues[index] = index % 3u == 0u ? 1 : 0;
    }
    if (!input.upload(movedSession, values.data(),
                      values.size() * sizeof(values[0]), 0u, error) ||
        !flags.upload(movedSession, flagValues.data(),
                      flagValues.size() * sizeof(flagValues[0]), 0u, error) ||
        !primitives.exclusiveScanInt(input, length, output, length, error) ||
        !output.download(movedSession, expected.data(),
                         expected.size() * sizeof(expected[0]), 0u, error)) {
      return false;
    }
    for (std::size_t index = 0u; index < length; ++index) {
      if (expected[index] != scanOracle(values, length, 0)[index]) {
        error = "recovered device scan mismatch at length " +
                std::to_string(length);
        return false;
      }
    }
    if (!primitives.exclusiveScanInt(flags, length, offsets, length, error) ||
        !primitives.writeCompactionCount(flags, offsets, length, count,
                                         error)) {
      return false;
    }
    std::int32_t actualCount = -1;
    if (!count.download(movedSession, &actualCount, sizeof(actualCount), 0u,
                        error) ||
        actualCount != static_cast<std::int32_t>((length + 2u) / 3u)) {
      error = "recovered device compaction count mismatch at length " +
              std::to_string(length);
      return false;
    }
  }
  primitives.reset();
  return true;
}

} // namespace

int main() {
  std::string error;
  runtime::ComputeSession session{};
  ReductionScanPrimitives primitives{};
  if (!session.initialize(error) ||
      !primitives.initialize(session, VIENNAPS_VULKAN_REDUCTION_SCAN_SPV_PATH,
                             error) ||
      !runReductionCases(primitives, error) ||
      !runScanCases(primitives, error) || !runScanWrapCase(primitives, error) ||
      !runInPlaceCases(primitives, error) ||
      !runValidationCases(primitives, error) ||
      !runDeviceScanCases(session, primitives, error) ||
      !runGenerationRecoveryCases(session, primitives, error)) {
    std::cerr << error << '\n';
    return 1;
  }
  std::cout << "[ReductionScanPrimitivesSmoke] PASS\n";
  return 0;
}
