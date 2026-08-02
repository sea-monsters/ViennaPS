// Copyright 2026 ViennaPS
// SPDX-License-Identifier: MIT

#include "surface_diffusion_executor.hpp"

#include "graph_diffusion.hpp"

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

using Work = viennaps::SurfaceDiffusionWork<float>;
using Status = viennaps::SurfaceDiffusionExecutionStatus;
using Buffer = runtime::HostVisibleBuffer;

[[nodiscard]] bool normalOrZero(const float value) {
  return value == 0.0F || std::isnormal(value);
}

[[nodiscard]] bool rawEqual(const float lhs, const float rhs) {
  return std::bit_cast<std::uint32_t>(lhs) ==
         std::bit_cast<std::uint32_t>(rhs);
}

[[nodiscard]] Status fail(std::string &error, const char *message) {
  error = message;
  return Status::FAILURE;
}

[[nodiscard]] bool checkedBytes(const std::size_t count,
                                const std::size_t elementSize,
                                std::size_t &bytes) {
  if (count > std::numeric_limits<std::size_t>::max() / elementSize)
    return false;
  bytes = count * elementSize;
  return true;
}

[[nodiscard]] bool cpuReference(const Work &work, std::vector<float> &expected,
                                std::string &error) {
  const auto count = work.field.size();
  expected.resize(count);
  if (work.rowOffsets.size() != count + 1U)
    return false;
  for (std::size_t row = 0U; row < count; ++row) {
    volatile float laplacian = 0.0F;
    const auto begin = work.rowOffsets[row];
    const auto end = work.rowOffsets[row + 1U];
    for (std::size_t edge = begin; edge < end; ++edge) {
      volatile float product =
          work.weights[edge] * work.field[work.columnIndices[edge]];
      if (!normalOrZero(product)) {
        error = "surface diffusion product leaves strict FP32";
        return false;
      }
      laplacian = laplacian + product;
      if (!normalOrZero(laplacian)) {
        error = "surface diffusion row sum leaves strict FP32";
        return false;
      }
    }
    volatile float scaled = work.diffusionStep * laplacian;
    if (!normalOrZero(scaled)) {
      error = "surface diffusion scaling leaves strict FP32";
      return false;
    }
    volatile float result = work.field[row] + scaled;
    if (!normalOrZero(result)) {
      error = "surface diffusion result leaves strict FP32";
      return false;
    }
    expected[row] = result;
  }
  return true;
}

} // namespace

struct VulkanSurfaceDiffusionExecutor::State {
  SurfaceGraphDiffusionFp32 model{};
  Buffer rowOffsets{};
  Buffer columnIndices{};
  Buffer weights{};
  Buffer field{};
  Buffer output{};
  mutable std::mutex mutex{};

  ~State() { resetUnlocked(); }

