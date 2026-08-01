#include <viennals_update_executor.hpp>

#include <lsAdvect.hpp>
#include <lsDomain.hpp>
#include <lsMakeGeometry.hpp>
#include <lsMesh.hpp>
#include <lsToSurfaceMesh.hpp>
#include <lsVelocityField.hpp>

#include <vcLogger.hpp>
#include <vcTestAsserts.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>
#include <tuple>
#include <vector>

namespace {

namespace ls = viennals;
namespace levelset = viennaps::vulkan::levelset;
namespace runtime = viennaps::vulkan::runtime;

struct QuantizedPoint {
  std::int64_t x = 0;
  std::int64_t y = 0;
  std::int64_t z = 0;

  [[nodiscard]] bool operator<(const QuantizedPoint &other) const {
    return std::tie(x, y, z) < std::tie(other.x, other.y, other.z);
  }
  [[nodiscard]] bool operator==(const QuantizedPoint &other) const = default;
};

using QuantizedLine = std::array<QuantizedPoint, 2>;

[[nodiscard]] QuantizedPoint quantize(const std::array<float, 3> &point,
                                      const float tolerance = 1.0e-6F) {
  return {static_cast<std::int64_t>(std::llround(point[0] / tolerance)),
          static_cast<std::int64_t>(std::llround(point[1] / tolerance)),
          static_cast<std::int64_t>(std::llround(point[2] / tolerance))};
}

struct SurfaceSummary {
  std::size_t activePoints = 0U;
  std::vector<QuantizedPoint> nodes;
  std::vector<QuantizedLine> lines;

  [[nodiscard]] bool operator==(const SurfaceSummary &other) const = default;
};

[[nodiscard]] SurfaceSummary
summarize(const ls::SmartPointer<ls::Domain<float, 2>> &domain) {
  SurfaceSummary result;
  result.activePoints = domain->getNumberOfPoints();
  auto mesh = ls::Mesh<float>::New();
  ls::ToSurfaceMesh<float, 2>(domain, mesh).apply();
  result.nodes.reserve(mesh->nodes.size());
  for (const auto &node : mesh->nodes) {
    result.nodes.push_back(quantize(node));
  }
  result.lines.reserve(mesh->lines.size());
  for (const auto &line : mesh->lines) {
    QuantizedLine quantized{quantize(mesh->nodes[line[0]]),
                            quantize(mesh->nodes[line[1]])};
    if (quantized[1] < quantized[0]) {
      std::swap(quantized[0], quantized[1]);
    }
    result.lines.push_back(quantized);
  }
  std::sort(result.nodes.begin(), result.nodes.end());
  std::sort(result.lines.begin(), result.lines.end());
  return result;
}

class ConstantVelocityField final : public ls::VelocityField<float> {
public:
  float getScalarVelocity(const std::array<float, 3> &, int,
                          const std::array<float, 3> &,
                          unsigned long) override {
    return 0.5F;
  }
};

[[nodiscard]] ls::SmartPointer<ls::Domain<float, 2>> makeDomain() {
  constexpr int dimension = 2;
  const viennahrle::CoordType extent = 12;
  const viennahrle::CoordType gridDelta = 0.5;
  viennahrle::CoordType bounds[2 * dimension] = {-extent, extent, -extent,
                                                 extent};
  typename ls::Domain<float, dimension>::BoundaryType
      boundaryConditions[dimension];
  for (auto &condition : boundaryConditions) {
    condition = ls::BoundaryConditionEnum::REFLECTIVE_BOUNDARY;
  }
  auto domain =
      ls::Domain<float, dimension>::New(bounds, boundaryConditions, gridDelta);
  float origin[dimension] = {0.0F, 0.0F};
  ls::MakeGeometry<float, dimension>(
      domain, ls::SmartPointer<ls::Sphere<float, dimension>>::New(origin, 4.0F))
      .apply();
  return domain;
}

struct RunResult {
  SurfaceSummary surface;
  double advectedTime = 0.0;
  unsigned executorCalls = 0U;
  ls::Advect<float, 2>::LevelSetUpdateStatus executorStatus =
      ls::Advect<float, 2>::LevelSetUpdateStatus::ERROR;
  std::string executorError;
};

[[nodiscard]] RunResult
runOneStep(runtime::ComputeSession *session = nullptr,
           std::shared_ptr<const runtime::SpirvProgram> program = nullptr) {
  auto domain = makeDomain();
  auto velocity = ls::SmartPointer<ConstantVelocityField>::New();
  using Advect = ls::Advect<float, 2>;
  Advect advect;
  advect.insertNextLevelSet(domain);
  advect.setVelocityField(velocity);
  advect.setSpatialScheme(ls::SpatialSchemeEnum::ENGQUIST_OSHER_1ST_ORDER);
  advect.setTemporalScheme(ls::TemporalSchemeEnum::FORWARD_EULER);
  advect.setAdvectionTime(1.0);
  advect.setTimeStepRatio(0.49);
  advect.setSingleStep(true);

  RunResult result;
  if (session != nullptr) {
    auto executor =
        levelset::makeViennaLsUpdateExecutorFp32<2>(*session, program);
    advect.setLevelSetUpdateExecutor(
        [executor, &result](const Advect::LevelSetUpdateContext &context,
                            Advect::LevelSetUpdateOutput &output,
                            std::string &error) mutable {
          ++result.executorCalls;
          result.executorStatus = executor(context, output, error);
          result.executorError = error;
          return result.executorStatus;
        });
  }

  advect.apply();
  result.surface = summarize(domain);
  result.advectedTime = advect.getAdvectedTime();
  return result;
}

} // namespace

int main() try {
  viennacore::Logger::setLogLevel(viennacore::LogLevel::WARNING);
  std::string error;
  runtime::ComputeSession session;
  VC_TEST_ASSERT(session.initialize(error));
  auto program = std::make_shared<runtime::SpirvProgram>();
  VC_TEST_ASSERT(
      runtime::readSpirv(VIENNAPS_LEVELSET_UPDATE_SPV_PATH, *program, error));

  const auto cpu = runOneStep();
  const auto vulkan = runOneStep(&session, program);
  VC_TEST_ASSERT(vulkan.executorCalls == 1U);
  VC_TEST_ASSERT((vulkan.executorStatus ==
                  ls::Advect<float, 2>::LevelSetUpdateStatus::HANDLED));
  VC_TEST_ASSERT(vulkan.executorError.empty());
  VC_TEST_ASSERT(cpu.advectedTime == vulkan.advectedTime);
  VC_TEST_ASSERT(cpu.surface == vulkan.surface);
  VC_TEST_ASSERT(cpu.surface.activePoints == 92U);
  VC_TEST_ASSERT(cpu.surface.nodes.size() == 68U);
  VC_TEST_ASSERT(cpu.surface.lines.size() == 68U);
  std::cout << "[VulkanViennaLsExecutor] CPU/Vulkan topology PASS\n";
  return EXIT_SUCCESS;
} catch (const std::exception &exception) {
  std::cerr << exception.what() << '\n';
  return EXIT_FAILURE;
}
