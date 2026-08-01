#include <geometries/psMakePlane.hpp>
#include <process/psAdvectionHandler.hpp>
#include <process/psProcess.hpp>

#include <vcTestAsserts.hpp>

#include <string>
#include <utility>
#include <vector>

namespace {

constexpr int kDimension = 2;
using NumericType = float;
using Advect = viennals::Advect<NumericType, kDimension>;

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
  auto domain = viennaps::Domain<NumericType, kDimension>::New(1.0, 10.0, 10.0);
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

[[nodiscard]] std::pair<std::vector<NumericType>, unsigned>
run(const bool installExecutor) {
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

  unsigned executorCalls = 0U;
  if (installExecutor) {
    context.levelSetUpdateExecutor =
        [&executorCalls](const Advect::LevelSetUpdateContext &,
                         Advect::LevelSetUpdateOutput &, std::string &) {
          ++executorCalls;
          return Advect::LevelSetUpdateStatus::FALLBACK;
        };
  }

  viennaps::AdvectionHandler<NumericType, kDimension> handler;
  VC_TEST_ASSERT(handler.initialize(context) ==
                 viennaps::ProcessResult::SUCCESS);
  handler.prepareAdvection(context);
  model->getVelocityField()->prepare(domain, nullptr, 0.0F);
  VC_TEST_ASSERT(handler.performAdvection(context) ==
                 viennaps::ProcessResult::SUCCESS);
  return {snapshot(domain), executorCalls};
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
}

} // namespace

int main() {
  viennacore::Logger::setLogLevel(viennacore::LogLevel::WARNING);
  checkProcessApi();
  const auto [cpuValues, cpuExecutorCalls] = run(false);
  const auto [fallbackValues, fallbackExecutorCalls] = run(true);

  VC_TEST_ASSERT(cpuExecutorCalls == 0U);
  VC_TEST_ASSERT(fallbackExecutorCalls > 0U);
  VC_TEST_ASSERT(!cpuValues.empty());
  VC_TEST_ASSERT(cpuValues == fallbackValues);
  return 0;
}
