#include <geometries/psMakePlane.hpp>
#include <process/psAdvectionHandler.hpp>

#include <vcTestAsserts.hpp>

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
};

[[nodiscard]] RunResult run(const NumericType velocity,
                            const double timeStepRatio,
                            const bool injectFallbackExecutor) {
  auto domain = viennaps::Domain<NumericType, kDimension>::New(1.0, 2.0, 2.0);
  viennaps::MakePlane<NumericType, kDimension>(domain).apply();
  auto model = viennacore::SmartPointer<AnalyticModel>::New(velocity);

  viennaps::ProcessContext<NumericType, kDimension> context;
  context.domain = domain;
  context.model = model;
  context.processDuration = 0.05;
  context.advectionParams.spatialScheme =
      viennals::SpatialSchemeEnum::ENGQUIST_OSHER_1ST_ORDER;
  context.advectionParams.timeStepRatio = timeStepRatio;
  context.advectionParams.dissipationAlpha = 0.0;
  context.advectionParams.checkDissipation = false;
  context.translationField = viennacore::
      SmartPointer<viennaps::TranslationField<NumericType, kDimension>>::New(
          model->getVelocityField(), domain->getMaterialMap(), 0);

  if (injectFallbackExecutor) {
    using Advect = viennals::Advect<NumericType, kDimension>;
    context.levelSetUpdateExecutor =
        [](const Advect::LevelSetUpdateContext &,
           Advect::LevelSetUpdateOutput &, std::string &) {
          return Advect::LevelSetUpdateStatus::FALLBACK;
        };
  }

  viennaps::AdvectionHandler<NumericType, kDimension> handler;
  VC_TEST_ASSERT(handler.initialize(context) ==
                 viennaps::ProcessResult::SUCCESS);
  handler.prepareAdvection(context);
  model->getVelocityField()->prepare(domain, nullptr, 0.0F);
  const auto result = handler.performAdvection(context);
  return {result, context.processTime, context.timeStep,
          handler.getTotalAdvectionSteps()};
}

} // namespace

int main() {
  viennacore::Logger::setLogLevel(viennacore::LogLevel::WARNING);

  // Executor-active path: fail-closed semantics are preserved.
  const auto noProgressExecutor = run(-0.1F, 0.0, true);
  VC_TEST_ASSERT(noProgressExecutor.result ==
                 viennaps::ProcessResult::EARLY_TERMINATION);
  VC_TEST_ASSERT(noProgressExecutor.processTime == 0.0);
  VC_TEST_ASSERT(noProgressExecutor.timeStep == 0.0);
  VC_TEST_ASSERT(noProgressExecutor.advectionSteps == 0U);

  const auto zeroVelocityExecutor = run(0.0F, 0.4999, true);
  VC_TEST_ASSERT(zeroVelocityExecutor.result ==
                 viennaps::ProcessResult::SUCCESS);
  VC_TEST_ASSERT(zeroVelocityExecutor.processTime == 0.05);
  VC_TEST_ASSERT(zeroVelocityExecutor.advectionSteps == 1U);

  // Legacy CPU path: matches original ViennaPS 4.6.2 behavior.
  const auto noProgressLegacy = run(-0.1F, 0.0, false);
  VC_TEST_ASSERT(noProgressLegacy.result == viennaps::ProcessResult::SUCCESS);
  VC_TEST_ASSERT(noProgressLegacy.processTime == 0.0);
  VC_TEST_ASSERT(noProgressLegacy.timeStep == 0.0);
  VC_TEST_ASSERT(noProgressLegacy.advectionSteps == 1U);

  const auto zeroVelocityLegacy = run(0.0F, 0.4999, false);
  VC_TEST_ASSERT(zeroVelocityLegacy.result == viennaps::ProcessResult::SUCCESS);
  VC_TEST_ASSERT(zeroVelocityLegacy.processTime == 0.05);
  VC_TEST_ASSERT(zeroVelocityLegacy.advectionSteps == 1U);

  return 0;
}
