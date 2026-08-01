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
  handler.prepareAdvection(context);
  model->getVelocityField()->prepare(domain, nullptr, 0.0F);
  auto beforeValues = snapshot(domain);
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
  process.setLevelSetUpdateFailurePolicy(FailurePolicy::FAIL);
  VC_TEST_ASSERT(process.getLevelSetUpdateFailurePolicy() ==
                 FailurePolicy::FAIL);
  process.clearLevelSetUpdateExecutor();
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
  else
    return 2;
  return 0;
}
