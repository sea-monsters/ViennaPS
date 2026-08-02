#include <geometries/psMakePlane.hpp>
#include <process/psAdvectionHandler.hpp>
#include <process/psProcess.hpp>

#include <vcTestAsserts.hpp>

#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

constexpr int kDimension = 2;
using NumericType = float;
using Advect = viennals::Advect<NumericType, kDimension>;
using LevelSetDomain = viennals::Domain<NumericType, kDimension>;
using FailurePolicy = viennaps::LevelSetUpdateFailurePolicy;

enum class ExecutorMode { NONE, FALLBACK, ERROR, THROW, INVALID };
enum class RebuildMode { NONE, FALLBACK, ERROR, THROW, INVALID };

class ConstantVelocityField final
    : public viennaps::VelocityField<NumericType, kDimension> {
public:
  NumericType getScalarVelocity(const viennaps::Vec3D<NumericType> &, int,
                                const viennaps::Vec3D<NumericType> &,
                                unsigned long) override {
    return -0.1F;
  }
};

class LsConstantVelocityField final
    : public viennals::VelocityField<NumericType> {
public:
  NumericType getScalarVelocity(const viennacore::Vec3D<NumericType> &, int,
                                const viennacore::Vec3D<NumericType> &,
                                unsigned long) override {
    return -0.1F;
  }
};

class AnalyticModel final
    : public viennaps::ProcessModelBase<NumericType, kDimension> {
public:
  AnalyticModel() {
    this->setVelocityField(
        viennacore::SmartPointer<ConstantVelocityField>::New());
    this->setSurfaceModel(
        viennacore::SmartPointer<viennaps::SurfaceModel<NumericType>>::New());
  }
};

[[nodiscard]] auto makeDomain() {
  auto domain = viennaps::Domain<NumericType, kDimension>::New(1.0, 2.0, 2.0);
  viennaps::MakePlane<NumericType, kDimension>(domain).apply();
  return domain;
}

[[nodiscard]] std::vector<NumericType> snapshot(
    const viennacore::SmartPointer<viennaps::Domain<NumericType, kDimension>>
        &domain) {
  std::vector<NumericType> values;
  const auto &sparseDomain = domain->getSurface()->getDomain();
  for (unsigned segment = 0U; segment < sparseDomain.getNumberOfSegments();
       ++segment) {
    const auto &segmentValues =
        sparseDomain.getDomainSegment(segment).definedValues;
    values.insert(values.end(), segmentValues.begin(), segmentValues.end());
  }
  return values;
}

struct RunResult {
  std::vector<NumericType> beforeValues;
  std::vector<NumericType> afterValues;
  unsigned executorCalls = 0U;
  viennaps::ProcessResult result = viennaps::ProcessResult::FAILURE;
  double processTime = 0.0;
  unsigned advectionSteps = 0U;
};

struct PointDataSnapshot {
  std::vector<std::vector<NumericType>> scalars;
  std::vector<std::vector<viennacore::Vec3D<NumericType>>> vectors;
};

[[nodiscard]] PointDataSnapshot
snapshotPointData(const LevelSetDomain &domain) {
  const auto &pointData = domain.getPointData();
  PointDataSnapshot result;
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
    auto &selectedScalars = result.scalars.emplace_back();
    for (const auto &segmentIds : sourceIds)
      for (const auto sourceId : segmentIds) {
        VC_TEST_ASSERT(sourceId < sourceScalars.size());
        selectedScalars.push_back(sourceScalars[sourceId]);
      }
  }
  for (const auto &sourceVectors : source.vectors) {
    auto &selectedVectors = result.vectors.emplace_back();
    for (const auto &segmentIds : sourceIds)
      for (const auto sourceId : segmentIds) {
        VC_TEST_ASSERT(sourceId < sourceVectors.size());
        selectedVectors.push_back(sourceVectors[sourceId]);
      }
  }
  return result;
}

