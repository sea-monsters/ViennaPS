#include <geometries/psMakePlane.hpp>
#include <process/psAdvectionHandler.hpp>

#include <vcLogger.hpp>
#include <vcTestAsserts.hpp>

#include <limits>
#include <string_view>
#include <vector>

namespace {

constexpr int kDimension = 2;
using NumericType = float;

class ConstantVelocityField final
    : public viennaps::VelocityField<NumericType, kDimension> {
public:
  explicit ConstantVelocityField(const NumericType velocity)
      : velocity_(velocity) {}

  NumericType getScalarVelocity(const viennaps::Vec3D<NumericType> &, int,
                                const viennaps::Vec3D<NumericType> &,
                                unsigned long) override {
    return velocity_;
  }

private:
  NumericType velocity_;
};

class AnalyticModel final
    : public viennaps::ProcessModelBase<NumericType, kDimension> {
public:
  explicit AnalyticModel(const NumericType velocity) {
    this->setVelocityField(
        viennacore::SmartPointer<ConstantVelocityField>::New(velocity));
    this->setSurfaceModel(
        viennacore::SmartPointer<viennaps::SurfaceModel<NumericType>>::New());
  }
};

struct RunResult {
  viennaps::ProcessResult result;
  double processTime;
  double timeStep;
  unsigned advectionSteps;
  bool valuesUnchanged;
  unsigned velocityUpdateInvocations;
};

[[nodiscard]] std::vector<NumericType>
collectDefinedValues(const viennaps::Domain<NumericType, kDimension> &domain) {
  std::vector<NumericType> values;
  const auto &levelSetDomain = domain.getSurface()->getDomain();
  for (unsigned segment = 0U; segment < levelSetDomain.getNumberOfSegments();
       ++segment) {
    const auto &segmentValues =
        levelSetDomain.getDomainSegment(segment).definedValues;
    values.insert(values.end(), segmentValues.begin(), segmentValues.end());
  }
  return values;
}

[[nodiscard]] RunResult run(const NumericType velocity,
                            const double timeStepRatio,
                            const viennals::TemporalSchemeEnum temporalScheme =
                                viennals::TemporalSchemeEnum::FORWARD_EULER,
                            const double advectionTime = 1.0,
                            const bool injectFallbackExecutor = true) {
  auto domain = viennaps::Domain<NumericType, kDimension>::New(1.0, 2.0, 2.0);
  viennaps::MakePlane<NumericType, kDimension>(domain).apply();
  auto model = viennacore::SmartPointer<AnalyticModel>::New(velocity);

  viennaps::ProcessContext<NumericType, kDimension> context;
  context.domain = domain;
  context.model = model;
  context.processDuration = 1.0;
  context.flags.isALP = true;
  context.advectionParams.spatialScheme =
      viennals::SpatialSchemeEnum::ENGQUIST_OSHER_1ST_ORDER;
  context.advectionParams.temporalScheme = temporalScheme;
  context.advectionParams.timeStepRatio = timeStepRatio;
  context.advectionParams.dissipationAlpha = 0.0;
  context.advectionParams.checkDissipation = false;
  context.translationField = viennacore::
      SmartPointer<viennaps::TranslationField<NumericType, kDimension>>::New(
          model->getVelocityField(), domain->getMaterialMap(), 0);

  if (injectFallbackExecutor) {
    using Advect = viennals::Advect<NumericType, kDimension>;
    context.levelSetUpdateExecutor =
        [](const Advect::LevelSetUpdateContext &, Advect::LevelSetUpdateOutput &,
           std::string &) {
          return Advect::LevelSetUpdateStatus::FALLBACK;
        };
    // Use FAIL policy for executor-active scenarios so that the ViennaLS
    // snapshot is actually captured and restored on advection-time errors.
    // The FALLBACK executor never reports an error, so this only exercises
    // rollback for the fail-closed advection-time validation path.
    context.levelSetUpdateFailurePolicy =
        viennaps::LevelSetUpdateFailurePolicy::FAIL;
  }

  viennaps::AdvectionHandler<NumericType, kDimension> handler;
  VC_TEST_ASSERT(handler.initialize(context) ==
                 viennaps::ProcessResult::SUCCESS);
  handler.disableSingleStep();
  handler.setAdvectionTime(advectionTime);

  // Capture the pre-prepare level-set state. For executor-active failure
  // scenarios with FAIL policy this proves the ViennaLS snapshot rollback
  // restores the state from before prepareAdvection. For legacy/no-executor
  // scenarios it witnesses that the original CPU path leaves the level set
  // defined values unchanged when the timestep is rejected or zero.
  const auto beforeValues = collectDefinedValues(*domain);

  unsigned velocityUpdateInvocations = 0U;
  handler.setVelocityUpdateCallback(
      [&velocityUpdateInvocations](
          viennacore::SmartPointer<viennals::Domain<NumericType, kDimension>>) {
        ++velocityUpdateInvocations;
        return true;
      });

  handler.prepareAdvection(context);
  model->getVelocityField()->prepare(domain, nullptr, 0.0F);

  const auto result = handler.performAdvection(context);
  const auto afterValues = collectDefinedValues(*domain);

  return {result,
          context.processTime,
          context.timeStep,
          handler.getTotalAdvectionSteps(),
          beforeValues == afterValues,
          velocityUpdateInvocations};
}

[[nodiscard]] RunResult runRecoveryAfterRejectedStep() {
  auto domain = viennaps::Domain<NumericType, kDimension>::New(1.0, 2.0, 2.0);
  viennaps::MakePlane<NumericType, kDimension>(domain).apply();
  auto model = viennacore::SmartPointer<AnalyticModel>::New(0.1F);

  viennaps::ProcessContext<NumericType, kDimension> context;
  context.domain = domain;
  context.model = model;
  context.processDuration = 1.0;
  context.flags.isALP = true;
  context.advectionParams.spatialScheme =
      viennals::SpatialSchemeEnum::ENGQUIST_OSHER_1ST_ORDER;
  context.advectionParams.timeStepRatio = 0.0;
  context.advectionParams.dissipationAlpha = 0.0;
  context.advectionParams.checkDissipation = false;
  context.translationField = viennacore::
      SmartPointer<viennaps::TranslationField<NumericType, kDimension>>::New(
          model->getVelocityField(), domain->getMaterialMap(), 0);

  const auto beforeValues = collectDefinedValues(*domain);

  {
    using Advect = viennals::Advect<NumericType, kDimension>;
    context.levelSetUpdateExecutor =
        [](const Advect::LevelSetUpdateContext &, Advect::LevelSetUpdateOutput &,
           std::string &) {
          return Advect::LevelSetUpdateStatus::FALLBACK;
        };
    context.levelSetUpdateFailurePolicy =
        viennaps::LevelSetUpdateFailurePolicy::FAIL;
  }

  unsigned velocityUpdateInvocations = 0U;

  viennaps::AdvectionHandler<NumericType, kDimension> handler;
  VC_TEST_ASSERT(handler.initialize(context) ==
                 viennaps::ProcessResult::SUCCESS);
  handler.disableSingleStep();
  handler.setAdvectionTime(1.0);
  handler.setVelocityUpdateCallback(
      [&velocityUpdateInvocations](
          viennacore::SmartPointer<viennals::Domain<NumericType, kDimension>>) {
        ++velocityUpdateInvocations;
        return true;
      });
  handler.prepareAdvection(context);
  model->getVelocityField()->prepare(domain, nullptr, 0.0F);
  VC_TEST_ASSERT(handler.performAdvection(context) ==
                 viennaps::ProcessResult::FAILURE);

  const auto afterRejectedValues = collectDefinedValues(*domain);

  context.advectionParams.timeStepRatio = 0.4999;
  VC_TEST_ASSERT(handler.initialize(context) ==
                 viennaps::ProcessResult::SUCCESS);
  handler.disableSingleStep();
  handler.setAdvectionTime(1.0);
  handler.prepareAdvection(context);
  model->getVelocityField()->prepare(domain, nullptr, 0.0F);
  const auto result = handler.performAdvection(context);
  return {result,
          context.processTime,
          context.timeStep,
          handler.getTotalAdvectionSteps(),
          beforeValues == afterRejectedValues,
          velocityUpdateInvocations};
}

} // namespace

