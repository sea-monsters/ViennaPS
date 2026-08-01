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
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <tuple>
#include <vector>

namespace viennacore {

namespace ls = viennals;

template <class NumericType> struct QuantizedPoint {
  std::int64_t x = 0;
  std::int64_t y = 0;
  std::int64_t z = 0;

  [[nodiscard]] bool operator<(const QuantizedPoint &other) const {
    return std::tie(x, y, z) < std::tie(other.x, other.y, other.z);
  }

  [[nodiscard]] bool operator==(const QuantizedPoint &other) const = default;
};

template <class NumericType>
[[nodiscard]] QuantizedPoint<NumericType>
quantize(const std::array<NumericType, 3> &point,
         const NumericType tolerance = static_cast<NumericType>(1e-6)) {
  return {static_cast<std::int64_t>(std::llround(point[0] / tolerance)),
          static_cast<std::int64_t>(std::llround(point[1] / tolerance)),
          static_cast<std::int64_t>(std::llround(point[2] / tolerance))};
}

template <class NumericType>
[[nodiscard]] QuantizedPoint<NumericType>
quantizeLine(const std::array<QuantizedPoint<NumericType>, 2> &line) {
  auto first = line[0];
  auto second = line[1];
  if (second < first) {
    std::swap(first, second);
  }
  return {first.x + second.x, first.y + second.y, first.z + second.z};
}

template <class NumericType>
[[nodiscard]] QuantizedPoint<NumericType>
quantizeTriangle(const std::array<QuantizedPoint<NumericType>, 3> &triangle) {
  auto nodes = triangle;
  std::sort(nodes.begin(), nodes.end());
  return {nodes[0].x + nodes[1].x + nodes[2].x,
          nodes[0].y + nodes[1].y + nodes[2].y,
          nodes[0].z + nodes[1].z + nodes[2].z};
}

template <class NumericType> struct LevelSetSummary {
  std::size_t activePointCount = 0;
  std::size_t surfaceNodeCount = 0;
  std::size_t surfaceLineCount = 0;
  std::size_t surfaceTriangleCount = 0;
  std::array<NumericType, 3> bboxMin{};
  std::array<NumericType, 3> bboxMax{};
  std::vector<QuantizedPoint<NumericType>> surfaceNodes;
  std::vector<QuantizedPoint<NumericType>> surfaceLines;
  std::vector<QuantizedPoint<NumericType>> surfaceTriangles;
};

template <class NumericType>
[[nodiscard]] LevelSetSummary<NumericType>
summarize(const ls::SmartPointer<ls::Domain<NumericType, 2>> &domain) {
  LevelSetSummary<NumericType> summary;
  summary.activePointCount = domain->getNumberOfPoints();

  auto surface = ls::Mesh<NumericType>::New();
  ls::ToSurfaceMesh<NumericType, 2>(domain, surface).apply();
  summary.surfaceNodeCount = surface->nodes.size();
  summary.surfaceLineCount = surface->lines.size();
  summary.surfaceTriangleCount = surface->triangles.size();
  VC_TEST_ASSERT(!surface->nodes.empty());
  summary.bboxMin = surface->nodes.front();
  summary.bboxMax = surface->nodes.front();
  summary.surfaceNodes.reserve(surface->nodes.size());
  for (const auto &node : surface->nodes) {
    for (std::size_t axis = 0; axis < 3; ++axis) {
      summary.bboxMin[axis] = std::min(summary.bboxMin[axis], node[axis]);
      summary.bboxMax[axis] = std::max(summary.bboxMax[axis], node[axis]);
    }
    summary.surfaceNodes.push_back(quantize(node));
  }

  summary.surfaceLines.reserve(surface->lines.size());
  for (const auto &line : surface->lines) {
    const auto linePoints = std::array{quantize(surface->nodes[line[0]]),
                                       quantize(surface->nodes[line[1]])};
    auto q = quantizeLine(linePoints);
    summary.surfaceLines.push_back(q);
  }

  summary.surfaceTriangles.reserve(surface->triangles.size());
  for (const auto &triangle : surface->triangles) {
    const auto trianglePoints =
        std::array{quantize(surface->nodes[triangle[0]]),
                   quantize(surface->nodes[triangle[1]]),
                   quantize(surface->nodes[triangle[2]])};
    auto q = quantizeTriangle(trianglePoints);
    summary.surfaceTriangles.push_back(q);
  }

  std::sort(summary.surfaceNodes.begin(), summary.surfaceNodes.end());
  std::sort(summary.surfaceLines.begin(), summary.surfaceLines.end());
  std::sort(summary.surfaceTriangles.begin(), summary.surfaceTriangles.end());

  return summary;
}

template <class NumericType> struct LevelSetOracle {
  std::size_t activePointCount = 0;
  std::size_t surfaceNodeCount = 0;
  std::size_t surfaceLineCount = 0;
  std::size_t surfaceTriangleCount = 0;
  std::size_t step = 0;
  double advectedTime = 0.0;
  std::uint64_t fingerprint = 0;
  unsigned executorCalls = 0;
};

enum class ExecutorMode { NONE, FALLBACK, ERROR, INVALID_OUTPUT, HANDLED_ECHO };

template <class NumericType>
[[nodiscard]] std::uint64_t
fingerprint(const LevelSetSummary<NumericType> &summary) {
  constexpr std::uint64_t offset = 1469598103934665603ULL;
  constexpr std::uint64_t prime = 1099511628211ULL;
  std::uint64_t hash = offset;
  const auto mix = [&hash](const std::uint64_t value) {
    hash ^= value;
    hash *= prime;
  };

  mix(summary.activePointCount);
  mix(summary.surfaceNodeCount);
  mix(summary.surfaceLineCount);
  mix(summary.surfaceTriangleCount);
  for (const auto &node : summary.surfaceNodes) {
    mix(static_cast<std::uint64_t>(node.x));
    mix(static_cast<std::uint64_t>(node.y));
    mix(static_cast<std::uint64_t>(node.z));
  }
  for (const auto &line : summary.surfaceLines) {
    mix(static_cast<std::uint64_t>(line.x));
    mix(static_cast<std::uint64_t>(line.y));
    mix(static_cast<std::uint64_t>(line.z));
  }
  for (const auto &triangle : summary.surfaceTriangles) {
    mix(static_cast<std::uint64_t>(triangle.x));
    mix(static_cast<std::uint64_t>(triangle.y));
    mix(static_cast<std::uint64_t>(triangle.z));
  }

  return hash;
}

[[nodiscard]] std::string formatFingerprint(const std::uint64_t value) {
  std::ostringstream output;
  output << "0x" << std::hex << std::setw(16) << std::setfill('0') << value;
  return output.str();
}

template <class NumericType>
class ConstantVelocityField final : public ls::VelocityField<NumericType> {
public:
  explicit ConstantVelocityField(const NumericType speed) : speed_(speed) {}

