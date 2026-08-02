// Copyright 2026 ViennaPS
// SPDX-License-Identifier: MIT

#include <levelset/psHrleRebuildCompaction.hpp>

#include <vcTestAsserts.hpp>

#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

namespace {

namespace classification = viennaps::levelset;
using Action = classification::HrleRebuildAction;
using Candidate = classification::HrleRebuildDecisionFp32;
using CompactResult = classification::HrleRebuildCompactionResultFp32;
using CompactDecision = classification::HrleRebuildCompactDecisionFp32;

Candidate makeDecision(const float value, const std::uint32_t sourcePointId,
                       const Action action) {
  return {value, sourcePointId, action};
}

void assertDecision(const CompactDecision &decision,
                    const std::uint32_t candidateIndex, const float value,
                    const std::uint32_t sourcePointId, const Action action) {
  VC_TEST_ASSERT(decision.candidateIndex == candidateIndex);
  VC_TEST_ASSERT(decision.sourcePointId == sourcePointId);
  VC_TEST_ASSERT(decision.action == action);
  VC_TEST_ASSERT(std::bit_cast<std::uint32_t>(decision.value) ==
                 std::bit_cast<std::uint32_t>(value));
}

void testMixedActionsPreserveDefinedOrder() {
  const std::vector<Candidate> decisions{
      makeDecision(std::numeric_limits<float>::max(),
                   classification::kInvalidHrlePointId,
                   Action::UNDEFINED_POSITIVE),
      makeDecision(0.25F, 101U, Action::DEFINED),
      makeDecision(-0.75F, 102U, Action::DEFINED),
      makeDecision(std::numeric_limits<float>::lowest(),
                   classification::kInvalidHrlePointId,
                   Action::UNDEFINED_NEGATIVE),
      makeDecision(0.5F, 103U, Action::DEFINED),
  };
  CompactResult output{};
  std::string error;
  VC_TEST_ASSERT(classification::compactHrleRebuildDecisionsCpu(
      std::span<const Candidate>(decisions), output, error));
  VC_TEST_ASSERT(error.empty());
  VC_TEST_ASSERT(
      (output.candidateActions ==
       std::vector<Action>{Action::UNDEFINED_POSITIVE, Action::DEFINED,
                           Action::DEFINED, Action::UNDEFINED_NEGATIVE,
                           Action::DEFINED}));
  VC_TEST_ASSERT(output.definedPoints.size() == 3U);
  VC_TEST_ASSERT(output.definedMask.size() == decisions.size());
  VC_TEST_ASSERT(output.definedExclusiveOffsets.size() ==
                 decisions.size() + 1U);
  assertDecision(output.definedPoints[0], 1U, 0.25F, 101U, Action::DEFINED);
  assertDecision(output.definedPoints[1], 2U, -0.75F, 102U, Action::DEFINED);
  assertDecision(output.definedPoints[2], 4U, 0.5F, 103U, Action::DEFINED);
  VC_TEST_ASSERT(
      (output.definedMask == std::vector<std::uint32_t>{0U, 1U, 1U, 0U, 1U}));
  VC_TEST_ASSERT((output.definedExclusiveOffsets ==
                  std::vector<std::uint32_t>{0U, 0U, 1U, 2U, 2U, 3U}));
}

void testAllDefinedAndAllUndefinedBranches() {
  const std::vector<Candidate> allDefined{
      makeDecision(0.1F, 10U, Action::DEFINED),
      makeDecision(0.2F, 11U, Action::DEFINED),
      makeDecision(-0.3F, 12U, Action::DEFINED),
  };
  CompactResult output{};
  std::string error;
  VC_TEST_ASSERT(classification::compactHrleRebuildDecisionsCpu(
      std::span<const Candidate>(allDefined), output, error));
  VC_TEST_ASSERT(error.empty());
  VC_TEST_ASSERT(
      (output.candidateActions ==
       std::vector<Action>{Action::DEFINED, Action::DEFINED, Action::DEFINED}));
  VC_TEST_ASSERT(output.definedPoints.size() == allDefined.size());
  VC_TEST_ASSERT(
      (output.definedMask == std::vector<std::uint32_t>{1U, 1U, 1U}));
  VC_TEST_ASSERT((output.definedExclusiveOffsets ==
                  std::vector<std::uint32_t>{0U, 1U, 2U, 3U}));

  const std::vector<Candidate> allUndefined{
      makeDecision(std::numeric_limits<float>::max(),
                   classification::kInvalidHrlePointId,
                   Action::UNDEFINED_POSITIVE),
      makeDecision(std::numeric_limits<float>::lowest(),
                   classification::kInvalidHrlePointId,
                   Action::UNDEFINED_NEGATIVE),
      makeDecision(std::numeric_limits<float>::max(),
                   classification::kInvalidHrlePointId,
                   Action::UNDEFINED_POSITIVE),
  };
  VC_TEST_ASSERT(classification::compactHrleRebuildDecisionsCpu(
      std::span<const Candidate>(allUndefined), output, error));
  VC_TEST_ASSERT(error.empty());
  VC_TEST_ASSERT((output.candidateActions ==
                  std::vector<Action>{Action::UNDEFINED_POSITIVE,
                                      Action::UNDEFINED_NEGATIVE,
                                      Action::UNDEFINED_POSITIVE}));
  VC_TEST_ASSERT(output.definedPoints.empty());
  VC_TEST_ASSERT(
      (output.definedMask == std::vector<std::uint32_t>{0U, 0U, 0U}));
  VC_TEST_ASSERT((output.definedExclusiveOffsets ==
                  std::vector<std::uint32_t>{0U, 0U, 0U, 0U}));
}

void testEmptyInput() {
  CompactResult output{};
  std::string error;
  VC_TEST_ASSERT(
      classification::compactHrleRebuildDecisionsCpu({}, output, error));
  VC_TEST_ASSERT(error.empty());
  VC_TEST_ASSERT(output.candidateActions.empty());
  VC_TEST_ASSERT(output.definedPoints.empty());
  VC_TEST_ASSERT(output.definedMask.empty());
  VC_TEST_ASSERT(
      (output.definedExclusiveOffsets == std::vector<std::uint32_t>{0U}));
}

void testValidationIsTransactional() {
  const CompactResult original{{Action::DEFINED},
                               {CompactDecision{5U, 0.5F, 7U, Action::DEFINED}},
                               {1U},
                               {0U, 1U}};
  CompactResult output = original;
  std::string error;

  const auto badActionDecision = Candidate{0.0F, 1U, static_cast<Action>(13U)};
  VC_TEST_ASSERT(!classification::compactHrleRebuildDecisionsCpu(
      std::span<const Candidate>(&badActionDecision, 1U), output, error));
  VC_TEST_ASSERT(output == original);
  VC_TEST_ASSERT(!error.empty());

  error.clear();
  output = original;
  const auto badSourceDecision =
      Candidate{1.0F, classification::kInvalidHrlePointId, Action::DEFINED};
  VC_TEST_ASSERT(!classification::compactHrleRebuildDecisionsCpu(
      std::span<const Candidate>(&badSourceDecision, 1U), output, error));
  VC_TEST_ASSERT(output == original);
  VC_TEST_ASSERT(!error.empty());

  error.clear();
  output = original;
  const auto badValueDecision =
      Candidate{std::numeric_limits<float>::quiet_NaN(), 1U, Action::DEFINED};
  VC_TEST_ASSERT(!classification::compactHrleRebuildDecisionsCpu(
      std::span<const Candidate>(&badValueDecision, 1U), output, error));
  VC_TEST_ASSERT(output == original);
  VC_TEST_ASSERT(!error.empty());
}

} // namespace

int main() {
  testMixedActionsPreserveDefinedOrder();
  testAllDefinedAndAllUndefinedBranches();
  testEmptyInput();
  testValidationIsTransactional();
  return 0;
}
