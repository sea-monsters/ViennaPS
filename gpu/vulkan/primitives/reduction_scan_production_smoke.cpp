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
#include <vector>

#ifndef VIENNAPS_VULKAN_REDUCTION_SCAN_SPV_PATH
#error "VIENNAPS_VULKAN_REDUCTION_SCAN_SPV_PATH must be defined."
#endif

namespace {

using viennaps::vulkan::primitives::ReductionScanOptions;
using viennaps::vulkan::primitives::ReductionScanPrimitives;
using viennaps::vulkan::primitives::ReductionScanStats;
namespace runtime = viennaps::vulkan::runtime;

constexpr std::size_t kWorkgroupSize = 256u;
constexpr std::array<std::size_t, 6u> kLengths = {
    0u, 1u, 16u, 257u, 65'535u, 1'000'003u};

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
                              const std::size_t count,
                              std::vector<T> &values, std::string &error) {
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
    const auto blockCount = currentCount / kWorkgroupSize +
                            (currentCount % kWorkgroupSize != 0u);
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

} // namespace

int main() {
  std::string error;
  ReductionScanPrimitives primitives{};
  if (!primitives.initialize(VIENNAPS_VULKAN_REDUCTION_SCAN_SPV_PATH, error) ||
      !runReductionCases(primitives, error) ||
      !runScanCases(primitives, error) ||
      !runScanWrapCase(primitives, error) ||
      !runInPlaceCases(primitives, error) ||
      !runValidationCases(primitives, error)) {
    std::cerr << error << '\n';
    return 1;
  }
  std::cout << "[ReductionScanPrimitivesSmoke] PASS\n";
  return 0;
}
