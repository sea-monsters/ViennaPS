// Copyright 2026 ViennaPS
// SPDX-License-Identifier: MIT

#include "hrle_rebuild_classification.hpp"
#include "hrle_rebuild_compaction.hpp"
#include "hrle_rebuild_pipeline.hpp"

#include <levelset/psHrleRebuildClassification.hpp>
#include <levelset/psHrleRebuildCompaction.hpp>

#include <hrleSparseStarIterator.hpp>
#include <lsDomain.hpp>
#include <lsExpand.hpp>
#include <lsMakeGeometry.hpp>

#include <vcTestAsserts.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <span>
#include <string>
#include <vector>

namespace {

namespace classification = viennaps::levelset;
namespace ls = viennals;
namespace primitives = viennaps::vulkan::primitives;
namespace runtime = viennaps::vulkan::runtime;
namespace vkLevelSet = viennaps::vulkan::levelset;

using Candidate = classification::HrleRebuildCandidateFp32;
using CompactDecision = classification::HrleRebuildCompactDecisionFp32;
using CompactResult = classification::HrleRebuildCompactionResultFp32;
using Decision = classification::HrleRebuildDecisionFp32;

[[nodiscard]] ls::SmartPointer<ls::Domain<float, 2>> makeDomain() {
  constexpr int dimension = 2;
  constexpr viennahrle::CoordType extent = 8.0;
  constexpr viennahrle::CoordType gridDelta = 0.5;
  viennahrle::CoordType bounds[2 * dimension] = {-extent, extent, -extent,
                                                 extent};
  typename ls::Domain<float, dimension>::BoundaryType
      boundaryConditions[dimension];
  for (auto &condition : boundaryConditions)
    condition = ls::BoundaryConditionEnum::REFLECTIVE_BOUNDARY;

  auto domain =
      ls::Domain<float, dimension>::New(bounds, boundaryConditions, gridDelta);
  float origin[dimension] = {0.0F, 0.0F};
  ls::MakeGeometry<float, dimension>(
      domain, ls::SmartPointer<ls::Sphere<float, dimension>>::New(origin, 3.0F))
      .apply();
  ls::Expand<float, dimension>(domain, 2).apply();
  return domain;
}

template <class Iterator>
[[nodiscard]] Candidate makeCandidate(const Iterator &iterator) {
  Candidate candidate{};
  const auto &center = iterator.getCenter();
  candidate.centerValue = center.getValue();
  candidate.centerDefinedValue =
      center.isDefined() ? center.getDefinedValue() : center.getValue();
  candidate.centerPointId =
      center.isDefined() ? static_cast<std::uint32_t>(center.getPointId())
                         : classification::kInvalidHrlePointId;

  for (std::size_t neighborId = 0U; neighborId < 4U; ++neighborId) {
    const auto &neighbor =
        iterator.getNeighbor(static_cast<unsigned int>(neighborId));
    candidate.neighborValues[neighborId] = neighbor.getValue();
    candidate.neighborDefinedValues[neighborId] =
        neighbor.isDefined() ? neighbor.getDefinedValue() : neighbor.getValue();
    candidate.neighborPointIds[neighborId] =
        neighbor.isDefined() ? static_cast<std::uint32_t>(neighbor.getPointId())
                             : classification::kInvalidHrlePointId;
  }
  return candidate;
}

[[nodiscard]] std::vector<Candidate>
collectCandidates(const ls::SmartPointer<ls::Domain<float, 2>> &levelSet,
                  std::vector<viennahrle::Index<2>> &candidateIndices) {
  std::vector<Candidate> candidates;
  const auto &grid = levelSet->getGrid();
  const auto &domain = levelSet->getDomain();
  for (unsigned segment = 0U; segment < domain.getNumberOfSegments();
       ++segment) {
    const viennahrle::Index<2> start =
        segment == 0U ? grid.getMinGridPoint()
                      : domain.getSegmentation()[segment - 1U];
    const viennahrle::Index<2> end =
        segment + 1U < domain.getNumberOfSegments()
            ? domain.getSegmentation()[segment]
            : grid.incrementIndices(grid.getMaxGridPoint());
    for (viennahrle::ConstSparseStarIterator<
             typename ls::Domain<float, 2>::DomainType, 1>
             iterator(domain, start);
         iterator.getIndices() < end; ++iterator) {
      candidates.push_back(makeCandidate(iterator));
      candidateIndices.push_back(iterator.getIndices());
    }
  }
  return candidates;
}

void assertExactResult(const CompactResult &actual,
                       const CompactResult &expected) {
  VC_TEST_ASSERT(actual.candidateActions == expected.candidateActions);
  VC_TEST_ASSERT(actual.definedMask == expected.definedMask);
  VC_TEST_ASSERT(actual.definedExclusiveOffsets ==
                 expected.definedExclusiveOffsets);
  VC_TEST_ASSERT(actual.definedPoints.size() == expected.definedPoints.size());
  for (std::size_t index = 0U; index < actual.definedPoints.size(); ++index) {
    const CompactDecision &left = actual.definedPoints[index];
    const CompactDecision &right = expected.definedPoints[index];
    VC_TEST_ASSERT(left.candidateIndex == right.candidateIndex);
    VC_TEST_ASSERT(left.sourcePointId == right.sourcePointId);
    VC_TEST_ASSERT(left.action == right.action);
    VC_TEST_ASSERT(std::bit_cast<std::uint32_t>(left.value) ==
                   std::bit_cast<std::uint32_t>(right.value));
  }
}

void assertDomainExact(const viennahrle::Domain<float, 2> &actual,
                       const viennahrle::Domain<float, 2> &expected,
                       const std::span<const viennahrle::Index<2>> indices) {
  for (const auto &index : indices) {
    viennahrle::ConstSparseIterator<viennahrle::Domain<float, 2>> left(actual,
                                                                       index);
    viennahrle::ConstSparseIterator<viennahrle::Domain<float, 2>> right(
        expected, index);
    VC_TEST_ASSERT(left.isDefined() == right.isDefined());
    VC_TEST_ASSERT(std::bit_cast<std::uint32_t>(left.getValue()) ==
                   std::bit_cast<std::uint32_t>(right.getValue()));
    if (left.isDefined())
      VC_TEST_ASSERT(std::bit_cast<std::uint32_t>(left.getDefinedValue()) ==
                     std::bit_cast<std::uint32_t>(right.getDefinedValue()));
  }
}

} // namespace

