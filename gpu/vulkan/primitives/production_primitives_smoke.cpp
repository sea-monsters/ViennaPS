// Minimal production-oriented Vulkan primitive smoke test.
//
// Exercises:
// - fill
// - copy
// - affineTransform
// with element counts: 0, 1, 16, 257, 65535.
// Includes invalid length and in-place validation checks for copy/affine.
//
// Exit code:
// 0 -> all tested cases pass
// 1 -> one or more failures

#include "vulkan_primitives.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

#ifndef VIENNAPS_VULKAN_PRIMITIVES_ELEMENTWISE_SPV_PATH
#error "VIENNAPS_VULKAN_PRIMITIVES_ELEMENTWISE_SPV_PATH must be defined."
#endif

using namespace viennaps::vulkan::primitives;
namespace {

constexpr std::array<std::size_t, 5> kTestLengths = {0u, 1u, 16u, 257u,
                                                     65'535u};
constexpr float kFillValue = 3.75f;
constexpr float kScale = 2.0f;
constexpr float kOffset = -1.0f;
constexpr float kSentinel = 12.345678f;
constexpr float kOutputSentinel = kSentinel + 1000.0f;

[[nodiscard]] bool
readBuffer(viennaps::vulkan::runtime::HostVisibleBuffer &buffer,
           const std::size_t elementCount, std::vector<float> &out,
           std::string &error) {
  if (out.size() < elementCount) {
    out.resize(elementCount);
  }
  if (!buffer.read(out.data(),
                   static_cast<VkDeviceSize>(elementCount * sizeof(float)), 0u,
                   error)) {
    return false;
  }
  return true;
}

[[nodiscard]] bool
writeBuffer(viennaps::vulkan::runtime::HostVisibleBuffer &buffer,
            const std::vector<float> &values, std::size_t elementCount,
            std::string &error) {
  if (elementCount > values.size()) {
    error = "writeBuffer value span is too small.";
    return false;
  }
  if (!buffer.write(values.data(),
                    static_cast<VkDeviceSize>(elementCount * sizeof(float)), 0u,
                    error)) {
    return false;
  }
  return true;
}

[[nodiscard]] bool
writeSentinel(viennaps::vulkan::runtime::HostVisibleBuffer &buffer,
              const std::size_t elementCount, const float sentinel,
              std::string &error) {
  const std::size_t maxElements =
      static_cast<std::size_t>(buffer.size() / sizeof(float));
  if (maxElements < elementCount) {
    error = "writeSentinel: output buffer too small.";
    return false;
  }
  std::vector<float> sentinels(elementCount, sentinel);
  return writeBuffer(buffer, sentinels, elementCount, error);
}

[[nodiscard]] bool checkMatch(const std::string_view label,
                              const std::vector<float> &actual,
                              const std::vector<float> &expected,
                              std::string &error) {
  for (std::size_t i = 0; i < expected.size(); ++i) {
    if (!viennaps::vulkan::runtime::exactlyEqualFloat(actual[i], expected[i])) {
      error = std::string(label) + ": mismatch at index " + std::to_string(i);
      return false;
    }
  }
  return true;
}

bool runFillSmoke(ElementwisePrimitives &primitives,
                  const std::size_t maxElements, std::string &error) {
  viennaps::vulkan::runtime::HostVisibleBuffer input{};
  viennaps::vulkan::runtime::HostVisibleBuffer output{};
  if (!primitives.createFloatBuffer(maxElements, input, error) ||
      !primitives.createFloatBuffer(maxElements, output, error)) {
    return false;
  }

  for (const auto length : kTestLengths) {
    std::vector<float> outputExpected(maxElements, kOutputSentinel);
    if (length > 0u) {
      std::fill_n(outputExpected.begin(), length, kFillValue);
    }

    std::vector<float> inputFill(maxElements, kSentinel);
    if (!writeBuffer(input, inputFill, maxElements, error)) {
      return false;
    }
    if (!writeSentinel(output, maxElements, kOutputSentinel, error)) {
      return false;
    }

    if (!primitives.fill(output, length, kFillValue, error)) {
      return false;
    }
    std::vector<float> outputHost(maxElements);
    if (!readBuffer(output, maxElements, outputHost, error)) {
      return false;
    }
    if (!checkMatch("fill", outputHost, outputExpected, error)) {
      std::cerr << error << '\n';
      return false;
    }
  }
  return true;
}

bool runCopySmoke(ElementwisePrimitives &primitives,
                  const std::size_t maxElements, std::string &error) {
  viennaps::vulkan::runtime::HostVisibleBuffer input{};
  viennaps::vulkan::runtime::HostVisibleBuffer output{};
  if (!primitives.createFloatBuffer(maxElements, input, error) ||
      !primitives.createFloatBuffer(maxElements, output, error)) {
    return false;
  }

  for (const auto length : kTestLengths) {
    std::vector<float> inputHost(maxElements, kSentinel);
    std::vector<float> expected(maxElements, kSentinel);
    for (std::size_t i = 0u; i < length; ++i) {
      inputHost[i] = static_cast<float>(i) * 1.25f;
      expected[i] = inputHost[i];
    }
    if (!writeBuffer(input, inputHost, maxElements, error) ||
        !writeSentinel(output, maxElements, kSentinel, error) ||
        !primitives.copy(output, length, input, length, error)) {
      return false;
    }
    std::vector<float> outputHost(maxElements);
    if (!readBuffer(output, maxElements, outputHost, error)) {
      return false;
    }
    if (!checkMatch("copy", outputHost, expected, error)) {
      std::cerr << error << '\n';
      return false;
    }
  }

  if (primitives.copy(output, 1u, output, 1u, error)) {
    std::cerr << "copy in-place should fail by default\n";
    return false;
  }
  error.clear();
  const std::vector<float> inPlaceCopyInput{4.0F};
  if (!writeBuffer(output, inPlaceCopyInput, 1u, error)) {
    return false;
  }
  if (!primitives.copy(output, 1u, output, 1u, error,
                       ElementwisePrimitivesOptions{true, false})) {
    return false;
  }
  std::vector<float> inPlaceCopyOutput(1u);
  if (!readBuffer(output, 1u, inPlaceCopyOutput, error) ||
      !checkMatch("copy in-place", inPlaceCopyOutput, inPlaceCopyInput,
                  error)) {
    return false;
  }

  viennaps::vulkan::runtime::HostVisibleBuffer smallInput{};
  viennaps::vulkan::runtime::HostVisibleBuffer smallOutput{};
  if (!primitives.createFloatBuffer(1u, smallInput, error) ||
      !primitives.createFloatBuffer(1u, smallOutput, error)) {
    return false;
  }
  if (primitives.copy(smallOutput, 2u, smallInput, 1u, error)) {
    std::cerr << "copy length mismatch should fail\n";
    return false;
  }
  return true;
}

bool runAffineSmoke(ElementwisePrimitives &primitives,
                    const std::size_t maxElements, std::string &error) {
  viennaps::vulkan::runtime::HostVisibleBuffer input{};
  viennaps::vulkan::runtime::HostVisibleBuffer output{};
  if (!primitives.createFloatBuffer(maxElements, input, error) ||
      !primitives.createFloatBuffer(maxElements, output, error)) {
    return false;
  }

  for (const auto length : kTestLengths) {
    std::vector<float> inputHost(maxElements, kSentinel);
    std::vector<float> expected(maxElements, kSentinel);
    for (std::size_t i = 0u; i < length; ++i) {
      inputHost[i] = -2.0f + static_cast<float>(i) * 0.5f;
      expected[i] = inputHost[i] * kScale + kOffset;
    }
    if (!writeBuffer(input, inputHost, maxElements, error) ||
        !writeSentinel(output, maxElements, kSentinel, error) ||
        !primitives.affineTransform(output, length, input, length, kScale,
                                    kOffset, error)) {
      return false;
    }
    std::vector<float> outputHost(maxElements);
    if (!readBuffer(output, maxElements, outputHost, error)) {
      return false;
    }
    if (!checkMatch("affine", outputHost, expected, error)) {
      std::cerr << error << '\n';
      return false;
    }
  }

  if (primitives.affineTransform(output, 1u, output, 1u, kScale, kOffset,
                                 error)) {
    std::cerr << "affine in-place should fail by default\n";
    return false;
  }
  error.clear();
  const std::vector<float> inPlaceAffineInput{4.0F};
  const std::vector<float> inPlaceAffineExpected{7.0F};
  if (!writeBuffer(output, inPlaceAffineInput, 1u, error)) {
    return false;
  }
  if (!primitives.affineTransform(output, 1u, output, 1u, kScale, kOffset,
                                  error,
                                  ElementwisePrimitivesOptions{false, true})) {
    return false;
  }
  std::vector<float> inPlaceAffineOutput(1u);
  if (!readBuffer(output, 1u, inPlaceAffineOutput, error) ||
      !checkMatch("affine in-place", inPlaceAffineOutput, inPlaceAffineExpected,
                  error)) {
    return false;
  }

  viennaps::vulkan::runtime::HostVisibleBuffer smallInput{};
  viennaps::vulkan::runtime::HostVisibleBuffer smallOutput{};
  if (!primitives.createFloatBuffer(1u, smallInput, error) ||
      !primitives.createFloatBuffer(1u, smallOutput, error)) {
    return false;
  }
  if (primitives.affineTransform(smallOutput, 2u, smallInput, 1u, kScale,
                                 kOffset, error)) {
    std::cerr << "affine length mismatch should fail\n";
    return false;
  }
  return true;
}

} // namespace

int main() {
  constexpr std::size_t kMaxElements = 65'535u;
  std::string error;

  ElementwisePrimitives primitives{};
  if (!primitives.initialize(VIENNAPS_VULKAN_PRIMITIVES_ELEMENTWISE_SPV_PATH,
                             error)) {
    std::cerr << error << '\n';
    return 1;
  }

  if (!runFillSmoke(primitives, kMaxElements, error) ||
      !runCopySmoke(primitives, kMaxElements, error) ||
      !runAffineSmoke(primitives, kMaxElements, error)) {
    std::cerr << error << '\n';
    return 1;
  }

  std::cout << "[ElementwisePrimitivesSmoke] PASS\n";
  return 0;
}
