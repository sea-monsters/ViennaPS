#include <models/psIsotropicProcess.hpp>
#include <process/psProcess.hpp>

#include <geometries/psMakeTrench.hpp>
#include <psDomain.hpp>

#include <vcLogger.hpp>
#include <vcTestAsserts.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <tuple>
#include <type_traits>
#include <vector>

namespace viennacore {

using namespace viennaps;

namespace {

template <class NumericType> struct QuantizedPoint3D {
  long long x;
  long long y;
  long long z;

  bool operator<(const QuantizedPoint3D &other) const {
    return std::tie(x, y, z) < std::tie(other.x, other.y, other.z);
  }

  bool operator==(const QuantizedPoint3D &other) const {
    return x == other.x && y == other.y && z == other.z;
  }
};

template <class NumericType>
QuantizedPoint3D<NumericType> quantize(const Vec3D<NumericType> &value,
                                       const NumericType tol = 1e-7) {
  return {static_cast<long long>(std::llround(value[0] / tol)),
          static_cast<long long>(std::llround(value[1] / tol)),
          static_cast<long long>(std::llround(value[2] / tol))};
}

template <class NumericType>
std::vector<long long>
getMaterialIds(const SmartPointer<Domain<NumericType, 2>> &domain) {
  std::vector<long long> materialIds;
  auto materialMap = domain->getMaterialMap();
  if (!materialMap) {
    return materialIds;
  }

  materialIds.reserve(materialMap->size());
  for (std::size_t i = 0; i < materialMap->size(); i++) {
    materialIds.push_back(materialMap->getMaterialIdAtIdx(i));
  }
  return materialIds;
}

template <class NumericType> struct StructureSummary {
  std::size_t numLevelSets = 0;
  std::size_t numMaterials = 0;
  std::size_t nodeCount = 0;
  // ViennaLS 5.8.5 publishes the disk mesh as a point cloud: one vertex per
  // extracted interface point plus cell data. Line/triangle element arrays
  // are structurally empty for this mesh type.
  std::size_t vertexCount = 0;
  std::size_t lineCount = 0;
  std::size_t triangleCount = 0;
  std::array<NumericType, 3> bboxMin{};
  std::array<NumericType, 3> bboxMax{};
  std::vector<QuantizedPoint3D<NumericType>> quantizedNodes;
  std::vector<long long> materialIds;
};

namespace {

template <class NumericType> NumericType sanitizeZero(const NumericType value) {
  if (value == static_cast<NumericType>(0.0)) {
    return static_cast<NumericType>(0.0);
  }
  return value;
}

template <class NumericType>
std::uint64_t hashNumeric(const NumericType value) {
  const auto normalized = sanitizeZero(value);
  if constexpr (std::is_same_v<NumericType, float>) {
    return static_cast<std::uint64_t>(std::bit_cast<std::uint32_t>(normalized));
  } else if constexpr (std::is_same_v<NumericType, double>) {
    return std::bit_cast<std::uint64_t>(normalized);
  } else {
    static_assert(std::is_integral_v<NumericType> ||
                      std::is_floating_point_v<NumericType>,
                  "Unexpected numeric type for hashNumeric");
    return static_cast<std::uint64_t>(normalized);
  }
}

template <class NumericType>
std::uint64_t computeFingerprint(const StructureSummary<NumericType> &summary) {
  constexpr std::uint64_t kFnvOffset = 1469598103934665603ull;
  constexpr std::uint64_t kFnvPrime = 1099511628211ull;
  auto mix = [](std::uint64_t &hash, const std::uint64_t value) {
    hash ^= value;
    hash *= kFnvPrime;
  };

  std::uint64_t digest = kFnvOffset;
  mix(digest, static_cast<std::uint64_t>(summary.numLevelSets));
  mix(digest, static_cast<std::uint64_t>(summary.numMaterials));
  mix(digest, static_cast<std::uint64_t>(summary.nodeCount));
  mix(digest, static_cast<std::uint64_t>(summary.lineCount));
  mix(digest, static_cast<std::uint64_t>(summary.triangleCount));
  for (const auto &material : summary.materialIds) {
    mix(digest, static_cast<std::uint64_t>(material));
  }
  for (const auto &coord : summary.bboxMin) {
    mix(digest, hashNumeric(coord));
  }
  for (const auto &coord : summary.bboxMax) {
    mix(digest, hashNumeric(coord));
  }
  for (const auto &node : summary.quantizedNodes) {
    mix(digest, static_cast<std::uint64_t>(node.x));
    mix(digest, static_cast<std::uint64_t>(node.y));
    mix(digest, static_cast<std::uint64_t>(node.z));
  }
  return digest;
}

std::string formatFingerprint(const std::uint64_t fingerprint) {
  std::ostringstream oss;
  oss << "0x" << std::hex << std::setw(16) << std::setfill('0') << fingerprint;
  return oss.str();
}

} // namespace

template <class NumericType>
StructureSummary<NumericType>
makeStructureSummary(const SmartPointer<Domain<NumericType, 2>> &domain) {
  StructureSummary<NumericType> summary;
  summary.numLevelSets = domain->getLevelSets().size();
  auto materialMap = domain->getMaterialMap();
  summary.numMaterials = materialMap ? materialMap->size() : 0;
  summary.materialIds = getMaterialIds(domain);

  auto mesh = domain->getDiskMesh();
  summary.nodeCount = mesh->nodes.size();
  summary.vertexCount = mesh->vertices.size();
  summary.lineCount = mesh->lines.size();
  summary.triangleCount = mesh->triangles.size();

  const auto boundingBox = domain->getBoundingBox();
  summary.bboxMin[0] = boundingBox[0][0];
  summary.bboxMin[1] = boundingBox[0][1];
  summary.bboxMin[2] = boundingBox[0][2];
  summary.bboxMax[0] = boundingBox[1][0];
  summary.bboxMax[1] = boundingBox[1][1];
  summary.bboxMax[2] = boundingBox[1][2];

  summary.quantizedNodes.reserve(mesh->nodes.size());
  for (const auto &node : mesh->nodes) {
    summary.quantizedNodes.push_back(quantize(node));
  }
  std::sort(summary.quantizedNodes.begin(), summary.quantizedNodes.end());

  return summary;
}

template <class NumericType>
void printSummary(const StructureSummary<NumericType> &summary,
                  const std::string &label) {
  std::cout << "==== " << label << " ====\n";
  std::cout << "LevelSets: " << summary.numLevelSets << "\n";
  std::cout << "Materials: " << summary.numMaterials << "\n";
  std::cout << "Mesh nodes/vertices/lines/triangles: " << summary.nodeCount
            << "/" << summary.vertexCount << "/" << summary.lineCount << "/"
            << summary.triangleCount << "\n";
  std::cout << "Bounding box min: [" << summary.bboxMin[0] << ", "
            << summary.bboxMin[1] << ", " << summary.bboxMin[2] << "]\n";
  std::cout << "Bounding box max: [" << summary.bboxMax[0] << ", "
            << summary.bboxMax[1] << ", " << summary.bboxMax[2] << "]\n";
  std::cout << "Fingerprint: " << formatFingerprint(computeFingerprint(summary))
            << "\n";
}

template <class NumericType>
void assertNear(const NumericType lhs, const NumericType rhs,
                const NumericType tol = static_cast<NumericType>(1e-12)) {
  VC_TEST_ASSERT(std::abs(lhs - rhs) <= tol);
}

template <class NumericType>
void assertSummariesEqual(const StructureSummary<NumericType> &a,
                          const StructureSummary<NumericType> &b) {
  VC_TEST_ASSERT(a.numLevelSets == b.numLevelSets);
  VC_TEST_ASSERT(a.numMaterials == b.numMaterials);
  VC_TEST_ASSERT(a.materialIds == b.materialIds);
  VC_TEST_ASSERT(a.nodeCount == b.nodeCount);
  VC_TEST_ASSERT(a.vertexCount == b.vertexCount);
  VC_TEST_ASSERT(a.lineCount == b.lineCount);
  VC_TEST_ASSERT(a.triangleCount == b.triangleCount);
  VC_TEST_ASSERT(a.quantizedNodes == b.quantizedNodes);
  for (int i = 0; i < 3; i++) {
    assertNear(a.bboxMin[i], b.bboxMin[i]);
    assertNear(a.bboxMax[i], b.bboxMax[i]);
  }
}

template <class NumericType>
SmartPointer<Domain<NumericType, 2>> runSimpleScenario() {
  auto domain = Domain<NumericType, 2>::New();
  MakeTrench<NumericType, 2>(domain, 1., 10., 10., 2.5, 5., 10., 1., false,
                             true, Material::Si)
      .apply();

  auto model =
      SmartPointer<IsotropicProcess<NumericType, 2>>::New(1.0, Material::Mask);
  Process<NumericType, 2> process(domain, model, 2.);
  process.setFluxEngineType(FluxEngineType::CPU_DISK);
  process.apply();
  return domain;
}

template <class NumericType> void RunTest() {
  Logger::setLogLevel(LogLevel::WARNING);

  auto runA = runSimpleScenario<NumericType>();
  auto runB = runSimpleScenario<NumericType>();

  auto summaryA = makeStructureSummary(runA);
  auto summaryB = makeStructureSummary(runB);
  const auto fingerprintA = computeFingerprint(summaryA);
  const auto fingerprintB = computeFingerprint(summaryB);

  printSummary(summaryA, "CPU baseline run A");
  printSummary(summaryB, "CPU baseline run B");
  std::cout << "Fingerprint A: " << formatFingerprint(fingerprintA) << "\n";
  std::cout << "Fingerprint B: " << formatFingerprint(fingerprintB) << "\n";

  VC_TEST_ASSERT(summaryA.numLevelSets > 0);
  VC_TEST_ASSERT(summaryA.numMaterials > 0);
  VC_TEST_ASSERT(summaryA.nodeCount > 0);
  // Disk-mesh point-cloud contract: every extracted interface point is
  // published as exactly one vertex; no line/triangle elements exist.
  VC_TEST_ASSERT(summaryA.vertexCount == summaryA.nodeCount);
  VC_TEST_ASSERT(summaryA.lineCount == 0);
  VC_TEST_ASSERT(summaryA.triangleCount == 0);
  VC_TEST_ASSERT(summaryA.bboxMin[0] < summaryA.bboxMax[0]);
  VC_TEST_ASSERT(summaryA.bboxMin[1] < summaryA.bboxMax[1]);
  VC_TEST_ASSERT(fingerprintA == fingerprintB);
  assertSummariesEqual(summaryA, summaryB);
}

} // namespace

} // namespace viennacore

int main() {
  // VC_TEST_ASSERT throws; surface the failing condition instead of letting
  // the uncaught exception terminate via an opaque fail-fast code.
  try {
    viennacore::RunTest<double>();
    viennacore::RunTest<float>();
  } catch (const std::exception &e) {
    std::cerr << "vulkanCpuBaseline FAILED: " << e.what() << std::endl;
    return 3;
  }
  return 0;
}
