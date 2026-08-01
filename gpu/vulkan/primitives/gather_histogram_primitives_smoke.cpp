// Minimal Vulkan gather/scatter/histogram primitive smoke test.
//
// Exit code:
//   0 -> all tested cases pass
//   1 -> one or more failures

#include "gather_histogram_primitives.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

#include <vulkan/vulkan.h>

#ifndef VIENNAPS_VULKAN_GATHER_HISTOGRAM_SPV_PATH
#error "VIENNAPS_VULKAN_GATHER_HISTOGRAM_SPV_PATH must be defined."
#endif

namespace {

using namespace viennaps::vulkan::primitives;
namespace runtime = viennaps::vulkan::runtime;

constexpr std::array<std::size_t, 5u> kLengths = {0u, 1u, 16u, 257u, 65'535u};

template <typename T>
[[nodiscard]] bool writeHostBuffer(
    runtime::HostVisibleBuffer &buffer, const std::vector<T> &values,
    const std::string_view label, std::string &error) {
  (void)label;
  if (values.empty()) {
    return true;
  }
  return buffer.write(values.data(),
                     static_cast<VkDeviceSize>(values.size() * sizeof(T)), 0u,
                     error);
}

template <typename T>
[[nodiscard]] bool readHostBuffer(runtime::HostVisibleBuffer &buffer,
                                 std::vector<T> &values,
                                 const std::size_t elementCount,
                                 const std::string_view label,
                                 std::string &error) {
  (void)label;
  values.resize(elementCount);
  if (elementCount == 0u) {
    return true;
  }
  return buffer.read(values.data(),
                    static_cast<VkDeviceSize>(elementCount * sizeof(T)), 0u,
                    error);
}

[[nodiscard]] bool buffersMatch(const std::vector<float> &lhs,
                               const std::vector<float> &rhs,
                               std::string &error) {
  if (lhs.size() != rhs.size()) {
    error = "buffer size mismatch for float comparison";
    return false;
  }
  for (std::size_t i = 0u; i < lhs.size(); ++i) {
    if (lhs[i] != rhs[i]) {
      error = "float buffer mismatch at index " + std::to_string(i) + " (lhs=" +
              std::to_string(lhs[i]) + ", rhs=" + std::to_string(rhs[i]) + ")";
      return false;
    }
  }
  return true;
}

[[nodiscard]] bool buffersMatch(const std::vector<std::uint32_t> &lhs,
                               const std::vector<std::uint32_t> &rhs,
                               std::string &error) {
  if (lhs.size() != rhs.size()) {
    error = "buffer size mismatch for uint32 comparison";
    return false;
  }
  for (std::size_t i = 0u; i < lhs.size(); ++i) {
    if (lhs[i] != rhs[i]) {
      error = "uint32 buffer mismatch at index " + std::to_string(i) +
              " (lhs=" + std::to_string(lhs[i]) +
              ", rhs=" + std::to_string(rhs[i]) + ")";
      return false;
    }
  }
  return true;
}

[[nodiscard]] bool runGatherSmoke(const std::size_t maxLength,
                                 GatherHistogramPrimitives &primitives,
                                 std::string &error) {
  if (!primitives.isInitialized()) {
    error = "primitives not initialized";
    return false;
  }

  runtime::HostVisibleBuffer input{};
  runtime::HostVisibleBuffer indices{};
  runtime::HostVisibleBuffer output{};
  if (!primitives.createFloatBuffer(maxLength, input, error) ||
      !primitives.createUInt32Buffer(maxLength, indices, error) ||
      !primitives.createFloatBuffer(maxLength + 1u, output, error)) {
    return false;
  }

  for (const auto inputLength : kLengths) {
    const std::size_t outputLength = inputLength;
    std::vector<float> inputValues(maxLength);
    std::vector<std::uint32_t> indexValues(maxLength);
    constexpr float kGuard = -9'999.0F;
    std::vector<float> expected(maxLength + 1u, kGuard);
    std::vector<float> got;
    std::vector<float> outputSentinel(maxLength + 1u, kGuard);

    for (std::size_t i = 0u; i < maxLength; ++i) {
      inputValues[i] = static_cast<float>(i) * 0.25f;
    }
    for (std::size_t i = 0u; i < outputLength; ++i) {
      indexValues[i] = static_cast<std::uint32_t>(i / 2u);
      expected[i] = inputValues[indexValues[i]];
    }
    if (!writeHostBuffer(input, inputValues, "gather input", error) ||
        !writeHostBuffer(indices, indexValues, "gather indices", error) ||
        !writeHostBuffer(output, outputSentinel, "gather output", error) ||
        !primitives.gather(output, outputLength, input, inputLength, indices,
                           outputLength, error)) {
      return false;
    }
    if (!readHostBuffer(output, got, maxLength + 1u, "gather output", error) ||
        !buffersMatch(got, expected, error)) {
      return false;
    }
  }
  return true;
}

[[nodiscard]] bool runScatterSmoke(const std::size_t maxLength,
                                  GatherHistogramPrimitives &primitives,
                                  std::string &error) {
  runtime::HostVisibleBuffer input{};
  runtime::HostVisibleBuffer indices{};
  runtime::HostVisibleBuffer output{};
  if (!primitives.createFloatBuffer(maxLength, input, error) ||
      !primitives.createUInt32Buffer(maxLength, indices, error) ||
      !primitives.createFloatBuffer(maxLength + 1u, output, error)) {
    return false;
  }

  for (const auto inputLength : kLengths) {
    const std::size_t outputLength = maxLength;
    std::vector<float> inputValues(maxLength);
    std::vector<std::uint32_t> indexValues(maxLength);
    std::vector<float> expected(outputLength + 1u, -1.0F);
    std::vector<float> got;

    for (std::size_t i = 0u; i < maxLength; ++i) {
      inputValues[i] = static_cast<float>(1'000u + i);
    }
    for (std::size_t i = 0u; i < inputLength; ++i) {
      indexValues[i] = static_cast<std::uint32_t>((i + 1u) % outputLength);
      expected[indexValues[i]] = inputValues[i];
    }
    std::vector<float> outputSentinel(outputLength + 1u, -1.0F);
    if (!writeHostBuffer(input, inputValues, "scatter input", error) ||
        !writeHostBuffer(indices, indexValues, "scatter indices", error) ||
        !writeHostBuffer(output, outputSentinel, "scatter output", error) ||
        !primitives.scatter(output, outputLength, input, inputLength, indices,
                           inputLength, error, {.enableDeterministicDuplicatePolicy =
                                                   false})) {
      return false;
    }
    if (!readHostBuffer(output, got, outputLength + 1u, "scatter output", error) ||
        !buffersMatch(got, expected, error)) {
      return false;
    }
  }

  // Deterministic duplicate policy should keep the last index entry.
  const std::size_t dupInputLength = 6u;
  std::vector<float> inputValues = {10.0F, 20.0F, 30.0F, 40.0F, 50.0F, 60.0F};
  std::vector<std::uint32_t> duplicateIndices = {2u, 1u, 2u, 4u, 1u, 2u};
  const std::size_t outputLength = 8u;
  std::vector<float> outputExpected = {0.0F, 50.0F, 60.0F, 0.0F, 40.0F, 0.0F,
                                      0.0F, 0.0F};
  std::vector<float> duplicateOutput(outputLength, 0.0F);
  std::vector<float> outputValues;

  if (!writeHostBuffer(input, inputValues, "scatter duplicate input", error) ||
      !writeHostBuffer(indices, duplicateIndices,
                       "scatter duplicate indices", error) ||
      !writeHostBuffer(output, duplicateOutput, "scatter duplicate output",
                       error)) {
    return false;
  }
  if (!primitives.scatter(output, outputLength, input, dupInputLength, indices,
                         dupInputLength, error,
                         {.enableDeterministicDuplicatePolicy = true}) ||
      !readHostBuffer(output, outputValues, outputLength,
                      "scatter duplicate output", error) ||
      !buffersMatch(outputValues, outputExpected, error)) {
    return false;
  }

  // Reject duplicates when policy is disabled.
  if (!primitives.scatter(output, outputLength, input, dupInputLength, indices,
                         dupInputLength, error, {})) {
    error.clear();
  } else {
    error = "duplicate scatter succeeded unexpectedly";
    return false;
  }
  return true;
}

[[nodiscard]] bool runHistogramSmoke(const std::size_t maxLength,
                                    GatherHistogramPrimitives &primitives,
                                    std::string &error) {
  constexpr std::size_t kBinCount = 16u;
  runtime::HostVisibleBuffer input{};
  runtime::HostVisibleBuffer histogram{};
  if (!primitives.createUInt32Buffer(maxLength, input, error) ||
      !primitives.createUInt32Buffer(kBinCount + 1u, histogram, error)) {
    return false;
  }

  for (const auto inputLength : kLengths) {
    std::vector<std::uint32_t> inputValues(maxLength);
    constexpr std::uint32_t kGuard = 0xdecafbadU;
    std::vector<std::uint32_t> expected(kBinCount + 1u, 0u);
    expected.back() = kGuard;
    std::vector<std::uint32_t> got;

    for (std::size_t i = 0u; i < inputLength; ++i) {
      inputValues[i] = static_cast<std::uint32_t>(i % kBinCount);
      ++expected[inputValues[i]];
    }
    std::vector<std::uint32_t> histogramOutput(kBinCount + 1u, kGuard);
    if (!writeHostBuffer(input, inputValues, "histogram input", error) ||
        !writeHostBuffer(histogram, histogramOutput, "histogram output", error) ||
        !primitives.histogram(histogram, kBinCount, input, inputLength, error) ||
        !readHostBuffer(histogram, got, kBinCount + 1u,
                        "histogram output readback", error) ||
        !buffersMatch(got, expected, error)) {
      return false;
    }
  }
  return true;
}

[[nodiscard]] bool runZeroLengthSmoke(GatherHistogramPrimitives &primitives,
                                     std::string &error) {
  runtime::HostVisibleBuffer input{};
  runtime::HostVisibleBuffer output{};
  runtime::HostVisibleBuffer indices{};
  runtime::HostVisibleBuffer histogram{};
  if (!primitives.createFloatBuffer(1u, input, error) ||
      !primitives.createFloatBuffer(1u, output, error) ||
      !primitives.createUInt32Buffer(1u, indices, error) ||
      !primitives.createUInt32Buffer(1u, histogram, error)) {
    return false;
  }
  if (!primitives.gather(output, 0u, input, 1u, indices, 0u, error) ||
      !primitives.scatter(output, 0u, input, 0u, indices, 0u, error) ||
      !primitives.histogram(histogram, 1u, indices, 0u, error)) {
    return false;
  }
  return true;
}

[[nodiscard]] bool runValidationSmoke(GatherHistogramPrimitives &primitives,
                                     std::string &error) {
  runtime::HostVisibleBuffer input{};
  runtime::HostVisibleBuffer output{};
  runtime::HostVisibleBuffer indices{};
  runtime::HostVisibleBuffer histogram{};
  if (!primitives.createFloatBuffer(4u, input, error) ||
      !primitives.createFloatBuffer(4u, output, error) ||
      !primitives.createUInt32Buffer(4u, indices, error) ||
      !primitives.createUInt32Buffer(4u, histogram, error)) {
    return false;
  }

  const std::vector<float> inputValues = {1.0F, 2.0F, 3.0F, 4.0F};
  const std::vector<std::uint32_t> invalidIndices = {0u, 4u};
  if (!writeHostBuffer(input, inputValues, "validation input", error) ||
      !writeHostBuffer(indices, invalidIndices, "validation indices", error)) {
    return false;
  }
  if (primitives.gather(output, 2u, input, 4u, indices, 2u, error)) {
    error = "out-of-range gather index succeeded unexpectedly";
    return false;
  }
  error.clear();
  if (primitives.scatter(output, 4u, input, 2u, indices, 2u, error)) {
    error = "out-of-range scatter index succeeded unexpectedly";
    return false;
  }
  error.clear();

  const std::vector<std::uint32_t> invalidBins = {0u, 1u, 4u};
  if (!writeHostBuffer(indices, invalidBins, "validation bins", error)) {
    return false;
  }
  if (primitives.histogram(histogram, 4u, indices, invalidBins.size(), error)) {
    error = "out-of-range histogram value succeeded unexpectedly";
    return false;
  }
  error.clear();
  if (primitives.histogram(histogram, 0u, indices, 0u, error)) {
    error = "zero-bin histogram succeeded unexpectedly";
    return false;
  }
  error.clear();
  return true;
}

} // namespace

int main() {
  constexpr std::size_t kMaxElements = 65'535u;
  std::string error;

  GatherHistogramPrimitives primitives{};
  if (!primitives.initialize(VIENNAPS_VULKAN_GATHER_HISTOGRAM_SPV_PATH, error)) {
    std::cerr << error << '\n';
    return 1;
  }

  if (!runGatherSmoke(kMaxElements, primitives, error) ||
      !runScatterSmoke(kMaxElements, primitives, error) ||
      !runHistogramSmoke(kMaxElements, primitives, error) ||
      !runZeroLengthSmoke(primitives, error) ||
      !runValidationSmoke(primitives, error)) {
    std::cerr << error << '\n';
    return 1;
  }
  std::cout << "[GatherHistogramPrimitivesSmoke] PASS\n";
  return 0;
}
