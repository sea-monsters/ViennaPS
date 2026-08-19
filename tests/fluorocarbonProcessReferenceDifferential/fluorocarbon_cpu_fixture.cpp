// CPU-authority fixture for the rank-7 FluorocarbonEtching fallback row.
// The inventory also names PlasmaEtching, but no production
// PlasmaEtching<NumericType, D> class exists in either tree; this fixture
// records only the concrete FluorocarbonEtching CPU model.
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

#ifndef VIENNAPS_FLUOROCARBON_MOD
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
#include <models/psFluorocarbonEtching.hpp>
#include <process/psProcess.hpp>
#include <psDomain.hpp>

namespace {

using T = float;
constexpr int D = 2;
constexpr std::uint32_t kSeed = 42U;

constexpr std::array<const char *, 5> kLabels = {
    "ionSputterFlux", "ionEnhancedFlux", "ionpeFlux", "etchantFlux",
    "polyFlux"};

std::uint32_t bits(const T value) { return std::bit_cast<std::uint32_t>(value); }

void writeValues(std::ofstream &out, const char *name,
                 const std::vector<T> &values) {
  out << name << ".count=" << values.size() << '\n';
  out << name << '=' << std::hex << std::setfill('0');
  for (const auto value : values)
    out << std::setw(8) << bits(value) << ' ';
  out << std::dec << '\n';
}

void writePoints(std::ofstream &out, const char *name,
                 const std::vector<viennacore::Vec3D<T>> &points) {
  out << name << ".count=" << points.size() << '\n';
  out << name << '=' << std::hex << std::setfill('0');
  for (const auto &point : points)
    out << std::setw(8) << bits(point[0]) << ':' << std::setw(8)
        << bits(point[1]) << ':' << std::setw(8) << bits(point[2]) << ' ';
  out << std::dec << '\n';
}

void requireFiniteNonnegative(const char *label,
                             const std::vector<T> &values) {
  if (values.empty())
    throw std::runtime_error(std::string(label) + " is empty");
  for (const auto value : values) {
    if (!std::isfinite(value) || value < T(0))
      throw std::runtime_error(std::string(label) +
                               " is not finite/nonnegative");
  }
}

viennaps::FluorocarbonParameters<T>::MaterialParameters makeMaterial(
    const viennaps::Material id) {
  viennaps::FluorocarbonParameters<T>::MaterialParameters material;
  material.id = id;
  return material;
}

void run(const std::string &outputPath) {
  using namespace viennaps;
  using viennacore::SmartPointer;

  units::Length::setUnit(units::Length::METER);
  units::Time::setUnit(units::Time::SECOND);

  FluorocarbonParameters<T> modelParameters;
  modelParameters.ionFlux = T(56);
  modelParameters.etchantFlux = T(500);
  modelParameters.polyFlux = T(100);
  modelParameters.delta_p = T(1);
  modelParameters.temperature = T(300);
  modelParameters.k_ie = T(2);
  modelParameters.k_ev = T(2);
  modelParameters.etchStopDepth = std::numeric_limits<T>::lowest();
  modelParameters.Ions.meanEnergy = T(100);
  modelParameters.Ions.sigmaEnergy = T(0);
  modelParameters.Ions.exponent = T(300);
  modelParameters.addMaterial(makeMaterial(Material::Si));
  modelParameters.addMaterial(makeMaterial(Material::Mask));
  modelParameters.addMaterial(makeMaterial(Material::Polymer));

  auto domain = Domain<T, D>::New(T(.5), T(10), T(10));
  MakePlane<T, D>(domain, T(0)).apply();
  auto model = SmartPointer<FluorocarbonEtching<T, D>>::New(modelParameters);

  Process<T, D> process(domain, model, T(0));
  process.setFluxEngineType(FluxEngineType::CPU_TRIANGLE);
  RayTracingParameters rayParameters;
  rayParameters.rngSeed = kSeed;
  rayParameters.useRandomSeeds = false;
  rayParameters.raysPerPoint = 1U;
  rayParameters.maxReflections = 0U;
  rayParameters.normalizationType = viennaray::NormalizationType::SOURCE;
  process.setParameters(rayParameters);

  const auto fluxMesh = process.calculateFlux();
  if (!fluxMesh)
    throw std::runtime_error("calculateFlux returned null");

  std::array<const std::vector<T> *, kLabels.size()> values{};
  for (std::size_t i = 0; i < kLabels.size(); ++i) {
    const auto data = fluxMesh->getCellData().getScalarData(kLabels[i], true);
    if (!data)
      throw std::runtime_error(std::string("missing CPU label: ") + kLabels[i]);
    requireFiniteNonnegative(kLabels[i], *data);
    values[i] = data;
  }
  for (std::size_t i = 1; i < values.size(); ++i) {
    if (values[i]->size() != values[0]->size())
      throw std::runtime_error("Fluorocarbon CPU label sizes differ");
  }

  const auto triangleMesh = process.getTriangleMesh();
  if (!triangleMesh || triangleMesh->nodes.empty() ||
      triangleMesh->triangles.empty())
    throw std::runtime_error("triangle geometry is missing");
  for (const auto &point : triangleMesh->nodes) {
    if (!std::isfinite(point[0]) || !std::isfinite(point[1]) ||
        !std::isfinite(point[2]))
      throw std::runtime_error("triangle geometry is not finite");
  }

  std::ofstream out(outputPath, std::ios::trunc);
  if (!out)
    throw std::runtime_error("cannot open output");
  out << "schema=1\nsource=cpu\nmodel=FluorocarbonEtching\n"
         "inventory_identity=rank-7-fluorocarbon-concrete-only\n"
         "plasma_model_production_class=absent-no-evidence\n"
         "precision=float\ndimension=2\ngridDelta=0.5\n"
         "xExtent=10\nyExtent=10\nseed=42\nraysPerPoint=1\n"
         "maxReflections=0\nnormalization=source\n"
         "engine=CPU_TRIANGLE\nroute=cpu_fallback_only\n"
         "auto_route=CPU\nmanual_vulkan=fail_closed_no_publication\n"
         "ionFlux=56\netchantFlux=500\npolyFlux=100\n"
         "delta_p=1\ntemperature=300\nk_ie=2\nk_ev=2\n"
         "ionMeanEnergy=100\nionSigmaEnergy=0\nionExponent=300\n"
         "materials=Si,Mask,Polymer\n"
         "labels=ionSputterFlux,ionEnhancedFlux,ionpeFlux,etchantFlux,polyFlux\n";
  for (std::size_t i = 0; i < kLabels.size(); ++i)
    writeValues(out, kLabels[i], *values[i]);
  writePoints(out, "triangle.nodes", triangleMesh->nodes);
  out << "triangle.triangles.count=" << triangleMesh->triangles.size()
      << '\n';
  out << "triangle.triangles=";
  for (const auto &triangle : triangleMesh->triangles)
    out << triangle[0] << ':' << triangle[1] << ':' << triangle[2] << ' ';
  out << '\n';
  out << "process.levelsets=" << domain->getNumberOfLevelSets() << '\n';
}

} // namespace

int main(int argc, char **argv) {
  if (argc != 2) {
    std::cerr << "usage: fluorocarbon_cpu_fixture output\n";
    return 2;
  }
  try {
    run(argv[1]);
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "fixture failure: " << error.what() << '\n';
    return 1;
  }
}