  void resetUnlocked() {
    output.reset();
    field.reset();
    weights.reset();
    columnIndices.reset();
    rowOffsets.reset();
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

  [[nodiscard]] Status invoke(Work &work, std::string &error) {
    std::lock_guard lock(mutex);
    error.clear();
    if (!model.isInitialized())
      return fail(error, "Vulkan surface diffusion executor is not initialized");

    const auto pointCount = work.field.size();
    if (pointCount == std::numeric_limits<std::size_t>::max() ||
        work.rowOffsets.size() != pointCount + 1U)
      return fail(error, "CSR row-offset count must equal field size + 1");
    if (work.output.size() != pointCount)
      return fail(error, "surface diffusion output shape disagrees with field");
    if (work.columnIndices.size() != work.weights.size())
      return fail(error, "CSR column and weight lengths disagree");
    if (!normalOrZero(work.diffusionStep) ||
        !std::isfinite(work.diffusionStep))
      return fail(error, "diffusion step must be finite normal FP32 or zero");
    if (pointCount > std::numeric_limits<std::uint32_t>::max() ||
        work.columnIndices.size() > std::numeric_limits<std::uint32_t>::max())
      return fail(error, "surface diffusion dimensions exceed Vulkan ABI");

    if (pointCount != 0U) {
      if (work.output.data() == nullptr || work.field.data() == nullptr)
        return fail(error, "surface diffusion spans must be initialized");
      std::size_t fieldBytes = 0U;
      if (!checkedBytes(pointCount, sizeof(float), fieldBytes))
        return fail(error, "surface diffusion field size overflows");
      const auto fieldBegin = reinterpret_cast<std::uintptr_t>(
          work.field.data());
      const auto outputBegin = reinterpret_cast<std::uintptr_t>(
          work.output.data());
      const auto rangesOverlap =
          fieldBegin < outputBegin
              ? outputBegin - fieldBegin < fieldBytes
              : fieldBegin - outputBegin < fieldBytes;
      if (rangesOverlap)
        return fail(error, "surface diffusion field and output must not alias");
    }
    if (work.rowOffsets.empty() || work.rowOffsets.front() != 0U)
      return fail(error, "CSR row offsets must start at zero");
    if (work.rowOffsets.back() != work.columnIndices.size())
      return fail(error, "CSR final offset disagrees with nonzero count");
    for (std::size_t row = 0U; row < pointCount; ++row) {
      const auto begin = work.rowOffsets[row];
      const auto end = work.rowOffsets[row + 1U];
      if (begin > end || end > work.columnIndices.size())
        return fail(error, "CSR row offsets are not monotonic");
    }
    for (std::size_t edge = 0U; edge < work.columnIndices.size(); ++edge) {
      if (work.columnIndices[edge] >= pointCount)
        return fail(error, "CSR column index is out of bounds");
      if (!normalOrZero(work.weights[edge]) ||
          !std::isfinite(work.weights[edge]))
        return fail(error, "CSR weights must be finite normal FP32 or zero");
    }
    for (const float value : work.field) {
      if (!normalOrZero(value) || !std::isfinite(value))
        return fail(error, "field values must be finite normal FP32 or zero");
    }
    std::size_t bytes = 0U;
    if (!checkedBytes(work.rowOffsets.size(), sizeof(std::uint32_t), bytes) ||
        !checkedBytes(work.columnIndices.size(), sizeof(std::uint32_t), bytes) ||
        !checkedBytes(work.weights.size(), sizeof(float), bytes) ||
        !checkedBytes(pointCount, sizeof(float), bytes))
      return fail(error, "surface diffusion buffer size overflows");

    std::vector<float> expected;
    if (!cpuReference(work, expected, error))
      return Status::FAILURE;

    auto ensureIndex = [&](Buffer &buffer, const std::size_t count) {
      if (buffer.isValid() &&
          count <= static_cast<std::size_t>(buffer.size() /
                                            sizeof(std::uint32_t)))
        return true;
      buffer.reset();
      return model.createIndexBuffer(count, buffer, error);
    };
    auto ensureFloat = [&](Buffer &buffer, const std::size_t count) {
      if (buffer.isValid() &&
          count <= static_cast<std::size_t>(buffer.size() / sizeof(float)))
        return true;
      buffer.reset();
      return model.createFloatBuffer(count, buffer, error);
    };
    if (!ensureIndex(rowOffsets, work.rowOffsets.size()) ||
        !ensureIndex(columnIndices, work.columnIndices.size()) ||
        !ensureFloat(weights, work.weights.size()) ||
        !ensureFloat(field, pointCount) || !ensureFloat(output, pointCount))
      return Status::FAILURE;

    if ((!work.rowOffsets.empty() &&
         !rowOffsets.write(work.rowOffsets.data(),
                           work.rowOffsets.size() * sizeof(std::uint32_t), 0U,
                           error)) ||
        (!work.columnIndices.empty() &&
         !columnIndices.write(work.columnIndices.data(),
                              work.columnIndices.size() * sizeof(std::uint32_t),
                              0U, error)) ||
        (!work.weights.empty() &&
         !weights.write(work.weights.data(), work.weights.size() * sizeof(float),
                        0U, error)) ||
        (!work.field.empty() &&
         !field.write(work.field.data(), work.field.size() * sizeof(float), 0U,
                      error)) ||
        (!expected.empty() &&
         !output.write(expected.data(), expected.size() * sizeof(float), 0U,
                       error)))
      return Status::FAILURE;

    if (!model.evaluate(rowOffsets, work.rowOffsets.size(), columnIndices,
                        work.columnIndices.size(), weights, work.weights.size(),
                        field, pointCount, output, pointCount,
                        work.diffusionStep, error))
      return Status::FAILURE;

    std::vector<float> candidate(pointCount);
    if (!candidate.empty() &&
        !output.read(candidate.data(), candidate.size() * sizeof(float), 0U,
                     error))
      return Status::FAILURE;
    for (std::size_t index = 0U; index < candidate.size(); ++index) {
      if (!rawEqual(candidate[index], expected[index]))
        return fail(error, "Vulkan surface diffusion differs from CPU oracle");
    }

    std::copy(candidate.begin(), candidate.end(), work.output.begin());
    work.writtenCount = pointCount;
    work.complete = true;
    return Status::SUCCESS;
  }
};

VulkanSurfaceDiffusionExecutor::VulkanSurfaceDiffusionExecutor()
    : state_(std::make_shared<State>()) {}

VulkanSurfaceDiffusionExecutor::~VulkanSurfaceDiffusionExecutor() = default;

bool VulkanSurfaceDiffusionExecutor::initialize(const std::string_view spirvPath,
                                                std::string &error) {
  return state_->initialize(spirvPath, error);
}

bool VulkanSurfaceDiffusionExecutor::initialize(
    runtime::ComputeSession &session, const std::string_view spirvPath,
    std::string &error) {
  return state_->initialize(session, spirvPath, error);
}

void VulkanSurfaceDiffusionExecutor::reset() { state_->reset(); }

bool VulkanSurfaceDiffusionExecutor::isInitialized() const {
  return state_->isInitialized();
}

VulkanSurfaceDiffusionExecutor::Executor
VulkanSurfaceDiffusionExecutor::makeExecutor() const {
  const auto state = state_;
  return [state](Work &work, std::string &error) {
    try {
      return state->invoke(work, error);
    } catch (const std::exception &exception) {
      error = exception.what();
      return Status::FAILURE;
    } catch (...) {
      error = "unknown Vulkan surface diffusion executor failure";
      return Status::FAILURE;
    }
  };
}

} // namespace viennaps::vulkan::surface
