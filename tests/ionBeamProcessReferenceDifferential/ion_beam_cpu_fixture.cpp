// CPU-authority fixture for the rank-3 IonBeamEtching inventory row.
// It records CPU parity and fallback evidence only; no Vulkan IBE route is
// admitted by this fixture.
#include <bit>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <string>

#ifndef VIENNAPS_ION_BEAM_MOD
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
#include <models/psIonBeamEtching.hpp>
#include <process/psProcess.hpp>
#include <psDomain.hpp>

namespace {

using T = float;
constexpr int D = 2;

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

void run(const std::string &outputPath) {
  using namespace viennaps;
  using viennacore::SmartPointer;

  units::Length::setUnit(units::Length::METER);
  units::Time::setUnit(units::Time::SECOND);

  IBEParameters<T> parameters;
  parameters.planeWaferRate = T(1);
  parameters.meanEnergy = T(100);
  parameters.sigmaEnergy = T(0);
  parameters.thresholdEnergy = T(20);
  parameters.exponent = T(1);
  parameters.inflectAngle = T(89);
  parameters.minAngle = T(85);
  parameters.thetaRMin = T(0);
  parameters.thetaRMax = T(90);
  parameters.tiltAngle = T(0);
  parameters.rotatingWafer = false;
  parameters.redepositionRate = T(0);

  auto domain = Domain<T, D>::New(T(.5), T(10), T(10));
  MakePlane<T, D>(domain, T(0)).apply();
  auto model = SmartPointer<IonBeamEtching<T, D>>::New(parameters);

  Process<T, D> process(domain, model, T(0));
  process.setFluxEngineType(FluxEngineType::CPU_TRIANGLE);
  RayTracingParameters rayParameters;
  rayParameters.rngSeed = 42U;
  rayParameters.useRandomSeeds = false;
  rayParameters.raysPerPoint = 1U;
  rayParameters.maxReflections = 0U;
  rayParameters.normalizationType = viennaray::NormalizationType::SOURCE;
  process.setParameters(rayParameters);

  const auto fluxMesh = process.calculateFlux();
  if (!fluxMesh)
    throw std::runtime_error("calculateFlux returned null");
  const auto ion = fluxMesh->getCellData().getScalarData("ionFlux", true);
  const auto redeposition =
      fluxMesh->getCellData().getScalarData("redepositionFlux", true);
  if (!ion || !redeposition || ion->empty() || ion->size() != redeposition->size())
    throw std::runtime_error("ion-beam flux labels are incomplete");
  for (const auto value : *ion)
    if (!std::isfinite(value) || value < T(0))
      throw std::runtime_error("ion flux is not finite/nonnegative");
  for (const auto value : *redeposition)
    if (!std::isfinite(value) || value < T(0))
      throw std::runtime_error("redeposition flux is not finite/nonnegative");

  const auto triangleMesh = process.getTriangleMesh();
  if (!triangleMesh || triangleMesh->nodes.empty() ||
      triangleMesh->triangles.empty())
    throw std::runtime_error("triangle geometry is missing");

  std::ofstream out(outputPath, std::ios::trunc);
  if (!out)
    throw std::runtime_error("cannot open output");
  out << "schema=1\nsource=cpu\nprecision=float\ndimension=2\n"
         "gridDelta=0.5\nxExtent=10\nyExtent=10\nseed=42\n"
         "raysPerPoint=1\nmaxReflections=0\nnormalization=source\n"
         "route=cpu_fallback_only\nmeanEnergy=100\nthresholdEnergy=20\n";
  writeValues(out, "ionFlux", *ion);
  writeValues(out, "redepositionFlux", *redeposition);
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
    std::cerr << "usage: ion_beam_cpu_fixture output\n";
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
