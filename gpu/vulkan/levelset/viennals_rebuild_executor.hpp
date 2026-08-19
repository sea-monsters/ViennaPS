// Copyright 2026 ViennaPS
// SPDX-License-Identifier: MIT

//
// Frozen semantic mirror of viennals::Advect<T,D>::rebuildLS() as implemented
// in ViennaLS 5.8.5 with the levelset-update-v2 patch.
//
// This file implements the LevelSetRebuildExecutor callback that is installed
// into viennals::Advect. The callback collects HRLE candidates, dispatches
// Vulkan classification/compaction, and materializes the result back into a
// canonical CPU domain. The underlying classification/ compaction/
// reconstruction logic mirrors rebuildLS; it is not a direct call into the
// upstream private rebuild body.
//
// Any upstream change to rebuildLS must be reviewed against this mirror and
// the hrleRebuildCpuFixture differential test must be re-baselined.
//
// Card: R1-F1 / P4-HRLE-REBUILD-MIRROR
//

#pragma once

#include "hrle_rebuild_pipeline.hpp"

#include <levelset/psHrleRebuildClassification.hpp>
#include <levelset/psHrleSparseReconstruction.hpp>

#include <hrleSparseStarIterator.hpp>
#include <lsAdvect.hpp>
#include <lsDomain.hpp>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace viennaps::vulkan::levelset {

using viennaps::levelset::HrleRebuildCandidateFp32;
using viennaps::levelset::kInvalidHrlePointId;

struct ViennaLsRebuildExecutorStateFp32 {
  std::shared_ptr<runtime::ComputeSession> session;
  std::shared_ptr<primitives::ReductionScanPrimitives> primitives;
  std::shared_ptr<const runtime::SpirvProgram> classificationProgram;
  std::shared_ptr<const runtime::SpirvProgram> actionFlagsProgram;
  std::shared_ptr<const runtime::SpirvProgram> compactProgram;
};

namespace detail {

template <int D, class Iterator>
[[nodiscard]] bool makeRebuildCandidate(const Iterator &iterator,
                                        const std::size_t sourcePointCount,
                                        HrleRebuildCandidateFp32 &candidate,
                                        std::string &error) {
  const auto toPointId = [&](const auto &point) -> std::uint32_t {
    if (!point.isDefined())
      return kInvalidHrlePointId;
    const auto pointId = point.getPointId();
    return pointId <= std::numeric_limits<std::uint32_t>::max()
               ? static_cast<std::uint32_t>(pointId)
               : kInvalidHrlePointId;
  };

  const auto &center = iterator.getCenter();
  candidate.centerValue = center.getValue();
  candidate.centerDefinedValue =
      center.isDefined() ? center.getDefinedValue() : center.getValue();
  candidate.centerPointId = toPointId(center);
  for (std::size_t neighbor = 0U; neighbor < 2U * D; ++neighbor) {
    const auto &point = iterator.getNeighbor(static_cast<unsigned>(neighbor));
    candidate.neighborValues[neighbor] = point.getValue();
    candidate.neighborDefinedValues[neighbor] =
        point.isDefined() ? point.getDefinedValue() : point.getValue();
    candidate.neighborPointIds[neighbor] = toPointId(point);
  }

  if (!std::isfinite(candidate.centerValue) ||
      !std::isfinite(candidate.centerDefinedValue)) {
    error = "ViennaLS Vulkan rebuild received a non-finite center value.";
    return false;
  }
  for (std::size_t neighbor = 0U; neighbor < 2U * D; ++neighbor) {
    if (!std::isfinite(candidate.neighborValues[neighbor]) ||
        !std::isfinite(candidate.neighborDefinedValues[neighbor])) {
      error = "ViennaLS Vulkan rebuild received a non-finite neighbor value.";
      return false;
    }
    const auto pointId = candidate.neighborPointIds[neighbor];
    if (pointId != kInvalidHrlePointId &&
        static_cast<std::size_t>(pointId) >= sourcePointCount) {
      error = "ViennaLS Vulkan rebuild received an invalid source point ID.";
      return false;
    }
  }
  if (candidate.centerPointId != kInvalidHrlePointId &&
      static_cast<std::size_t>(candidate.centerPointId) >= sourcePointCount) {
    error = "ViennaLS Vulkan rebuild received an invalid center point ID.";
    return false;
  }
  return true;
}

template <int D>
[[nodiscard]] bool collectSegmentCandidates(
    const typename viennals::Domain<float, D>::DomainType &domain,
    const viennahrle::Index<D> &start, const viennahrle::Index<D> &end,
    const std::size_t sourcePointCount,
    std::vector<HrleRebuildCandidateFp32> &candidates,
    std::vector<viennahrle::Index<D>> &indices, std::string &error) {
  using DomainType = typename viennals::Domain<float, D>::DomainType;
  for (viennahrle::ConstSparseStarIterator<DomainType, 1> iterator(domain,
                                                                   start);
       iterator.getIndices() < end; ++iterator) {
    HrleRebuildCandidateFp32 candidate{};
    if (!makeRebuildCandidate<D>(iterator, sourcePointCount, candidate, error))
      return false;
    candidates.push_back(candidate);
    indices.push_back(iterator.getIndices());
  }
  return true;
}

} // namespace detail

