// Copyright 2026 ViennaPS
// SPDX-License-Identifier: MIT

#include <levelset/psHrleRebuildClassification.hpp>

#include <vcTestAsserts.hpp>

#include <bit>
#include <cmath>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

namespace {

namespace classification = viennaps::levelset;
using Action = classification::HrleRebuildAction;
using Candidate = classification::HrleRebuildCandidateFp32;
using Decision = classification::HrleRebuildDecisionFp32;

[[nodiscard]] Candidate makeCandidate(const float centerValue,
                                      const float centerDefinedValue,
                                      const std::uint32_t centerPointId) {
  Candidate candidate{};
  candidate.centerValue = centerValue;
  candidate.centerDefinedValue = centerDefinedValue;
  candidate.centerPointId = centerPointId;
  candidate.neighborValues.fill(std::numeric_limits<float>::max());
  candidate.neighborDefinedValues.fill(std::numeric_limits<float>::max());
  for (std::size_t index = 0U; index < candidate.neighborPointIds.size();
       ++index)
    candidate.neighborPointIds[index] =
        static_cast<std::uint32_t>(200U + index);
  return candidate;
}

void assertDecision(const Decision &decision, const float value,
                    const std::uint32_t sourcePointId, const Action action) {
  VC_TEST_ASSERT(decision.action == action);
  VC_TEST_ASSERT(decision.sourcePointId == sourcePointId);
  VC_TEST_ASSERT(std::bit_cast<std::uint32_t>(decision.value) ==
                 std::bit_cast<std::uint32_t>(value));
}

[[nodiscard]] Decision classifyOne(const Candidate &candidate,
                                   const std::uint32_t dimensions = 2U) {
  std::vector<Decision> output;
  std::string error;
  VC_TEST_ASSERT(classification::classifyHrleRebuildCpu(
      std::span<const Candidate>(&candidate, 1U), dimensions, 1.0F, output,
      error));
  VC_TEST_ASSERT(error.empty());
  VC_TEST_ASSERT(output.size() == 1U);
  return output.front();
}

void testActiveSurfaceBranches() {
  auto center = makeCandidate(0.25F, 0.25F, 100U);
  center.neighborValues[0] = -0.25F;
  center.neighborDefinedValues[0] = -0.25F;
  assertDecision(classifyOne(center), 0.25F, 100U, Action::DEFINED);

  auto positiveClamp = makeCandidate(0.75F, 0.75F, 101U);
  positiveClamp.neighborValues[0] = -0.75F;
  positiveClamp.neighborDefinedValues[0] = -0.75F;
  assertDecision(classifyOne(positiveClamp), 0.5F, 200U, Action::DEFINED);

  auto negativeClamp = makeCandidate(-0.75F, -0.75F, 102U);
  negativeClamp.neighborValues[0] = 0.75F;
  negativeClamp.neighborDefinedValues[0] = 0.75F;
  assertDecision(classifyOne(negativeClamp), -0.5F, 200U, Action::DEFINED);

  auto positiveNoCrossing = makeCandidate(0.25F, 0.25F, 103U);
  positiveNoCrossing.neighborValues.fill(0.75F);
  positiveNoCrossing.neighborDefinedValues.fill(0.75F);
  assertDecision(
      classifyOne(positiveNoCrossing), std::numeric_limits<float>::max(),
      classification::kInvalidHrlePointId, Action::UNDEFINED_POSITIVE);

  auto negativeNoCrossing = makeCandidate(-0.25F, -0.25F, 104U);
  negativeNoCrossing.neighborValues.fill(-0.75F);
  negativeNoCrossing.neighborDefinedValues.fill(-0.75F);
  assertDecision(
      classifyOne(negativeNoCrossing), std::numeric_limits<float>::lowest(),
      classification::kInvalidHrlePointId, Action::UNDEFINED_NEGATIVE);
}

void testInactiveBranches() {
  auto positive = makeCandidate(2.0F, 2.0F, 110U);
  positive.neighborValues[0] = -0.25F;
  positive.neighborDefinedValues[0] = -0.25F;
  assertDecision(classifyOne(positive), 0.75F, 200U, Action::DEFINED);

  const auto positiveUndefined = makeCandidate(2.0F, 2.0F, 111U);
  assertDecision(
      classifyOne(positiveUndefined), std::numeric_limits<float>::max(),
      classification::kInvalidHrlePointId, Action::UNDEFINED_POSITIVE);

  auto negative = makeCandidate(-2.0F, -2.0F, 112U);
  negative.neighborValues[0] = 0.25F;
  negative.neighborDefinedValues[0] = 0.25F;
  assertDecision(classifyOne(negative), -0.75F, 200U, Action::DEFINED);

  auto negativeUndefined = makeCandidate(-2.0F, -2.0F, 113U);
  negativeUndefined.neighborValues.fill(-2.0F);
  negativeUndefined.neighborDefinedValues.fill(-2.0F);
  assertDecision(
      classifyOne(negativeUndefined), std::numeric_limits<float>::lowest(),
      classification::kInvalidHrlePointId, Action::UNDEFINED_NEGATIVE);
}

void testDimensionAndOrderingContracts() {
  auto twoDimensional = makeCandidate(0.25F, 0.25F, 120U);
  twoDimensional.neighborValues.fill(0.75F);
  twoDimensional.neighborDefinedValues.fill(0.75F);
  twoDimensional.neighborValues[4] = std::numeric_limits<float>::quiet_NaN();
  twoDimensional.neighborDefinedValues[5] =
      std::numeric_limits<float>::quiet_NaN();
  assertDecision(
      classifyOne(twoDimensional, 2U), std::numeric_limits<float>::max(),
      classification::kInvalidHrlePointId, Action::UNDEFINED_POSITIVE);

  auto threeDimensional = makeCandidate(0.25F, 0.25F, 121U);
  threeDimensional.neighborValues.fill(0.75F);
  threeDimensional.neighborDefinedValues.fill(0.75F);
  threeDimensional.neighborValues[5] = -0.25F;
  threeDimensional.neighborDefinedValues[5] = -0.25F;
  assertDecision(classifyOne(threeDimensional, 3U), 0.25F, 121U,
                 Action::DEFINED);

  auto batchFirst = makeCandidate(0.25F, 0.25F, 130U);
  batchFirst.neighborValues[0] = -0.25F;
  batchFirst.neighborDefinedValues[0] = -0.25F;
  auto batchSecond = makeCandidate(2.0F, 2.0F, 131U);
  batchSecond.neighborValues[0] = -0.25F;
  batchSecond.neighborDefinedValues[0] = -0.25F;
  const std::vector<Candidate> batch = {batchFirst, batchSecond};
  std::vector<Decision> output;
  std::string error;
  VC_TEST_ASSERT(classification::classifyHrleRebuildCpu(
      std::span<const Candidate>(batch), 2U, 1.0F, output, error));
  VC_TEST_ASSERT(output.size() == 2U);
  assertDecision(output[0], 0.25F, 130U, Action::DEFINED);
  assertDecision(output[1], 0.75F, 200U, Action::DEFINED);
}

void testValidationIsTransactional() {
  Decision sentinel{};
  sentinel.value = 7.0F;
  sentinel.sourcePointId = 9U;
  sentinel.action = Action::DEFINED;
  const std::vector<Decision> original = {sentinel};
  std::vector<Decision> output = original;
  std::string error;
  const auto candidate = makeCandidate(0.0F, 0.0F, 1U);

  VC_TEST_ASSERT(!classification::classifyHrleRebuildCpu(
      std::span<const Candidate>(&candidate, 1U), 1U, 1.0F, output, error));
  VC_TEST_ASSERT(output == original);
  VC_TEST_ASSERT(!error.empty());

  error.clear();
  VC_TEST_ASSERT(!classification::classifyHrleRebuildCpu(
      std::span<const Candidate>(&candidate, 1U), 2U,
      std::numeric_limits<float>::infinity(), output, error));
  VC_TEST_ASSERT(output == original);
  VC_TEST_ASSERT(!error.empty());

  error.clear();
  VC_TEST_ASSERT(
      classification::classifyHrleRebuildCpu({}, 2U, 1.0F, output, error));
  VC_TEST_ASSERT(output.empty());
}

} // namespace

int main() {
  testActiveSurfaceBranches();
  testInactiveBranches();
  testDimensionAndOrderingContracts();
  testValidationIsTransactional();
  return 0;
}
