// Copyright 2026 ViennaPS
// SPDX-License-Identifier: MIT

#pragma once

#include "levelset_update.hpp"

#include <lsAdvect.hpp>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace viennaps::vulkan::levelset {

// Copyable ViennaLS executor that owns the shader payload and borrows one
// process-lifetime ComputeSession. The session must outlive every callback
// invocation; DeploymentComputeContext provides that lifetime in production.
template <int D> class ViennaLsUpdateExecutorFp32 {
public:
  using AdvectType = viennals::Advect<float, D>;
  using Context = typename AdvectType::LevelSetUpdateContext;
  using Output = typename AdvectType::LevelSetUpdateOutput;
  using Status = typename AdvectType::LevelSetUpdateStatus;

  ViennaLsUpdateExecutorFp32(
      runtime::ComputeSession &session,
      std::shared_ptr<const runtime::SpirvProgram> program)
      : session_(&session), program_(std::move(program)) {}

  [[nodiscard]] Status operator()(const Context &context, Output &output,
                                  std::string &error) const {
    output = {};
    error.clear();
    if (session_ == nullptr || !session_->isValid()) {
      error = "ViennaLS Vulkan executor has no valid compute session";
      return Status::ERROR;
    }
    if (!program_ || program_->words.empty()) {
      error = "ViennaLS Vulkan executor has no SPIR-V program";
      return Status::ERROR;
    }
    if (context.saveVelocities) {
      return Status::FALLBACK;
    }
    if (!std::isfinite(context.timeStep) || context.timeStep < 0.0 ||
        context.timeStep > std::numeric_limits<float>::max() ||
        !std::isfinite(context.integrationCutoff) ||
        context.integrationCutoff < 0.0 ||
        context.integrationCutoff > std::numeric_limits<float>::max()) {
      error = "ViennaLS Vulkan executor received an invalid scalar parameter";
      return Status::ERROR;
    }

    std::size_t pointCount = 0U;
    for (unsigned segment = 0U; segment < context.domain.getNumberOfSegments();
         ++segment) {
      const auto segmentPoints =
          context.domain.getDomainSegment(segment).getNumberOfPoints();
      if (segmentPoints >
          std::numeric_limits<std::size_t>::max() - pointCount) {
        error = "ViennaLS Vulkan executor point-count overflow";
        return Status::ERROR;
      }
      pointCount += segmentPoints;
    }
    if (pointCount > std::numeric_limits<std::uint32_t>::max() ||
        pointCount == std::numeric_limits<std::size_t>::max()) {
      error = "ViennaLS Vulkan executor exceeds point/offset range";
      return Status::ERROR;
    }

    std::vector<float> values;
    std::vector<std::uint32_t> rateOffsets;
    std::vector<float> gradients;
    std::vector<float> dissipations;
    std::vector<float> stopValues;
    values.reserve(pointCount);
    rateOffsets.reserve(pointCount + 1U);
    rateOffsets.push_back(0U);

    if (context.rates.size() != context.domain.getNumberOfSegments()) {
      error = "ViennaLS Vulkan executor segment/rate count mismatch";
      return Status::ERROR;
    }

    const float cutoff = static_cast<float>(context.integrationCutoff);
    constexpr float sentinel = std::numeric_limits<float>::max();
    for (unsigned segment = 0U; segment < context.domain.getNumberOfSegments();
         ++segment) {
      const auto &definedValues =
          context.domain.getDomainSegment(segment).definedValues;
      const auto &segmentRates = context.rates[segment];
      std::size_t rateCursor = 0U;
      for (const float value : definedValues) {
        values.push_back(value);
        if (std::abs(value) > cutoff) {
          if (!appendRate(0.0F, 0.0F, sentinel, gradients, dissipations,
                          stopValues, rateOffsets, error)) {
            return Status::ERROR;
          }
          continue;
        }

        bool foundSentinel = false;
        while (rateCursor < segmentRates.size()) {
          const auto &rate = segmentRates[rateCursor++];
          if (!appendRate(rate.first.first, rate.first.second, rate.second,
                          gradients, dissipations, stopValues, rateOffsets,
                          error, false)) {
            return Status::ERROR;
          }
          if (std::abs(rate.second) == sentinel) {
            foundSentinel = true;
            break;
          }
        }
        if (!foundSentinel) {
          error = "ViennaLS Vulkan executor rate stream lacks a sentinel";
          return Status::ERROR;
        }
        rateOffsets.push_back(static_cast<std::uint32_t>(gradients.size()));
      }
      if (rateCursor != segmentRates.size()) {
        error = "ViennaLS Vulkan executor found trailing segment rates";
        return Status::ERROR;
      }
    }

    if (rateOffsets.size() != pointCount + 1U) {
      error = "ViennaLS Vulkan executor produced an invalid CSR offset count";
      return Status::ERROR;
    }

    std::vector<float> updatedValues;
    const LevelSetUpdateInput input{
        values,     rateOffsets,
        gradients,  dissipations,
        stopValues, static_cast<float>(context.timeStep),
        cutoff,     context.checkDissipation};
    if (!updateLevelSetFp32(*session_, *program_, input, updatedValues,
                            error)) {
      return Status::ERROR;
    }
    if (updatedValues.size() != pointCount) {
      error = "ViennaLS Vulkan executor readback size mismatch";
      return Status::ERROR;
    }

    output.values.resize(context.domain.getNumberOfSegments());
    std::size_t valueOffset = 0U;
    for (unsigned segment = 0U; segment < context.domain.getNumberOfSegments();
         ++segment) {
      const auto segmentPoints =
          context.domain.getDomainSegment(segment).getNumberOfPoints();
      output.values[segment].assign(
          updatedValues.begin() + static_cast<std::ptrdiff_t>(valueOffset),
          updatedValues.begin() +
              static_cast<std::ptrdiff_t>(valueOffset + segmentPoints));
      valueOffset += segmentPoints;
    }
    return Status::HANDLED;
  }

private:
  [[nodiscard]] static bool
  appendRate(const float gradient, const float dissipation,
             const float stopValue, std::vector<float> &gradients,
             std::vector<float> &dissipations, std::vector<float> &stopValues,
             std::vector<std::uint32_t> &rateOffsets, std::string &error,
             const bool appendOffset = true) {
    if (gradients.size() >= std::numeric_limits<std::uint32_t>::max()) {
      error = "ViennaLS Vulkan executor exceeds uint32 rate range";
      return false;
    }
    gradients.push_back(gradient);
    dissipations.push_back(dissipation);
    stopValues.push_back(stopValue);
    if (appendOffset) {
      rateOffsets.push_back(static_cast<std::uint32_t>(gradients.size()));
    }
    return true;
  }

  runtime::ComputeSession *session_ = nullptr;
  std::shared_ptr<const runtime::SpirvProgram> program_;
};

template <int D>
[[nodiscard]] ViennaLsUpdateExecutorFp32<D> makeViennaLsUpdateExecutorFp32(
    runtime::ComputeSession &session,
    std::shared_ptr<const runtime::SpirvProgram> program) {
  return {session, std::move(program)};
}

} // namespace viennaps::vulkan::levelset