  NumericType getScalarVelocity(const std::array<NumericType, 3> &, int,
                                const std::array<NumericType, 3> &,
                                unsigned long) override {
    return speed_;
  }

private:
  NumericType speed_ = NumericType(0);
};

template <class NumericType>
[[nodiscard]] ls::SmartPointer<ls::Domain<NumericType, 2>>
runScenario(const NumericType radius = static_cast<NumericType>(4)) {
  constexpr int dimension = 2;
  const viennahrle::CoordType extent = 12;
  const viennahrle::CoordType gridDelta = 0.5;
  viennahrle::CoordType bounds[2 * dimension] = {-extent, extent, -extent,
                                                 extent};
  typename ls::Domain<NumericType, dimension>::BoundaryType
      boundaryConditions[dimension];
  for (auto &condition : boundaryConditions) {
    condition = ls::BoundaryConditionEnum::REFLECTIVE_BOUNDARY;
  }

  auto domain = ls::SmartPointer<ls::Domain<NumericType, dimension>>::New(
      bounds, boundaryConditions, gridDelta);
  NumericType origin[dimension] = {NumericType(0), NumericType(0)};
  ls::MakeGeometry<NumericType, dimension>(
      domain,
      ls::SmartPointer<ls::Sphere<NumericType, dimension>>::New(origin, radius))
      .apply();
  return domain;
}

template <class NumericType>
[[nodiscard]] LevelSetOracle<NumericType>
runOneStep(const NumericType processDuration = static_cast<NumericType>(1),
           const NumericType advectionSpeed = static_cast<NumericType>(0.5),
           const ExecutorMode executorMode = ExecutorMode::NONE,
           const ls::TemporalSchemeEnum temporalScheme =
               ls::TemporalSchemeEnum::FORWARD_EULER) {
  auto domain = runScenario<NumericType>();
  auto velocity =
      ls::SmartPointer<ConstantVelocityField<NumericType>>::New(advectionSpeed);
  using AdvectType = ls::Advect<NumericType, 2>;
  using ExecutorStatus = typename AdvectType::LevelSetUpdateStatus;
  ls::Advect<NumericType, 2> advect;
  advect.insertNextLevelSet(domain);
  advect.setVelocityField(velocity);
  advect.setSpatialScheme(ls::SpatialSchemeEnum::ENGQUIST_OSHER_1ST_ORDER);
  advect.setTemporalScheme(temporalScheme);
  advect.setAdvectionTime(processDuration);
  advect.setTimeStepRatio(static_cast<NumericType>(0.49));
  advect.setSingleStep(true);

  unsigned executorCalls = 0;
  if (executorMode != ExecutorMode::NONE) {
    advect.setLevelSetUpdateExecutor(
        [&executorCalls, executorMode](
            const typename AdvectType::LevelSetUpdateContext &context,
            typename AdvectType::LevelSetUpdateOutput &output,
            std::string &error) {
          ++executorCalls;
          VC_TEST_ASSERT(context.timeStep > 0.0);
          VC_TEST_ASSERT(context.rates.size() ==
                         context.domain.getNumberOfSegments());
          if (executorMode == ExecutorMode::ERROR) {
            error = "injected executor failure";
            return ExecutorStatus::ERROR;
          }
          if (executorMode == ExecutorMode::INVALID_OUTPUT) {
            return ExecutorStatus::HANDLED;
          }
          if (executorMode == ExecutorMode::HANDLED_ECHO) {
            output.values.resize(context.domain.getNumberOfSegments());
            for (unsigned p = 0; p < context.domain.getNumberOfSegments();
                 ++p) {
              output.values[p] =
                  context.domain.getDomainSegment(p).definedValues;
            }
            return ExecutorStatus::HANDLED;
          }
          return ExecutorStatus::FALLBACK;
        });
  }

  const auto before = summarize(domain);
  const auto beforeFingerprint = fingerprint(before);
  advect.apply();
  const auto after = summarize(domain);
  const auto afterFingerprint = fingerprint(after);
  const auto advectedTime = advect.getAdvectedTime();

  VC_TEST_ASSERT(after.surfaceLineCount == after.surfaceNodeCount);
  VC_TEST_ASSERT(after.surfaceTriangleCount == 0);
  VC_TEST_ASSERT(after.activePointCount > 0);
  VC_TEST_ASSERT(advectedTime > 0);
  VC_TEST_ASSERT(advectedTime <= processDuration);

  return {after.activePointCount,
          after.surfaceNodeCount,
          after.surfaceLineCount,
          after.surfaceTriangleCount,
          1ull,
          advectedTime,
          afterFingerprint ^ beforeFingerprint,
          executorCalls};
};

template <class NumericType>
void assertSameOracle(const LevelSetOracle<NumericType> &left,
                      const LevelSetOracle<NumericType> &right) {
  VC_TEST_ASSERT(left.activePointCount == right.activePointCount);
  VC_TEST_ASSERT(left.surfaceNodeCount == right.surfaceNodeCount);
  VC_TEST_ASSERT(left.surfaceLineCount == right.surfaceLineCount);
  VC_TEST_ASSERT(left.surfaceTriangleCount == right.surfaceTriangleCount);
  VC_TEST_ASSERT(left.advectedTime == right.advectedTime);
  VC_TEST_ASSERT(left.fingerprint == right.fingerprint);
}

template <class NumericType>
void runBaseline(const char *label, const std::uint64_t expected,
                 const double expectedAdvectedTime) {
  auto first = runOneStep<NumericType>();
  auto second = runOneStep<NumericType>();
  auto fallback = runOneStep<NumericType>(NumericType(1), NumericType(0.5),
                                          ExecutorMode::FALLBACK);
  auto error = runOneStep<NumericType>(NumericType(1), NumericType(0.5),
                                       ExecutorMode::ERROR);
  auto invalidOutput = runOneStep<NumericType>(NumericType(1), NumericType(0.5),
                                               ExecutorMode::INVALID_OUTPUT);
  auto handledEcho = runOneStep<NumericType>(NumericType(1), NumericType(0.5),
                                             ExecutorMode::HANDLED_ECHO);

  std::cout << "CPU Level Set step oracle (" << label
            << ") advected time = " << std::setprecision(17)
            << first.advectedTime << "s\n";
  std::cout << "CPU Level Set step oracle (" << label
            << ") fingerprint: " << formatFingerprint(first.fingerprint)
            << '\n';
  std::cout << "CPU Level Set step oracle (" << label
            << ") mesh points/lines = " << first.activePointCount << '/'
            << first.surfaceLineCount << '\n';
  VC_TEST_ASSERT(first.activePointCount > 0);
  VC_TEST_ASSERT(first.step == 1);
  VC_TEST_ASSERT(first.surfaceNodeCount == second.surfaceNodeCount);
  VC_TEST_ASSERT(first.surfaceLineCount == second.surfaceLineCount);
  VC_TEST_ASSERT(first.surfaceTriangleCount == second.surfaceTriangleCount);
  VC_TEST_ASSERT(first.activePointCount == second.activePointCount);
  VC_TEST_ASSERT(first.fingerprint == second.fingerprint);
  VC_TEST_ASSERT(first.advectedTime == second.advectedTime);
  assertSameOracle(first, fallback);
  assertSameOracle(first, error);
  assertSameOracle(first, invalidOutput);
  VC_TEST_ASSERT(fallback.executorCalls == 1);
  VC_TEST_ASSERT(error.executorCalls == 1);
  VC_TEST_ASSERT(invalidOutput.executorCalls == 1);
  VC_TEST_ASSERT(handledEcho.executorCalls == 1);
  VC_TEST_ASSERT(handledEcho.fingerprint != first.fingerprint);
  VC_TEST_ASSERT(first.activePointCount == 92);
  VC_TEST_ASSERT(first.surfaceNodeCount == 68);
  VC_TEST_ASSERT(first.surfaceLineCount == 68);
  VC_TEST_ASSERT(first.advectedTime == expectedAdvectedTime);
  VC_TEST_ASSERT(first.fingerprint == expected);
}

template <class NumericType> void runTemporalFallbackParity() {
  const auto verify = [](const ls::TemporalSchemeEnum scheme,
                         const unsigned expectedCalls) {
    const auto cpu = runOneStep<NumericType>(NumericType(1), NumericType(0.5),
                                             ExecutorMode::NONE, scheme);
    const auto fallback = runOneStep<NumericType>(
        NumericType(1), NumericType(0.5), ExecutorMode::FALLBACK, scheme);
    assertSameOracle(cpu, fallback);
    VC_TEST_ASSERT(fallback.executorCalls == expectedCalls);
  };
  verify(ls::TemporalSchemeEnum::RUNGE_KUTTA_2ND_ORDER, 2);
  verify(ls::TemporalSchemeEnum::RUNGE_KUTTA_3RD_ORDER, 3);
}

} // namespace viennacore

int main() try {
  using NumericTypeF = float;
  using NumericTypeD = double;

  viennacore::Logger::setLogLevel(viennacore::LogLevel::WARNING);
  // Frozen after two deterministic local runs under MSVC C++20.
  viennacore::runBaseline<NumericTypeF>("FP32", 0x51052040fecec990ULL,
                                        0.40459004530160253);
  viennacore::runBaseline<NumericTypeD>("FP64", 0x6af00ac25d8971d0ULL,
                                        0.40459001562216945);
  viennacore::runTemporalFallbackParity<NumericTypeF>();
  viennacore::runTemporalFallbackParity<NumericTypeD>();
  return 0;
} catch (const std::exception &error) {
  std::cerr << error.what() << '\n';
  return 1;
}