int main(const int argc, const char *const argv[]) try {
  viennacore::Logger::setLogLevel(viennacore::LogLevel::WARNING);
  const std::string_view scenario = argc > 1 ? argv[1] : "core";

  if (scenario == "core") {
    std::cerr << "[advectionInnerLoopGuard] start core" << std::endl;
    const auto noProgressExecutor = run(-0.1F, 0.0);
    std::cerr << "[advectionInnerLoopGuard] noProgressExecutor done" << std::endl;
    VC_TEST_ASSERT(noProgressExecutor.result == viennaps::ProcessResult::FAILURE);
    VC_TEST_ASSERT(noProgressExecutor.processTime == 0.0);
    VC_TEST_ASSERT(noProgressExecutor.timeStep == 0.0);
    VC_TEST_ASSERT(noProgressExecutor.advectionSteps == 0U);
    VC_TEST_ASSERT(noProgressExecutor.valuesUnchanged);
    VC_TEST_ASSERT(noProgressExecutor.velocityUpdateInvocations == 0U);

    const auto zeroVelocityExecutor = run(0.0F, 0.4999);
    std::cerr << "[advectionInnerLoopGuard] zeroVelocityExecutor done" << std::endl;
    VC_TEST_ASSERT(zeroVelocityExecutor.result ==
                   viennaps::ProcessResult::SUCCESS);
    VC_TEST_ASSERT(zeroVelocityExecutor.processTime == 1.0);
    VC_TEST_ASSERT(zeroVelocityExecutor.advectionSteps == 1U);
    VC_TEST_ASSERT(zeroVelocityExecutor.timeStep == 1.0);
    VC_TEST_ASSERT(zeroVelocityExecutor.velocityUpdateInvocations == 0U);

    const auto positiveExecutor = run(0.1F, 0.4999);
    std::cerr << "[advectionInnerLoopGuard] positiveExecutor done" << std::endl;
    VC_TEST_ASSERT(positiveExecutor.result == viennaps::ProcessResult::SUCCESS);
    VC_TEST_ASSERT(positiveExecutor.processTime > 0.0);
    VC_TEST_ASSERT(positiveExecutor.advectionSteps == 1U);
    VC_TEST_ASSERT(positiveExecutor.velocityUpdateInvocations == 0U);

    std::cerr << "[advectionInnerLoopGuard] legacy scenarios start" << std::endl;
    // Legacy CPU path: no executor means original ViennaPS semantics.
    const auto noProgressLegacy = run(-0.1F, 0.0,
                                      viennals::TemporalSchemeEnum::FORWARD_EULER,
                                      0.0, false);
    std::cerr << "[advectionInnerLoopGuard] noProgressLegacy done" << std::endl;
    VC_TEST_ASSERT(noProgressLegacy.result == viennaps::ProcessResult::SUCCESS);
    VC_TEST_ASSERT(noProgressLegacy.processTime == 0.0);
    VC_TEST_ASSERT(noProgressLegacy.timeStep == 0.0);
    VC_TEST_ASSERT(noProgressLegacy.advectionSteps == 1U);
    VC_TEST_ASSERT(noProgressLegacy.velocityUpdateInvocations == 0U);

    const auto zeroVelocityLegacy = run(
        0.0F, 0.4999, viennals::TemporalSchemeEnum::FORWARD_EULER, 1.0, false);
    std::cerr << "[advectionInnerLoopGuard] zeroVelocityLegacy done" << std::endl;
    VC_TEST_ASSERT(zeroVelocityLegacy.result == viennaps::ProcessResult::SUCCESS);
    VC_TEST_ASSERT(zeroVelocityLegacy.processTime == 1.0);
    VC_TEST_ASSERT(zeroVelocityLegacy.advectionSteps == 1U);
    VC_TEST_ASSERT(zeroVelocityLegacy.timeStep == 1.0);
    VC_TEST_ASSERT(zeroVelocityLegacy.velocityUpdateInvocations == 0U);

    // RK2/RK3 executor-active paths remain fail-closed and roll back.
    std::cerr << "[advectionInnerLoopGuard] executor-active RK2/RK3 start"
              << std::endl;
    const auto rk2NoProgressExecutor =
        run(-0.1F, 0.0, viennals::TemporalSchemeEnum::RUNGE_KUTTA_2ND_ORDER);
    std::cerr << "[advectionInnerLoopGuard] rk2NoProgressExecutor done"
              << std::endl;
    VC_TEST_ASSERT(rk2NoProgressExecutor.result ==
                   viennaps::ProcessResult::FAILURE);
    VC_TEST_ASSERT(rk2NoProgressExecutor.processTime == 0.0);
    VC_TEST_ASSERT(rk2NoProgressExecutor.timeStep == 0.0);
    VC_TEST_ASSERT(rk2NoProgressExecutor.advectionSteps == 0U);
    VC_TEST_ASSERT(rk2NoProgressExecutor.valuesUnchanged);
    VC_TEST_ASSERT(rk2NoProgressExecutor.velocityUpdateInvocations == 0U);

    const auto rk3NoProgressExecutor =
        run(-0.1F, 0.0, viennals::TemporalSchemeEnum::RUNGE_KUTTA_3RD_ORDER);
    std::cerr << "[advectionInnerLoopGuard] rk3NoProgressExecutor done"
              << std::endl;
    VC_TEST_ASSERT(rk3NoProgressExecutor.result ==
                   viennaps::ProcessResult::FAILURE);
    VC_TEST_ASSERT(rk3NoProgressExecutor.processTime == 0.0);
    VC_TEST_ASSERT(rk3NoProgressExecutor.timeStep == 0.0);
    VC_TEST_ASSERT(rk3NoProgressExecutor.advectionSteps == 0U);
    VC_TEST_ASSERT(rk3NoProgressExecutor.valuesUnchanged);
    VC_TEST_ASSERT(rk3NoProgressExecutor.velocityUpdateInvocations == 0U);

    // RK2/RK3 legacy paths preserve the original CPU integration sequence.
    std::cerr << "[advectionInnerLoopGuard] legacy RK2/RK3 start" << std::endl;
    const auto rk2NoProgressLegacy =
        run(-0.1F, 0.0, viennals::TemporalSchemeEnum::RUNGE_KUTTA_2ND_ORDER,
            0.0, false);
    std::cerr << "[advectionInnerLoopGuard] rk2NoProgressLegacy done" << std::endl;
    VC_TEST_ASSERT(rk2NoProgressLegacy.result == viennaps::ProcessResult::SUCCESS);
    VC_TEST_ASSERT(rk2NoProgressLegacy.processTime == 0.0);
    VC_TEST_ASSERT(rk2NoProgressLegacy.timeStep == 0.0);
    VC_TEST_ASSERT(rk2NoProgressLegacy.advectionSteps == 1U);

    const auto rk2PositiveLegacy =
        run(0.1F, 0.4999, viennals::TemporalSchemeEnum::RUNGE_KUTTA_2ND_ORDER,
            1.0, false);
    std::cerr << "[advectionInnerLoopGuard] rk2PositiveLegacy done" << std::endl;
    VC_TEST_ASSERT(rk2PositiveLegacy.result == viennaps::ProcessResult::SUCCESS);
    VC_TEST_ASSERT(rk2PositiveLegacy.processTime > 0.0);
    VC_TEST_ASSERT(rk2PositiveLegacy.advectionSteps > 0U);
    VC_TEST_ASSERT(rk2PositiveLegacy.velocityUpdateInvocations == 1U);

    const auto rk2ZeroVelocityLegacy = run(
        0.0F, 0.4999, viennals::TemporalSchemeEnum::RUNGE_KUTTA_2ND_ORDER, 1.0,
        false);
    VC_TEST_ASSERT(rk2ZeroVelocityLegacy.result ==
                   viennaps::ProcessResult::SUCCESS);
    VC_TEST_ASSERT(rk2ZeroVelocityLegacy.processTime == 1.0);
    VC_TEST_ASSERT(rk2ZeroVelocityLegacy.advectionSteps == 1U);
    VC_TEST_ASSERT(rk2ZeroVelocityLegacy.timeStep == 1.0);
    VC_TEST_ASSERT(rk2ZeroVelocityLegacy.velocityUpdateInvocations == 1U);

    const auto rk3NoProgressLegacy =
        run(-0.1F, 0.0, viennals::TemporalSchemeEnum::RUNGE_KUTTA_3RD_ORDER,
            0.0, false);
    VC_TEST_ASSERT(rk3NoProgressLegacy.result == viennaps::ProcessResult::SUCCESS);
    VC_TEST_ASSERT(rk3NoProgressLegacy.processTime == 0.0);
    VC_TEST_ASSERT(rk3NoProgressLegacy.timeStep == 0.0);
    VC_TEST_ASSERT(rk3NoProgressLegacy.advectionSteps == 1U);

    const auto rk3PositiveLegacy =
        run(0.1F, 0.4999, viennals::TemporalSchemeEnum::RUNGE_KUTTA_3RD_ORDER,
            1.0, false);
    std::cerr << "[advectionInnerLoopGuard] rk3PositiveLegacy done" << std::endl;
    VC_TEST_ASSERT(rk3PositiveLegacy.result == viennaps::ProcessResult::SUCCESS);
    VC_TEST_ASSERT(rk3PositiveLegacy.processTime > 0.0);
    VC_TEST_ASSERT(rk3PositiveLegacy.advectionSteps > 0U);
    VC_TEST_ASSERT(rk3PositiveLegacy.velocityUpdateInvocations == 2U);

    const auto rk3ZeroVelocityLegacy = run(
        0.0F, 0.4999, viennals::TemporalSchemeEnum::RUNGE_KUTTA_3RD_ORDER, 1.0,
        false);
    VC_TEST_ASSERT(rk3ZeroVelocityLegacy.result ==
                   viennaps::ProcessResult::SUCCESS);
    VC_TEST_ASSERT(rk3ZeroVelocityLegacy.processTime == 1.0);
    VC_TEST_ASSERT(rk3ZeroVelocityLegacy.advectionSteps == 1U);
    VC_TEST_ASSERT(rk3ZeroVelocityLegacy.timeStep == 1.0);
    VC_TEST_ASSERT(rk3ZeroVelocityLegacy.velocityUpdateInvocations == 2U);
  } else if (scenario == "rk2-no-progress") {
    const auto result = run(
        -0.1F, 0.0, viennals::TemporalSchemeEnum::RUNGE_KUTTA_2ND_ORDER);
    VC_TEST_ASSERT(result.result == viennaps::ProcessResult::FAILURE);
    VC_TEST_ASSERT(result.processTime == 0.0);
    VC_TEST_ASSERT(result.timeStep == 0.0);
    VC_TEST_ASSERT(result.advectionSteps == 0U);
    VC_TEST_ASSERT(result.valuesUnchanged);
  } else if (scenario == "rk3-no-progress") {
    const auto result = run(
        -0.1F, 0.0, viennals::TemporalSchemeEnum::RUNGE_KUTTA_3RD_ORDER);
    VC_TEST_ASSERT(result.result == viennaps::ProcessResult::FAILURE);
    VC_TEST_ASSERT(result.processTime == 0.0);
    VC_TEST_ASSERT(result.timeStep == 0.0);
    VC_TEST_ASSERT(result.advectionSteps == 0U);
    VC_TEST_ASSERT(result.valuesUnchanged);
  } else if (scenario == "recovery") {
    const auto result = runRecoveryAfterRejectedStep();
    VC_TEST_ASSERT(result.result == viennaps::ProcessResult::SUCCESS);
    VC_TEST_ASSERT(result.processTime > 0.0);
    VC_TEST_ASSERT(result.advectionSteps > 0U);
    VC_TEST_ASSERT(result.valuesUnchanged);
  } else if (scenario == "rk2-max" || scenario == "rk3-max") {
    const auto temporalScheme =
        scenario == "rk2-max"
            ? viennals::TemporalSchemeEnum::RUNGE_KUTTA_2ND_ORDER
            : viennals::TemporalSchemeEnum::RUNGE_KUTTA_3RD_ORDER;
    const auto result = run(
        0.0F, 0.4999, temporalScheme, 0.0);
    VC_TEST_ASSERT(result.result == viennaps::ProcessResult::SUCCESS);
    VC_TEST_ASSERT(
        result.timeStep ==
        static_cast<double>(std::numeric_limits<NumericType>::max()));
    VC_TEST_ASSERT(result.valuesUnchanged);
    VC_TEST_ASSERT(result.velocityUpdateInvocations == 0U);
  } else {
    return 2;
  }

  return 0;
} catch (const std::exception &exception) {
  std::cerr << "[advectionInnerLoopGuard] failed: " << exception.what()
            << std::endl;
  return 3;
}
