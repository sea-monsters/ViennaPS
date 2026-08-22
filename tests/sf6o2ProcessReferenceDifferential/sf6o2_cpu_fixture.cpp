// CPU-authority fixture for the rank-5 SF6O2Etching fallback row.
// This is a five-label CPU differential only. It does not admit a Vulkan
// plasma transport or surface route and does not widen backend eligibility.
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

#ifndef VIENNAPS_SF6O2_MOD
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
#include <models/psSF6O2Etching.hpp>
#include <process/psProcess.hpp>
#include <psDomain.hpp>

namespace {

using T = float;
constexpr int D = 2;
constexpr std::uint32_t kSeed = 42U;

constexpr std::array<const char *, 5> kLabels = {
    "ionSputterFlux", "ionEnhancedFlux", "ionEnhancedPassivationFlux",
    "etchantFlux", "passivationFlux"};

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

void run(const std::string &outputPath) {
  using namespace viennaps;
  using viennacore::SmartPointer;

  units::Length::setUnit(units::Length::METER);
  units::Time::setUnit(units::Time::SECOND);

  PlasmaEtchingParameters<T> modelParameters;
  modelParameters.ionFlux = T(12);
  modelParameters.etchantFlux = T(1800);
  modelParameters.passivationFlux = T(100);
  modelParameters.beta_E.set(Material::Si, T(.7));
  modelParameters.beta_E.set(Material::Mask, T(.7));
  modelParameters.beta_P.set(Material::Si, T(1));
  modelParameters.beta_P.set(Material::Mask, T(1));
  modelParameters.etchStopDepth = std::numeric_limits<T>::lowest();
  modelParameters.Ions.meanEnergy = T(100);
  modelParameters.Ions.sigmaEnergy = T(0);
  modelParameters.Ions.exponent = T(300);

  auto domain = Domain<T, D>::New(T(.5), T(10), T(10));
  MakePlane<T, D>(domain, T(0)).apply();
  auto model = SmartPointer<SF6O2Etching<T, D>>::New(modelParameters);

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
      throw std::runtime_error("SF6O2 CPU label sizes differ");
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
  out << "schema=1\nsource=cpu\nmodel=SF6O2Etching\n"
         "precision=float\ndimension=2\ngridDelta=0.5\n"
         "xExtent=10\nyExtent=10\nseed=42\nraysPerPoint=1\n"
         "maxReflections=0\nnormalization=source\n"
         "engine=CPU_TRIANGLE\nroute=cpu_fallback_only\n"
         "auto_route=CPU\nmanual_vulkan=fail_closed_no_publication\n"
         "labels=ionSputterFlux,ionEnhancedFlux,"
         "ionEnhancedPassivationFlux,etchantFlux,passivationFlux\n";
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
    std::cerr << "usage: sf6o2_cpu_fixture output\n";
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
