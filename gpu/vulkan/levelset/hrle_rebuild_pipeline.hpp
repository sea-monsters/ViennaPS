// Copyright 2026 ViennaPS
// SPDX-License-Identifier: MIT

#pragma once

#include "../primitives/reduction_scan_primitives.hpp"
#include "hrle_rebuild_classification.hpp"
#include "hrle_rebuild_compaction.hpp"

#include <levelset/psHrleSparseReconstruction.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace viennaps::vulkan::levelset {

// Runs the device classification/compaction stages and reconstructs the
// canonical ViennaHRLE field on the CPU. All output arguments are
// transactional: they are changed only after every stage succeeds.
template <int D>
[[nodiscard]] bool rebuildHrleRebuildFp32DeviceToCpu(
    runtime::ComputeSession &session,
    const runtime::SpirvProgram &classificationProgram,
    const runtime::SpirvProgram &actionFlagsProgram,
    const runtime::SpirvProgram &compactProgram,
    viennaps::vulkan::primitives::ReductionScanPrimitives &primitives,
    std::span<const viennaps::levelset::HrleRebuildCandidateFp32> candidates,
    std::uint32_t dimensions, float cutoff,
    std::span<const viennahrle::Index<D>> candidateIndices,
    std::size_t sourcePointCount, viennahrle::Grid<D> &grid,
    viennahrle::Domain<float, D> &output,
    std::vector<std::uint32_t> &definedSourcePointIds, std::string &error) {
  static_assert(D == 2 || D == 3,
                "HRLE sparse reconstruction supports two or three dimensions");
  error.clear();
  if (!session.isValid()) {
    error = "HRLE rebuild pipeline: compute session is not initialized.";
    return false;
  }
  if (dimensions != static_cast<std::uint32_t>(D)) {
    error = "HRLE rebuild pipeline: runtime and grid dimensions differ.";
    return false;
  }
  if (candidates.size() != candidateIndices.size()) {
    error = "HRLE rebuild pipeline: candidate and index counts differ.";
    return false;
  }

  const auto generation = session.generation();
  HrleRebuildClassificationDeviceFp32 classification{};
  if (!classifyHrleRebuildFp32Device(session, classificationProgram, candidates,
                                     dimensions, cutoff, classification, error))
    return false;
  if (session.generation() != generation) {
    error = "HRLE rebuild pipeline: session generation changed after "
            "classification.";
    return false;
  }

  HrleRebuildCompactionDeviceFp32 compaction{};
  if (!compactHrleRebuildDecisionsFp32Device(session, actionFlagsProgram,
                                             compactProgram, primitives,
                                             classification, compaction, error))
    return false;
  if (session.generation() != generation) {
    error =
        "HRLE rebuild pipeline: session generation changed after compaction.";
    return false;
  }

  viennaps::levelset::HrleRebuildCompactionResultFp32 compactResult{};
  if (!materializeHrleRebuildCompactionFp32Device(
          session, classification, compaction, compactResult, error))
    return false;
  if (session.generation() != generation) {
    error = "HRLE rebuild pipeline: session generation changed after "
            "materialization.";
    return false;
  }

  viennahrle::Domain<float, D> localOutput;
  std::vector<std::uint32_t> localSourcePointIds;
  if (!viennaps::levelset::reconstructHrleRebuildCpu<D>(
          compactResult, candidateIndices, sourcePointCount, grid, localOutput,
          localSourcePointIds, error))
    return false;
  if (session.generation() != generation) {
    error = "HRLE rebuild pipeline: session generation changed before publish.";
    return false;
  }

  output.deepCopy(grid, localOutput);
  definedSourcePointIds = std::move(localSourcePointIds);
  return true;
}

// Segmented counterpart used by the production rebuild executor. Device
// stages are identical to the flat compatibility API, but canonical CPU
// materialization writes directly into the caller-owned segment so a complete
// replacement can be assembled without flattening segment boundaries.
template <int D>
[[nodiscard]] bool rebuildHrleRebuildFp32DeviceToCpuSegment(
    runtime::ComputeSession &session,
    const runtime::SpirvProgram &classificationProgram,
    const runtime::SpirvProgram &actionFlagsProgram,
    const runtime::SpirvProgram &compactProgram,
    viennaps::vulkan::primitives::ReductionScanPrimitives &primitives,
    std::span<const viennaps::levelset::HrleRebuildCandidateFp32> candidates,
    std::uint32_t dimensions, float cutoff,
    std::span<const viennahrle::Index<D>> candidateIndices,
    std::size_t sourcePointCount, viennahrle::Grid<D> &grid,
    unsigned outputSegment, viennahrle::Domain<float, D> &output,
    std::vector<std::uint32_t> &definedSourcePointIds, std::string &error) {
  static_assert(D == 2 || D == 3,
                "HRLE sparse reconstruction supports two or three dimensions");
  error.clear();
  if (!session.isValid()) {
    error = "HRLE rebuild pipeline: compute session is not initialized.";
    return false;
  }
  if (dimensions != static_cast<std::uint32_t>(D)) {
    error = "HRLE rebuild pipeline: runtime and grid dimensions differ.";
    return false;
  }
  if (candidates.size() != candidateIndices.size()) {
    error = "HRLE rebuild pipeline: candidate and index counts differ.";
    return false;
  }
  const auto generation = session.generation();
  HrleRebuildClassificationDeviceFp32 classification{};
  if (!classifyHrleRebuildFp32Device(session, classificationProgram, candidates,
                                     dimensions, cutoff, classification, error))
    return false;
  if (session.generation() != generation) {
    error = "HRLE rebuild pipeline: session generation changed after "
            "classification.";
    return false;
  }
  HrleRebuildCompactionDeviceFp32 compaction{};
  if (!compactHrleRebuildDecisionsFp32Device(session, actionFlagsProgram,
                                             compactProgram, primitives,
                                             classification, compaction, error))
    return false;
  if (session.generation() != generation) {
    error =
        "HRLE rebuild pipeline: session generation changed after compaction.";
    return false;
  }
  viennaps::levelset::HrleRebuildCompactionResultFp32 compactResult{};
  if (!materializeHrleRebuildCompactionFp32Device(
          session, classification, compaction, compactResult, error))
    return false;
  if (session.generation() != generation) {
    error = "HRLE rebuild pipeline: session generation changed after "
            "materialization.";
    return false;
  }
  if (!viennaps::levelset::reconstructHrleRebuildCpuIntoSegment<D>(
          compactResult, candidateIndices, sourcePointCount, grid,
          outputSegment, output, definedSourcePointIds, error))
    return false;
  if (session.generation() != generation) {
    error = "HRLE rebuild pipeline: session generation changed before publish.";
    return false;
  }
  return true;
}

} // namespace viennaps::vulkan::levelset
