// Copyright 2026 ViennaPS
// SPDX-License-Identifier: MIT

#include "hrle_rebuild_classification.hpp"

#include <levelset/psHrleRebuildClassification.hpp>

#include <hrleSparseStarIterator.hpp>
#include <lsDomain.hpp>
#include <lsExpand.hpp>
#include <lsMakeGeometry.hpp>

#include <vcTestAsserts.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <string>
#include <vector>

namespace {

namespace classification = viennaps::levelset;
namespace ls = viennals;
namespace runtime = viennaps::vulkan::runtime;
namespace vkLevelSet = viennaps::vulkan::levelset;

using Candidate = classification::HrleRebuildCandidateFp32;
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

  for (std::size_t neighborId = 0; neighborId < 4U; ++neighborId) {
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
collectCandidates(const ls::SmartPointer<ls::Domain<float, 2>> &levelSet) {
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
    }
  }
  return candidates;
}

[[nodiscard]] bool equalBits(const Decision &left, const Decision &right) {
  return left.action == right.action &&
         left.sourcePointId == right.sourcePointId &&
         std::bit_cast<std::uint32_t>(left.value) ==
             std::bit_cast<std::uint32_t>(right.value);
}

} // namespace

int main() try {
  auto levelSet = makeDomain();
  const auto candidates = collectCandidates(levelSet);
  VC_TEST_ASSERT(!candidates.empty());

  std::vector<Decision> cpu;
  std::string error;
  VC_TEST_ASSERT(
      classification::classifyHrleRebuildCpu(candidates, 2U, 1.0F, cpu, error));
  VC_TEST_ASSERT(error.empty());
  VC_TEST_ASSERT(cpu.size() == candidates.size());

  runtime::ComputeSession session;
  VC_TEST_ASSERT(session.initialize(error));
  runtime::SpirvProgram program;
  VC_TEST_ASSERT(runtime::readSpirv(VIENNAPS_HRLE_CLASSIFICATION_SPV_PATH,
                                    program, error));

  std::vector<Decision> vulkan;
  VC_TEST_ASSERT(vkLevelSet::classifyHrleRebuildFp32(
      session, program, candidates, 2U, 1.0F, vulkan, error));
  VC_TEST_ASSERT(error.empty());
  VC_TEST_ASSERT(vulkan.size() == cpu.size());
  for (std::size_t index = 0U; index < cpu.size(); ++index)
    VC_TEST_ASSERT(equalBits(vulkan[index], cpu[index]));

  const auto definedCount = static_cast<std::size_t>(
      std::count_if(cpu.begin(), cpu.end(), [](const Decision &decision) {
        return decision.action == classification::HrleRebuildAction::DEFINED;
      }));
  VC_TEST_ASSERT(definedCount > 0U);
  VC_TEST_ASSERT(definedCount < cpu.size());

  std::vector<Decision> emptyOutput = cpu;
  VC_TEST_ASSERT(vkLevelSet::classifyHrleRebuildFp32(session, program, {}, 2U,
                                                     1.0F, emptyOutput, error));
  VC_TEST_ASSERT(emptyOutput.empty());

  Decision sentinelDecision{};
  sentinelDecision.value = 17.0F;
  sentinelDecision.sourcePointId = classification::kInvalidHrlePointId;
  sentinelDecision.action =
      classification::HrleRebuildAction::UNDEFINED_POSITIVE;
  const std::vector<Decision> failureSentinel = {sentinelDecision};
  auto rejectedOutput = failureSentinel;
  VC_TEST_ASSERT(!vkLevelSet::classifyHrleRebuildFp32(
      session, program, candidates, 1U, 1.0F, rejectedOutput, error));
  VC_TEST_ASSERT(rejectedOutput == failureSentinel);

  std::cout << "[HrleRebuildClassification] candidates=" << candidates.size()
            << " defined=" << definedCount << " CPU/Vulkan exact PASS\n";
  return EXIT_SUCCESS;
} catch (const std::exception &exception) {
  std::cerr << exception.what() << '\n';
  return EXIT_FAILURE;
}
