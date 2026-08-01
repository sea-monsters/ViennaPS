#include <lsAdvect.hpp>
#include <lsDomain.hpp>
#include <lsMakeGeometry.hpp>
#include <lsMesh.hpp>
#include <lsToSurfaceMesh.hpp>

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

struct QuantizedPoint {
  std::int64_t x = 0;
  std::int64_t y = 0;
  std::int64_t z = 0;

  [[nodiscard]] bool operator<(const QuantizedPoint &other) const {
    return std::tie(x, y, z) < std::tie(other.x, other.y, other.z);
  }

  [[nodiscard]] bool operator==(const QuantizedPoint &other) const = default;
};

template <class NumericType>
[[nodiscard]] QuantizedPoint
quantize(const std::array<NumericType, 3> &point,
         const NumericType tolerance = static_cast<NumericType>(1e-6)) {
  return {static_cast<std::int64_t>(std::llround(point[0] / tolerance)),
          static_cast<std::int64_t>(std::llround(point[1] / tolerance)),
          static_cast<std::int64_t>(std::llround(point[2] / tolerance))};
}

template <class NumericType> struct LevelSetSummary {
  std::size_t activePointCount = 0;
  std::size_t surfaceNodeCount = 0;
  std::size_t surfaceLineCount = 0;
  std::size_t surfaceTriangleCount = 0;
  std::array<NumericType, 3> bboxMin{};
  std::array<NumericType, 3> bboxMax{};
  std::vector<QuantizedPoint> surfaceNodes;
};

template <class NumericType>
class ConstantVelocityField : public ls::VelocityField<NumericType> {
public:
  explicit ConstantVelocityField(const NumericType rate) : rate_(rate) {}

  NumericType getScalarVelocity(const std::array<NumericType, 3> &, int,
                                const std::array<NumericType, 3> &,
                                unsigned long) override {
    return rate_;
  }

  std::array<NumericType, 3>
  getVectorVelocity(const std::array<NumericType, 3> &, int,
                    const std::array<NumericType, 3> &,
                    unsigned long) override {
    return {};
  }

private:
  NumericType rate_;
};

template <class NumericType>
[[nodiscard]] ls::SmartPointer<ls::Domain<NumericType, 2>> runScenario() {
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
  NumericType origin[dimension] = {static_cast<NumericType>(0),
                                   static_cast<NumericType>(0)};
  ls::MakeGeometry<NumericType, dimension>(
      domain, ls::SmartPointer<ls::Sphere<NumericType, dimension>>::New(
                  origin, static_cast<NumericType>(4)))
      .apply();

  auto velocity = ls::SmartPointer<ConstantVelocityField<NumericType>>::New(
      static_cast<NumericType>(0.5));
  ls::Advect<NumericType, dimension> advect;
  advect.insertNextLevelSet(domain);
  advect.setVelocityField(velocity);
  advect.setSpatialScheme(ls::SpatialSchemeEnum::ENGQUIST_OSHER_1ST_ORDER);
  advect.setTemporalScheme(ls::TemporalSchemeEnum::FORWARD_EULER);
  advect.setAdvectionTime(static_cast<NumericType>(1));
  advect.apply();
  return domain;
}

template <class NumericType>
[[nodiscard]] LevelSetSummary<NumericType>
summarize(const ls::SmartPointer<ls::Domain<NumericType, 2>> &domain) {
  LevelSetSummary<NumericType> summary;
  summary.activePointCount = domain->getNumberOfPoints();

  auto surface = ls::SmartPointer<ls::Mesh<NumericType>>::New();
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
  std::sort(summary.surfaceNodes.begin(), summary.surfaceNodes.end());
  return summary;
}

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
  return hash;
}

[[nodiscard]] std::string formatFingerprint(const std::uint64_t value) {
  std::ostringstream output;
  output << "0x" << std::hex << std::setw(16) << std::setfill('0') << value;
  return output.str();
}

template <class NumericType>
void runBaseline(const char *precisionName,
                 const std::uint64_t expectedFingerprint) {
  const auto first = summarize(runScenario<NumericType>());
  const auto second = summarize(runScenario<NumericType>());
  const auto firstFingerprint = fingerprint(first);
  const auto secondFingerprint = fingerprint(second);

  std::cout << "CPU Level Set " << precisionName
            << " active/surface/lines: " << first.activePointCount << '/'
            << first.surfaceNodeCount << '/' << first.surfaceLineCount << '\n';
  std::cout << "CPU Level Set " << precisionName
            << " fingerprint A/B: " << formatFingerprint(firstFingerprint)
            << '/' << formatFingerprint(secondFingerprint) << '\n';

  VC_TEST_ASSERT(first.activePointCount > 0);
  VC_TEST_ASSERT(first.surfaceNodeCount > 0);
  VC_TEST_ASSERT(first.surfaceLineCount > 0);
  VC_TEST_ASSERT(first.surfaceTriangleCount == 0);
  VC_TEST_ASSERT(first.bboxMin[0] < first.bboxMax[0]);
  VC_TEST_ASSERT(first.bboxMin[1] < first.bboxMax[1]);
  VC_TEST_ASSERT(first.surfaceNodes == second.surfaceNodes);
  VC_TEST_ASSERT(firstFingerprint == secondFingerprint);
  VC_TEST_ASSERT(first.activePointCount == 96);
  VC_TEST_ASSERT(first.surfaceNodeCount == 68);
  VC_TEST_ASSERT(first.surfaceLineCount == 68);
  VC_TEST_ASSERT(firstFingerprint == expectedFingerprint);
}

} // namespace viennacore

int main() {
  viennacore::Logger::setLogLevel(viennacore::LogLevel::WARNING);
  viennacore::runBaseline<float>("FP32", 0xd3a34f98ceafe71bULL);
  viennacore::runBaseline<double>("FP64", 0x39e664622bea5583ULL);
  return 0;
}
