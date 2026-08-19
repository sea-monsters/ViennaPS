// CPU-authority fixture for the rank-8 SingleParticleALD fallback row.
// Ballistic transport is frozen with no evaporation or surface diffusion;
// coverage remains a serialized CPU-owned ALD field.
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

#ifndef VIENNAPS_SINGLE_PARTICLE_ALD_MOD
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
#include <models/psSingleParticleALD.hpp>
#include <process/psProcess.hpp>
#include <psDomain.hpp>

namespace {

using T = float;
constexpr int D = 2;
constexpr std::uint32_t kSeed = 42U;

constexpr std::array<const char *, 1> kLabels = {"ParticleFlux"};
constexpr char kCoverageLabel[] = "Coverage";

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

void requireCoverage(const std::vector<T> &values) {
  if (values.empty())
    throw std::runtime_error("Coverage is empty");
  for (const auto value : values) {
    if (!std::isfinite(value) || value < T(0) || value > T(1))
      throw std::runtime_error("Coverage is not finite/in [0,1]");
  }
}

void run(const std::string &outputPath) {
  using namespace viennaps;
  using viennacore::SmartPointer;

  units::Length::setUnit(units::Length::METER);
  units::Time::setUnit(units::Time::SECOND);

  SingleParticleALDParams modelParameters;
  modelParameters.stickingProbability = 1.;
  modelParameters.gasMeanFreePath = -1.;
  modelParameters.growthPerCycle = 0.;
  modelParameters.evaporationFlux = 0.;
  modelParameters.incomingFlux = 1.;
  modelParameters.s0 = 1.;
  modelParameters.coverageDiffusionCoefficient = 0.;

  auto domain = Domain<T, D>::New(T(.5), T(10), T(10));
  MakePlane<T, D>(domain, T(0)).apply();
  auto model = SmartPointer<SingleParticleALD<T, D>>::New(modelParameters);

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

  const auto coverages = model->getSurfaceModel()->getCoverages();
  if (!coverages)
    throw std::runtime_error("CPU coverage field is missing");
  const auto coverage = coverages->getScalarData(kCoverageLabel, true);
  if (!coverage)
    throw std::runtime_error("CPU Coverage label is missing");
  requireCoverage(*coverage);
  if (coverage->size() != values[0]->size())
    throw std::runtime_error("Coverage and ParticleFlux sizes differ");

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
  out << "schema=1\nsource=cpu\nmodel=SingleParticleALD\n"
         "precision=float\ndimension=2\ngridDelta=0.5\n"
         "xExtent=10\nyExtent=10\nseed=42\nraysPerPoint=1\n"
         "maxReflections=0\nnormalization=source\n"
         "engine=CPU_TRIANGLE\nroute=cpu_fallback_only\n"
         "auto_route=CPU\nmanual_vulkan=fail_closed_no_publication\n"
         "stickingProbability=1\ngasMeanFreePath=-1\n"
         "growthPerCycle=0\nevaporationFlux=0\nincomingFlux=1\n"
         "s0=1\ncoverageDiffusionCoefficient=0\n"
         "labels=ParticleFlux\ncoverage_labels=Coverage\n";
  for (std::size_t i = 0; i < kLabels.size(); ++i)
    writeValues(out, kLabels[i], *values[i]);
  writeValues(out, kCoverageLabel, *coverage);
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
    std::cerr << "usage: single_particle_ald_cpu_fixture output\n";
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