int main() try {
  auto levelSet = makeDomain();
  std::vector<viennahrle::Index<2>> candidateIndices;
  const auto candidates = collectCandidates(levelSet, candidateIndices);
  VC_TEST_ASSERT(candidates.size() == 183U);
  VC_TEST_ASSERT(candidateIndices.size() == candidates.size());

  std::string error;
  std::vector<Decision> cpuDecisions;
  VC_TEST_ASSERT(classification::classifyHrleRebuildCpu(candidates, 2U, 1.0F,
                                                        cpuDecisions, error));
  CompactResult cpuResult;
  VC_TEST_ASSERT(classification::compactHrleRebuildDecisionsCpu(
      cpuDecisions, cpuResult, error));
  VC_TEST_ASSERT(cpuResult.definedPoints.size() == 68U);

  runtime::ComputeSession session;
  VC_TEST_ASSERT(session.initialize(error));
  runtime::SpirvProgram classificationProgram;
  VC_TEST_ASSERT(runtime::readSpirv(VIENNAPS_HRLE_CLASSIFICATION_SPV_PATH,
                                    classificationProgram, error));
  runtime::SpirvProgram actionFlagsProgram;
  VC_TEST_ASSERT(runtime::readSpirv(VIENNAPS_HRLE_ACTION_FLAGS_SPV_PATH,
                                    actionFlagsProgram, error));
  runtime::SpirvProgram compactProgram;
  VC_TEST_ASSERT(runtime::readSpirv(VIENNAPS_HRLE_COMPACT_SPV_PATH,
                                    compactProgram, error));
  std::vector<Decision> vulkanDecisions;
  VC_TEST_ASSERT(vkLevelSet::classifyHrleRebuildFp32(
      session, classificationProgram, candidates, 2U, 1.0F, vulkanDecisions,
      error));

  primitives::ReductionScanPrimitives compactionPrimitives;
  VC_TEST_ASSERT(compactionPrimitives.initialize(
      session, VIENNAPS_REDUCTION_SCAN_SPV_PATH, error));
  VC_TEST_ASSERT(compactionPrimitives.device().get() == session.device().get());

  viennahrle::Domain<float, 2> pipelineDomain;
  std::vector<std::uint32_t> pipelineSourcePointIds = {99U};
  VC_TEST_ASSERT(vkLevelSet::rebuildHrleRebuildFp32DeviceToCpu<2>(
      session, classificationProgram, actionFlagsProgram, compactProgram,
      compactionPrimitives, candidates, 2U, 1.0F, candidateIndices,
      levelSet->getDomain().getNumberOfPoints(), levelSet->getGrid(),
      pipelineDomain, pipelineSourcePointIds, error));
  VC_TEST_ASSERT(error.empty());
  viennahrle::Domain<float, 2> cpuDomain;
  std::vector<std::uint32_t> cpuSourcePointIds;
  VC_TEST_ASSERT(classification::reconstructHrleRebuildCpu<2>(
      cpuResult, candidateIndices, levelSet->getDomain().getNumberOfPoints(),
      levelSet->getGrid(), cpuDomain, cpuSourcePointIds, error));
  VC_TEST_ASSERT(pipelineSourcePointIds == cpuSourcePointIds);
  assertDomainExact(pipelineDomain, cpuDomain, candidateIndices);

  viennahrle::Domain<float, 2> preservedDomain;
  preservedDomain.deepCopy(levelSet->getGrid(), cpuDomain);
  std::vector<std::uint32_t> preservedIds = {7U};
  const auto beforeIds = preservedIds;
  VC_TEST_ASSERT(!vkLevelSet::rebuildHrleRebuildFp32DeviceToCpu<2>(
      session, classificationProgram, actionFlagsProgram, compactProgram,
      compactionPrimitives, candidates, 2U, 1.0F,
      std::span<const viennahrle::Index<2>>(candidateIndices.data(), 1U),
      levelSet->getDomain().getNumberOfPoints(), levelSet->getGrid(),
      preservedDomain, preservedIds, error));
  VC_TEST_ASSERT(preservedIds == beforeIds);
  assertDomainExact(preservedDomain, cpuDomain, candidateIndices);
  VC_TEST_ASSERT(!error.empty());

  VC_TEST_ASSERT(!vkLevelSet::rebuildHrleRebuildFp32DeviceToCpu<2>(
      session, classificationProgram, actionFlagsProgram, compactProgram,
      compactionPrimitives, candidates, 3U, 1.0F, candidateIndices,
      levelSet->getDomain().getNumberOfPoints(), levelSet->getGrid(),
      preservedDomain, preservedIds, error));
  VC_TEST_ASSERT(preservedIds == beforeIds);
  assertDomainExact(preservedDomain, cpuDomain, candidateIndices);
  VC_TEST_ASSERT(!error.empty());

  viennahrle::Domain<float, 2> emptyDomain;
  std::vector<std::uint32_t> emptyIds = {9U};
  VC_TEST_ASSERT(vkLevelSet::rebuildHrleRebuildFp32DeviceToCpu<2>(
      session, classificationProgram, actionFlagsProgram, compactProgram,
      compactionPrimitives, {}, 2U, 1.0F, {}, 0U, levelSet->getGrid(),
      emptyDomain, emptyIds, error));
  VC_TEST_ASSERT(emptyIds.empty());
  CompactResult vulkanResult;
  VC_TEST_ASSERT(vkLevelSet::compactHrleRebuildDecisionsFp32(
      compactionPrimitives, vulkanDecisions, vulkanResult, error));
  VC_TEST_ASSERT(error.empty());
  assertExactResult(vulkanResult, cpuResult);

  vkLevelSet::HrleRebuildClassificationDeviceFp32 deviceClassification;
  VC_TEST_ASSERT(vkLevelSet::classifyHrleRebuildFp32Device(
      session, classificationProgram, candidates, 2U, 1.0F,
      deviceClassification, error));
  vkLevelSet::HrleRebuildCompactionDeviceFp32 deviceCompaction;
  VC_TEST_ASSERT(vkLevelSet::compactHrleRebuildDecisionsFp32Device(
      session, actionFlagsProgram, compactProgram, compactionPrimitives,
      deviceClassification, deviceCompaction, error));
  CompactResult materializedResult;
  VC_TEST_ASSERT(vkLevelSet::materializeHrleRebuildCompactionFp32Device(
      session, deviceClassification, deviceCompaction, materializedResult,
      error));
  VC_TEST_ASSERT(error.empty());
  assertExactResult(materializedResult, cpuResult);

  const auto savedCandidateCount = deviceCompaction.candidateCount;
  deviceCompaction.candidateCount += 1U;
  CompactResult sentinelResult = materializedResult;
  VC_TEST_ASSERT(!vkLevelSet::materializeHrleRebuildCompactionFp32Device(
      session, deviceClassification, deviceCompaction, sentinelResult, error));
  assertExactResult(sentinelResult, materializedResult);
  deviceCompaction.candidateCount = savedCandidateCount;

  Candidate allPositive{};
  allPositive.centerValue = 2.0F;
  allPositive.centerDefinedValue = 2.0F;
  allPositive.centerPointId = classification::kInvalidHrlePointId;
  allPositive.neighborValues.fill(2.0F);
  allPositive.neighborDefinedValues.fill(2.0F);
  allPositive.neighborPointIds.fill(classification::kInvalidHrlePointId);
  std::array<Candidate, 1U> positiveCandidates = {allPositive};
  vkLevelSet::HrleRebuildClassificationDeviceFp32 positiveClassification;
  VC_TEST_ASSERT(vkLevelSet::classifyHrleRebuildFp32Device(
      session, classificationProgram, positiveCandidates, 2U, 1.0F,
      positiveClassification, error));
  vkLevelSet::HrleRebuildCompactionDeviceFp32 positiveCompaction;
  VC_TEST_ASSERT(vkLevelSet::compactHrleRebuildDecisionsFp32Device(
      session, actionFlagsProgram, compactProgram, compactionPrimitives,
      positiveClassification, positiveCompaction, error));
  CompactResult positiveResult;
  VC_TEST_ASSERT(vkLevelSet::materializeHrleRebuildCompactionFp32Device(
      session, positiveClassification, positiveCompaction, positiveResult,
      error));
  CompactResult positiveExpected;
  std::vector<Decision> positiveCpuDecisions;
  VC_TEST_ASSERT(classification::classifyHrleRebuildCpu(
      positiveCandidates, 2U, 1.0F, positiveCpuDecisions, error));
  VC_TEST_ASSERT(classification::compactHrleRebuildDecisionsCpu(
      positiveCpuDecisions, positiveExpected, error));
  VC_TEST_ASSERT(positiveExpected.definedPoints.empty());
  assertExactResult(positiveResult, positiveExpected);

  runtime::ComputeSession differentSession;
  VC_TEST_ASSERT(differentSession.initialize(error));
  VC_TEST_ASSERT(!compactionPrimitives.initialize(
      differentSession, VIENNAPS_REDUCTION_SCAN_SPV_PATH, error));
  VC_TEST_ASSERT(!error.empty());
  VC_TEST_ASSERT(compactionPrimitives.isInitialized());
  VC_TEST_ASSERT(compactionPrimitives.device().get() == session.device().get());

  compactionPrimitives.reset();
  VC_TEST_ASSERT(session.isValid());

  VC_TEST_ASSERT(compactionPrimitives.initialize(
      session, VIENNAPS_REDUCTION_SCAN_SPV_PATH, error));
  VC_TEST_ASSERT(compactionPrimitives.device().get() == session.device().get());
  VC_TEST_ASSERT(vkLevelSet::compactHrleRebuildDecisionsFp32(
      compactionPrimitives, vulkanDecisions, vulkanResult, error));
  VC_TEST_ASSERT(error.empty());
  assertExactResult(vulkanResult, cpuResult);

  CompactResult emptyResult = cpuResult;
  VC_TEST_ASSERT(vkLevelSet::compactHrleRebuildDecisionsFp32(
      compactionPrimitives, {}, emptyResult, error));
  const CompactResult expectedEmpty{{}, {}, {}, {0U}};
  assertExactResult(emptyResult, expectedEmpty);

  const CompactResult sentinel = cpuResult;
  const auto assertTransactionalRejection =
      [&](const Decision &invalidDecision) {
        CompactResult rejected = sentinel;
        VC_TEST_ASSERT(!vkLevelSet::compactHrleRebuildDecisionsFp32(
            compactionPrimitives,
            std::span<const Decision>(&invalidDecision, 1U), rejected, error));
        assertExactResult(rejected, sentinel);
        VC_TEST_ASSERT(!error.empty());
      };
  assertTransactionalRejection(
      Decision{0.0F, 1U, static_cast<classification::HrleRebuildAction>(13U)});
  assertTransactionalRejection(
      Decision{0.25F, classification::kInvalidHrlePointId,
               classification::HrleRebuildAction::DEFINED});
  assertTransactionalRejection(
      Decision{std::numeric_limits<float>::quiet_NaN(), 1U,
               classification::HrleRebuildAction::DEFINED});

  std::cout << "[HrleRebuildCompaction] candidates=" << candidates.size()
            << " defined=" << cpuResult.definedPoints.size()
            << " CPU/Vulkan exact PASS\n";
  return EXIT_SUCCESS;
} catch (const std::exception &exception) {
  std::cerr << exception.what() << '\n';
  return EXIT_FAILURE;
}
