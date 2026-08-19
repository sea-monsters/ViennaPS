// Copyright 2026 ViennaPS
// SPDX-License-Identifier: MIT

//
// Frozen semantic mirror of viennals::Advect<T,D>::rebuildLS() as implemented
// in ViennaLS 5.8.5 with the levelset-update-v2 patch.
//
// This header is NOT a direct call into the upstream private rebuild body.
// It reimplements the active/inactive point classification, sign-crossing
// test, defined-value clamping, and Manhattan-distance fallback that
// rebuildLS performs, in FP32, so that the same logic can run on CPU (as an
// oracle) and be dispatched to Vulkan compute.
//
// Any upstream change to rebuildLS must be reviewed against this mirror and
// the hrleRebuildCpuFixture differential test must be re-baselined.
//
// Card: R1-F1 / P4-HRLE-REBUILD-MIRROR
//

#pragma once

#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace viennaps::levelset {

enum class HrleRebuildAction : std::uint32_t {
  UNDEFINED_POSITIVE = 0U,
  UNDEFINED_NEGATIVE = 1U,
  DEFINED = 2U,
};

inline constexpr std::uint32_t kInvalidHrlePointId =
    std::numeric_limits<std::uint32_t>::max();

struct HrleRebuildCandidateFp32 {
  float centerValue = 0.0F;
  float centerDefinedValue = 0.0F;
  std::uint32_t centerPointId = kInvalidHrlePointId;
  std::array<float, 6U> neighborValues{};
  std::array<float, 6U> neighborDefinedValues{};
  std::array<std::uint32_t, 6U> neighborPointIds = {
      kInvalidHrlePointId, kInvalidHrlePointId, kInvalidHrlePointId,
      kInvalidHrlePointId, kInvalidHrlePointId, kInvalidHrlePointId};
};

struct HrleRebuildDecisionFp32 {
  float value = 0.0F;
  std::uint32_t sourcePointId = kInvalidHrlePointId;
  HrleRebuildAction action = HrleRebuildAction::UNDEFINED_POSITIVE;

  bool operator==(const HrleRebuildDecisionFp32 &) const = default;
};

namespace detail {

[[nodiscard]] inline HrleRebuildDecisionFp32
makeUndefinedDecision(const bool negative) {
  return {negative ? std::numeric_limits<float>::lowest()
                   : std::numeric_limits<float>::max(),
          kInvalidHrlePointId,
          negative ? HrleRebuildAction::UNDEFINED_NEGATIVE
                   : HrleRebuildAction::UNDEFINED_POSITIVE};
}

[[nodiscard]] inline HrleRebuildDecisionFp32
makeDefinedDecision(const float value, const std::uint32_t sourcePointId) {
  return {value, sourcePointId, HrleRebuildAction::DEFINED};
}

[[nodiscard]] inline HrleRebuildDecisionFp32
classifyHrleRebuildCandidate(const HrleRebuildCandidateFp32 &candidate,
                             const std::uint32_t neighborCount,
                             const float cutoff) {
  constexpr float signEpsilon = 1.0e-7F;
  if (std::abs(candidate.centerValue) <= 1.0F) {
    bool crossesInterface = false;
    for (std::uint32_t neighbor = 0U; neighbor < neighborCount; ++neighbor) {
      if (std::signbit(candidate.neighborValues[neighbor] - signEpsilon) !=
          std::signbit(candidate.centerValue + signEpsilon)) {
        crossesInterface = true;
        break;
      }
    }
    if (!crossesInterface)
      return makeUndefinedDecision(candidate.centerDefinedValue < 0.0F);

    if (candidate.centerDefinedValue > 0.5F) {
      for (std::uint32_t neighbor = 0U; neighbor < neighborCount; ++neighbor) {
        if (std::abs(candidate.neighborValues[neighbor]) <= 1.0F &&
            candidate.neighborDefinedValues[neighbor] < -0.5F) {
          return makeDefinedDecision(0.5F,
                                     candidate.neighborPointIds[neighbor]);
        }
      }
    } else if (candidate.centerDefinedValue < -0.5F) {
      for (std::uint32_t neighbor = 0U; neighbor < neighborCount; ++neighbor) {
        if (std::abs(candidate.neighborValues[neighbor]) <= 1.0F &&
            candidate.neighborDefinedValues[neighbor] > 0.5F) {
          return makeDefinedDecision(-0.5F,
                                     candidate.neighborPointIds[neighbor]);
        }
      }
    }
    return makeDefinedDecision(candidate.centerDefinedValue,
                               candidate.centerPointId);
  }

  if (candidate.centerValue >= 0.0F) {
    float distance = std::numeric_limits<float>::max();
    std::uint32_t sourcePointId = kInvalidHrlePointId;
    for (std::uint32_t neighbor = 0U; neighbor < neighborCount; ++neighbor) {
      const float value = candidate.neighborValues[neighbor];
      if (std::abs(value) <= 1.0F && value < 0.0F && distance > value + 1.0F) {
        distance = value + 1.0F;
        sourcePointId = candidate.neighborPointIds[neighbor];
      }
    }
    return distance <= cutoff ? makeDefinedDecision(distance, sourcePointId)
                              : makeUndefinedDecision(false);
  }

  float distance = std::numeric_limits<float>::lowest();
  std::uint32_t sourcePointId = kInvalidHrlePointId;
  for (std::uint32_t neighbor = 0U; neighbor < neighborCount; ++neighbor) {
    const float value = candidate.neighborValues[neighbor];
    if (std::abs(value) <= 1.0F && value > 0.0F && distance < value - 1.0F) {
      distance = value - 1.0F;
      sourcePointId = candidate.neighborPointIds[neighbor];
    }
  }
  return distance >= -cutoff ? makeDefinedDecision(distance, sourcePointId)
                             : makeUndefinedDecision(true);
}

} // namespace detail

[[nodiscard]] inline bool classifyHrleRebuildCpu(
    const std::span<const HrleRebuildCandidateFp32> candidates,
    const std::uint32_t dimensions, const float cutoff,
    std::vector<HrleRebuildDecisionFp32> &output, std::string &error) {
  error.clear();
  if (dimensions != 2U && dimensions != 3U) {
    error = "HRLE rebuild classification requires two or three dimensions.";
    return false;
  }
  if (!std::isfinite(cutoff) || cutoff < 0.0F) {
    error = "HRLE rebuild cutoff must be finite and non-negative.";
    return false;
  }

  const std::uint32_t neighborCount = 2U * dimensions;
  std::vector<HrleRebuildDecisionFp32> candidateOutput;
  candidateOutput.reserve(candidates.size());
  for (const auto &candidate : candidates) {
    if (!std::isfinite(candidate.centerValue) ||
        !std::isfinite(candidate.centerDefinedValue)) {
      error = "HRLE rebuild center values must be finite.";
      return false;
    }
    for (std::uint32_t neighbor = 0U; neighbor < neighborCount; ++neighbor) {
      if (!std::isfinite(candidate.neighborValues[neighbor]) ||
          !std::isfinite(candidate.neighborDefinedValues[neighbor])) {
        error = "HRLE rebuild neighbor values must be finite.";
        return false;
      }
    }

    const auto decision =
        detail::classifyHrleRebuildCandidate(candidate, neighborCount, cutoff);
    if (decision.action == HrleRebuildAction::DEFINED &&
        decision.sourcePointId == kInvalidHrlePointId) {
      error = "Defined HRLE rebuild output requires a valid source point ID.";
      return false;
    }
    candidateOutput.push_back(decision);
  }
  output = std::move(candidateOutput);
  return true;
}

} // namespace viennaps::levelset
