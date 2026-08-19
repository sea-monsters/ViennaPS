// Copyright 2026 ViennaPS
// SPDX-License-Identifier: MIT

//
// Frozen semantic mirror of viennals::Advect<T,D>::rebuildLS() as implemented
// in ViennaLS 5.8.5 with the levelset-update-v2 patch.
//
// This header compacts the per-candidate rebuild actions produced by
// psHrleRebuildClassification.hpp into a stable defined-point stream. It is
// part of the rebuildLS mirror, not a direct call into the upstream private
// rebuild body.
//
// Any upstream change to rebuildLS must be reviewed against this mirror and
// the hrleRebuildCpuFixture differential test must be re-baselined.
//
// Card: R1-F1 / P4-HRLE-REBUILD-MIRROR
//

#pragma once

#include <cmath>
#include <cstdint>
#include <limits>
#include <span>
#include <string>
#include <vector>

#include <levelset/psHrleRebuildClassification.hpp>

namespace viennaps::levelset {

struct HrleRebuildCompactDecisionFp32 {
  std::uint32_t candidateIndex = 0U;
  float value = 0.0F;
  std::uint32_t sourcePointId = kInvalidHrlePointId;
  HrleRebuildAction action = HrleRebuildAction::UNDEFINED_POSITIVE;

  bool operator==(const HrleRebuildCompactDecisionFp32 &) const = default;
};

struct HrleRebuildCompactionResultFp32 {
  std::vector<HrleRebuildAction> candidateActions{};
  std::vector<HrleRebuildCompactDecisionFp32> definedPoints{};
  std::vector<std::uint32_t> definedMask{};
  std::vector<std::uint32_t> definedExclusiveOffsets{};

  bool operator==(const HrleRebuildCompactionResultFp32 &) const = default;
};

namespace detail {

[[nodiscard]] inline bool
isValidHrleRebuildAction(const HrleRebuildAction action) {
  return action == HrleRebuildAction::DEFINED ||
         action == HrleRebuildAction::UNDEFINED_NEGATIVE ||
         action == HrleRebuildAction::UNDEFINED_POSITIVE;
}

} // namespace detail

[[nodiscard]] inline bool compactHrleRebuildDecisionsCpu(
    const std::span<const HrleRebuildDecisionFp32> decisions,
    HrleRebuildCompactionResultFp32 &output, std::string &error) {
  error.clear();

  if (decisions.size() > std::numeric_limits<std::uint32_t>::max()) {
    error = "HRLE rebuild decisions must fit in uint32_t index space.";
    return false;
  }

  std::vector<HrleRebuildAction> localCandidateActions;
  std::vector<std::uint32_t> localDefinedMask;
  std::vector<std::uint32_t> localOffsets;
  std::vector<HrleRebuildCompactDecisionFp32> localDefinedPoints;

  localCandidateActions.reserve(decisions.size());
  localDefinedMask.reserve(decisions.size());
  localOffsets.reserve(decisions.size() + 1U);
  localOffsets.push_back(0U);
  std::uint32_t definedCount = 0U;

  for (std::uint32_t candidateIndex = 0U; candidateIndex < decisions.size();
       ++candidateIndex) {
    const auto &decision = decisions[candidateIndex];
    if (!detail::isValidHrleRebuildAction(decision.action)) {
      error = "HRLE rebuild decisions contain an invalid action.";
      return false;
    }
    if (!std::isfinite(decision.value)) {
      error = "HRLE rebuild decisions require finite values.";
      return false;
    }
    if (decision.action == HrleRebuildAction::DEFINED &&
        decision.sourcePointId == kInvalidHrlePointId) {
      error = "Defined HRLE rebuild decision requires a valid source point ID.";
      return false;
    }

    localCandidateActions.push_back(decision.action);
    const bool isDefined = decision.action == HrleRebuildAction::DEFINED;
    localDefinedMask.push_back(isDefined ? 1U : 0U);
    if (isDefined) {
      localDefinedPoints.push_back({candidateIndex, decision.value,
                                    decision.sourcePointId,
                                    HrleRebuildAction::DEFINED});
      ++definedCount;
    }
    localOffsets.push_back(definedCount);
  }

  HrleRebuildCompactionResultFp32 localResult;
  localResult.candidateActions = std::move(localCandidateActions);
  localResult.definedPoints = std::move(localDefinedPoints);
  localResult.definedMask = std::move(localDefinedMask);
  localResult.definedExclusiveOffsets = std::move(localOffsets);
  output = std::move(localResult);
  return true;
}

} // namespace viennaps::levelset