void seedPointData(LevelSetDomain &domain) {
  const unsigned pointCount = domain.getNumberOfPoints();
  std::vector<NumericType> scalars(pointCount);
  std::vector<viennacore::Vec3D<NumericType>> vectors(pointCount);
  for (unsigned i = 0; i < pointCount; ++i) {
    scalars[i] = static_cast<NumericType>(i) + 0.25F;
    vectors[i] = {static_cast<NumericType>(i),
                  static_cast<NumericType>(2U * i) + 0.5F,
                  static_cast<NumericType>(-static_cast<int>(i))};
  }
  domain.getPointData().insertNextScalarData(std::move(scalars), "ScalarProbe");
  domain.getPointData().insertNextVectorData(std::move(vectors), "VectorProbe");
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

struct HandledResult {
  PointDataSnapshot expectedPointData;
  PointDataSnapshot afterPointData;
  std::vector<std::vector<NumericType>> replacementValues;
  std::vector<std::vector<NumericType>> finalValues;
  std::vector<std::vector<unsigned>> sourceIds;
  unsigned executorCalls = 0U;
  unsigned replacementSegments = 0U;
  unsigned replacementPoints = 0U;
};

[[nodiscard]] HandledResult runHandledReplacement(const bool updatePointData) {
  auto domain = makeDomain();
  auto levelSet = domain->getSurface();
  seedPointData(*levelSet);

  Advect advect;
  advect.insertNextLevelSet(levelSet);
  advect.setVelocityField(
      viennacore::SmartPointer<LsConstantVelocityField>::New());
  advect.setSpatialScheme(
      viennals::SpatialSchemeEnum::ENGQUIST_OSHER_1ST_ORDER);
  advect.setTemporalScheme(viennals::TemporalSchemeEnum::FORWARD_EULER);
  advect.setAdvectionTime(0.05);
  advect.setTimeStepRatio(0.4999);
  advect.setSingleStep(true);
  advect.setUpdatePointData(updatePointData);

  HandledResult result;
  advect.setLevelSetRebuildExecutor(
      [&result, levelSet](const Advect::LevelSetRebuildContext &context,
                          Advect::LevelSetRebuildOutput &output,
                          std::string &) {
        ++result.executorCalls;
        if (result.executorCalls > 1U)
          return Advect::LevelSetRebuildStatus::FALLBACK;
        output.domain = viennacore::SmartPointer<LevelSetDomain>::New(
            context.domain.getGrid());
        output.domain->getDomain().deepCopy(&output.domain->getGrid(),
                                            context.domain);
        const auto &oldDomain = context.domain;
        auto &replacementDomain = output.domain->getDomain();
        result.replacementSegments = replacementDomain.getNumberOfSegments();
        result.replacementPoints = replacementDomain.getNumberOfPoints();
        const auto sourcePointData = snapshotPointData(*levelSet);
        result.replacementValues.clear();
        result.replacementValues.reserve(result.replacementSegments);
        if (context.updatePointData)
          output.sourceIds.resize(result.replacementSegments);
        for (unsigned segment = 0; segment < result.replacementSegments;
             ++segment) {
          auto &replacementSegment =
              replacementDomain.getDomainSegment(segment);
          auto &values = result.replacementValues.emplace_back();
          values.reserve(replacementSegment.definedValues.size());
          if (context.updatePointData)
            output.sourceIds[segment].reserve(
                replacementSegment.definedValues.size());
          const unsigned sourceOffset = oldDomain.getPointIdOffset(segment);
          for (unsigned local = 0;
               local < replacementSegment.definedValues.size(); ++local) {
            replacementSegment.definedValues[local] += 0.125F;
            values.push_back(replacementSegment.definedValues[local]);
            if (context.updatePointData)
              output.sourceIds[segment].push_back(sourceOffset + local);
          }
        }
        result.sourceIds = output.sourceIds;
        if (context.updatePointData)
          result.expectedPointData =
              selectPointData(sourcePointData, result.sourceIds);
        return Advect::LevelSetRebuildStatus::HANDLED;
      });
  advect.apply();

  const auto &resultDomain = *domain->getSurface();
  const auto &finalSparseDomain = resultDomain.getDomain();
  for (unsigned segment = 0; segment < finalSparseDomain.getNumberOfSegments();
       ++segment)
    result.finalValues.push_back(
        finalSparseDomain.getDomainSegment(segment).definedValues);
  result.afterPointData = snapshotPointData(resultDomain);
  return result;
}

void checkHandledReplacement() {
  const auto result = runHandledReplacement(true);
  VC_TEST_ASSERT(result.executorCalls > 0U);
  VC_TEST_ASSERT(result.replacementSegments > 0U);
  VC_TEST_ASSERT(result.replacementPoints > 0U);
  VC_TEST_ASSERT(result.finalValues == result.replacementValues);
  assertPointDataEqual(result.expectedPointData, result.afterPointData);
}

void checkHandledWithoutPointData() {
  const auto result = runHandledReplacement(false);
  VC_TEST_ASSERT(result.executorCalls > 0U);
  VC_TEST_ASSERT(result.replacementSegments > 0U);
  VC_TEST_ASSERT(result.replacementPoints > 0U);
  VC_TEST_ASSERT(result.finalValues == result.replacementValues);
  VC_TEST_ASSERT(result.afterPointData.scalars.empty());
  VC_TEST_ASSERT(result.afterPointData.vectors.empty());
}

[[nodiscard]] RunResult
run(const ExecutorMode mode,
    const FailurePolicy policy = FailurePolicy::FALLBACK) {
  auto domain = makeDomain();
  auto model = viennacore::SmartPointer<AnalyticModel>::New();
  viennaps::ProcessContext<NumericType, kDimension> context;
  context.domain = domain;
  context.model = model;
  context.processDuration = 0.05;
  context.advectionParams.spatialScheme =
      viennals::SpatialSchemeEnum::ENGQUIST_OSHER_1ST_ORDER;
  context.advectionParams.timeStepRatio = 0.4999;
  context.advectionParams.dissipationAlpha = 0.0;
  context.advectionParams.checkDissipation = false;
  context.translationField = viennacore::
      SmartPointer<viennaps::TranslationField<NumericType, kDimension>>::New(
          model->getVelocityField(), domain->getMaterialMap(), 0);

  context.levelSetUpdateFailurePolicy = policy;
  unsigned executorCalls = 0U;
  if (mode != ExecutorMode::NONE) {
    context.levelSetUpdateExecutor =
        [mode, &executorCalls](const Advect::LevelSetUpdateContext &,
                               Advect::LevelSetUpdateOutput &, std::string &) {
          ++executorCalls;
          if (mode == ExecutorMode::ERROR)
            return Advect::LevelSetUpdateStatus::ERROR;
          if (mode == ExecutorMode::THROW)
            throw std::runtime_error("executor failure");
          if (mode == ExecutorMode::INVALID)
            return Advect::LevelSetUpdateStatus::HANDLED;
          return Advect::LevelSetUpdateStatus::FALLBACK;
        };
  }

  viennaps::AdvectionHandler<NumericType, kDimension> handler;
  VC_TEST_ASSERT(handler.initialize(context) ==
                 viennaps::ProcessResult::SUCCESS);
  // FAIL policy owns the full prepareLS + update transaction.
  auto beforeValues = snapshot(domain);
  handler.prepareAdvection(context);
  model->getVelocityField()->prepare(domain, nullptr, 0.0F);
  auto result = handler.performAdvection(context);
  return {std::move(beforeValues), snapshot(domain),
          executorCalls,           result,
          context.processTime,     handler.getTotalAdvectionSteps()};
}

void checkProcessApi() {
  viennaps::Process<NumericType, kDimension> process;
  typename viennaps::Process<NumericType, kDimension>::LevelSetUpdateExecutor
      executor = [](const Advect::LevelSetUpdateContext &,
                    Advect::LevelSetUpdateOutput &, std::string &) {
        return Advect::LevelSetUpdateStatus::FALLBACK;
      };
  process.setLevelSetUpdateExecutor(std::move(executor));
  process.clearLevelSetUpdateExecutor();
  typename viennaps::Process<NumericType, kDimension>::LevelSetRebuildExecutor
      rebuildExecutor = [](const Advect::LevelSetRebuildContext &,
                           Advect::LevelSetRebuildOutput &, std::string &) {
        return Advect::LevelSetRebuildStatus::FALLBACK;
      };
  process.setLevelSetRebuildExecutor(rebuildExecutor);
  VC_TEST_ASSERT(process.getLevelSetRebuildExecutor() != nullptr);
  process.clearLevelSetRebuildExecutor();
  process.setLevelSetUpdateFailurePolicy(FailurePolicy::FAIL);
  VC_TEST_ASSERT(process.getLevelSetUpdateFailurePolicy() ==
                 FailurePolicy::FAIL);
  process.clearLevelSetUpdateExecutor();
}

[[nodiscard]] RunResult runRebuild(const RebuildMode mode,
                                   const FailurePolicy policy) {
  auto domain = makeDomain();
  auto model = viennacore::SmartPointer<AnalyticModel>::New();
  viennaps::ProcessContext<NumericType, kDimension> context;
  context.domain = domain;
  context.model = model;
  context.processDuration = 0.05;
  context.advectionParams.spatialScheme =
      viennals::SpatialSchemeEnum::ENGQUIST_OSHER_1ST_ORDER;
  context.advectionParams.timeStepRatio = 0.4999;
  context.advectionParams.dissipationAlpha = 0.0;
  context.advectionParams.checkDissipation = false;
  context.translationField = viennacore::
      SmartPointer<viennaps::TranslationField<NumericType, kDimension>>::New(
          model->getVelocityField(), domain->getMaterialMap(), 0);
  context.levelSetUpdateFailurePolicy = policy;
  unsigned calls = 0;
  if (mode != RebuildMode::NONE) {
    context.levelSetRebuildExecutor =
        [mode, &calls](const Advect::LevelSetRebuildContext &,
                       Advect::LevelSetRebuildOutput &, std::string &) {
          ++calls;
          if (mode == RebuildMode::ERROR)
            return Advect::LevelSetRebuildStatus::ERROR;
          if (mode == RebuildMode::THROW)
            throw std::runtime_error("rebuild executor failure");
          if (mode == RebuildMode::INVALID)
            return Advect::LevelSetRebuildStatus::HANDLED;
          return Advect::LevelSetRebuildStatus::FALLBACK;
        };
  }
  viennaps::AdvectionHandler<NumericType, kDimension> handler;
  VC_TEST_ASSERT(handler.initialize(context) ==
                 viennaps::ProcessResult::SUCCESS);
  const auto before = snapshot(domain);
  handler.prepareAdvection(context);
  model->getVelocityField()->prepare(domain, nullptr, 0.0F);
  const auto result = handler.performAdvection(context);
  return {before, snapshot(domain),    calls,
          result, context.processTime, handler.getTotalAdvectionSteps()};
}

void checkProcessStrictFailure() {
  auto domain = makeDomain();
  auto model = viennacore::SmartPointer<AnalyticModel>::New();
  viennaps::Process<NumericType, kDimension> process(domain, model, 0.05F);
  viennaps::AdvectionParameters parameters;
  parameters.spatialScheme =
      viennals::SpatialSchemeEnum::ENGQUIST_OSHER_1ST_ORDER;
  parameters.timeStepRatio = 0.4999;
  parameters.dissipationAlpha = 0.0;
  parameters.checkDissipation = false;
  process.setParameters(parameters);
  process.setLevelSetUpdateFailurePolicy(FailurePolicy::FAIL);
  unsigned executorCalls = 0U;
  process.setLevelSetUpdateExecutor(
      [&executorCalls](const Advect::LevelSetUpdateContext &,
                       Advect::LevelSetUpdateOutput &, std::string &error) {
        ++executorCalls;
        error = "injected process failure";
        return Advect::LevelSetUpdateStatus::ERROR;
      });

  const auto beforeValues = snapshot(domain);
  bool processFailureThrown = false;
  try {
    process.apply();
  } catch (const std::runtime_error &) {
    processFailureThrown = true;
  }
  VC_TEST_ASSERT(processFailureThrown);
  VC_TEST_ASSERT(process.getLastProcessResult() ==
                 viennaps::ProcessResult::FAILURE);
  VC_TEST_ASSERT(executorCalls == 1U);
  VC_TEST_ASSERT(snapshot(domain) == beforeValues);
}

void checkFallback(const ExecutorMode mode) {
  const auto cpu = run(ExecutorMode::NONE);
  const auto fallback = run(mode, FailurePolicy::FALLBACK);
  VC_TEST_ASSERT(cpu.result == viennaps::ProcessResult::SUCCESS);
  VC_TEST_ASSERT(cpu.executorCalls == 0U);
  VC_TEST_ASSERT(!cpu.afterValues.empty());
  VC_TEST_ASSERT(fallback.result == viennaps::ProcessResult::SUCCESS);
  VC_TEST_ASSERT(fallback.executorCalls > 0U);
  VC_TEST_ASSERT(fallback.afterValues == cpu.afterValues);
}

void checkStrictFailure(const ExecutorMode mode) {
  const auto strict = run(mode, FailurePolicy::FAIL);
  VC_TEST_ASSERT(strict.result == viennaps::ProcessResult::FAILURE);
  VC_TEST_ASSERT(strict.executorCalls > 0U);
  VC_TEST_ASSERT(strict.beforeValues == strict.afterValues);
  VC_TEST_ASSERT(strict.processTime == 0.0);
  VC_TEST_ASSERT(strict.advectionSteps == 0U);
}

} // namespace

