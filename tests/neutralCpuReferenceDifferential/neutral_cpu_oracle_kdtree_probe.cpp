// P5-N1E/N1F real-tree query probe (test-only, caller-owned).
//
// Replicates the exact CPUTriangleEngine<float,2> post-processing boundary on
// the real Neutral oracle geometry:
//   MakePlane domain -> ToDiskMesh disk mesh -> CreateSurfaceMesh ->
//   convertLinesToTriangles -> element KDTree setPoints/build ->
//   per-node findNearestWithinRadius validation -> ElementToPointData::apply()
//   (the historical prepare$omp$1 -> traverseDown crash frame).
//
// Modes (runtime selector, so the executable is linked once):
//   notrace (default): no ray-tracer execution. N1E showed this passes at
//     OMP 1/2/4/8 on both header roots, exonerating the built tree and the
//     exact post-processing frame on real data.
//   trace: after the pre-trace validation, replicate the
//     calculateSourceFluxes pre-trace coverage mapping and runRayTracer
//     (real NeutralTransport particle, raysPerPoint=1, seed=42,
//     maxReflections default), then re-validate the element tree and run
//     ElementToPointData::apply() with the traced element fluxes. This
//     discriminates whether the executed ray-trace phase invalidates the
//     tree/query state before post-processing.
//
// The probe changes no production, reference, cache, flag, or fixture
// semantics. It is diagnostic only and cannot unlock P5-N2 by itself.

#include <string>
#include <vector>

#ifndef VIENNAPS_NEUTRAL_ORACLE_MOD
#ifndef VIENNALS_USE_VTK
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
#endif // VIENNALS_USE_VTK
#endif

#include <geometries/psMakePlane.hpp>
#include <models/psNeutralTransport.hpp>
#include <psCreateSurfaceMesh.hpp>
#include <psDomain.hpp>
#include <psElementToPointData.hpp>
#include <psPointToElementData.hpp>
#include <psUtil.hpp>

#include <lsToDiskMesh.hpp>
#include <rayMesh.hpp>
#include <rayTraceTriangle.hpp>

#include <vcLogger.hpp>

#include <cmath>
#include <exception>
#include <fstream>
#include <iostream>
#include <optional>
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

