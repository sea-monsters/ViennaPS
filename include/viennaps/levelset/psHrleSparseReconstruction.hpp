// Copyright 2026 ViennaPS
// SPDX-License-Identifier: MIT

#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <string>
#include <vector>

#include <hrleDomain.hpp>

#include <levelset/psHrleRebuildCompaction.hpp>

namespace viennaps::levelset {

namespace detail {

[[nodiscard]] inline bool
isValidHrleRebuildSourcePoint(const std::uint32_t sourcePointId,
                             const std::size_t sourcePointCount) {
  return sourcePointId != kInvalidHrlePointId &&
         static_cast<std::size_t>(sourcePointId) < sourcePointCount;
}

} // namespace detail

template <int D> [[nodiscard]] inline bool reconstructHrleRebuildCpu(
    const HrleRebuildCompactionResultFp32 &compactResult,
    const std::span<const viennahrle::Index<D>> candidateIndices,
    const std::size_t sourcePointCount, viennahrle::Grid<D> &grid,
    viennahrle::Domain<float, D> &output,
    std::vector<std::uint32_t> &definedSourcePointIds, std::string &error) {
  static_assert(D == 2 || D == 3,
                "HRLE sparse reconstruction supports two or three dimensions");
  error.clear();

  constexpr float kPositiveUndefinedValue = std::numeric_limits<float>::max();
  constexpr float kNegativeUndefinedValue =
      std::numeric_limits<float>::lowest();

  if (candidateIndices.size() > std::numeric_limits<std::uint32_t>::max()) {
    error = "HRLE rebuild candidates must fit in uint32_t index space.";
    return false;
  }

  const auto candidateCount = candidateIndices.size();
  if (compactResult.candidateActions.size() != candidateCount) {
    error = "HRLE rebuild reconstruction candidate action size mismatch.";
    return false;
  }

  if (compactResult.definedMask.size() != candidateCount) {
    error = "HRLE rebuild reconstruction defined-mask size mismatch.";
    return false;
  }

  if (compactResult.definedExclusiveOffsets.size() != candidateCount + 1U) {
    error = "HRLE rebuild reconstruction offset size mismatch.";
    return false;
  }

  if (compactResult.definedExclusiveOffsets.empty() ||
      compactResult.definedExclusiveOffsets.front() != 0U) {
    error = "HRLE rebuild reconstruction offsets must start at zero.";
    return false;
  }

  std::size_t definedPointCursor = 0U;
  for (std::uint32_t candidate = 0U; candidate < candidateCount; ++candidate) {
    if (!detail::isValidHrleRebuildAction(
            compactResult.candidateActions[candidate])) {
      error = "HRLE rebuild reconstruction contains an invalid action.";
      return false;
    }

    const auto &index = candidateIndices[candidate];
    if (candidate > 0U &&
        !(candidateIndices[candidate - 1U] < candidateIndices[candidate])) {
      error = "HRLE rebuild reconstruction requires candidate indices in "
              "lexicographic order.";
      return false;
    }

    if (!grid.isInDomain(index)) {
      error = "HRLE rebuild reconstruction contains candidate indices outside "
              "the grid.";
      return false;
    }

    const auto currentOffset = compactResult.definedExclusiveOffsets[candidate];
    const auto nextOffset =
        compactResult.definedExclusiveOffsets[candidate + 1U];
    if (nextOffset < currentOffset ||
        nextOffset > compactResult.definedPoints.size()) {
      error =
          "HRLE rebuild reconstruction has invalid defined-exclusive offsets.";
      return false;
    }

    const auto action = compactResult.candidateActions[candidate];
    const bool isDefined = action == HrleRebuildAction::DEFINED;
    if (compactResult.definedMask[candidate] > 1U) {
      error = "HRLE rebuild reconstruction requires a binary defined-mask.";
      return false;
    }
    if ((compactResult.definedMask[candidate] != 0U) != isDefined) {
      error = "HRLE rebuild reconstruction has inconsistent defined-mask.";
      return false;
    }

    const auto expectedNextOffset =
        currentOffset + static_cast<std::uint32_t>(isDefined ? 1U : 0U);
    if (nextOffset != expectedNextOffset) {
      error = "HRLE rebuild reconstruction has invalid defined mask to offset "
              "mapping.";
      return false;
    }

    if (isDefined) {
      if (!std::isfinite(
              compactResult.definedPoints[definedPointCursor].value)) {
        error = "HRLE rebuild reconstruction requires finite defined "
                "reconstruction values.";
        return false;
      }
      if (compactResult.definedPoints[definedPointCursor].candidateIndex !=
          candidate) {
        error = "HRLE rebuild reconstruction has inconsistent compacted "
                "defined index map.";
        return false;
      }
      if (compactResult.definedPoints[definedPointCursor].action !=
          HrleRebuildAction::DEFINED) {
        error = "HRLE rebuild reconstruction has invalid defined action in "
                "compact list.";
        return false;
      }
      if (!detail::isValidHrleRebuildSourcePoint(
              compactResult.definedPoints[definedPointCursor].sourcePointId,
              sourcePointCount)) {
        error = "HRLE rebuild reconstruction has invalid source point in "
                "compact list.";
        return false;
      }

      ++definedPointCursor;
    }
  }

  if (compactResult.definedExclusiveOffsets[candidateCount] !=
      compactResult.definedPoints.size()) {
    error = "HRLE rebuild reconstruction has inconsistent final defined count.";
    return false;
  }

  if (definedPointCursor != compactResult.definedPoints.size()) {
    error = "HRLE rebuild reconstruction has inconsistent compacted-defined "
            "entries.";
    return false;
  }

  viennahrle::Domain<float, D> localDomain(&grid);
  std::vector<std::uint32_t> localDefinedSourcePointIds;
  localDefinedSourcePointIds.reserve(compactResult.definedPoints.size());
  std::size_t definedPointCursorForOutput = 0U;

  for (std::uint32_t candidate = 0U; candidate < candidateCount; ++candidate) {
    const auto action = compactResult.candidateActions[candidate];
    if (action == HrleRebuildAction::DEFINED) {
      const auto &definedPoint =
          compactResult.definedPoints[definedPointCursorForOutput++];
      localDomain.insertNextDefinedPoint(0, candidateIndices[candidate],
                                        definedPoint.value);
      localDefinedSourcePointIds.push_back(definedPoint.sourcePointId);
    } else if (action == HrleRebuildAction::UNDEFINED_NEGATIVE) {
      localDomain.insertNextUndefinedPoint(0, candidateIndices[candidate],
                                          kNegativeUndefinedValue);
    } else {
      localDomain.insertNextUndefinedPoint(0, candidateIndices[candidate],
                                          kPositiveUndefinedValue);
    }
  }

  localDomain.finalize();
  output.deepCopy(grid, localDomain);
  definedSourcePointIds = std::move(localDefinedSourcePointIds);
  return true;
}

} // namespace viennaps::levelset
