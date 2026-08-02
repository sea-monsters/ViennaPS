// Copyright 2026 ViennaPS
// SPDX-License-Identifier: MIT

#include <geometries/psMakePlane.hpp>

#include <lsAdvect.hpp>

#include <vcTestAsserts.hpp>

#include <cmath>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using NumericType = float;
constexpr int kDimension = 3;
using Advect = viennals::Advect<NumericType, kDimension>;
using LevelSet = viennals::Domain<NumericType, kDimension>;

class ConstantVelocityField final : public viennals::VelocityField<NumericType> {
public:
  NumericType getScalarVelocity(const viennacore::Vec3D<NumericType> &, int,
                                const viennacore::Vec3D<NumericType> &,
                                unsigned long) override {
    return -0.1F;
  }
};

struct PointDataSnapshot {
  std::vector<std::vector<NumericType>> scalars;
  std::vector<std::vector<viennacore::Vec3D<NumericType>>> vectors;
};

[[nodiscard]] PointDataSnapshot snapshotPointData(const LevelSet &levelSet) {
  PointDataSnapshot result;
  const auto &pointData = levelSet.getPointData();
  for (unsigned i = 0; i < pointData.getScalarDataSize(); ++i)
    result.scalars.push_back(*pointData.getScalarData(i));
  for (unsigned i = 0; i < pointData.getVectorDataSize(); ++i)
    result.vectors.push_back(*pointData.getVectorData(i));
  return result;
}

[[nodiscard]] PointDataSnapshot
selectPointData(const PointDataSnapshot &source,
                const std::vector<std::vector<unsigned>> &sourceIds) {
  PointDataSnapshot result;
  for (const auto &sourceScalars : source.scalars) {
    auto &selected = result.scalars.emplace_back();
    for (const auto &segmentIds : sourceIds)
      for (const auto sourceId : segmentIds) {
        VC_TEST_ASSERT(sourceId < sourceScalars.size());
        selected.push_back(sourceScalars[sourceId]);
      }
  }
  for (const auto &sourceVectors : source.vectors) {
    auto &selected = result.vectors.emplace_back();
    for (const auto &segmentIds : sourceIds)
      for (const auto sourceId : segmentIds) {
        VC_TEST_ASSERT(sourceId < sourceVectors.size());
        selected.push_back(sourceVectors[sourceId]);
      }
  }
  return result;
}

void assertPointDataEqual(const PointDataSnapshot &expected,
                          const PointDataSnapshot &actual) {
  VC_TEST_ASSERT(expected.scalars == actual.scalars);
  VC_TEST_ASSERT(expected.vectors.size() == actual.vectors.size());
  for (unsigned data = 0; data < expected.vectors.size(); ++data) {
    VC_TEST_ASSERT(expected.vectors[data].size() ==
                   actual.vectors[data].size());
    for (unsigned point = 0; point < expected.vectors[data].size(); ++point)
      for (unsigned component = 0; component < 3U; ++component)
        VC_TEST_ASSERT(expected.vectors[data][point][component] ==
                       actual.vectors[data][point][component]);
  }
}

void seedPointData(LevelSet &levelSet) {
  const unsigned pointCount = levelSet.getNumberOfPoints();
  std::vector<NumericType> scalar(pointCount);
  std::vector<viennacore::Vec3D<NumericType>> vector(pointCount);
  for (unsigned point = 0; point < pointCount; ++point) {
    scalar[point] = static_cast<NumericType>(point) + 0.25F;
    vector[point] = {static_cast<NumericType>(point),
                     static_cast<NumericType>(point * 2U) + 0.5F,
                     -static_cast<NumericType>(point)};
  }
  levelSet.getPointData().insertNextScalarData(std::move(scalar), "scalar");
  levelSet.getPointData().insertNextVectorData(std::move(vector), "vector");
}

struct HandledResult {
  PointDataSnapshot expectedPointData;
  PointDataSnapshot actualPointData;
  std::vector<std::vector<NumericType>> replacementValues;
  std::vector<std::vector<NumericType>> actualValues;
  std::vector<std::vector<unsigned>> sourceIds;
  unsigned executorCalls = 0U;
  unsigned initialSegments = 0U;
};

