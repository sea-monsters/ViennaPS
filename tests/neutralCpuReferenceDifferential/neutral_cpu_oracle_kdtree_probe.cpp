// P5-N1E real-tree query probe (test-only, caller-owned).
//
// Replicates the exact CPUTriangleEngine<float,2> post-processing boundary on
// the real Neutral oracle geometry WITHOUT executing the ray tracer:
//   MakePlane domain -> ToDiskMesh disk mesh -> CreateSurfaceMesh ->
//   convertLinesToTriangles -> element KDTree setPoints/build ->
//   per-node findNearestWithinRadius validation -> ElementToPointData::apply()
//   (the historical prepare$omp$1 -> traverseDown crash frame).
//
// Discrimination contract:
//  - A crash or contract failure here proves the Release failure does NOT
//    require the ray-trace phase; the boundary is the real-data mesh/tree
//    construction itself (ViennaCore/viennaps algorithm side).
//  - A clean pass proves the built tree is queryable on real data, so the
//    composed failure must be induced during the ray-trace phase; that
//    direction requires a separately approved boundary.
// The probe changes no production, reference, cache, flag, or fixture
// semantics. It is diagnostic only and cannot unlock P5-N2 by itself.

#include <string>
#include <vector>

#ifndef VIENNAPS_NEUTRAL_ORACLE_MOD
namespace viennals {
template <typename T> class VTKWriter;
template <typename T, int D> class WriteVisualizationMesh {
public:
  void setFileName(const std::string &) {}
  void setWrappingLayerEpsilon(double) {}
  template <typename LevelSet> void insertNextLevelSet(LevelSet) {}
  template <typename MaterialMap> void setMaterialMap(const MaterialMap &) {}
  template <typename MetaData> void setMetaData(const MetaData &) {}
  void apply() {}
};
} // namespace viennals
#endif

#include <geometries/psMakePlane.hpp>
#include <psCreateSurfaceMesh.hpp>
#include <psDomain.hpp>
#include <psElementToPointData.hpp>

#include <lsToDiskMesh.hpp>
#include <rayMesh.hpp>

#include <vcLogger.hpp>

#include <cmath>
#include <exception>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

using namespace viennacore;

using T = float;
constexpr int D = 2;

void writePhase(const std::string &message) {
  viennacore::Logger::getInstance().addDebug(message).print();
}

