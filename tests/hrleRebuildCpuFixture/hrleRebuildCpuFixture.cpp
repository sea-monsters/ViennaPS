// Copyright 2026 ViennaPS
// SPDX-License-Identifier: MIT

// Differential fixture for the P4 HRLE rebuild frozen mirror.
//
// The reference uses ViennaLS Advect without a rebuild executor. The mirror
// uses the same Advect seam with the local CPU classification, compaction, and
// sparse reconstruction pipeline. Both results must be bit-exact before the
// fixed upstream fingerprint is accepted.

#include <lsAdvect.hpp>
#include <lsDomain.hpp>
#include <lsMakeGeometry.hpp>
#include <lsMesh.hpp>
#include <lsToSurfaceMesh.hpp>

#include <hrleSparseStarIterator.hpp>

#include <levelset/psHrleRebuildClassification.hpp>
#include <levelset/psHrleRebuildCompaction.hpp>
#include <levelset/psHrleSparseReconstruction.hpp>

#include <vcLogger.hpp>
#include <vcTestAsserts.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <sstream>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

namespace {

namespace ls = viennals;
namespace rebuild = viennaps::levelset;

template <int D>
using AdvectType = ls::Advect<float, D>;

template <int D>
using DomainType = ls::Domain<float, D>;

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

class ConstantVelocityField final : public ls::VelocityField<float> {
public:
  float getScalarVelocity(const std::array<float, 3> &, int,
                          const std::array<float, 3> &,
                          unsigned long) override {
    return -0.5F;
  }