int main(const int argc, const char *const argv[]) {
  viennacore::Logger::setLogLevel(viennacore::LogLevel::WARNING);
  checkProcessApi();
  // Keep each ViennaLS integration scenario independently time-bounded.
  const std::string_view scenario = argc > 1 ? argv[1] : "explicit-fallback";
  if (scenario == "explicit-fallback")
    checkFallback(ExecutorMode::FALLBACK);
  else if (scenario == "fallback-error")
    checkFallback(ExecutorMode::ERROR);
  else if (scenario == "fallback-throw")
    checkFallback(ExecutorMode::THROW);
  else if (scenario == "fallback-invalid")
    checkFallback(ExecutorMode::INVALID);
  else if (scenario == "strict-error")
    checkStrictFailure(ExecutorMode::ERROR);
  else if (scenario == "strict-throw")
    checkStrictFailure(ExecutorMode::THROW);
  else if (scenario == "strict-invalid")
    checkStrictFailure(ExecutorMode::INVALID);
  else if (scenario == "process-strict")
    checkProcessStrictFailure();
  else if (scenario == "rebuild-fallback") {
    const auto cpu = runRebuild(RebuildMode::NONE, FailurePolicy::FALLBACK);
    const auto fallback =
        runRebuild(RebuildMode::FALLBACK, FailurePolicy::FALLBACK);
    VC_TEST_ASSERT(cpu.result == viennaps::ProcessResult::SUCCESS);
    VC_TEST_ASSERT(fallback.result == viennaps::ProcessResult::SUCCESS);
    VC_TEST_ASSERT(cpu.afterValues == fallback.afterValues);
    VC_TEST_ASSERT(fallback.executorCalls > 0U);
  } else if (scenario == "rebuild-fallback-error" ||
             scenario == "rebuild-fallback-throw" ||
             scenario == "rebuild-fallback-invalid") {
    const auto cpu = runRebuild(RebuildMode::NONE, FailurePolicy::FALLBACK);
    const auto mode = scenario == "rebuild-fallback-error" ? RebuildMode::ERROR
                      : scenario == "rebuild-fallback-throw"
                          ? RebuildMode::THROW
                          : RebuildMode::INVALID;
    const auto result = runRebuild(mode, FailurePolicy::FALLBACK);
    VC_TEST_ASSERT(result.result == viennaps::ProcessResult::SUCCESS);
    VC_TEST_ASSERT(result.afterValues == cpu.afterValues);
    VC_TEST_ASSERT(result.executorCalls > 0U);
  } else if (scenario == "rebuild-strict-error" ||
             scenario == "rebuild-strict-throw" ||
             scenario == "rebuild-strict-invalid") {
    const auto mode = scenario == "rebuild-strict-error" ? RebuildMode::ERROR
                      : scenario == "rebuild-strict-throw"
                          ? RebuildMode::THROW
                          : RebuildMode::INVALID;
    const auto result = runRebuild(mode, FailurePolicy::FAIL);
    VC_TEST_ASSERT(result.result == viennaps::ProcessResult::FAILURE);
    VC_TEST_ASSERT(result.beforeValues == result.afterValues);
    VC_TEST_ASSERT(result.processTime == 0.0);
    VC_TEST_ASSERT(result.advectionSteps == 0U);
    VC_TEST_ASSERT(result.executorCalls > 0U);
  } else if (scenario == "rebuild-handled") {
    checkHandledReplacement();
  } else if (scenario == "rebuild-handled-no-point-data") {
    checkHandledWithoutPointData();
  } else
    return 2;
  return 0;
}
