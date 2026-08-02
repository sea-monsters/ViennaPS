// Copyright 2026 ViennaPS
// SPDX-License-Identifier: MIT

#include "viennals_rebuild_executor.hpp"

#include <hrleSparseIterator.hpp>
#include <lsExpand.hpp>
#include <lsMakeGeometry.hpp>

#include <vcTestAsserts.hpp>

#include <bit>
#include <cstdint>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

namespace {

namespace ls = viennals;
namespace runtime = viennaps::vulkan::runtime;
namespace primitives = viennaps::vulkan::primitives;
namespace vkLevelSet = viennaps::vulkan::levelset;

template <int D>
[[nodiscard]] ls::SmartPointer<ls::Domain<float, D>> makeDomain() {
  constexpr viennahrle::CoordType extent = 8.0;
  constexpr viennahrle::CoordType gridDelta = D == 2 ? 0.5 : 1.0;
  viennahrle::CoordType bounds[2 * D]{};
  typename ls::Domain<float, D>::BoundaryType boundaryConditions[D]{};
  for (int dimension = 0; dimension < D; ++dimension) {
    bounds[2 * dimension] = -extent;
    bounds[2 * dimension + 1] = extent;
    boundaryConditions[dimension] =
        ls::BoundaryConditionEnum::REFLECTIVE_BOUNDARY;
  }
  auto domain =
      ls::Domain<float, D>::New(bounds, boundaryConditions, gridDelta);
  float origin[D]{};
  ls::MakeGeometry<float, D>(
      domain,
      ls::SmartPointer<ls::Sphere<float, D>>::New(origin, D == 2 ? 3.0F : 2.0F))
      .apply();
  ls::Expand<float, D>(domain, D == 2 ? 2 : 1).apply();
  return domain;
}

class ConstantVelocity final : public ls::VelocityField<float> {
public:
  float getScalarVelocity(const viennacore::Vec3D<float> &, int,
                          const viennacore::Vec3D<float> &,
                          unsigned long) override {
    return -0.1F;
  }
};

template <int D> void seedPointData(ls::Domain<float, D> &domain) {
  const auto count = domain.getNumberOfPoints();
  std::vector<float> scalars(count);
  std::vector<viennacore::Vec3D<float>> vectors(count);
  for (unsigned i = 0U; i < count; ++i) {
    scalars[i] = static_cast<float>(i) + 0.25F;
    vectors[i] = {static_cast<float>(i), static_cast<float>(2U * i) + 0.5F,
                  -static_cast<float>(i)};
  }
  domain.getPointData().insertNextScalarData(std::move(scalars), "scalar");
  domain.getPointData().insertNextVectorData(std::move(vectors), "vector");
}

template <int D>
void assertDomainsEqual(const ls::Domain<float, D> &left,
                        const ls::Domain<float, D> &right) {
  const auto &a = left.getDomain();
  const auto &b = right.getDomain();
  VC_TEST_ASSERT(a.getNumberOfSegments() == b.getNumberOfSegments());
  VC_TEST_ASSERT(a.getSegmentation() == b.getSegmentation());
  for (unsigned segment = 0U; segment < a.getNumberOfSegments(); ++segment) {
    const auto &as = a.getDomainSegment(segment);
    const auto &bs = b.getDomainSegment(segment);
    VC_TEST_ASSERT(as.definedValues.size() == bs.definedValues.size());
    VC_TEST_ASSERT(as.undefinedValues.size() == bs.undefinedValues.size());
    for (std::size_t i = 0U; i < as.definedValues.size(); ++i)
      VC_TEST_ASSERT(std::bit_cast<std::uint32_t>(as.definedValues[i]) ==
                     std::bit_cast<std::uint32_t>(bs.definedValues[i]));
    for (std::size_t i = 0U; i < as.undefinedValues.size(); ++i)
      VC_TEST_ASSERT(std::bit_cast<std::uint32_t>(as.undefinedValues[i]) ==
                     std::bit_cast<std::uint32_t>(bs.undefinedValues[i]));
    for (unsigned dimension = 0U; dimension < D; ++dimension) {
      VC_TEST_ASSERT(as.runTypes[dimension] == bs.runTypes[dimension]);
      VC_TEST_ASSERT(as.startIndices[dimension] == bs.startIndices[dimension]);
      VC_TEST_ASSERT(as.runBreaks[dimension] == bs.runBreaks[dimension]);
    }
  }
}

template <int D>
void assertPointDataEqual(const ls::Domain<float, D> &left,
                          const ls::Domain<float, D> &right) {
  const auto &a = left.getPointData();
  const auto &b = right.getPointData();
  VC_TEST_ASSERT(a.getScalarDataSize() == b.getScalarDataSize());
  VC_TEST_ASSERT(a.getVectorDataSize() == b.getVectorDataSize());
  for (unsigned i = 0U; i < a.getScalarDataSize(); ++i)
    VC_TEST_ASSERT(*a.getScalarData(i) == *b.getScalarData(i));
  for (unsigned i = 0U; i < a.getVectorDataSize(); ++i) {
    const auto &leftValues = *a.getVectorData(i);
    const auto &rightValues = *b.getVectorData(i);
    VC_TEST_ASSERT(leftValues.size() == rightValues.size());
    for (std::size_t p = 0U; p < leftValues.size(); ++p)
      for (unsigned component = 0U; component < 3U; ++component)
        VC_TEST_ASSERT(leftValues[p][component] == rightValues[p][component]);
  }
}

template <int D>
unsigned runDifferential(
    const std::shared_ptr<vkLevelSet::ViennaLsRebuildExecutorStateFp32>
        &state) {
  auto cpu = makeDomain<D>();
  auto vulkan = makeDomain<D>();
  seedPointData(*cpu);
  seedPointData(*vulkan);

  ls::Advect<float, D> cpuAdvect;
  cpuAdvect.insertNextLevelSet(cpu);
  cpuAdvect.setVelocityField(ls::SmartPointer<ConstantVelocity>::New());
  cpuAdvect.setSpatialScheme(ls::SpatialSchemeEnum::ENGQUIST_OSHER_1ST_ORDER);
  cpuAdvect.setTemporalScheme(ls::TemporalSchemeEnum::FORWARD_EULER);
  cpuAdvect.setAdvectionTime(0.05);
  cpuAdvect.setTimeStepRatio(0.4999);
  cpuAdvect.setSingleStep(true);
  cpuAdvect.setUpdatePointData(true);

  ls::Advect<float, D> vulkanAdvect;
  vulkanAdvect.insertNextLevelSet(vulkan);
  vulkanAdvect.setVelocityField(ls::SmartPointer<ConstantVelocity>::New());
  vulkanAdvect.setSpatialScheme(
      ls::SpatialSchemeEnum::ENGQUIST_OSHER_1ST_ORDER);
  vulkanAdvect.setTemporalScheme(ls::TemporalSchemeEnum::FORWARD_EULER);
  vulkanAdvect.setAdvectionTime(0.05);
  vulkanAdvect.setTimeStepRatio(0.4999);
  vulkanAdvect.setSingleStep(true);
  vulkanAdvect.setUpdatePointData(true);

  unsigned rebuildCalls = 0U;
  bool callbackHandled = false;
  auto executor = vkLevelSet::makeViennaLsRebuildExecutorFp32<D>(state);
  vulkanAdvect.setLevelSetRebuildExecutor(
      [executor = std::move(executor), &rebuildCalls, &callbackHandled](
          const typename ls::Advect<float, D>::LevelSetRebuildContext &context,
          typename ls::Advect<float, D>::LevelSetRebuildOutput &output,
          std::string &error) {
        ++rebuildCalls;
        const auto status = executor(context, output, error);
        callbackHandled =
            status == ls::Advect<float, D>::LevelSetRebuildStatus::HANDLED;
        return status;
      });

  cpuAdvect.apply();
  vulkanAdvect.apply();
  VC_TEST_ASSERT(!cpuAdvect.hasLevelSetRebuildError());
  VC_TEST_ASSERT(!vulkanAdvect.hasLevelSetRebuildError());
  VC_TEST_ASSERT(rebuildCalls > 0U);
  VC_TEST_ASSERT(callbackHandled);
  assertDomainsEqual(*cpu, *vulkan);
  assertPointDataEqual(*cpu, *vulkan);
  return rebuildCalls;
}

} // namespace

