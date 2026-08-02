// Copyright 2026 ViennaPS
// SPDX-License-Identifier: MIT

#include "coverage_delta_executor.hpp"

#include "coverage_delta_metric.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <exception>
#include <limits>
#include <mutex>
#include <utility>
#include <vector>

namespace viennaps::vulkan::surface {

namespace {

using Model = CoverageDeltaMetricFp32;
using Buffer = runtime::HostVisibleBuffer;
using Work = viennaps::CoverageDeltaWork<float>;

[[nodiscard]] bool isNormalOrZero(const float value) {
  return value == 0.0F || std::isnormal(value);
}

[[nodiscard]] bool validStrictFloat(const float value) {
  return std::isfinite(value) && isNormalOrZero(value);
}

[[nodiscard]] bool fail(std::string &error, const char *message) {
  error = message;
  return false;
}

[[nodiscard]] bool checkedBytes(const std::size_t count,
                                std::size_t &bytes) {
  if (count > std::numeric_limits<std::size_t>::max() / sizeof(float))
    return false;
  bytes = count * sizeof(float);
  return true;
}

[[nodiscard]] bool rawEqual(const float lhs, const float rhs) {
  return std::bit_cast<std::uint32_t>(lhs) ==
         std::bit_cast<std::uint32_t>(rhs);
}

[[nodiscard]] bool cpuOracle(const Work &work, const std::size_t pointCount,
                             std::vector<float> &expected,
                             std::string &error) {
  expected.resize(work.channelCount);
  for (std::size_t channel = 0U; channel < work.channelCount; ++channel) {
    volatile float sum = 0.0F;
    const auto base = channel * pointCount;
    for (std::size_t point = 0U; point < pointCount; ++point) {
      const auto index = base + point;
      const float updated = work.updated[index];
      const float previous = work.previous[index];
      if (!validStrictFloat(updated) || !validStrictFloat(previous))
        return fail(error, "coverage values are outside strict FP32");
      volatile float difference = updated - previous;
      if (!isNormalOrZero(difference))
        return fail(error, "coverage difference leaves strict FP32");
      volatile float square = difference * difference;
      if (!isNormalOrZero(square))
        return fail(error, "coverage square leaves strict FP32");
      sum = sum + square;
      if (!isNormalOrZero(sum))
        return fail(error, "coverage sum leaves strict FP32");
    }
    volatile float mean = sum / static_cast<float>(pointCount);
    if (!isNormalOrZero(mean))
      return fail(error, "coverage mean leaves strict FP32");
    expected[channel] = mean;
  }
  return true;
}

} // namespace

struct VulkanCoverageDeltaExecutor::State {
  Model model{};
  Buffer updated{};
  Buffer previous{};
  Buffer output{};
  mutable std::mutex mutex{};

  ~State() { resetUnlocked(); }

  void resetUnlocked() {
    // HostVisibleBuffer stores the VkDevice needed for destruction. Release
    // buffers before the metric's owned ComputeSession is reset.
    output.reset();
    previous.reset();
    updated.reset();
    model.reset();
  }

  bool initialize(const std::string_view spirvPath, std::string &error) {
    std::lock_guard lock(mutex);
    resetUnlocked();
    return model.initialize(spirvPath, error);
  }

  bool initialize(runtime::ComputeSession &session,
                  const std::string_view spirvPath, std::string &error) {
    std::lock_guard lock(mutex);
    resetUnlocked();
    return model.initialize(session, spirvPath, error);
  }

  void reset() {
    std::lock_guard lock(mutex);
    resetUnlocked();
  }

  [[nodiscard]] bool isInitialized() const {
    std::lock_guard lock(mutex);
    return model.isInitialized();
  }

