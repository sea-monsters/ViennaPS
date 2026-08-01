// CPU-differential smoke for stable Vulkan stream compaction.

#include "reduction_scan_primitives.hpp"

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

#ifndef VIENNAPS_VULKAN_REDUCTION_SCAN_SPV_PATH
#error "VIENNAPS_VULKAN_REDUCTION_SCAN_SPV_PATH must be defined."
#endif

namespace {

using viennaps::vulkan::primitives::ReductionScanPrimitives;
namespace runtime = viennaps::vulkan::runtime;

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

[[nodiscard]] std::vector<std::uint32_t>
makeFlags(const std::size_t capacity, const std::size_t elementCount) {
  std::vector<std::uint32_t> flags(capacity, 0u);
  for (std::size_t index = 0u; index < elementCount; ++index) {
    if ((index % 7u) == 1u || (index % 11u) == 3u) {
      flags[index] = (index % 2u) == 0u ? 2u : 0xffffffffu;
    }
  }
  return flags;
}

template <typename T>
[[nodiscard]] std::vector<T> compactOracle(
    const std::vector<T> &input, const std::vector<std::uint32_t> &flags,
    const std::size_t elementCount, const std::size_t capacity, const T guard) {
  std::vector<T> result(capacity, guard);
  std::size_t outputIndex = 0u;
  for (std::size_t index = 0u; index < elementCount; ++index) {
    if (flags[index] != 0u) {
      result[outputIndex++] = input[index];
    }
  }
  return result;
}

[[nodiscard]] std::size_t countSelected(const std::vector<std::uint32_t> &flags,
                                        const std::size_t elementCount) {
  std::size_t result = 0u;
  for (std::size_t index = 0u; index < elementCount; ++index) {
    result += flags[index] != 0u ? 1u : 0u;
  }
  return result;
}

[[nodiscard]] bool sameFloatBits(const std::vector<float> &left,
                                 const std::vector<float> &right) {
  if (left.size() != right.size()) {
    return false;
  }
  for (std::size_t index = 0u; index < left.size(); ++index) {
    if (std::bit_cast<std::uint32_t>(left[index]) !=
        std::bit_cast<std::uint32_t>(right[index])) {
      return false;
    }
  }
  return true;
}

[[nodiscard]] bool runFloatCases(ReductionScanPrimitives &primitives,
                                 std::string &error) {
  constexpr float kGuard = -91'827.25F;
  const auto capacity = kLengths.back() + 1u;
  runtime::HostVisibleBuffer input{};
  runtime::HostVisibleBuffer flags{};
  runtime::HostVisibleBuffer output{};
  if (!primitives.createFloatBuffer(capacity, input, error) ||
      !primitives.createIntBuffer(capacity, flags, error) ||
      !primitives.createFloatBuffer(capacity, output, error)) {
    return false;
  }

  for (const auto length : kLengths) {
    std::vector<float> values(capacity, kGuard);
    for (std::size_t index = 0u; index < length; ++index) {
      const auto signedValue = static_cast<std::int32_t>(index % 257u) - 128;
      values[index] = static_cast<float>(signedValue) * 0.25F;
    }
    constexpr std::array<std::uint32_t, 6u> boundaryPatterns = {
        0u, 0x7fc00001u, 0x80000000u, 0x7f800001u, 0xff800000u, 0x3f800000u};
    for (std::size_t index = 0u;
         index < length && index < boundaryPatterns.size(); ++index) {
      values[index] = std::bit_cast<float>(boundaryPatterns[index]);
    }
    const auto flagValues = makeFlags(capacity, length);
    const auto expectedCount = countSelected(flagValues, length);
    const auto expected =
        compactOracle(values, flagValues, length, capacity, kGuard);
    std::vector<float> sentinels(capacity, kGuard);
    if (!writeBuffer(input, values, error) ||
        !writeBuffer(flags, flagValues, error) ||
        !writeBuffer(output, sentinels, error)) {
      return false;
    }
    std::size_t actualCount = 0u;
    if (!primitives.stableCompactFloat(input, length, flags, length, output,
                                       length, actualCount, error)) {
      return false;
    }
    std::vector<float> actual;
    std::vector<float> inputAfter;
    std::vector<std::uint32_t> flagsAfter;
    if (!readBuffer(output, capacity, actual, error) ||
        !sameFloatBits(actual, expected) ||
        !readBuffer(input, capacity, inputAfter, error) ||
        !sameFloatBits(inputAfter, values) ||
        !readBuffer(flags, capacity, flagsAfter, error) ||
        flagsAfter != flagValues || actualCount != expectedCount) {
      error = "float compaction mismatch or guard overwrite at length " +
              std::to_string(length);
      return false;
    }
  }
  return true;
}

[[nodiscard]] bool runUInt32Cases(ReductionScanPrimitives &primitives,
                                  std::string &error) {
  constexpr std::uint32_t kGuard = 0xdeadbeefu;
  constexpr std::array<std::size_t, 4u> lengths = {0u, 1u, 257u, 1'000'003u};
  const auto capacity = lengths.back() + 1u;
  runtime::HostVisibleBuffer input{};
  runtime::HostVisibleBuffer flags{};
  runtime::HostVisibleBuffer output{};
  if (!primitives.createIntBuffer(capacity, input, error) ||
      !primitives.createIntBuffer(capacity, flags, error) ||
      !primitives.createIntBuffer(capacity, output, error)) {
    return false;
  }

  for (const auto length : lengths) {
    std::vector<std::uint32_t> values(capacity, kGuard);
    for (std::size_t index = 0u; index < length; ++index) {
      values[index] =
          static_cast<std::uint32_t>(index * 2'654'435'761u) ^ 0x80000000u;
    }
    constexpr std::array<std::uint32_t, 6u> boundaryPatterns = {
        0u, 0xffffffffu, 0x7fc00001u, 0x7f800001u, 0x80000000u, 0x3f800000u};
    for (std::size_t index = 0u;
         index < length && index < boundaryPatterns.size(); ++index) {
      values[index] = boundaryPatterns[index];
    }
    const auto flagValues = makeFlags(capacity, length);
    const auto expectedCount = countSelected(flagValues, length);
    const auto expected =
        compactOracle(values, flagValues, length, capacity, kGuard);
    std::vector<std::uint32_t> sentinels(capacity, kGuard);
    if (!writeBuffer(input, values, error) ||
        !writeBuffer(flags, flagValues, error) ||
        !writeBuffer(output, sentinels, error)) {
      return false;
    }
    std::size_t actualCount = 0u;
    if (!primitives.stableCompactUInt32(input, length, flags, length, output,
                                        length, actualCount, error)) {
      return false;
    }
    std::vector<std::uint32_t> actual;
    if (!readBuffer(output, capacity, actual, error) || actual != expected ||
        actualCount != expectedCount) {
      error = "uint32 compaction mismatch or guard overwrite at length " +
              std::to_string(length);
      return false;
    }
  }
  return true;
}

[[nodiscard]] bool runAllZeroCase(ReductionScanPrimitives &primitives,
                                  std::string &error) {
  constexpr std::size_t length = 257u;
  constexpr std::uint32_t guard = 0xa55aa55au;
  std::vector<std::uint32_t> values(length, 7u);
  std::vector<std::uint32_t> flagsValues(length, 0u);
  std::vector<std::uint32_t> sentinels(length, guard);
  runtime::HostVisibleBuffer input{};
  runtime::HostVisibleBuffer flags{};
  runtime::HostVisibleBuffer output{};
  if (!primitives.createIntBuffer(length, input, error) ||
      !primitives.createIntBuffer(length, flags, error) ||
      !primitives.createIntBuffer(length, output, error) ||
      !writeBuffer(input, values, error) ||
      !writeBuffer(flags, flagsValues, error) ||
      !writeBuffer(output, sentinels, error)) {
    return false;
  }
  std::size_t selectedCount = 1u;
  if (!primitives.stableCompactUInt32(input, length, flags, length, output, 0u,
                                      selectedCount, error)) {
    return false;
  }
  std::vector<std::uint32_t> actual;
  if (selectedCount != 0u || !readBuffer(output, length, actual, error) ||
      actual != sentinels) {
    error = "all-zero compaction modified output or returned a nonzero count";
    return false;
  }
  return true;
}

[[nodiscard]] bool runValidationCases(ReductionScanPrimitives &primitives,
                                      std::string &error) {
  constexpr std::size_t length = 16u;
  std::vector<std::uint32_t> values(length, 3u);
  std::vector<std::uint32_t> flagsValues(length, 1u);
  std::vector<std::uint32_t> sentinels(length, 0xcafebabeu);
  runtime::HostVisibleBuffer input{};
  runtime::HostVisibleBuffer flags{};
  runtime::HostVisibleBuffer output{};
  if (!primitives.createIntBuffer(length, input, error) ||
      !primitives.createIntBuffer(length, flags, error) ||
      !primitives.createIntBuffer(length, output, error) ||
      !writeBuffer(input, values, error) ||
      !writeBuffer(flags, flagsValues, error) ||
      !writeBuffer(output, sentinels, error)) {
    return false;
  }

  std::size_t selectedCount = 0u;
  if (primitives.stableCompactUInt32(input, length, flags, length - 1u, output,
                                     length, selectedCount, error)) {
    error = "mismatched compaction lengths succeeded unexpectedly";
    return false;
  }
  error.clear();
  if (primitives.stableCompactUInt32(input, length, flags, length, output,
                                     length - 1u, selectedCount, error)) {
    error = "undersized compaction output succeeded unexpectedly";
    return false;
  }
  if (selectedCount != length) {
    error = "capacity rejection did not return the required selected count";
    return false;
  }
  error.clear();
  std::vector<std::uint32_t> outputAfter;
  if (!readBuffer(output, length, outputAfter, error) ||
      outputAfter != sentinels) {
    error = "capacity rejection modified compaction output";
    return false;
  }
  if (primitives.stableCompactUInt32(input, length, flags, length, input,
                                     length, selectedCount, error)) {
    error = "in-place compaction succeeded unexpectedly";
    return false;
  }
  error.clear();
  if (primitives.stableCompactUInt32(input, length, input, length, output,
                                     length, selectedCount, error)) {
    error = "aliased compaction flags succeeded unexpectedly";
    return false;
  }
  error.clear();

  runtime::HostVisibleBuffer oversized{};
  const auto maxStorageBytes =
      primitives.device().selection().properties.limits.maxStorageBufferRange;
  const auto oversizedElements =
      static_cast<std::size_t>(maxStorageBytes / sizeof(std::uint32_t)) + 1u;
  if (primitives.createIntBuffer(oversizedElements, oversized, error)) {
    error = "oversized storage buffer succeeded unexpectedly";
    return false;
  }
  error.clear();

  ReductionScanPrimitives other{};
  runtime::HostVisibleBuffer foreign{};
  if (!other.initialize(VIENNAPS_VULKAN_REDUCTION_SCAN_SPV_PATH, error) ||
      !other.createIntBuffer(length, foreign, error)) {
    return false;
  }
  if (primitives.stableCompactUInt32(foreign, length, flags, length, output,
                                     length, selectedCount, error)) {
    error = "cross-device compaction input succeeded unexpectedly";
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
      !runFloatCases(primitives, error) || !runUInt32Cases(primitives, error) ||
      !runAllZeroCase(primitives, error) ||
      !runValidationCases(primitives, error)) {
    std::cerr << error << '\n';
    return 1;
  }
  std::cout << "[CompactionPrimitivesSmoke] PASS\n";
  return 0;
}