void runProbe(const std::string &path, bool trace) {
  std::ofstream out(path, std::ios::trunc);
  if (!out)
    throw std::runtime_error("cannot open probe output");

  viennacore::Logger::setLogLevel(viennacore::LogLevel::DEBUG);
  if (!viennacore::Logger::setLogFile(path + ".phase.log"))
    throw std::runtime_error("cannot open probe phase log");
  writePhase("phase=probe_start");
  writePhase(std::string("phase=mode_") + (trace ? "trace" : "notrace"));

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
  auto elementKdTree = SmartPointer<KDTree<T, std::array<T, 3>>>::New();
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

  // Same ray-tracer configuration as CPUTriangleEngine::initialize (D==2).
  std::optional<viennaray::TraceTriangle<T, D>> rayTracerOpt;
  if (trace) {
    auto &rayTracer = rayTracerOpt.emplace();
    viennaray::BoundaryCondition rayBoundaryCondition[D];
    for (unsigned i = 0; i < D; ++i)
      rayBoundaryCondition[i] = viennaps::util::convertBoundaryCondition(
          domain->getGrid().getBoundaryConditions(i));
    rayTracer.setSourceDirection(viennaray::TraceDirection::POS_Y);
    rayTracer.setBoundaryConditions(rayBoundaryCondition);
    rayTracer.setNumberOfRaysPerPoint(1);
    rayTracer.setMaxBoundaryHits(1000);
    rayTracer.setUseRandomSeeds(false);
    rayTracer.setRngSeed(42);
    rayTracer.setGeometry(triangleMesh);
  }

  // Same surface-mesh refill as updateSurface lines 134-141.
  surfaceMesh->nodes = std::move(triangleMesh.nodes);
  surfaceMesh->triangles = std::move(triangleMesh.triangles);
  surfaceMesh->getCellData().insertReplaceVectorData(
      std::move(triangleMesh.normals), "Normals");
  surfaceMesh->minimumExtent = triangleMesh.minimumExtent;
  surfaceMesh->maximumExtent = triangleMesh.maximumExtent;

  // Same element material mapping as updateSurface lines 143-149.
  SmartPointer<KDTree<T, std::array<T, 3>>> pointKdTree;
  if (trace) {
    auto &rayTracer = *rayTracerOpt;
    pointKdTree = SmartPointer<KDTree<T, std::array<T, 3>>>::New();
    pointKdTree->setPoints(diskMesh->nodes);
    pointKdTree->build();
    auto const &pointMaterialIds = *diskMesh->getMaterialIds();
    std::vector<int> elementMaterialIds;
    viennaps::PointToElementDataSingle<T, T, int, float>(
        pointMaterialIds, elementMaterialIds, *pointKdTree, surfaceMesh)
        .apply();
    rayTracer.setMaterialIds(elementMaterialIds);
  }
  writePhase("phase=tree_built elements=" + std::to_string(numElements));

  // smoothingNeighbors default is 1; radius matches the engine initialize().
  const T lookupRadius = static_cast<T>(domain->getGridDelta()) * T(2);

  // Radius-query contract validation on the real tree.
  const auto &points = diskMesh->nodes;
  const auto validateTree = [&](const char *phaseLabel) {
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
    writePhase(std::string("phase=") + phaseLabel +
               " results=" + std::to_string(totalResults));
    return totalResults;
  };
  const std::size_t preTraceResults = validateTree("queries_done");

  // Trace mode: replicate calculateSourceFluxes coverage mapping and
  // runRayTracer, then re-validate the tree before post-processing.
  std::vector<std::vector<T>> elementFluxes;
  std::vector<std::string> elementFluxLabels;
  std::size_t postTraceResults = 0;
  if (trace) {
    auto &rayTracer = *rayTracerOpt;
    viennaps::NeutralTransportParameters<T> params;
    params.kEtch = T(2);
    params.surfaceSiteDensity = T(3);
    params.siliconDensity = T(6);
    params.etchFrontMaterial = viennaps::Material::Si;
    params.fluxLabel = "neutralFlux";
    params.coverageLabel = "neutralCoverage";
    auto model =
        SmartPointer<viennaps::NeutralTransport<T, D>>::New(params);
    auto surfaceModel = model->getSurfaceModel();
    surfaceModel->initializeCoverages(
        static_cast<unsigned>(diskMesh->nodes.size()));
    model->initializeParticleDataLogs();

    auto globalTracingData = PointData<T>::New();
    viennaps::PointToElementData<T, float>(
        *globalTracingData, surfaceModel->getCoverages(), *pointKdTree,
        surfaceMesh, viennacore::Logger::hasIntermediate())
        .apply();
    rayTracer.setGlobalData(globalTracingData);

    writePhase("phase=before_trace");
    unsigned particleIdx = 0;
    for (auto &particle : model->getParticleTypes()) {
      int dataLogSize = model->getParticleLogSize(particleIdx);
      if (dataLogSize > 0) {
        rayTracer.getDataLog().data.resize(1);
        rayTracer.getDataLog().data[0].resize(dataLogSize, 0.);
      }
      rayTracer.setParticleType(particle);
      rayTracer.apply();
      writePhase("phase=particle_traced idx=" + std::to_string(particleIdx));

      auto &localData = rayTracer.getLocalData();
      int numFluxes =
          static_cast<int>(particle->getLocalDataLabels().size());
      std::vector<std::vector<T>> particleFluxes;
      particleFluxes.reserve(numFluxes);
      std::vector<std::string> particleFluxLabels;
      particleFluxLabels.reserve(numFluxes);
      for (int i = 0; i < numFluxes; ++i) {
        auto flux = std::move(*localData.getScalarData(i));
        rayTracer.normalizeFlux(flux, viennaray::NormalizationType::SOURCE);
        particleFluxLabels.push_back(localData.getScalarDataLabel(i));
        particleFluxes.push_back(std::move(flux));
      }
      for (int i = 0; i < numFluxes; ++i) {
        elementFluxLabels.push_back(std::move(particleFluxLabels[i]));
        elementFluxes.push_back(std::move(particleFluxes[i]));
      }
      model->mergeParticleData(rayTracer.getDataLog(), particleIdx);
      ++particleIdx;
    }
    writePhase("phase=trace_done fluxes=" +
               std::to_string(elementFluxes.size()));

    postTraceResults = validateTree("post_trace_queries");
    for (const auto &flux : elementFluxes) {
      if (flux.size() != numElements)
        throw std::runtime_error("traced element flux size mismatch");
    }
  }

  // Exact post-processing boundary: ElementToPointData::apply() ->
  // prepare() (historical prepare$omp$1 -> traverseDown frame) -> convert().
  auto pointData = PointData<T>::New();
  viennaps::ElementToPointData<T, float, T, true, false> postProcessing;
  if (trace && !elementFluxes.empty()) {
    postProcessing.setDataLabels(elementFluxLabels);
    postProcessing.setElementDataArrays(std::move(elementFluxes));
  } else {
    postProcessing.setDataLabels({"neutralFlux"});
    postProcessing.setElementDataArrays(
        {std::vector<T>(numElements, T(1))});
  }
  postProcessing.setConversionRadius(lookupRadius);
  postProcessing.setSurfaceMesh(surfaceMesh);
  postProcessing.setElementKdTree(elementKdTree);
  postProcessing.setDiskMesh(diskMesh);
  postProcessing.setPointData(pointData);
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
      << "mode=" << (trace ? "trace" : "notrace") << '\n'
      << "precision=float\ndimension=2\ngridDelta=0.5\nxExtent=4\nyExtent=4\n"
      << "smoothingNeighbors=1\nconversionRadius=" << lookupRadius << '\n'
      << "disk_nodes=" << points.size() << '\n'
      << "elements=" << numElements << '\n'
      << "query_results=" << preTraceResults << '\n'
      << "post_trace_query_results=" << postTraceResults << '\n'
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
  if (argc < 2 || argc > 3) {
    std::cerr << "usage: neutral_cpu_oracle_kdtree_probe output [notrace|trace]\n";
    return 2;
  }
  bool trace = false;
  if (argc == 3) {
    const std::string mode = argv[2];
    if (mode == "trace") {
      trace = true;
    } else if (mode != "notrace") {
      std::cerr << "unknown mode: " << mode << '\n';
      return 2;
    }
  }
  try {
    runProbe(argv[1], trace);
  } catch (const std::exception &error) {
    std::cerr << "probe failure: " << error.what() << '\n';
    return 1;
  }
  return 0;
}