void runProbe(const std::string &path) {
  std::ofstream out(path, std::ios::trunc);
  if (!out)
    throw std::runtime_error("cannot open probe output");

  viennacore::Logger::setLogLevel(viennacore::LogLevel::DEBUG);
  if (!viennacore::Logger::setLogFile(path + ".phase.log"))
    throw std::runtime_error("cannot open probe phase log");
  writePhase("phase=probe_start");

  // Same geometry as the paired Neutral oracle fixture.
  auto domain = viennaps::Domain<T, D>::New(T(.5), T(4), T(4));
  viennaps::MakePlane<T, D>(domain, T(0)).apply();

  // Same disk-mesh generation as FluxProcessStrategy::setupProcess.
  auto diskMesh = viennals::Mesh<T>::New();
  viennals::ToDiskMesh<T, D> meshGenerator;
  meshGenerator.setMesh(diskMesh);
  for (auto &levelSet : domain->getLevelSets())
    meshGenerator.insertNextLevelSet(levelSet);
  auto translator = SmartPointer<
      std::unordered_map<unsigned long, unsigned long>>::New();
  meshGenerator.setTranslator(translator);
  if (domain->getMaterialMap() &&
      domain->getMaterialMap()->size() == domain->getLevelSets().size())
    meshGenerator.setMaterialMap(domain->getMaterialMap()->getMaterialMap());
  meshGenerator.apply();
  if (diskMesh->nodes.empty())
    throw std::runtime_error("disk mesh is empty");
  writePhase("phase=disk_mesh nodes=" + std::to_string(diskMesh->nodes.size()));

  // Same element tree construction as CPUTriangleEngine::updateSurface (D==2).
  auto surfaceMesh = viennals::Mesh<float>::New();
  auto elementKdTree =
      SmartPointer<KDTree<T, std::array<T, 3>>>::New();
  viennaps::CreateSurfaceMesh<T, float, D>(domain->getLevelSets().back(),
                                           surfaceMesh, elementKdTree, 1e-12,
                                           0.05)
      .apply();

  viennaray::LineMesh lineMesh;
  lineMesh.gridDelta = static_cast<float>(domain->getGridDelta());
  lineMesh.lines = std::move(surfaceMesh->lines);
  lineMesh.nodes = std::move(surfaceMesh->nodes);
  lineMesh.minimumExtent = surfaceMesh->minimumExtent;
  lineMesh.maximumExtent = surfaceMesh->maximumExtent;
  auto triangleMesh = viennaray::convertLinesToTriangles(lineMesh);
  if (triangleMesh.triangles.empty())
    throw std::runtime_error("triangle conversion produced no elements");

  std::vector<Vec3D<T>> triangleCenters;
  triangleCenters.reserve(triangleMesh.triangles.size());
  for (const auto &tri : triangleMesh.triangles) {
    Vec3D<T> center = {0, 0, 0};
    for (int i = 0; i < 3; ++i) {
      center[0] += triangleMesh.nodes[tri[i]][0];
      center[1] += triangleMesh.nodes[tri[i]][1];
      center[2] += triangleMesh.nodes[tri[i]][2];
    }
    triangleCenters.push_back(center / static_cast<T>(3.0));
  }
  elementKdTree->setPoints(triangleCenters);
  elementKdTree->build();
  const auto numElements = elementKdTree->getNumberOfPoints();

  surfaceMesh->nodes = std::move(triangleMesh.nodes);
  surfaceMesh->triangles = std::move(triangleMesh.triangles);
  surfaceMesh->getCellData().insertReplaceVectorData(
      std::move(triangleMesh.normals), "Normals");
  surfaceMesh->minimumExtent = triangleMesh.minimumExtent;
  surfaceMesh->maximumExtent = triangleMesh.maximumExtent;
  writePhase("phase=tree_built elements=" + std::to_string(numElements));

  // smoothingNeighbors default is 1; radius matches the engine initialize().
  const T lookupRadius = static_cast<T>(domain->getGridDelta()) * T(2);

  // Direct radius-query validation on the real tree (no ray tracer executed).
  const auto &points = diskMesh->nodes;
  std::size_t totalResults = 0;
  for (std::size_t i = 0; i < points.size(); ++i) {
    auto close =
        elementKdTree->findNearestWithinRadius(points[i], lookupRadius);
    if (!close)
      throw std::runtime_error("radius query returned no result");
    for (const auto &hit : *close) {
      if (hit.first >= numElements || !std::isfinite(hit.second) ||
          hit.second > lookupRadius + T(1e-5))
        throw std::runtime_error("radius query result out of contract");
    }
    totalResults += close->size();
  }
  writePhase("phase=queries_done results=" + std::to_string(totalResults));

  // Exact post-processing boundary: ElementToPointData::apply() ->
  // prepare() (historical prepare$omp$1 -> traverseDown frame) -> convert().
  auto pointData = PointData<T>::New();
  viennaps::ElementToPointData<T, float, T, true, false> postProcessing;
  postProcessing.setDataLabels({"neutralFlux"});
  postProcessing.setConversionRadius(lookupRadius);
  postProcessing.setSurfaceMesh(surfaceMesh);
  postProcessing.setElementKdTree(elementKdTree);
  postProcessing.setDiskMesh(diskMesh);
  postProcessing.setPointData(pointData);
  postProcessing.setElementDataArrays(
      {std::vector<T>(numElements, T(1))});
  writePhase("phase=before_apply");
  postProcessing.apply();
  writePhase("phase=after_apply");

  const auto *converted = pointData->getScalarData(0);
  if (converted == nullptr || converted->size() != points.size())
    throw std::runtime_error("converted point data missing or mis-sized");

  out << "schema=1\nprobe=real-tree-query\nsource="
#ifdef VIENNAPS_NEUTRAL_ORACLE_MOD
      << "mod\n"
#else
      << "reference\n"
#endif
      << "precision=float\ndimension=2\ngridDelta=0.5\nxExtent=4\nyExtent=4\n"
      << "smoothingNeighbors=1\nconversionRadius=" << lookupRadius << '\n'
      << "disk_nodes=" << points.size() << '\n'
      << "elements=" << numElements << '\n'
      << "query_results=" << totalResults << '\n'
      << "point_data_count=" << converted->size() << '\n'
      << "result=PROBE_PASS\n";
  out.flush();
  writePhase("phase=probe_output_bytes=" +
             std::to_string(static_cast<long long>(out.tellp())));
  writePhase("phase=probe_complete");
  viennacore::Logger::closeLogFile();
}

} // namespace

int main(int argc, char **argv) {
  if (argc != 2) {
    std::cerr << "usage: neutral_cpu_oracle_kdtree_probe output\n";
    return 2;
  }
  try {
    runProbe(argv[1]);
  } catch (const std::exception &error) {
    std::cerr << "probe failure: " << error.what() << '\n';
    return 1;
  }
  return 0;
}