  std::array<float, 3>
  getVectorVelocity(const std::array<float, 3> &, int,
                    const std::array<float, 3> &, unsigned long) override {
    return {};
  }
};

template <int D>
[[nodiscard]] ls::SmartPointer<DomainType<D>> makeDomain() {
  constexpr viennahrle::CoordType extent = 12;
  constexpr viennahrle::CoordType gridDelta = 0.5;
  viennahrle::CoordType bounds[2 * D]{};
  typename DomainType<D>::BoundaryType boundaryConditions[D]{};
  for (int dimension = 0; dimension < D; ++dimension) {
    bounds[2 * dimension] = -extent;
    bounds[2 * dimension + 1] = extent;
    boundaryConditions[dimension] = ls::BoundaryConditionEnum::REFLECTIVE_BOUNDARY;
  }

  auto domain = DomainType<D>::New(bounds, boundaryConditions, gridDelta);
  float origin[D]{};
  ls::MakeGeometry<float, D>(
      domain, ls::SmartPointer<ls::Sphere<float, D>>::New(origin, 4.0F))
      .apply();

  std::vector<float> scalarData(domain->getNumberOfPoints());
  for (std::size_t index = 0U; index < scalarData.size(); ++index)
    scalarData[index] = static_cast<float>(index) + 0.25F;
  domain->getPointData().insertNextScalarData(std::move(scalarData), "fixtureId");
  return domain;
}

template <int D, class Iterator>
[[nodiscard]] bool makeRebuildCandidate(const Iterator &iterator,
                                        const std::size_t sourcePointCount,
                                        rebuild::HrleRebuildCandidateFp32 &candidate,
                                        std::string &error) {
  const auto toPointId = [](const auto &point) -> std::uint32_t {
    if (!point.isDefined())
      return rebuild::kInvalidHrlePointId;
    return static_cast<std::uint32_t>(point.getPointId());
  };

  const auto &center = iterator.getCenter();
  candidate.centerValue = center.getValue();
  candidate.centerDefinedValue =
      center.isDefined() ? center.getDefinedValue() : center.getValue();
  candidate.centerPointId = toPointId(center);
  for (std::size_t neighbor = 0U; neighbor < 2U * D; ++neighbor) {
    const auto &point = iterator.getNeighbor(static_cast<unsigned>(neighbor));
    candidate.neighborValues[neighbor] = point.getValue();
    candidate.neighborDefinedValues[neighbor] =
        point.isDefined() ? point.getDefinedValue() : point.getValue();
    candidate.neighborPointIds[neighbor] = toPointId(point);
  }

  const auto hasInvalidSourceId = [sourcePointCount](const std::uint32_t pointId) {
    return pointId != rebuild::kInvalidHrlePointId &&
           static_cast<std::size_t>(pointId) >= sourcePointCount;
  };
  if (!std::isfinite(candidate.centerValue) ||
      !std::isfinite(candidate.centerDefinedValue) ||
      hasInvalidSourceId(candidate.centerPointId)) {
    error = "HRLE fixture mirror received an invalid center candidate.";
    return false;
  }
  for (std::size_t neighbor = 0U; neighbor < 2U * D; ++neighbor) {
    if (!std::isfinite(candidate.neighborValues[neighbor]) ||
        !std::isfinite(candidate.neighborDefinedValues[neighbor]) ||
        hasInvalidSourceId(candidate.neighborPointIds[neighbor])) {
      error = "HRLE fixture mirror received an invalid neighbor candidate.";
      return false;
    }
  }
  return true;
}

template <int D>
[[nodiscard]] bool collectSegmentCandidates(
    const typename DomainType<D>::DomainType &domain,
    const viennahrle::Index<D> &start, const viennahrle::Index<D> &end,
    const std::size_t sourcePointCount,
    std::vector<rebuild::HrleRebuildCandidateFp32> &candidates,
    std::vector<viennahrle::Index<D>> &indices, std::string &error) {
  using SparseDomain = typename DomainType<D>::DomainType;
  for (viennahrle::ConstSparseStarIterator<SparseDomain, 1> iterator(domain,
                                                                      start);
       iterator.getIndices() < end; ++iterator) {
    rebuild::HrleRebuildCandidateFp32 candidate{};
    if (!makeRebuildCandidate<D>(iterator, sourcePointCount, candidate, error))
      return false;
    candidates.push_back(candidate);
    indices.push_back(iterator.getIndices());
  }
  return true;
}

template <int D>
[[nodiscard]] typename AdvectType<D>::LevelSetRebuildExecutor
makeCpuMirrorRebuildExecutor(
    const std::shared_ptr<std::size_t> &invocationCount) {
  using Status = typename AdvectType<D>::LevelSetRebuildStatus;
  return [invocationCount](
      const typename AdvectType<D>::LevelSetRebuildContext &context,
      typename AdvectType<D>::LevelSetRebuildOutput &output,
      std::string &error) -> Status {
    ++*invocationCount;
    error.clear();
    const auto sourcePointCount = context.domain.getNumberOfPoints();
    if (sourcePointCount > std::numeric_limits<std::uint32_t>::max()) {
      error = "HRLE fixture mirror source point count exceeds uint32_t.";
      return Status::ERROR;
    }

    auto replacement = DomainType<D>::New(context.domain.getGrid());
    auto &replacementDomain = replacement->getDomain();
    replacementDomain.initialize(context.domain.getNewSegmentation(),
                                 context.domain.getAllocation());
    const auto segmentCount = replacementDomain.getNumberOfSegments();
    std::vector<std::vector<unsigned>> sourceIds;
    if (context.updatePointData)
      sourceIds.resize(segmentCount);

    viennahrle::Grid<D> sourceGrid = context.domain.getGrid();
    for (unsigned segment = 0U; segment < segmentCount; ++segment) {
      const auto start =
          segment == 0U ? sourceGrid.getMinGridPoint()
                        : replacementDomain.getSegmentation()[segment - 1U];
      const auto end =
          segment + 1U < segmentCount
              ? replacementDomain.getSegmentation()[segment]
              : sourceGrid.incrementIndices(sourceGrid.getMaxGridPoint());

      std::vector<rebuild::HrleRebuildCandidateFp32> candidates;
      std::vector<viennahrle::Index<D>> indices;
      if (!collectSegmentCandidates<D>(context.domain, start, end,
                                       sourcePointCount, candidates, indices,
                                       error))
        return Status::ERROR;

      std::vector<rebuild::HrleRebuildDecisionFp32> decisions;
      if (!rebuild::classifyHrleRebuildCpu(candidates, static_cast<std::uint32_t>(D),
                                           context.cutoff, decisions, error))
        return Status::ERROR;

      rebuild::HrleRebuildCompactionResultFp32 compact;
      if (!rebuild::compactHrleRebuildDecisionsCpu(decisions, compact, error))
        return Status::ERROR;

      std::vector<std::uint32_t> segmentSourceIds;
      if (!rebuild::reconstructHrleRebuildCpuIntoSegment<D>(
              compact, indices, sourcePointCount, sourceGrid, segment,
              replacementDomain, segmentSourceIds, error))
        return Status::ERROR;
      if (context.updatePointData) {
        auto &targetIds = sourceIds[segment];
        targetIds.assign(segmentSourceIds.begin(), segmentSourceIds.end());
      }
    }

    std::vector<unsigned> flatSourceIds;
    if (context.updatePointData) {
      flatSourceIds.reserve(sourcePointCount);
      for (const auto &segmentIds : sourceIds)
        flatSourceIds.insert(flatSourceIds.end(), segmentIds.begin(),
                             segmentIds.end());
    }
    replacementDomain.finalize();
    replacementDomain.segment();

    if (context.updatePointData) {
      std::vector<std::vector<unsigned>> repartitionedIds(
          replacementDomain.getNumberOfSegments());
      std::size_t sourceCursor = 0U;
      for (unsigned segment = 0U;
           segment < replacementDomain.getNumberOfSegments(); ++segment) {
        const auto definedCount =
            replacementDomain.getDomainSegment(segment).definedValues.size();
        if (sourceCursor > flatSourceIds.size() ||
            definedCount > flatSourceIds.size() - sourceCursor) {
          error = "HRLE fixture mirror source-ID repartition mismatch.";
          return Status::ERROR;
        }
        auto &targetIds = repartitionedIds[segment];
        targetIds.insert(targetIds.end(), flatSourceIds.begin() + sourceCursor,
                         flatSourceIds.begin() + sourceCursor + definedCount);
        sourceCursor += definedCount;
      }
      if (sourceCursor != flatSourceIds.size()) {
        error = "HRLE fixture mirror source-ID repartition mismatch.";
        return Status::ERROR;
      }
      sourceIds = std::move(repartitionedIds);
    }

    typename AdvectType<D>::LevelSetRebuildOutput candidateOutput;
    candidateOutput.domain = std::move(replacement);
    candidateOutput.sourceIds = std::move(sourceIds);
    output = std::move(candidateOutput);
    return Status::HANDLED;
  };
}

template <int D>
void configureAdvection(AdvectType<D> &advect,
                        const ls::SmartPointer<DomainType<D>> &domain) {
  advect.insertNextLevelSet(domain);
  advect.setVelocityField(ls::SmartPointer<ConstantVelocityField>::New());
  advect.setSpatialScheme(ls::SpatialSchemeEnum::ENGQUIST_OSHER_1ST_ORDER);
  advect.setTemporalScheme(ls::TemporalSchemeEnum::FORWARD_EULER);
  advect.setAdvectionTime(1.0);
  advect.setUpdatePointData(true);
}

template <int D>
void assertDomainsEqual(const DomainType<D> &left, const DomainType<D> &right) {
  const auto &a = left.getDomain();
  const auto &b = right.getDomain();
  VC_TEST_ASSERT(a.getNumberOfSegments() == b.getNumberOfSegments());
  VC_TEST_ASSERT(a.getSegmentation() == b.getSegmentation());
  for (unsigned segment = 0U; segment < a.getNumberOfSegments(); ++segment) {
    const auto &as = a.getDomainSegment(segment);
    const auto &bs = b.getDomainSegment(segment);
    VC_TEST_ASSERT(as.definedValues.size() == bs.definedValues.size());
    VC_TEST_ASSERT(as.undefinedValues.size() == bs.undefinedValues.size());
    for (std::size_t index = 0U; index < as.definedValues.size(); ++index)
      VC_TEST_ASSERT(std::bit_cast<std::uint32_t>(as.definedValues[index]) ==
                     std::bit_cast<std::uint32_t>(bs.definedValues[index]));
    for (std::size_t index = 0U; index < as.undefinedValues.size(); ++index)
      VC_TEST_ASSERT(std::bit_cast<std::uint32_t>(as.undefinedValues[index]) ==
                     std::bit_cast<std::uint32_t>(bs.undefinedValues[index]));
    for (unsigned dimension = 0U; dimension < D; ++dimension) {
      VC_TEST_ASSERT(as.runTypes[dimension] == bs.runTypes[dimension]);
      VC_TEST_ASSERT(as.startIndices[dimension] == bs.startIndices[dimension]);
      VC_TEST_ASSERT(as.runBreaks[dimension] == bs.runBreaks[dimension]);
    }
  }
}

template <int D>
void assertPointDataEqual(const DomainType<D> &left, const DomainType<D> &right) {
  const auto &a = left.getPointData();
  const auto &b = right.getPointData();
  VC_TEST_ASSERT(a.getScalarDataSize() == b.getScalarDataSize());
  VC_TEST_ASSERT(a.getVectorDataSize() == b.getVectorDataSize());
  for (unsigned index = 0U; index < a.getScalarDataSize(); ++index)
    VC_TEST_ASSERT(*a.getScalarData(index) == *b.getScalarData(index));
  for (unsigned index = 0U; index < a.getVectorDataSize(); ++index) {
    const auto &leftValues = *a.getVectorData(index);
    const auto &rightValues = *b.getVectorData(index);
    VC_TEST_ASSERT(leftValues.size() == rightValues.size());
    for (std::size_t point = 0U; point < leftValues.size(); ++point)
      for (unsigned component = 0U; component < 3U; ++component)
        VC_TEST_ASSERT(leftValues[point][component] == rightValues[point][component]);
  }
}

template <class NumericType, int D>
[[nodiscard]] LevelSetSummary<NumericType>
summarize(const ls::SmartPointer<ls::Domain<NumericType, D>> &domain) {
  LevelSetSummary<NumericType> summary;
  summary.activePointCount = domain->getNumberOfPoints();

  auto surface = ls::SmartPointer<ls::Mesh<NumericType>>::New();
  ls::ToSurfaceMesh<NumericType, D>(domain, surface).apply();
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

template <int D>
[[nodiscard]] std::uint64_t expectedFingerprint() {
  static_assert(D == 2 || D == 3, "Only 2-D and 3-D fixtures are supported.");
  if constexpr (D == 2)
    return 0x0e839fa59b04e247ULL;
  else
    return 0x899ac3f2b0e90b79ULL;
}

template <int D>
void runFixture(const char *name) {
  std::cerr << "HRLE fixture " << name << ": constructing scenarios" << std::endl;
  auto reference = makeDomain<D>();
  auto mirror = makeDomain<D>();
  auto mirrorRebuildInvocations = std::make_shared<std::size_t>(0U);
  AdvectType<D> referenceAdvect;
  AdvectType<D> mirrorAdvect;
  configureAdvection(referenceAdvect, reference);
  configureAdvection(mirrorAdvect, mirror);
  mirrorAdvect.setLevelSetRebuildExecutor(
      makeCpuMirrorRebuildExecutor<D>(mirrorRebuildInvocations));

  std::cerr << "HRLE fixture " << name << ": reference advection" << std::endl;
  referenceAdvect.apply();
  VC_TEST_ASSERT(!referenceAdvect.hasLevelSetRebuildError());

  std::cerr << "HRLE fixture " << name << ": mirror advection" << std::endl;
  mirrorAdvect.apply();
  VC_TEST_ASSERT(!mirrorAdvect.hasLevelSetRebuildError());
  VC_TEST_ASSERT(*mirrorRebuildInvocations > 0U);

  std::cerr << "HRLE fixture " << name << ": exact comparison" << std::endl;
  assertDomainsEqual(*reference, *mirror);
  assertPointDataEqual(*reference, *mirror);

  const auto referenceSummary = summarize<float, D>(reference);
  const auto mirrorSummary = summarize<float, D>(mirror);
  const auto referenceFingerprint = fingerprint(referenceSummary);
  const auto mirrorFingerprint = fingerprint(mirrorSummary);
  const auto expected = expectedFingerprint<D>();

  std::cerr << "HRLE rebuild CPU fixture " << name
            << " active=" << referenceSummary.activePointCount
            << " nodes=" << referenceSummary.surfaceNodeCount
            << " lines=" << referenceSummary.surfaceLineCount
            << " tris=" << referenceSummary.surfaceTriangleCount
            << " fingerprint=" << formatFingerprint(referenceFingerprint)
            << std::endl;

  VC_TEST_ASSERT(referenceSummary.activePointCount > 0U);
  VC_TEST_ASSERT(referenceSummary.surfaceNodeCount > 0U);
  if constexpr (D == 2) {
    VC_TEST_ASSERT(referenceSummary.surfaceLineCount > 0U);
    VC_TEST_ASSERT(referenceSummary.surfaceTriangleCount == 0U);
  } else {
    VC_TEST_ASSERT(referenceSummary.surfaceLineCount == 0U);
    VC_TEST_ASSERT(referenceSummary.surfaceTriangleCount > 0U);
  }
  VC_TEST_ASSERT(referenceSummary.bboxMin[0] < referenceSummary.bboxMax[0]);
  VC_TEST_ASSERT(referenceSummary.bboxMin[1] < referenceSummary.bboxMax[1]);
  VC_TEST_ASSERT(referenceFingerprint == mirrorFingerprint);

  const char *baselineEnv = std::getenv("HRLE_REBUILD_BASELINE");
  if (baselineEnv && std::string_view(baselineEnv) == "1") {
    std::cerr << "  [BASELINE] expected fingerprint for " << name << " = "
              << formatFingerprint(referenceFingerprint) << std::endl;
    return;
  }

  VC_TEST_ASSERT(referenceFingerprint == expected);
  VC_TEST_ASSERT(mirrorFingerprint == expected);
}

} // namespace

int main() try {
  viennacore::Logger::setLogLevel(viennacore::LogLevel::WARNING);
  runFixture<2>("2D");
  runFixture<3>("3D");
  return EXIT_SUCCESS;
} catch (const std::exception &exception) {
  std::cerr << "HRLE rebuild CPU fixture failed: " << exception.what() << std::endl;
  return EXIT_FAILURE;
}