// Exercises the complete segmented callback with a small CPU differential
// oracle.
int main() try {
  std::string error;
  auto session = std::make_shared<runtime::ComputeSession>();
  VC_TEST_ASSERT(session->initialize(error));
  auto primitivesState =
      std::make_shared<primitives::ReductionScanPrimitives>();
  VC_TEST_ASSERT(primitivesState->initialize(
      *session, VIENNAPS_REDUCTION_SCAN_SPV_PATH, error));
  auto loadProgram = [&](const char *path) {
    auto program = std::make_shared<runtime::SpirvProgram>();
    VC_TEST_ASSERT(runtime::readSpirv(path, *program, error));
    return std::shared_ptr<const runtime::SpirvProgram>(std::move(program));
  };
  auto state = std::make_shared<vkLevelSet::ViennaLsRebuildExecutorStateFp32>();
  state->session = session;
  state->primitives = primitivesState;
  state->classificationProgram =
      loadProgram(VIENNAPS_HRLE_CLASSIFICATION_SPV_PATH);
  state->actionFlagsProgram = loadProgram(VIENNAPS_HRLE_ACTION_FLAGS_SPV_PATH);
  state->compactProgram = loadProgram(VIENNAPS_HRLE_COMPACT_SPV_PATH);

  const auto twoDimensionalRebuildCalls = runDifferential<2>(state);
  const auto threeDimensionalRebuildCalls = runDifferential<3>(state);
  VC_TEST_ASSERT(twoDimensionalRebuildCalls > 0U);
  VC_TEST_ASSERT(threeDimensionalRebuildCalls > 0U);

  auto sentinel = ls::SmartPointer<ls::Domain<float, 2>>::New(makeDomain<2>());
  ls::Advect<float, 2>::LevelSetRebuildOutput output;
  output.domain = sentinel;
  output.sourceIds = {{7U}};
  const auto beforeIds = output.sourceIds;
  auto badState =
      std::make_shared<vkLevelSet::ViennaLsRebuildExecutorStateFp32>(*state);
  badState->classificationProgram = std::make_shared<runtime::SpirvProgram>();
  auto badExecutor = vkLevelSet::makeViennaLsRebuildExecutorFp32<2>(badState);
  const ls::Advect<float, 2>::LevelSetRebuildContext badContext{
      sentinel->getDomain(), 1.0F, 2, true, false};
  const auto status = badExecutor(badContext, output, error);
  using RebuildStatus = ls::Advect<float, 2>::LevelSetRebuildStatus;
  VC_TEST_ASSERT(status == RebuildStatus::ERROR);
  VC_TEST_ASSERT(output.domain == sentinel);
  VC_TEST_ASSERT(output.sourceIds == beforeIds);
  return 0;
} catch (const std::exception &exception) {
  std::cerr << exception.what() << '\n';
  return 1;
}
