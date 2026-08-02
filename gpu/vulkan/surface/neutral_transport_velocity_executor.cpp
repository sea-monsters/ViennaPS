// Copyright 2026 ViennaPS
// SPDX-License-Identifier: MIT

#include "neutral_transport_velocity_executor.hpp"

#include "neutral_transport_surface.hpp"

#include <materials/psBuiltInMaterial.hpp>

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

using Model = NeutralTransportSurfaceModelFp32;
using Buffer = runtime::HostVisibleBuffer;
using Work = viennaps::NeutralTransportVelocityWork<float>;

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

[[nodiscard]] bool validMaterialId(const float value) {
  if (!validStrictFloat(value) || value < 0.0F || std::trunc(value) != value)
    return false;
  return static_cast<double>(value) <=
         static_cast<double>(std::numeric_limits<int>::max());
}

[[nodiscard]] bool knownLegacyMaterial(const float value) {
  if (!validMaterialId(value))
    return false;
  if (static_cast<double>(value) >
      static_cast<double>(viennaps::kBuiltInMaterialMaxId))
    return false;
  const auto legacyId = static_cast<std::uint16_t>(value);
  return viennaps::isValidBuiltInMaterialId(legacyId);
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

[[nodiscard]] float legacyCpuVelocity(
    const float coverage, const float materialId,
    const NeutralTransportVelocityParameters<float> &parameters) {
  if (materialId != static_cast<float>(parameters.etchFrontMaterialId))
    return 0.0F;
  const auto etchVelocity =
      parameters.siliconDensity > 0.0F
          ? parameters.kEtch * parameters.surfaceSiteDensity * coverage /
                parameters.siliconDensity
          : 0.0F;
  return -etchVelocity * parameters.timeToSecond /
         parameters.lengthToMeter;
}

} // namespace

struct VulkanNeutralTransportVelocityExecutor::State {
  Model model{};
  Buffer coverage{};
  Buffer materialIds{};
  Buffer velocity{};
  mutable std::mutex mutex{};

  ~State() { resetUnlocked(); }

  void resetUnlocked() {
    // HostVisibleBuffer stores the VkDevice needed for destruction. It must be
    // released before the ComputeSession owned by model is reset.
    velocity.reset();
    materialIds.reset();
    coverage.reset();
    model.reset();
  }

  bool initialize(std::string_view spirvPath, std::string &error) {
    std::lock_guard lock(mutex);
    resetUnlocked();
    return model.initialize(spirvPath, error);
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
      return fail(error, "Vulkan neutral-transport executor is not initialized");

    const auto count = work.coverage.size();
    if (work.materialIds.size() != count || work.output.size() != count)
      return fail(error, "neutral-transport spans must have equal lengths");
    if (count > std::numeric_limits<std::uint32_t>::max())
      return fail(error, "neutral-transport length exceeds the Vulkan ABI");

    std::size_t bytes = 0U;
    if (!checkedBytes(count, bytes))
      return fail(error, "neutral-transport buffer size overflows");

    const auto &parameters = work.parameters;
    if (parameters.etchFrontMaterialId < 0)
      return fail(error, "negative legacy material id is not supported");
    if (!validStrictFloat(parameters.kEtch) ||
        !validStrictFloat(parameters.surfaceSiteDensity) ||
        !validStrictFloat(parameters.siliconDensity) ||
        !validStrictFloat(parameters.timeToSecond) ||
        !validStrictFloat(parameters.lengthToMeter) ||
        parameters.lengthToMeter == 0.0F) {
      return fail(error, "neutral-transport parameters are outside strict FP32");
    }

    const float frontMaterialId =
        static_cast<float>(parameters.etchFrontMaterialId);
    if (!knownLegacyMaterial(frontMaterialId) ||
        static_cast<int>(frontMaterialId) != parameters.etchFrontMaterialId) {
      return fail(error, "legacy material id is not exactly representable");
    }
    for (std::size_t index = 0U; index < count; ++index) {
      if (!validStrictFloat(work.coverage[index]) ||
          !knownLegacyMaterial(work.materialIds[index])) {
        return fail(error, "coverage/material input is outside strict FP32");
      }
    }

    if (count == 0U) {
      work.writtenCount = 0U;
      work.complete = true;
      return true;
    }

    auto ensureBuffer = [&](Buffer &buffer, const char *label) {
      if (buffer.isValid() && buffer.size() >= bytes)
        return true;
      buffer.reset();
      if (!model.createFloatBuffer(count, buffer, error)) {
        if (error.empty())
          error = std::string("failed to allocate ") + label + " buffer";
        return false;
      }
      return true;
    };
    if (!ensureBuffer(coverage, "coverage") ||
        !ensureBuffer(materialIds, "material-id") ||
        !ensureBuffer(velocity, "velocity")) {
      return false;
    }

    std::vector<float> cpu(count);
    const NeutralTransportSurfaceParamsFp32 gpuParams{
        parameters.kEtch, parameters.surfaceSiteDensity, parameters.siliconDensity,
        parameters.timeToSecond, parameters.lengthToMeter,
        static_cast<std::uint32_t>(parameters.etchFrontMaterialId)};
    for (std::size_t index = 0U; index < count; ++index) {
      cpu[index] = legacyCpuVelocity(work.coverage[index],
                                     work.materialIds[index], parameters);
    }

    if (!coverage.write(work.coverage.data(), bytes, 0U, error) ||
        !materialIds.write(work.materialIds.data(), bytes, 0U, error) ||
        !velocity.write(cpu.data(), bytes, 0U, error)) {
      return false;
    }
    if (!model.evaluate(coverage, count, materialIds, count, velocity, count,
                        gpuParams, error)) {
      return false;
    }

    std::vector<float> candidate(count);
    if (!velocity.read(candidate.data(), bytes, 0U, error))
      return false;
    if (!std::equal(candidate.begin(), candidate.end(), cpu.begin(), rawEqual))
      return fail(error, "Vulkan velocity differs from the CPU raw-bit oracle");

    std::copy(candidate.begin(), candidate.end(), work.output.begin());
    work.writtenCount = count;
    work.complete = true;
    return true;
  }
};

VulkanNeutralTransportVelocityExecutor::VulkanNeutralTransportVelocityExecutor()
    : state_(std::make_shared<State>()) {}

VulkanNeutralTransportVelocityExecutor::~VulkanNeutralTransportVelocityExecutor() =
    default;

bool VulkanNeutralTransportVelocityExecutor::initialize(
    const std::string_view spirvPath, std::string &error) {
  return state_->initialize(spirvPath, error);
}

void VulkanNeutralTransportVelocityExecutor::reset() { state_->reset(); }

bool VulkanNeutralTransportVelocityExecutor::isInitialized() const {
  return state_->isInitialized();
}

VulkanNeutralTransportVelocityExecutor::Executor
VulkanNeutralTransportVelocityExecutor::makeExecutor() const {
  const auto state = state_;
  return [state](Work &work, std::string &error) {
    try {
      return state->invoke(work, error);
    } catch (const std::exception &exception) {
      error = exception.what();
      return false;
    } catch (...) {
      error = "unknown Vulkan neutral-transport executor failure";
      return false;
    }
  };
}

} // namespace viennaps::vulkan::surface
