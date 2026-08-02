// Copyright 2026 ViennaPS
// SPDX-License-Identifier: MIT

#include "hrle_rebuild_compaction.hpp"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <string_view>
#include <vector>

namespace {

namespace Classification = viennaps::levelset;
namespace runtime = viennaps::vulkan::runtime;

using Compaction = viennaps::levelset::HrleRebuildCompactionResultFp32;
using Decision = viennaps::levelset::HrleRebuildDecisionFp32;
using DecisionIndex = viennaps::levelset::HrleRebuildCompactDecisionFp32;
using Primitive = viennaps::vulkan::primitives::ReductionScanPrimitives;

[[nodiscard]] bool fail(std::string &error, const std::string_view phase,
                        const std::string_view message) {
  error = std::string(phase) + ": " + std::string(message);
  return false;
}

template <class T>
[[nodiscard]] bool writeBuffer(runtime::HostVisibleBuffer &buffer,
                               const std::span<const T> values,
                               std::string &error) {
  if (values.empty()) {
    return true;
  }
  return buffer.write(values.data(), values.size() * sizeof(T), 0U, error);
}

template <class T>
[[nodiscard]] bool readBuffer(runtime::HostVisibleBuffer &buffer,
                              const std::size_t count, std::vector<T> &values,
                              std::string &error) {
  values.resize(count);
  if (count == 0U) {
    return true;
  }
  return buffer.read(values.data(), values.size() * sizeof(T), 0U, error);
}

} // namespace

namespace viennaps::vulkan::levelset {

bool compactHrleRebuildDecisionsFp32(Primitive &primitives,
                                     const std::span<const Decision> decisions,
                                     Compaction &output, std::string &error) {
  const std::size_t decisionCount = decisions.size();
  error.clear();
  if (!primitives.isInitialized()) {
    return fail(error, "execution",
                "reduction/scan primitives are not initialized");
  }
  if (decisionCount > std::numeric_limits<std::uint32_t>::max()) {
    return fail(error, "validation",
                "HRLE rebuild decisions must fit in uint32_t index space.");
  }

  std::vector<Classification::HrleRebuildAction> localCandidateActions;
  std::vector<DecisionIndex> localDefinedPoints;
  std::vector<std::uint32_t> localDefinedMask;
  std::vector<std::uint32_t> localCompactFlags;
  std::vector<std::uint32_t> localCompactIndices;

  localCandidateActions.reserve(decisionCount);
  localDefinedMask.reserve(decisionCount);
  localCompactFlags.reserve(decisionCount);
  localCompactIndices.reserve(decisionCount);

  for (std::size_t candidateIndex = 0U; candidateIndex < decisionCount;
       ++candidateIndex) {
    const auto &decision = decisions[candidateIndex];
    if (!Classification::detail::isValidHrleRebuildAction(decision.action)) {
      return fail(error, "validation",
                  "HRLE rebuild decisions contain an invalid action.");
    }
    if (!std::isfinite(decision.value)) {
      return fail(error, "validation",
                  "HRLE rebuild decisions require finite values.");
    }
    if (decision.action == Classification::HrleRebuildAction::DEFINED &&
        decision.sourcePointId == Classification::kInvalidHrlePointId) {
      return fail(error, "validation",
                  "Defined HRLE rebuild decision requires a valid source point "
                  "ID.");
    }

    localCandidateActions.push_back(decision.action);
    const bool isDefined =
        decision.action == Classification::HrleRebuildAction::DEFINED;
    localDefinedMask.push_back(isDefined ? 1U : 0U);
    localCompactFlags.push_back(isDefined ? 1U : 0U);
    localCompactIndices.push_back(static_cast<std::uint32_t>(candidateIndex));
  }

  if (decisionCount == 0U) {
    Compaction localResult;
    localResult.candidateActions = std::move(localCandidateActions);
    localResult.definedPoints = {};
    localResult.definedMask = std::move(localDefinedMask);
    localResult.definedExclusiveOffsets = {0U};
    output = std::move(localResult);
    return true;
  }

  runtime::HostVisibleBuffer indexInput{};
  runtime::HostVisibleBuffer compactIndexOutput{};
  runtime::HostVisibleBuffer flagsInput{};
  runtime::HostVisibleBuffer offsets{};

  std::size_t outputCount = 0U;
  if (!primitives.createIntBuffer(decisionCount, indexInput, error) ||
      !primitives.createIntBuffer(decisionCount, compactIndexOutput, error) ||
      !primitives.createIntBuffer(decisionCount, flagsInput, error) ||
      !primitives.createIntBuffer(decisionCount, offsets, error) ||
      !writeBuffer(indexInput,
                   std::span<const std::uint32_t>(localCompactIndices),
                   error) ||
      !writeBuffer(flagsInput,
                   std::span<const std::uint32_t>(localCompactFlags), error) ||
      !primitives.exclusiveScanInt(flagsInput, decisionCount, offsets,
                                   decisionCount, error) ||
      !primitives.stableCompactUInt32(indexInput, decisionCount, flagsInput,
                                      decisionCount, compactIndexOutput,
                                      decisionCount, outputCount, error)) {
    return false;
  }

  std::vector<std::uint32_t> localExclusiveOffsets{};
  if (!readBuffer(offsets, decisionCount, localExclusiveOffsets, error)) {
    return false;
  }

  if (outputCount > decisionCount ||
      outputCount > std::numeric_limits<std::uint32_t>::max()) {
    return fail(error, "gpu result",
                "GPU compaction returned an invalid selected count.");
  }

  std::vector<std::uint32_t> compactedIndices{};
  if (!readBuffer(compactIndexOutput, outputCount, compactedIndices, error)) {
    return false;
  }

  localExclusiveOffsets.push_back(static_cast<std::uint32_t>(outputCount));

  std::uint32_t expectedOffset = 0U;
  for (std::size_t index = 0U; index < decisionCount; ++index) {
    if (localExclusiveOffsets[index] != expectedOffset) {
      return fail(error, "gpu result",
                  "GPU scan returned an invalid exclusive offset.");
    }
    expectedOffset += localDefinedMask[index];
  }
  if (expectedOffset != outputCount ||
      localExclusiveOffsets.back() != expectedOffset) {
    return fail(error, "gpu result",
                "GPU scan and compaction selected counts disagree.");
  }

  localDefinedPoints.reserve(outputCount);
  for (std::size_t compactedIndex = 0U; compactedIndex < outputCount;
       ++compactedIndex) {
    const auto candidateIndex = compactedIndices[compactedIndex];
    if (candidateIndex >= decisions.size()) {
      return fail(error, "gpu result",
                  "GPU compaction returned an out-of-range candidate index.");
    }
    if (compactedIndex > 0U &&
        candidateIndex <= compactedIndices[compactedIndex - 1U]) {
      return fail(error, "gpu result",
                  "GPU compaction did not preserve stable candidate order.");
    }
    const auto &decision = decisions[candidateIndex];
    if (decision.action != Classification::HrleRebuildAction::DEFINED) {
      return fail(error, "gpu result",
                  "GPU compaction selected an undefined candidate.");
    }
    localDefinedPoints.push_back({candidateIndex, decision.value,
                                  decision.sourcePointId,
                                  Classification::HrleRebuildAction::DEFINED});
  }

  Compaction localResult{};
  localResult.candidateActions = std::move(localCandidateActions);
  localResult.definedPoints = std::move(localDefinedPoints);
  localResult.definedMask = std::move(localDefinedMask);
  localResult.definedExclusiveOffsets = std::move(localExclusiveOffsets);
  output = std::move(localResult);
  return true;
}

} // namespace viennaps::vulkan::levelset
