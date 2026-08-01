// CPU-differential smoke for Vulkan 4-bit radix sort primitives.

#include "radix_sort_primitives.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <limits>
#include <string>
#include <utility>
#include <vector>

#include <vulkan/vulkan.h>

#ifndef VIENNAPS_VULKAN_RADIX_SORT_SPV_PATH
#error "VIENNAPS_VULKAN_RADIX_SORT_SPV_PATH must be defined."
#endif

namespace {

using viennaps::vulkan::primitives::RadixSortPrimitives;
namespace runtime = viennaps::vulkan::runtime;

constexpr std::array<std::size_t, 6u> kLengths = {0u,   1u,      16u,
                                                  257u, 65'535u, 1'000'003u};

template <typename T>
[[nodiscard]] bool
writeBuffer(runtime::HostVisibleBuffer &buffer, const std::vector<T> &values,
            const std::size_t elementCount, std::string &error) {
  if (elementCount == 0u) {
    return true;
  }
  if (values.size() < elementCount) {
    error = "writeBuffer: insufficient source values";
    return false;
  }
  return buffer.write(values.data(),
                      static_cast<VkDeviceSize>(elementCount * sizeof(T)), 0u,
                      error);
}

template <typename T>
[[nodiscard]] bool
readBuffer(runtime::HostVisibleBuffer &buffer, std::vector<T> &values,
           const std::size_t elementCount, std::string &error) {
  values.resize(elementCount);
  if (elementCount == 0u) {
    return true;
  }
  return buffer.read(values.data(),
                     static_cast<VkDeviceSize>(elementCount * sizeof(T)), 0u,
                     error);
}

[[nodiscard]] bool
sameSortedPairs(const std::vector<std::pair<std::uint32_t, std::uint32_t>> &lhs,
                const std::vector<std::pair<std::uint32_t, std::uint32_t>> &rhs,
                std::string &error) {
  if (lhs.size() != rhs.size()) {
    error = "sorted pair result length mismatch";
    return false;
  }
  if (lhs != rhs) {
    error = "sorted key/value pairs differ";
    return false;
  }
  return true;
}

[[nodiscard]] bool runCase(const std::size_t elementCount,
                           RadixSortPrimitives &primitives, std::string &error,
                           const std::uint32_t pattern = 0u) {
  const std::size_t capacity = std::max<std::size_t>(1u, elementCount + 8u);
  runtime::HostVisibleBuffer inputKeys{};
  runtime::HostVisibleBuffer inputValues{};
  runtime::HostVisibleBuffer sortedKeys{};
  runtime::HostVisibleBuffer sortedValues{};
  if (!primitives.createKeyBuffer(capacity, inputKeys, error) ||
      !primitives.createValueBuffer(capacity, inputValues, error) ||
      !primitives.createKeyBuffer(capacity, sortedKeys, error) ||
      !primitives.createValueBuffer(capacity, sortedValues, error)) {
    return false;
  }

  std::vector<std::uint32_t> keys(capacity, 0x12345678u);
  std::vector<std::uint32_t> values(capacity, 0x87654321u);
  std::vector<std::uint32_t> outputKeyGuards(capacity, 0xCAFEBABEu);
  std::vector<std::uint32_t> outputValueGuards(capacity, 0xDEADC0DEu);
  std::vector<std::pair<std::uint32_t, std::uint32_t>> expected;
  expected.reserve(elementCount);
  for (std::size_t index = 0u; index < elementCount; ++index) {
    std::uint32_t key = 0u;
    if (pattern == 1u) {
      key = 0xA5A5A5A5u;
    } else if (pattern == 2u) {
      key = static_cast<std::uint32_t>(elementCount - index);
    } else if (index % 29u == 0u) {
      key = 0u;
    } else if (index % 31u == 0u) {
      key = std::numeric_limits<std::uint32_t>::max();
    } else {
      const auto mixed = static_cast<std::uint32_t>(index * 2'654'435'761ULL);
      key = (mixed ^ (mixed >> 13u)) & 0x0000FFFFu;
    }
    const std::uint32_t value = static_cast<std::uint32_t>(index);
    keys[index] = key;
    values[index] = value;
    expected.emplace_back(key, value);
  }
  std::stable_sort(
      expected.begin(), expected.end(),
      [](const auto &lhs, const auto &rhs) { return lhs.first < rhs.first; });

  if (!writeBuffer(inputKeys, keys, elementCount, error) ||
      !writeBuffer(inputValues, values, elementCount, error) ||
      !writeBuffer(sortedKeys, outputKeyGuards, capacity, error) ||
      !writeBuffer(sortedValues, outputValueGuards, capacity, error)) {
    return false;
  }
  if (!primitives.sortByKey(inputKeys, elementCount, inputValues, elementCount,
                            sortedKeys, capacity, sortedValues, capacity,
                            error)) {
    return false;
  }

  std::vector<std::uint32_t> outKeys;
  std::vector<std::uint32_t> outValues;
  std::vector<std::uint32_t> preservedKeys;
  std::vector<std::uint32_t> preservedValues;
  if (!readBuffer(sortedKeys, outKeys, capacity, error) ||
      !readBuffer(sortedValues, outValues, capacity, error) ||
      !readBuffer(inputKeys, preservedKeys, elementCount, error) ||
      !readBuffer(inputValues, preservedValues, elementCount, error)) {
    return false;
  }
  if (!std::equal(preservedKeys.begin(), preservedKeys.end(), keys.begin()) ||
      !std::equal(preservedValues.begin(), preservedValues.end(),
                  values.begin())) {
    error = "out-of-place radix sort modified its input";
    return false;
  }
  for (std::size_t index = elementCount; index < capacity; ++index) {
    if (outKeys[index] != outputKeyGuards[index] ||
        outValues[index] != outputValueGuards[index]) {
      error = "radix sort wrote beyond the requested element count";
      return false;
    }
  }

  std::vector<std::pair<std::uint32_t, std::uint32_t>> actual;
  actual.reserve(elementCount);
  for (std::size_t index = 0u; index < elementCount; ++index) {
    actual.emplace_back(outKeys[index], outValues[index]);
  }
  return sameSortedPairs(actual, expected, error);
}

[[nodiscard]] bool runInPlaceCase(const std::size_t elementCount,
                                  RadixSortPrimitives &primitives,
                                  std::string &error) {
  const auto capacity = std::max<std::size_t>(1u, elementCount);
  runtime::HostVisibleBuffer keys{};
  runtime::HostVisibleBuffer values{};
  if (!primitives.createKeyBuffer(capacity, keys, error) ||
      !primitives.createValueBuffer(capacity, values, error)) {
    return false;
  }

  std::vector<std::pair<std::uint32_t, std::uint32_t>> expected;
  std::vector<std::uint32_t> rawKeys(capacity);
  std::vector<std::uint32_t> rawValues(capacity);
  expected.reserve(elementCount);
  for (std::size_t index = 0u; index < elementCount; ++index) {
    const std::uint32_t key = static_cast<std::uint32_t>((index * 37u) % 19u);
    const std::uint32_t value = static_cast<std::uint32_t>(index);
    rawKeys[index] = key;
    rawValues[index] = value;
    expected.emplace_back(key, value);
  }
  std::stable_sort(
      expected.begin(), expected.end(),
      [](const auto &lhs, const auto &rhs) { return lhs.first < rhs.first; });

  if (!writeBuffer(keys, rawKeys, elementCount, error) ||
      !writeBuffer(values, rawValues, elementCount, error)) {
    return false;
  }
  if (!primitives.sortByKey(keys, elementCount, values, elementCount, keys,
                            capacity, values, capacity, error,
                            {.allowInPlaceSort = true})) {
    return false;
  }

  std::vector<std::uint32_t> outKeys;
  std::vector<std::uint32_t> outValues;
  if (!readBuffer(keys, outKeys, elementCount, error) ||
      !readBuffer(values, outValues, elementCount, error)) {
    return false;
  }

  std::vector<std::pair<std::uint32_t, std::uint32_t>> actual;
  actual.reserve(elementCount);
  for (std::size_t index = 0u; index < elementCount; ++index) {
    actual.emplace_back(outKeys[index], outValues[index]);
  }
  return sameSortedPairs(actual, expected, error);
}

[[nodiscard]] bool runAliasValidation(RadixSortPrimitives &primitives,
                                      std::string &error) {
  runtime::HostVisibleBuffer keys{};
  runtime::HostVisibleBuffer values{};
  if (!primitives.createKeyBuffer(8u, keys, error) ||
      !primitives.createValueBuffer(8u, values, error)) {
    return false;
  }
  runtime::HostVisibleBuffer scratch{};
  if (!primitives.createKeyBuffer(8u, scratch, error)) {
    return false;
  }
  (void)scratch;
  if (primitives.sortByKey(keys, 8u, values, 8u, keys, 8u, values, 8u, error)) {
    error = "alias-protection test did not fail";
    return false;
  }
  if (error.empty()) {
    error = "alias-protection test failed without an error message";
    return false;
  }
  error.clear();

  runtime::HostVisibleBuffer separateValues{};
  if (!primitives.createValueBuffer(8u, separateValues, error)) {
    return false;
  }
  if (primitives.sortByKey(keys, 8u, values, 8u, keys, 8u, separateValues, 8u,
                           error, {.allowInPlaceSort = true})) {
    error = "partial in-place key/value alias did not fail";
    return false;
  }
  error.clear();

  runtime::HostVisibleBuffer shortKeys{};
  runtime::HostVisibleBuffer shortValues{};
  if (!primitives.createKeyBuffer(7u, shortKeys, error) ||
      !primitives.createValueBuffer(7u, shortValues, error)) {
    return false;
  }
  if (primitives.sortByKey(keys, 8u, values, 8u, shortKeys, 7u, shortValues, 7u,
                           error)) {
    error = "undersized radix output did not fail";
    return false;
  }
  error.clear();

  runtime::HostVisibleBuffer oversized{};
  const auto maxStorageRange =
      primitives.device().selection().properties.limits.maxStorageBufferRange;
  const auto oversizedElements =
      static_cast<std::size_t>(maxStorageRange / sizeof(std::uint32_t)) + 1u;
  if (primitives.createKeyBuffer(oversizedElements, oversized, error)) {
    error = "oversized radix storage buffer did not fail";
    return false;
  }
  error.clear();
  return true;
}

[[nodiscard]] bool runCrossDeviceValidation(RadixSortPrimitives &primitives,
                                            std::string &error) {
  RadixSortPrimitives second{};
  if (!second.initialize(VIENNAPS_VULKAN_RADIX_SORT_SPV_PATH, error)) {
    return false;
  }
  runtime::HostVisibleBuffer keys{};
  runtime::HostVisibleBuffer values{};
  runtime::HostVisibleBuffer foreignKeys{};
  runtime::HostVisibleBuffer foreignValues{};
  if (!primitives.createKeyBuffer(8u, keys, error) ||
      !primitives.createValueBuffer(8u, values, error) ||
      !second.createKeyBuffer(8u, foreignKeys, error) ||
      !second.createValueBuffer(8u, foreignValues, error)) {
    return false;
  }
  if (primitives.sortByKey(keys, 8u, values, 8u, foreignKeys, 8u, foreignValues,
                           8u, error)) {
    error = "cross-device radix buffers did not fail";
    return false;
  }
  if (error.empty()) {
    error = "cross-device radix rejection did not report an error";
    return false;
  }
  error.clear();
  return true;
}

} // namespace

int main() {
  std::string error;
  RadixSortPrimitives primitives{};
  if (!primitives.initialize(VIENNAPS_VULKAN_RADIX_SORT_SPV_PATH, error) ||
      error.size() != 0u) {
    std::cerr << error << '\n';
    return 1;
  }

  for (const auto length : kLengths) {
    if (!runCase(length, primitives, error)) {
      std::cerr << error << '\n';
      return 1;
    }
  }

  if (!runCase(257u, primitives, error, 1u) ||
      !runCase(65'535u, primitives, error, 2u)) {
    std::cerr << error << '\n';
    return 1;
  }

  if (!runInPlaceCase(257u, primitives, error)) {
    std::cerr << error << '\n';
    return 1;
  }

  if (!runAliasValidation(primitives, error)) {
    std::cerr << error << '\n';
    return 1;
  }

  if (!runCrossDeviceValidation(primitives, error)) {
    std::cerr << error << '\n';
    return 1;
  }

  std::cout << "[RadixSortPrimitivesSmoke] PASS\n";
  return 0;
}