// Copyable, lifetime-safe rebuild executor. The complete replacement is built
// in private state; the ViennaLS seam only sees HANDLED after every segment has
// passed device classification/compaction and CPU canonical materialization.
template <int D> class ViennaLsRebuildExecutorFp32 {
public:
  static_assert(D == 2 || D == 3,
                "ViennaLS Vulkan rebuild supports two or three dimensions");
  using AdvectType = viennals::Advect<float, D>;
  using Context = typename AdvectType::LevelSetRebuildContext;
  using Output = typename AdvectType::LevelSetRebuildOutput;
  using Status = typename AdvectType::LevelSetRebuildStatus;

  explicit ViennaLsRebuildExecutorFp32(
      std::shared_ptr<const ViennaLsRebuildExecutorStateFp32> state)
      : state_(std::move(state)) {}

  [[nodiscard]] Status operator()(const Context &context, Output &output,
                                  std::string &error) const {
    error.clear();
    if (!state_ || !state_->session || !state_->primitives ||
        !state_->classificationProgram || !state_->actionFlagsProgram ||
        !state_->compactProgram ||
        state_->classificationProgram->words.empty() ||
        state_->actionFlagsProgram->words.empty() ||
        state_->compactProgram->words.empty()) {
      error = "ViennaLS Vulkan rebuild executor has incomplete runtime state.";
      return Status::ERROR;
    }
    auto &session = *state_->session;
    auto &primitives = *state_->primitives;
    if (!session.isValid()) {
      error = "ViennaLS Vulkan rebuild executor has no valid compute session.";
      return Status::ERROR;
    }
    if (!std::isfinite(context.cutoff) || context.cutoff < 0.0F ||
        !std::isfinite(static_cast<float>(context.finalWidth)) ||
        context.finalWidth < 0) {
      error = "ViennaLS Vulkan rebuild executor received invalid parameters.";
      return Status::ERROR;
    }

    const auto sourcePointCount = context.domain.getNumberOfPoints();
    if (sourcePointCount > std::numeric_limits<std::uint32_t>::max()) {
      error = "ViennaLS Vulkan rebuild source point count exceeds uint32_t.";
      return Status::ERROR;
    }

    auto replacement = viennals::SmartPointer<viennals::Domain<float, D>>::New(
        context.domain.getGrid());
    auto &replacementDomain = replacement->getDomain();
    replacementDomain.initialize(context.domain.getNewSegmentation(),
                                 context.domain.getAllocation());
    const auto generation = session.generation();
    const auto segmentCount = replacementDomain.getNumberOfSegments();
    std::vector<std::vector<std::uint32_t>> sourceIds;
    if (context.updatePointData)
      sourceIds.resize(segmentCount);

    viennahrle::Grid<D> sourceGrid = context.domain.getGrid();
    for (unsigned segment = 0U; segment < segmentCount; ++segment) {
      if (session.generation() != generation) {
        error = "ViennaLS Vulkan rebuild session generation changed.";
        return Status::ERROR;
      }
      const auto start =
          segment == 0U ? sourceGrid.getMinGridPoint()
                        : replacementDomain.getSegmentation()[segment - 1U];
      const auto end =
          segment + 1U < segmentCount
              ? replacementDomain.getSegmentation()[segment]
              : sourceGrid.incrementIndices(sourceGrid.getMaxGridPoint());
      std::vector<HrleRebuildCandidateFp32> candidates;
      std::vector<viennahrle::Index<D>> indices;
      if (!detail::collectSegmentCandidates<D>(context.domain, start, end,
                                               sourcePointCount, candidates,
                                               indices, error))
        return Status::ERROR;

      std::vector<std::uint32_t> segmentSourceIds;
      if (!rebuildHrleRebuildFp32DeviceToCpuSegment<D>(
              session, *state_->classificationProgram,
              *state_->actionFlagsProgram, *state_->compactProgram, primitives,
              candidates, static_cast<std::uint32_t>(D),
              static_cast<float>(context.cutoff), indices, sourcePointCount,
              sourceGrid, segment, replacementDomain, segmentSourceIds, error))
        return Status::ERROR;
      if (context.updatePointData)
        sourceIds[segment] = std::move(segmentSourceIds);
    }

    if (session.generation() != generation) {
      error =
          "ViennaLS Vulkan rebuild session generation changed before publish.";
      return Status::ERROR;
    }

    std::vector<std::uint32_t> flatSourceIds;
    if (context.updatePointData) {
      flatSourceIds.reserve(sourcePointCount);
      for (auto &segmentSourceIds : sourceIds)
        flatSourceIds.insert(flatSourceIds.end(), segmentSourceIds.begin(),
                             segmentSourceIds.end());
    }
    replacementDomain.finalize();
    replacementDomain.segment();
    if (context.updatePointData) {
      std::vector<std::vector<std::uint32_t>> segmentedSourceIds(
          replacementDomain.getNumberOfSegments());
      std::size_t sourceCursor = 0U;
      for (unsigned segment = 0U;
           segment < replacementDomain.getNumberOfSegments(); ++segment) {
        const auto definedCount =
            replacementDomain.getDomainSegment(segment).definedValues.size();
        if (sourceCursor > flatSourceIds.size() ||
            definedCount > flatSourceIds.size() - sourceCursor) {
          error = "ViennaLS Vulkan rebuild source-ID repartition mismatch.";
          return Status::ERROR;
        }
        auto &segmentIds = segmentedSourceIds[segment];
        segmentIds.insert(segmentIds.end(),
                          flatSourceIds.begin() + sourceCursor,
                          flatSourceIds.begin() + sourceCursor + definedCount);
        sourceCursor += definedCount;
      }
      if (sourceCursor != flatSourceIds.size()) {
        error = "ViennaLS Vulkan rebuild source-ID repartition mismatch.";
        return Status::ERROR;
      }
      sourceIds = std::move(segmentedSourceIds);
    }
    Output candidateOutput;
    candidateOutput.domain = std::move(replacement);
    candidateOutput.sourceIds = std::move(sourceIds);
    output = std::move(candidateOutput);
    return Status::HANDLED;
  }

private:
  std::shared_ptr<const ViennaLsRebuildExecutorStateFp32> state_;
};

template <int D>
[[nodiscard]] ViennaLsRebuildExecutorFp32<D> makeViennaLsRebuildExecutorFp32(
    std::shared_ptr<const ViennaLsRebuildExecutorStateFp32> state) {
  return ViennaLsRebuildExecutorFp32<D>(std::move(state));
}

} // namespace viennaps::vulkan::levelset