  [[nodiscard]] bool invoke(Work &work, std::string &error) {
    std::lock_guard lock(mutex);
    error.clear();

    if (!model.isInitialized())
      return fail(error, "Vulkan coverage executor is not initialized");
    if (work.channelCount == 0U)
      return fail(error, "coverage channel count must be nonzero");
    if (work.channelCount > std::numeric_limits<std::uint32_t>::max())
      return fail(error, "coverage channel count exceeds the Vulkan ABI");
    if (work.channelOffsets.size() != work.channelCount + 1U)
      return fail(error, "coverage offsets must contain channel count + 1 entries");
    if (work.output.size() < work.channelCount)
      return fail(error, "coverage output is shorter than channel count");
    if (work.updated.size() != work.previous.size())
      return fail(error, "coverage input spans must have equal lengths");

    const auto offsets = work.channelOffsets;
    if (offsets.front() != 0U)
      return fail(error, "coverage offsets must start at zero");
    if (offsets[1U] <= offsets[0U])
      return fail(error, "coverage point count must be nonzero");
    const auto pointCount = offsets[1U] - offsets[0U];
    if (pointCount > std::numeric_limits<std::uint32_t>::max())
      return fail(error, "coverage point count exceeds the Vulkan ABI");
    for (std::size_t channel = 0U; channel < work.channelCount; ++channel) {
      if (offsets[channel + 1U] <= offsets[channel] ||
          offsets[channel + 1U] - offsets[channel] != pointCount)
        return fail(error, "coverage offsets are not equal-width and monotonic");
    }
    const auto totalCount = offsets.back();
    if (totalCount != work.updated.size())
      return fail(error, "coverage offsets disagree with input lengths");
    std::size_t bytes = 0U;
    if (!checkedBytes(totalCount, bytes))
      return fail(error, "coverage input byte size overflows");
    if (!checkedBytes(work.channelCount, bytes))
      return fail(error, "coverage output byte size overflows");

    std::vector<float> expected;
    if (!cpuOracle(work, pointCount, expected, error))
      return false;

    auto ensureBuffer = [&](Buffer &buffer, const std::size_t count,
                            const char *label) {
      std::size_t allocationBytes = 0U;
      if (!checkedBytes(count, allocationBytes))
        return fail(error, "coverage buffer byte size overflows");
      if (buffer.isValid() && buffer.size() >= allocationBytes)
        return true;
      buffer.reset();
      if (!model.createFloatBuffer(count, buffer, error)) {
        if (error.empty())
          error = std::string("failed to allocate ") + label + " buffer";
        return false;
      }
      return true;
    };
    if (!ensureBuffer(updated, totalCount, "updated coverage") ||
        !ensureBuffer(previous, totalCount, "previous coverage") ||
        !ensureBuffer(output, work.channelCount, "coverage output"))
      return false;

    if (!updated.write(work.updated.data(), totalCount * sizeof(float), 0U,
                       error) ||
        !previous.write(work.previous.data(), totalCount * sizeof(float), 0U,
                        error) ||
        !output.write(expected.data(), work.channelCount * sizeof(float), 0U,
                      error))
      return false;
    if (!model.evaluate(updated, totalCount, previous, totalCount, output,
                        work.channelCount, pointCount, work.channelCount,
                        error))
      return false;

    std::vector<float> candidate(work.channelCount);
    if (!output.read(candidate.data(), work.channelCount * sizeof(float), 0U,
                     error))
      return false;
    if (!std::equal(candidate.begin(), candidate.end(), expected.begin(),
                    rawEqual))
      return fail(error, "Vulkan coverage differs from CPU raw-bit oracle");

    std::copy(candidate.begin(), candidate.end(), work.output.begin());
    work.writtenCount = work.channelCount;
    work.complete = true;
    return true;
  }
};

VulkanCoverageDeltaExecutor::VulkanCoverageDeltaExecutor()
    : state_(std::make_shared<State>()) {}

VulkanCoverageDeltaExecutor::~VulkanCoverageDeltaExecutor() = default;

bool VulkanCoverageDeltaExecutor::initialize(const std::string_view spirvPath,
                                             std::string &error) {
  return state_->initialize(spirvPath, error);
}

bool VulkanCoverageDeltaExecutor::initialize(runtime::ComputeSession &session,
                                             const std::string_view spirvPath,
                                             std::string &error) {
  return state_->initialize(session, spirvPath, error);
}

void VulkanCoverageDeltaExecutor::reset() { state_->reset(); }

bool VulkanCoverageDeltaExecutor::isInitialized() const {
  return state_->isInitialized();
}

VulkanCoverageDeltaExecutor::Executor
VulkanCoverageDeltaExecutor::makeExecutor() const {
  const auto state = state_;
  return [state](Work &work, std::string &error) {
    try {
      return state->invoke(work, error);
    } catch (const std::exception &exception) {
      error = exception.what();
      return false;
    } catch (...) {
      error = "unknown Vulkan coverage executor failure";
      return false;
    }
  };
}

} // namespace viennaps::vulkan::surface
