// Copyright 2026 ViennaPS
// SPDX-License-Identifier: MIT

#include <levelset/psHrleSparseReconstruction.hpp>

#include <hrleDomain.hpp>
#include <hrleGrid.hpp>
#include <vcTestAsserts.hpp>

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace {

namespace reconstruction = viennaps::levelset;
using Action = reconstruction::HrleRebuildAction;
using CompactResult = reconstruction::HrleRebuildCompactionResultFp32;
using CompactPoint = reconstruction::HrleRebuildCompactDecisionFp32;

template <int D> using Domain = viennahrle::Domain<float, D>;
template <int D> using Grid = viennahrle::Grid<D>;
template <int D> using Index = viennahrle::Index<D>;
template <int D>
using ConstSparseIterator = viennahrle::ConstSparseIterator<Domain<D>>;

using PointSample2D = std::pair<Index<2>, float>;

void assertFloatExact(const float lhs, const float rhs) {
  VC_TEST_ASSERT(std::bit_cast<std::uint32_t>(lhs) ==
                 std::bit_cast<std::uint32_t>(rhs));
}

void assertDefinedPoints(const std::vector<PointSample2D> &actual,
                        const std::vector<PointSample2D> &expected) {
  VC_TEST_ASSERT(actual.size() == expected.size());
  for (std::size_t index = 0U; index < actual.size(); ++index) {
    VC_TEST_ASSERT(actual[index].first == expected[index].first);
    assertFloatExact(actual[index].second, expected[index].second);
  }
}

template <int D>
void assertSample(const Domain<D> &domain, const Index<D> &index,
                  const bool isDefined, const float expectedValue) {
  ConstSparseIterator<D> iterator(domain, index);
  VC_TEST_ASSERT(!iterator.isFinished());
  VC_TEST_ASSERT(iterator.isDefined() == isDefined);
  assertFloatExact(iterator.getValue(), expectedValue);
}

template <int D>
std::vector<std::pair<Index<D>, float>> collectDefinedPoints(
    const Domain<D> &domain,
    const std::span<const Index<D>> candidateIndices) {
  std::vector<std::pair<Index<D>, float>> output;
  for (const auto &index : candidateIndices) {
    ConstSparseIterator<D> iterator(domain, index);
    if (iterator.isDefined()) {
      output.push_back({index, iterator.getDefinedValue()});
    }
  }
  return output;
}

std::vector<Index<2>> makeCandidateIndices() {
  return {Index<2>{0, 0}, Index<2>{1, 0}, Index<2>{2, 0}, Index<2>{3, 0}};
}

CompactResult makeCompactMixedResult() {
  CompactResult result{};
  result.candidateActions = {
      Action::UNDEFINED_NEGATIVE,
      Action::DEFINED,
      Action::DEFINED,
      Action::UNDEFINED_POSITIVE,
  };
  result.definedPoints = {CompactPoint{1U, 0.25F, 2U, Action::DEFINED},
                         CompactPoint{2U, -0.75F, 1U, Action::DEFINED}};
  result.definedMask = {0U, 1U, 1U, 0U};
  result.definedExclusiveOffsets = {0U, 0U, 1U, 2U, 2U};
  return result;
}

void testReconstructionWithSourceMapping() {
  Grid<2> grid(std::array<int, 2>{0, 0}.data(),
               std::array<int, 2>{6, 2}.data());
  const auto candidateIndices = makeCandidateIndices();
  auto compact = makeCompactMixedResult();

  Domain<2> domain;
  std::vector<std::uint32_t> sourcePointIds;
  std::string error;
  VC_TEST_ASSERT(reconstruction::reconstructHrleRebuildCpu(
      compact, std::span<const Index<2>>(candidateIndices),
      4U, grid, domain, sourcePointIds, error));
  VC_TEST_ASSERT(error.empty());
  VC_TEST_ASSERT(sourcePointIds == std::vector<std::uint32_t>({2U, 1U}));

  const auto defined = collectDefinedPoints(
      domain, std::span<const Index<2>>(candidateIndices));
  const std::vector<PointSample2D> expected = {
      {Index<2>{1, 0}, 0.25F}, {Index<2>{2, 0}, -0.75F}};
  assertDefinedPoints(defined, expected);
  assertSample(domain, candidateIndices[0], false,
               std::numeric_limits<float>::lowest());
  assertSample(domain, candidateIndices[1], true, 0.25F);
  assertSample(domain, candidateIndices[2], true, -0.75F);
  assertSample(domain, candidateIndices[3], false,
               std::numeric_limits<float>::max());
}

void testAllDefinedAndAllUndefinedBranches() {
  Grid<2> grid(std::array<int, 2>{0, 0}.data(),
               std::array<int, 2>{4, 2}.data());
  const auto candidateIndices = makeCandidateIndices();

  {
    CompactResult allDefined;
    allDefined.candidateActions = {Action::DEFINED, Action::DEFINED,
                                   Action::DEFINED, Action::DEFINED};
    allDefined.definedPoints = {
        {0U, 0.1F, 0U, Action::DEFINED},
        {1U, 0.2F, 1U, Action::DEFINED},
        {2U, -0.3F, 2U, Action::DEFINED},
        {3U, 0.4F, 3U, Action::DEFINED},
    };
    allDefined.definedMask = {1U, 1U, 1U, 1U};
    allDefined.definedExclusiveOffsets = {0U, 1U, 2U, 3U, 4U};

    Domain<2> domain;
    std::vector<std::uint32_t> sourcePointIds;
    std::string error;
    VC_TEST_ASSERT(reconstruction::reconstructHrleRebuildCpu(
        allDefined, std::span<const Index<2>>(candidateIndices),
        4U, grid, domain, sourcePointIds, error));
    VC_TEST_ASSERT(sourcePointIds ==
                   std::vector<std::uint32_t>({0U, 1U, 2U, 3U}));
    const auto defined = collectDefinedPoints(
        domain, std::span<const Index<2>>(candidateIndices));
    VC_TEST_ASSERT(defined.size() == candidateIndices.size());
    assertFloatExact(defined[0].second, 0.1F);
    assertFloatExact(defined[1].second, 0.2F);
    assertFloatExact(defined[2].second, -0.3F);
    assertFloatExact(defined[3].second, 0.4F);
  }

  {
    CompactResult allUndefined;
    allUndefined.candidateActions = {Action::UNDEFINED_NEGATIVE,
                                    Action::UNDEFINED_POSITIVE,
                                    Action::UNDEFINED_NEGATIVE,
                                    Action::UNDEFINED_POSITIVE};
    allUndefined.definedPoints = {};
    allUndefined.definedMask = {0U, 0U, 0U, 0U};
    allUndefined.definedExclusiveOffsets = {0U, 0U, 0U, 0U, 0U};
    Domain<2> domain;
    std::vector<std::uint32_t> sourcePointIds = {99U};
    std::string error;
    VC_TEST_ASSERT(reconstruction::reconstructHrleRebuildCpu(
        allUndefined, std::span<const Index<2>>(candidateIndices),
        0U, grid, domain, sourcePointIds, error));
    VC_TEST_ASSERT(sourcePointIds.empty());
    const auto defined = collectDefinedPoints(
        domain, std::span<const Index<2>>(candidateIndices));
    VC_TEST_ASSERT(defined.empty());
    assertSample(domain, candidateIndices[0], false,
                 std::numeric_limits<float>::lowest());
    assertSample(domain, candidateIndices[1], false,
                 std::numeric_limits<float>::max());
    assertSample(domain, candidateIndices[2], false,
                 std::numeric_limits<float>::lowest());
    assertSample(domain, candidateIndices[3], false,
                 std::numeric_limits<float>::max());
  }

  {
    CompactResult empty;
    empty.definedExclusiveOffsets = {0U};
    Domain<2> domain;
    std::vector<std::uint32_t> sourcePointIds = {99U};
    std::string error;
    VC_TEST_ASSERT(reconstruction::reconstructHrleRebuildCpu(
        empty, std::span<const Index<2>>{}, 0U, grid, domain, sourcePointIds,
        error));
    VC_TEST_ASSERT(sourcePointIds.empty());
    const auto defined =
        collectDefinedPoints(domain, std::span<const Index<2>>{});
    VC_TEST_ASSERT(defined.empty());
  }
}

void testThreeDimensionalReconstruction() {
  Grid<3> grid(std::array<int, 3>{0, 0, 0}.data(),
               std::array<int, 3>{2, 2, 2}.data());
  const std::vector<Index<3>> candidateIndices = {
      Index<3>{0, 0, 0}, Index<3>{1, 0, 0}};

  CompactResult compact;
  compact.candidateActions = {Action::DEFINED,
                              Action::UNDEFINED_POSITIVE};
  compact.definedPoints = {
      CompactPoint{0U, 0.125F, 0U, Action::DEFINED}};
  compact.definedMask = {1U, 0U};
  compact.definedExclusiveOffsets = {0U, 1U, 1U};

  Domain<3> domain;
  std::vector<std::uint32_t> sourcePointIds;
  std::string error;
  VC_TEST_ASSERT(reconstruction::reconstructHrleRebuildCpu(
      compact, std::span<const Index<3>>(candidateIndices), 1U, grid, domain,
      sourcePointIds, error));
  VC_TEST_ASSERT(error.empty());
  VC_TEST_ASSERT(sourcePointIds == std::vector<std::uint32_t>({0U}));
  assertSample(domain, candidateIndices[0], true, 0.125F);
  assertSample(domain, candidateIndices[1], false,
               std::numeric_limits<float>::max());
}

void testBoundarySemanticsAreHrleAuthoritative() {
  // Reflective x-axis: the maxIndex plane (x == 4) is a legal hrle
  // definition plane and must pass validation.
  {
    std::array<int, 2> min{0, 0};
    std::array<int, 2> max{4, 2};
    std::array<viennahrle::BoundaryType, 2> bcs{
        viennahrle::BoundaryType::REFLECTIVE_BOUNDARY,
        viennahrle::BoundaryType::REFLECTIVE_BOUNDARY};
    Grid<2> grid(min.data(), max.data(), 1.0, bcs.data());
    const std::vector<Index<2>> candidateIndices = {Index<2>{4, 0}};

    CompactResult compact;
    compact.candidateActions = {Action::DEFINED};
    compact.definedPoints = {CompactPoint{0U, 0.5F, 0U, Action::DEFINED}};
    compact.definedMask = {1U};
    compact.definedExclusiveOffsets = {0U, 1U};

    Domain<2> domain;
    std::vector<std::uint32_t> sourcePointIds;
    std::string error;
    VC_TEST_ASSERT(reconstruction::reconstructHrleRebuildCpu(
        compact, std::span<const Index<2>>(candidateIndices), 1U, grid, domain,
        sourcePointIds, error));
    VC_TEST_ASSERT(error.empty());
  }

  // Periodic x-axis: the maxIndex plane (x == 4) is identified by hrle as
  // outside the domain and must still be rejected.
  {
    std::array<int, 2> min{0, 0};
    std::array<int, 2> max{4, 2};
    std::array<viennahrle::BoundaryType, 2> bcs{
        viennahrle::BoundaryType::PERIODIC_BOUNDARY,
        viennahrle::BoundaryType::REFLECTIVE_BOUNDARY};
    Grid<2> grid(min.data(), max.data(), 1.0, bcs.data());
    const std::vector<Index<2>> candidateIndices = {Index<2>{4, 0}};

    CompactResult compact;
    compact.candidateActions = {Action::DEFINED};
    compact.definedPoints = {CompactPoint{0U, 0.5F, 0U, Action::DEFINED}};
    compact.definedMask = {1U};
    compact.definedExclusiveOffsets = {0U, 1U};

    Domain<2> domain;
    std::vector<std::uint32_t> sourcePointIds;
    std::string error;
    VC_TEST_ASSERT(!reconstruction::reconstructHrleRebuildCpu(
        compact, std::span<const Index<2>>(candidateIndices), 1U, grid, domain,
        sourcePointIds, error));
    VC_TEST_ASSERT(!error.empty());
  }
}

void testValidationIsTransactional() {
  Grid<2> grid(std::array<int, 2>{0, 0}.data(),
               std::array<int, 2>{6, 2}.data());
  const auto candidateIndices = makeCandidateIndices();
  const auto validCompact = makeCompactMixedResult();

  Domain<2> domain;
  std::vector<std::uint32_t> sourcePointIds;
  std::string error;
  VC_TEST_ASSERT(reconstruction::reconstructHrleRebuildCpu(
      validCompact, std::span<const Index<2>>(candidateIndices),
      4U, grid, domain, sourcePointIds, error));
  const auto expected = collectDefinedPoints(
      domain, std::span<const Index<2>>(candidateIndices));
  const auto expectedSourcePointIds = sourcePointIds;
  VC_TEST_ASSERT(error.empty());

  CompactResult badActionCount = validCompact;
  badActionCount.candidateActions.pop_back();
  VC_TEST_ASSERT(!reconstruction::reconstructHrleRebuildCpu(
      badActionCount, std::span<const Index<2>>(candidateIndices),
      4U, grid, domain, sourcePointIds, error));
  VC_TEST_ASSERT(!error.empty());
  VC_TEST_ASSERT(collectDefinedPoints(
                     domain, std::span<const Index<2>>(candidateIndices)) ==
                 expected);
  VC_TEST_ASSERT(sourcePointIds == expectedSourcePointIds);

  CompactResult badSource = validCompact;
  badSource.definedPoints[0].sourcePointId = 999U;
  error.clear();
  VC_TEST_ASSERT(!reconstruction::reconstructHrleRebuildCpu(
      badSource, std::span<const Index<2>>(candidateIndices),
      4U, grid, domain, sourcePointIds, error));
  VC_TEST_ASSERT(!error.empty());
  VC_TEST_ASSERT(collectDefinedPoints(
                     domain, std::span<const Index<2>>(candidateIndices)) ==
                 expected);
  VC_TEST_ASSERT(sourcePointIds == expectedSourcePointIds);

  CompactResult badOffset = validCompact;
  badOffset.definedExclusiveOffsets[1] =
      badOffset.definedExclusiveOffsets[0] + 1U;
  error.clear();
  VC_TEST_ASSERT(!reconstruction::reconstructHrleRebuildCpu(
      badOffset, std::span<const Index<2>>(candidateIndices),
      4U, grid, domain, sourcePointIds, error));
  VC_TEST_ASSERT(!error.empty());
  VC_TEST_ASSERT(collectDefinedPoints(
                     domain, std::span<const Index<2>>(candidateIndices)) ==
                 expected);
  VC_TEST_ASSERT(sourcePointIds == expectedSourcePointIds);

  CompactResult nonBinaryMask = validCompact;
  nonBinaryMask.definedMask[1] = 2U;
  error.clear();
  VC_TEST_ASSERT(!reconstruction::reconstructHrleRebuildCpu(
      nonBinaryMask, std::span<const Index<2>>(candidateIndices), 4U, grid,
      domain, sourcePointIds, error));
  VC_TEST_ASSERT(!error.empty());
  VC_TEST_ASSERT(collectDefinedPoints(
                     domain, std::span<const Index<2>>(candidateIndices)) ==
                 expected);
  VC_TEST_ASSERT(sourcePointIds == expectedSourcePointIds);

  CompactResult invalidAction = validCompact;
  invalidAction.candidateActions[0] = static_cast<Action>(99U);
  error.clear();
  VC_TEST_ASSERT(!reconstruction::reconstructHrleRebuildCpu(
      invalidAction, std::span<const Index<2>>(candidateIndices), 4U, grid,
      domain, sourcePointIds, error));
  VC_TEST_ASSERT(!error.empty());
  VC_TEST_ASSERT(collectDefinedPoints(
                     domain, std::span<const Index<2>>(candidateIndices)) ==
                 expected);
  VC_TEST_ASSERT(sourcePointIds == expectedSourcePointIds);

  CompactResult nonFiniteDefined = validCompact;
  nonFiniteDefined.definedPoints[0].value =
      std::numeric_limits<float>::quiet_NaN();
  error.clear();
  VC_TEST_ASSERT(!reconstruction::reconstructHrleRebuildCpu(
      nonFiniteDefined, std::span<const Index<2>>(candidateIndices), 4U, grid,
      domain, sourcePointIds, error));
  VC_TEST_ASSERT(!error.empty());
  VC_TEST_ASSERT(collectDefinedPoints(
                     domain, std::span<const Index<2>>(candidateIndices)) ==
                 expected);
  VC_TEST_ASSERT(sourcePointIds == expectedSourcePointIds);

  std::vector<Index<2>> unordered = candidateIndices;
  std::swap(unordered[1], unordered[2]);
  error.clear();
  VC_TEST_ASSERT(!reconstruction::reconstructHrleRebuildCpu(
      validCompact, std::span<const Index<2>>(unordered),
      4U, grid, domain, sourcePointIds, error));
  VC_TEST_ASSERT(!error.empty());
  VC_TEST_ASSERT(collectDefinedPoints(
                     domain, std::span<const Index<2>>(candidateIndices)) ==
                 expected);
  VC_TEST_ASSERT(sourcePointIds == expectedSourcePointIds);

  std::vector<Index<2>> outsideGrid = candidateIndices;
  outsideGrid.back() = Index<2>{7, 0};
  error.clear();
  VC_TEST_ASSERT(!reconstruction::reconstructHrleRebuildCpu(
      validCompact, std::span<const Index<2>>(outsideGrid), 4U, grid, domain,
      sourcePointIds, error));
  VC_TEST_ASSERT(!error.empty());
  VC_TEST_ASSERT(collectDefinedPoints(
                     domain, std::span<const Index<2>>(candidateIndices)) ==
                 expected);
  VC_TEST_ASSERT(sourcePointIds == expectedSourcePointIds);

  error.clear();
  VC_TEST_ASSERT(!reconstruction::reconstructHrleRebuildCpu(
      validCompact, std::span<const Index<2>>(candidateIndices), 2U, grid,
      domain, sourcePointIds, error));
  VC_TEST_ASSERT(!error.empty());
  VC_TEST_ASSERT(collectDefinedPoints(
                     domain, std::span<const Index<2>>(candidateIndices)) ==
                 expected);
  VC_TEST_ASSERT(sourcePointIds == expectedSourcePointIds);
}

} // namespace

int main() {
  testReconstructionWithSourceMapping();
  testAllDefinedAndAllUndefinedBranches();
  testThreeDimensionalReconstruction();
  testBoundarySemanticsAreHrleAuthoritative();
  testValidationIsTransactional();
  return 0;
}
