// Minimal production-oriented Vulkan RNG primitive smoke test.
//
// Exercises:
// - deterministic uint32 generation
// - deterministic float [0,1) conversion
// - zero-length no-op
// - seed variation and repeated invocation stability
// - stable counter-offset chunk mapping
// - overflow and device-limit dispatch rejection
//
// Exit code:
// 0 -> all tested cases pass
// 1 -> one or more failures

#include "rng_primitives.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <limits>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include <vulkan/vulkan.h>

#ifndef VIENNAPS_VULKAN_PRIMITIVES_RNG_SPV_PATH
#error "VIENNAPS_VULKAN_PRIMITIVES_RNG_SPV_PATH must be defined."
#endif

using namespace viennaps::vulkan::primitives;
namespace {

constexpr std::array<std::size_t, 6u> kTestLengths = {
    0u, 1u, 16u, 257u, 65'535u, 1'000'003u};
constexpr std::size_t kDispatchGuardMaxLength = 16'777'217u;
constexpr std::uint32_t kMixConstant = 0x9e3779b9u;
constexpr std::uint32_t kSeedA = 0x1234ABCDu;
constexpr std::uint32_t kSeedB = 0x55667788u;
constexpr std::uint32_t kSeedC = 0xFFFFFFFFu;
constexpr std::uint32_t kZeroSeed = 0u;
constexpr std::uint32_t kUintGuard = 0x5A5A5A5Au;
constexpr float kFloatGuard = -1.0F;

[[nodiscard]] std::uint32_t cpuXorShift(std::uint32_t value) {
  value ^= value << 13;
  value ^= value >> 17;
  value ^= value << 5;
  return value;
}

[[nodiscard]] std::uint32_t
cpuOracleU32(const std::size_t index, const std::uint32_t seed,
             const std::uint32_t counterOffset = 0u) {
  const auto idx = static_cast<std::uint32_t>(index) + counterOffset;
  return cpuXorShift(seed ^ (idx * kMixConstant));
}

[[nodiscard]] float cpuOracleFloat01(const std::size_t index,
                                     const std::uint32_t seed) {
  return static_cast<float>(cpuOracleU32(index, seed) >> 8) / 16'777'216.0F;
}

[[nodiscard]] bool
readBuffer(viennaps::vulkan::runtime::HostVisibleBuffer &buffer,
           const std::size_t elementCount, void *out,
           const std::size_t elementSize, std::string &error) {
  if (out == nullptr) {
    error = "readBuffer: destination is null.";
    return false;
  }
  if (elementSize == 0u) {
    error = "readBuffer: element size is zero.";
    return false;
  }
  return buffer.read(out, static_cast<VkDeviceSize>(elementCount * elementSize),
                     0u, error);
}

[[nodiscard]] bool checkExact(const std::string_view label,
                              const std::vector<std::uint32_t> &actual,
                              const std::vector<std::uint32_t> &expected,
                              std::string &error) {
  if (actual.size() != expected.size()) {
    error = std::string(label) + ": size mismatch";
    return false;
  }
  for (std::size_t i = 0u; i < expected.size(); ++i) {
    if (actual[i] != expected[i]) {
      error = std::string(label) + ": uint32 mismatch at index " +
              std::to_string(i);
      return false;
    }
  }
  return true;
}

[[nodiscard]] bool checkFloat01IntervalAndExact(
    const std::string_view label, const std::vector<float> &actual,
    const std::vector<float> &expected, std::string &error) {
  if (actual.size() != expected.size()) {
    error = std::string(label) + ": size mismatch";
    return false;
  }
  for (std::size_t i = 0u; i < expected.size(); ++i) {
    const auto value = actual[i];
    if (value < 0.0F || value >= 1.0F) {
      std::ostringstream out;
      out << std::string(label) << ": value out of [0,1) at index " << i
          << " -> " << std::fixed << std::setprecision(8) << value;
      error = out.str();
      return false;
    }
    if (!viennaps::vulkan::runtime::exactlyEqualFloat(value, expected[i])) {
      error =
          std::string(label) + ": float mismatch at index " + std::to_string(i);
      return false;
    }
  }
  return true;
}

template <class ValueType>
[[nodiscard]] bool checkTail(const std::vector<ValueType> &values,
                             const std::size_t activeCount,
                             const ValueType guard, std::string &error) {
  for (std::size_t i = activeCount; i < values.size(); ++i) {
    if (values[i] != guard) {
      error = "RNG guard changed at index " + std::to_string(i);
      return false;
    }
  }
  return true;
}

[[nodiscard]] bool runUintSmoke(RNGPrimitives &primitives,
                                const std::size_t maxElements,
                                std::string &error) {
  const auto bufferElements = maxElements + 1u;
  viennaps::vulkan::runtime::HostVisibleBuffer output{};
  if (!primitives.createUint32Buffer(bufferElements, output, error)) {
    return false;
  }
  std::vector<std::uint32_t> host(bufferElements, kUintGuard);
  if (!output.write(
          host.data(),
          static_cast<VkDeviceSize>(bufferElements * sizeof(std::uint32_t)), 0u,
          error)) {
    return false;
  }

  for (const auto length : kTestLengths) {
    std::vector<std::uint32_t> expected(length);
    for (std::size_t i = 0u; i < length; ++i) {
      expected[i] = cpuOracleU32(i, kSeedA);
    }

    if (!primitives.generate(length, kSeedA, output, error)) {
      return false;
    }
    if (!readBuffer(output, bufferElements, host.data(), sizeof(std::uint32_t),
                    error)) {
      return false;
    }
    std::vector<std::uint32_t> got(host.begin(), host.begin() + length);
    if (!checkExact("rng U32 first seed", got, expected, error)) {
      return false;
    }
    if (!checkTail(host, length, kUintGuard, error)) {
      return false;
    }
    if (!primitives.generate(length, kSeedA, output, error)) {
      return false;
    }
    if (!readBuffer(output, bufferElements, host.data(), sizeof(std::uint32_t),
                    error)) {
      return false;
    }
    got.assign(host.begin(), host.begin() + length);
    if (!checkExact("rng U32 repeatability", got, expected, error)) {
      return false;
    }

    if (length > 0u) {
      std::vector<std::uint32_t> second(length);
      if (primitives.generate(length, kSeedB, output, error) &&
          readBuffer(output, bufferElements, host.data(), sizeof(std::uint32_t),
                     error)) {
        second.assign(host.begin(), host.begin() + length);
        bool changed = false;
        for (std::size_t i = 0u; i < length; ++i) {
          if (second[i] != expected[i]) {
            changed = true;
            break;
          }
        }
        if (!changed) {
          std::cerr << "rng U32 seed variation unchanged for length " << length
                    << '\n';
          return false;
        }
      } else {
        return false;
      }
    }
  }
  constexpr std::uint32_t kOffset = 17u;
  constexpr std::size_t kOffsetLength = 257u;
  std::vector<std::uint32_t> offsetExpected(kOffsetLength);
  for (std::size_t i = 0u; i < kOffsetLength; ++i) {
    offsetExpected[i] = cpuOracleU32(i, kSeedA, kOffset);
  }
  if (!primitives.generate(kOffsetLength, kSeedA, output, error, kOffset) ||
      !readBuffer(output, bufferElements, host.data(), sizeof(std::uint32_t),
                  error)) {
    return false;
  }
  const std::vector<std::uint32_t> offsetGot(host.begin(),
                                             host.begin() + kOffsetLength);
  if (!checkExact("rng U32 counter offset", offsetGot, offsetExpected, error)) {
    return false;
  }
  return true;
}

[[nodiscard]] bool runFloatSmoke(RNGPrimitives &primitives,
                                 const std::size_t maxElements,
                                 std::string &error) {
  const auto bufferElements = maxElements + 1u;
  viennaps::vulkan::runtime::HostVisibleBuffer output{};
  if (!primitives.createFloatBuffer(bufferElements, output, error)) {
    return false;
  }
  std::vector<float> host(bufferElements, kFloatGuard);
  if (!output.write(host.data(),
                    static_cast<VkDeviceSize>(bufferElements * sizeof(float)),
                    0u, error)) {
    return false;
  }

  for (const auto length : kTestLengths) {
    std::vector<float> expected(length);
    for (std::size_t i = 0u; i < length; ++i) {
      expected[i] = cpuOracleFloat01(i, kSeedC);
    }

    if (!primitives.generateFloat01(length, kSeedC, output, error)) {
      return false;
    }
    if (!readBuffer(output, bufferElements, host.data(), sizeof(float),
                    error)) {
      return false;
    }
    std::vector<float> got(host.begin(), host.begin() + length);
    if (!checkFloat01IntervalAndExact("rng F32 first seed", got, expected,
                                      error)) {
      return false;
    }
    if (!checkTail(host, length, kFloatGuard, error)) {
      return false;
    }

    if (!primitives.generateFloat01(length, kSeedC, output, error)) {
      return false;
    }
    if (!readBuffer(output, bufferElements, host.data(), sizeof(float),
                    error)) {
      return false;
    }
    std::vector<float> repeat(host.begin(), host.begin() + length);
    if (!checkFloat01IntervalAndExact("rng F32 repeatability", repeat, expected,
                                      error)) {
      return false;
    }
  }
  return true;
}

[[nodiscard]] bool runOverflowAndLimitChecks(RNGPrimitives &primitives,
                                             const std::size_t,
                                             std::string &error) {
  viennaps::vulkan::runtime::HostVisibleBuffer output{};
  if (!primitives.createUint32Buffer(1u, output, error)) {
    return false;
  }
  if (primitives.generate(
          std::numeric_limits<std::size_t>::max() / sizeof(std::uint32_t) + 1u,
          kZeroSeed, output, error)) {
    std::cerr << "expected overflow rejection for uint32 generate\n";
    return false;
  }
  error.clear();

  if (primitives.generate(2u, kSeedA, output, error,
                          std::numeric_limits<std::uint32_t>::max())) {
    std::cerr << "expected counter overflow rejection\n";
    return false;
  }
  error.clear();

  std::string dispatchError{};
  const auto maxWorkgroups = static_cast<std::size_t>(
      primitives.device()
          .selection()
          .properties.limits.maxComputeWorkGroupCount[0]);
  const auto lengthToReject = (maxWorkgroups + 1u) * 256u;
  if (lengthToReject == 0u) {
    std::cerr
        << "dispatch-limit check skipped due to device limit wraparound\n";
    return true;
  }
  if (!primitives.isDispatchLengthSupported(lengthToReject, dispatchError)) {
    return true;
  }

  if (lengthToReject > kDispatchGuardMaxLength) {
    std::cerr << "dispatch-limit check skipped for unsupported length "
              << lengthToReject << '\n';
    return true;
  }

  if (!primitives.createUint32Buffer(lengthToReject, output, error)) {
    std::cerr << "dispatch-limit check skipped due to output allocation: "
              << error << '\n';
    error.clear();
    return true;
  }
  if (primitives.generate(lengthToReject, kSeedA, output, error)) {
    std::cerr << "dispatch-length should have been rejected but succeeded\n";
    return false;
  }
  return true;
}

} // namespace

int main() {
  constexpr std::size_t kMaxElements = 1'000'003u;
  std::string error;

  RNGPrimitives primitives{};
  if (!primitives.initialize(VIENNAPS_VULKAN_PRIMITIVES_RNG_SPV_PATH, error)) {
    std::cerr << error << '\n';
    return 1;
  }

  if (!runUintSmoke(primitives, kMaxElements, error) ||
      !runFloatSmoke(primitives, kMaxElements, error) ||
      !runOverflowAndLimitChecks(primitives, kMaxElements, error)) {
    std::cerr << error << '\n';
    return 1;
  }
  std::cout << "[RNGPrimitivesSmoke] PASS\n";
  return 0;
}