[[nodiscard]] HandledResult runHandled(const bool updatePointData,
                                       const bool requireMultipleSegments) {
  auto domain = viennaps::Domain<NumericType, kDimension>::New(1.0, 8.0, 8.0);
  viennaps::MakePlane<NumericType, kDimension>(domain).apply();
  auto levelSet = domain->getSurface();
  levelSet->getDomain().segment();
  const unsigned initialSegments = levelSet->getNumberOfSegments();
  VC_TEST_ASSERT(initialSegments > 0U);
  if (requireMultipleSegments)
    VC_TEST_ASSERT(initialSegments > 1U);
  seedPointData(*levelSet);

  Advect advect;
  advect.insertNextLevelSet(levelSet);
  advect.setVelocityField(
      viennacore::SmartPointer<ConstantVelocityField>::New());
  advect.setSpatialScheme(
      viennals::SpatialSchemeEnum::ENGQUIST_OSHER_1ST_ORDER);
  advect.setTemporalScheme(viennals::TemporalSchemeEnum::FORWARD_EULER);
  advect.setAdvectionTime(0.05);
  advect.setTimeStepRatio(0.4999);
  advect.setSingleStep(true);
  advect.setUpdatePointData(updatePointData);

  HandledResult result;
  result.initialSegments = initialSegments;
  advect.setLevelSetRebuildExecutor(
      [&result, levelSet](
          const Advect::LevelSetRebuildContext &context,
          Advect::LevelSetRebuildOutput &output, std::string &) {
        ++result.executorCalls;
        const auto sourcePointData = snapshotPointData(*levelSet);
        output.domain = viennacore::SmartPointer<LevelSet>::New(
            context.domain.getGrid());
        output.domain->getDomain().deepCopy(&output.domain->getGrid(),
                                             context.domain);
        auto &replacement = output.domain->getDomain();
        result.replacementValues.clear();
        result.replacementValues.reserve(replacement.getNumberOfSegments());
        if (context.updatePointData)
          output.sourceIds.resize(replacement.getNumberOfSegments());

        for (unsigned segment = 0; segment < replacement.getNumberOfSegments();
             ++segment) {
          auto &replacementSegment = replacement.getDomainSegment(segment);
          auto &values = result.replacementValues.emplace_back();
          values.reserve(replacementSegment.definedValues.size());
          if (context.updatePointData)
            output.sourceIds[segment].reserve(
                replacementSegment.definedValues.size());
          const unsigned sourceOffset = context.domain.getPointIdOffset(segment);
          for (unsigned local = 0;
               local < replacementSegment.definedValues.size(); ++local) {
            replacementSegment.definedValues[local] += 0.125F;
            values.push_back(replacementSegment.definedValues[local]);
            if (context.updatePointData)
              output.sourceIds[segment].push_back(sourceOffset + local);
          }
        }
        if (context.updatePointData) {
          result.sourceIds = output.sourceIds;
          result.expectedPointData =
              selectPointData(sourcePointData, result.sourceIds);
        }
        return Advect::LevelSetRebuildStatus::HANDLED;
      });
  advect.apply();

  const auto &finalDomain = levelSet->getDomain();
  result.actualValues.reserve(finalDomain.getNumberOfSegments());
  for (unsigned segment = 0; segment < finalDomain.getNumberOfSegments();
       ++segment)
    result.actualValues.push_back(
        finalDomain.getDomainSegment(segment).definedValues);
  result.actualPointData = snapshotPointData(*levelSet);
  return result;
}

void check3dHandled() {
  const auto result = runHandled(true, false);
  VC_TEST_ASSERT(result.executorCalls == 1U);
  VC_TEST_ASSERT(result.initialSegments > 0U);
  VC_TEST_ASSERT(result.actualValues == result.replacementValues);
  assertPointDataEqual(result.expectedPointData, result.actualPointData);
}

void checkMultiSegmentHandled() {
  const auto result = runHandled(true, true);
  VC_TEST_ASSERT(result.executorCalls == 1U);
  VC_TEST_ASSERT(result.initialSegments > 1U);
  VC_TEST_ASSERT(result.actualValues == result.replacementValues);
  assertPointDataEqual(result.expectedPointData, result.actualPointData);
}

void checkHandledWithoutPointData() {
  const auto result = runHandled(false, true);
  VC_TEST_ASSERT(result.executorCalls == 1U);
  VC_TEST_ASSERT(result.initialSegments > 1U);
  VC_TEST_ASSERT(result.actualValues == result.replacementValues);
  VC_TEST_ASSERT(result.actualPointData.scalars.empty());
  VC_TEST_ASSERT(result.actualPointData.vectors.empty());
}

} // namespace

int main(const int argc, const char *const argv[]) {
  viennacore::Logger::setLogLevel(viennacore::LogLevel::WARNING);
  const std::string_view scenario = argc > 1 ? argv[1] : "3d";
  if (scenario == "3d") {
    check3dHandled();
  } else if (scenario == "multi-segment") {
    checkMultiSegmentHandled();
    checkHandledWithoutPointData();
  } else {
    return 2;
  }
  return 0;
}
