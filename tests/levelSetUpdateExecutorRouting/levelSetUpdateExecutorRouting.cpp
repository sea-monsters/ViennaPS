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
  } else
    return 2;
  return 0;
}
