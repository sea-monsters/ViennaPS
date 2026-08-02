#include <models/psNeutralTransport.hpp>
#include <models/psNeutralTransportVelocityExecutor.hpp>

#include <vcTestAsserts.hpp>

#include <algorithm>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

template <typename NumericType>
using Model =
    viennaps::impl::NeutralTransportSurfaceModel<NumericType, 2>;

template <typename NumericType>
viennaps::NeutralTransportParameters<NumericType> makeParameters() {
  viennaps::NeutralTransportParameters<NumericType> params;
  params.kEtch = NumericType(2);
  params.surfaceSiteDensity = NumericType(3);
  params.siliconDensity = NumericType(6);
  params.etchFrontMaterial = viennaps::Material::Si;
  return params;
}

template <typename NumericType>
void runContract() {
  auto params = makeParameters<NumericType>();
  Model<NumericType> model(params);
  model.initializeCoverages(3U);

  auto fluxes = viennacore::PointData<NumericType>::New();
  fluxes->insertNextScalarData(std::vector<NumericType>(3U, NumericType(1)),
                               params.fluxLabel);
  auto coverage = model.getCoverages()->getScalarData(params.coverageLabel);
  *coverage = {NumericType(0.25), NumericType(0.5), NumericType(0.75)};
  const std::vector<NumericType> materials{
      static_cast<NumericType>(params.etchFrontMaterial.legacyId()),
      NumericType(1),
      static_cast<NumericType>(params.etchFrontMaterial.legacyId())};
  const std::vector<viennacore::Vec3D<NumericType>> coordinates(3U);

  const auto historical = model.calculateVelocities(fluxes, coordinates,
                                                    materials);
  VC_TEST_ASSERT(historical->size() == 3U);
  VC_TEST_ASSERT(historical->at(0) != NumericType(0));
  VC_TEST_ASSERT(historical->at(1) == NumericType(0));
  VC_TEST_ASSERT(historical->at(2) != NumericType(0));

  bool observedWork = false;
  model.setVelocityExecutor(
      [&observedWork](auto &work, std::string &) {
        observedWork = work.coverage.size() == 3U &&
                       work.materialIds.size() == 3U &&
                       work.output.size() == 3U &&
                       work.parameters.siliconDensity == NumericType(6) &&
                       std::all_of(work.output.begin(), work.output.end(),
                                   [](NumericType value) {
                                     return value == NumericType(0);
                                   });
        for (std::size_t i = 0; i < work.output.size(); ++i)
          work.output[i] = work.coverage[i] + NumericType(10);
        work.writtenCount = work.output.size();
        work.complete = true;
        return true;
      });
  const auto injected = model.calculateVelocities(fluxes, coordinates, materials);
  VC_TEST_ASSERT(observedWork);
  VC_TEST_ASSERT(injected->at(0) == NumericType(10.25));
  VC_TEST_ASSERT(injected->at(1) == NumericType(10.5));
  VC_TEST_ASSERT(injected->at(2) == NumericType(10.75));

  model.setVelocityExecutor([](auto &work, std::string &) {
    work.output[0] = NumericType(99);
    return false;
  });
  const auto failed = model.calculateVelocities(fluxes, coordinates, materials);
  VC_TEST_ASSERT(failed->at(0) == historical->at(0));
  VC_TEST_ASSERT(failed->at(1) == historical->at(1));
  VC_TEST_ASSERT(failed->at(2) == historical->at(2));

  model.setVelocityExecutor([](auto &, std::string &) -> bool {
    throw std::runtime_error("executor failure");
  });
  const auto thrown = model.calculateVelocities(fluxes, coordinates, materials);
  VC_TEST_ASSERT(thrown->at(0) == historical->at(0));
  VC_TEST_ASSERT(thrown->at(1) == historical->at(1));
  VC_TEST_ASSERT(thrown->at(2) == historical->at(2));

  model.setVelocityExecutor([](auto &work, std::string &) {
    work.output[0] = NumericType(77);
    return true;
  });
  const auto incomplete =
      model.calculateVelocities(fluxes, coordinates, materials);
  VC_TEST_ASSERT(incomplete->at(0) == historical->at(0));
  VC_TEST_ASSERT(incomplete->at(1) == historical->at(1));
  VC_TEST_ASSERT(incomplete->at(2) == historical->at(2));

  params.siliconDensity = NumericType(0);
  Model<NumericType> zeroDensity(params);
  zeroDensity.initializeCoverages(3U);
  auto zeroCoverage =
      zeroDensity.getCoverages()->getScalarData(params.coverageLabel);
  *zeroCoverage = *coverage;
  const auto zeroCpu =
      zeroDensity.calculateVelocities(fluxes, coordinates, materials);
  VC_TEST_ASSERT(zeroCpu->at(0) == NumericType(0));
  VC_TEST_ASSERT(zeroCpu->at(1) == NumericType(0));
  VC_TEST_ASSERT(zeroCpu->at(2) == NumericType(0));
  zeroDensity.setVelocityExecutor([](auto &work, std::string &) {
    work.output[0] = NumericType(17);
    work.writtenCount = work.output.size();
    work.complete = true;
    return true;
  });
  const auto zero =
      zeroDensity.calculateVelocities(fluxes, coordinates, materials);
  VC_TEST_ASSERT(zero->at(0) == NumericType(17));
}

} // namespace

int main() {
  viennaps::units::Length::setUnit(viennaps::units::Length::METER);
  viennaps::units::Time::setUnit(viennaps::units::Time::SECOND);
  runContract<float>();
  runContract<double>();
  return 0;
}
