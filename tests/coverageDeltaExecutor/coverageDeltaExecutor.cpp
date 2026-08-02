#include <process/psCoverageManager.hpp>

#include <vcTestAsserts.hpp>

#include <cmath>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

template <typename NumericType>
class TestSurfaceModel : public viennaps::SurfaceModel<NumericType> {
public:
  void initializeCoverages(unsigned pointCount) override {
    this->coverages = viennacore::PointData<NumericType>::New();
    this->coverages->insertNextScalarData(
        std::vector<NumericType>(pointCount, NumericType(0)), "coverage");
    this->coverages->insertNextScalarData(
        std::vector<NumericType>(pointCount, NumericType(0)), "second");
  }

  auto coverage() { return this->coverages->getScalarData("coverage"); }
  auto second() { return this->coverages->getScalarData("second"); }
};

template <typename NumericType>
struct Fixture {
  using Model = viennaps::ProcessModelCPU<NumericType, 2>;
  using Context = viennaps::ProcessContext<NumericType, 2>;

  viennacore::SmartPointer<TestSurfaceModel<NumericType>> surface =
      viennacore::SmartPointer<TestSurfaceModel<NumericType>>::New();
  viennacore::SmartPointer<Model> model =
      viennacore::SmartPointer<Model>::New();
  Context context;

  Fixture() {
    surface->initializeCoverages(4U);
    model->setSurfaceModel(surface);
    context.model = model;
    context.coverageParams.tolerance = NumericType(0.1);
  }
};

template <typename NumericType>
void runContract() {
  Fixture<NumericType> fixture;
  viennaps::CoverageManager<NumericType, 2> manager;
  *fixture.surface->coverage() = std::vector<NumericType>(4U, NumericType(0));
  *fixture.surface->second() = std::vector<NumericType>(4U, NumericType(0));
  manager.saveCoverages(fixture.context);

  *fixture.surface->coverage() = std::vector<NumericType>(4U, NumericType(1));
  *fixture.surface->second() = std::vector<NumericType>(4U, NumericType(0));
  VC_TEST_ASSERT(!manager.checkCoveragesConvergence(fixture.context));

  bool sawWork = false;
  manager.setCoverageDeltaExecutor(
      [&sawWork](viennaps::CoverageDeltaWork<NumericType> &work,
                 std::string &) {
        sawWork = work.channelCount == 2U && work.channelOffsets.size() == 3U &&
                  work.channelOffsets[0] == 0U &&
                  work.channelOffsets[1] == 4U &&
                  work.channelOffsets[2] == 8U && work.updated.size() == 8U &&
                  work.previous.size() == 8U && work.output.size() == 2U;
        work.output[0] = NumericType(0);
        work.output[1] = NumericType(0);
        work.writtenCount = work.output.size();
        work.complete = true;
        return true;
      });
  VC_TEST_ASSERT(manager.checkCoveragesConvergence(fixture.context));
  VC_TEST_ASSERT(sawWork);

  manager.setCoverageDeltaExecutor(
      [](viennaps::CoverageDeltaWork<NumericType> &work, std::string &) {
        work.output[0] = NumericType(0);
        return false;
      });
  VC_TEST_ASSERT(!manager.checkCoveragesConvergence(fixture.context));

  manager.setCoverageDeltaExecutor(
      [](viennaps::CoverageDeltaWork<NumericType> &, std::string &) -> bool {
        throw std::runtime_error("executor failure");
      });
  VC_TEST_ASSERT(!manager.checkCoveragesConvergence(fixture.context));

  manager.setCoverageDeltaExecutor(
      [](viennaps::CoverageDeltaWork<NumericType> &work, std::string &) {
        work.output[0] = NumericType(0);
        work.output[1] = NumericType(0);
        work.writtenCount = 1U;
        work.complete = true;
        return true;
      });
  VC_TEST_ASSERT(!manager.checkCoveragesConvergence(fixture.context));

  manager.setCoverageDeltaExecutor(
      [](viennaps::CoverageDeltaWork<NumericType> &work, std::string &) {
        work.output[0] = NumericType(0);
        work.output[1] = NumericType(0);
        work.writtenCount = work.output.size();
        return true;
      });
  VC_TEST_ASSERT(!manager.checkCoveragesConvergence(fixture.context));

  manager.clearCoverageDeltaExecutor();
  VC_TEST_ASSERT(!manager.getCoverageDeltaExecutor());
}

} // namespace

int main() {
  runContract<float>();
  runContract<double>();
  return 0;
}
