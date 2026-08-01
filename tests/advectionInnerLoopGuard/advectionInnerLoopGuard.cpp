#include <geometries/psMakePlane.hpp>
#include <process/psAdvectionHandler.hpp>

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
};

[[nodiscard]] RunResult run(const NumericType velocity,
                            const double timeStepRatio,
                            const viennals::TemporalSchemeEnum temporalScheme =
                                viennals::TemporalSchemeEnum::FORWARD_EULER,
                            const double advectionTime = 1.0) {
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

  viennaps::AdvectionHandler<NumericType, kDimension> handler;
  VC_TEST_ASSERT(handler.initialize(context) ==
                 viennaps::ProcessResult::SUCCESS);
  handler.disableSingleStep();
  handler.setAdvectionTime(advectionTime);
  handler.prepareAdvection(context);
  model->getVelocityField()->prepare(domain, nullptr, 0.0F);
  std::vector<NumericType> beforeValues;
  const auto &beforeDomain = domain->getSurface()->getDomain();
  for (unsigned segment = 0U; segment < beforeDomain.getNumberOfSegments();
       ++segment) {
    const auto &segmentValues =
        beforeDomain.getDomainSegment(segment).definedValues;
    beforeValues.insert(beforeValues.end(), segmentValues.begin(),
                        segmentValues.end());
  }
  const auto result = handler.performAdvection(context);
  std::vector<NumericType> afterValues;
  const auto &afterDomain = domain->getSurface()->getDomain();
  for (unsigned segment = 0U; segment < afterDomain.getNumberOfSegments();
       ++segment) {
    const auto &segmentValues =
        afterDomain.getDomainSegment(segment).definedValues;
    afterValues.insert(afterValues.end(), segmentValues.begin(),
                       segmentValues.end());
  }
  return {result, context.processTime, context.timeStep,
          handler.getTotalAdvectionSteps(), beforeValues == afterValues};
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

  viennaps::AdvectionHandler<NumericType, kDimension> handler;
  VC_TEST_ASSERT(handler.initialize(context) ==
                 viennaps::ProcessResult::SUCCESS);
  handler.disableSingleStep();
  handler.setAdvectionTime(1.0);
  handler.prepareAdvection(context);
  model->getVelocityField()->prepare(domain, nullptr, 0.0F);
  VC_TEST_ASSERT(handler.performAdvection(context) ==
                 viennaps::ProcessResult::FAILURE);

  context.advectionParams.timeStepRatio = 0.4999;
  VC_TEST_ASSERT(handler.initialize(context) ==
                 viennaps::ProcessResult::SUCCESS);
  handler.disableSingleStep();
  handler.setAdvectionTime(1.0);
  handler.prepareAdvection(context);
  model->getVelocityField()->prepare(domain, nullptr, 0.0F);
  const auto result = handler.performAdvection(context);
  return {result, context.processTime, context.timeStep,
          handler.getTotalAdvectionSteps(), false};
}

} // namespace

int main(const int argc, const char *const argv[]) {
  viennacore::Logger::setLogLevel(viennacore::LogLevel::WARNING);
  const std::string_view scenario = argc > 1 ? argv[1] : "core";

  if (scenario == "core") {
    const auto noProgress = run(-0.1F, 0.0);
    VC_TEST_ASSERT(noProgress.result == viennaps::ProcessResult::FAILURE);
    VC_TEST_ASSERT(noProgress.processTime == 0.0);
    VC_TEST_ASSERT(noProgress.timeStep == 0.0);
    VC_TEST_ASSERT(noProgress.advectionSteps == 0U);
    VC_TEST_ASSERT(noProgress.valuesUnchanged);

    const auto zeroVelocity = run(0.0F, 0.4999);
    VC_TEST_ASSERT(zeroVelocity.result == viennaps::ProcessResult::SUCCESS);
    VC_TEST_ASSERT(zeroVelocity.processTime == 1.0);
    VC_TEST_ASSERT(zeroVelocity.advectionSteps == 1U);
    VC_TEST_ASSERT(zeroVelocity.timeStep == 1.0);

    const auto positive = run(0.1F, 0.4999);
    VC_TEST_ASSERT(positive.result == viennaps::ProcessResult::SUCCESS);
    VC_TEST_ASSERT(positive.processTime > 0.0);
    VC_TEST_ASSERT(positive.advectionSteps == 1U);
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
  } else {
    return 2;
  }

  return 0;
}
