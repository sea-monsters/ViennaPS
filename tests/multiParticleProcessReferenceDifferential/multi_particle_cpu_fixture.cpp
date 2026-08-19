// CPU-authority fixture for the rank-2 MultiParticleProcess inventory row.
// This is a reference differential and fallback contract only: it does not
// claim a Vulkan species/energy/material route.
#include <bit>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#ifndef VIENNAPS_MULTI_PARTICLE_MOD
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
#include <models/psMultiParticleProcess.hpp>
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

void writeLines(std::ofstream &out, const char *name,
                const std::vector<std::array<unsigned, 2>> &lines) {
  out << name << ".count=" << lines.size() << '\n';
  out << name << '=';
  for (const auto &line : lines)
    out << line[0] << ':' << line[1] << ' ';
  out << '\n';
}

void run(const std::string &outputPath) {
  using namespace viennaps;
  using viennacore::SmartPointer;

  units::Length::setUnit(units::Length::METER);
  units::Time::setUnit(units::Time::SECOND);

  auto domain = Domain<T, D>::New(T(.5), T(10), T(10));
  MakePlane<T, D>(domain, T(0)).apply();

  auto model = SmartPointer<MultiParticleProcess<T, D>>::New();
  // Both particles are intentionally fully sticking. This freezes a
  // no-reflection CPU row while retaining two independent species labels.
  model->addNeutralParticle(T(1));
  model->addIonParticle(T(1000));

  Process<T, D> process(domain, model, T(0));
  process.setFluxEngineType(FluxEngineType::CPU_TRIANGLE);
  RayTracingParameters parameters;
  parameters.rngSeed = 42U;
  parameters.useRandomSeeds = false;
  parameters.raysPerPoint = 1U;
  parameters.maxReflections = 0U;
  parameters.normalizationType = viennaray::NormalizationType::SOURCE;
  process.setParameters(parameters);

  const auto fluxMesh = process.calculateFlux();
  if (!fluxMesh)
    throw std::runtime_error("calculateFlux returned null");
  const auto neutral =
      fluxMesh->getCellData().getScalarData("neutralFlux0", true);
  const auto ion = fluxMesh->getCellData().getScalarData("ionFlux1", true);
  if (!neutral || !ion || neutral->empty() || neutral->size() != ion->size())
    throw std::runtime_error("multi-particle flux labels are incomplete");
  for (const auto value : *neutral)
    if (!std::isfinite(value) || value < T(0))
      throw std::runtime_error("neutral flux is not finite/nonnegative");
  for (const auto value : *ion)
    if (!std::isfinite(value) || value < T(0))
      throw std::runtime_error("ion flux is not finite/nonnegative");

  const auto triangleMesh = process.getTriangleMesh();
  if (!triangleMesh || triangleMesh->nodes.empty() ||
      triangleMesh->triangles.empty())
    throw std::runtime_error("triangle geometry is missing");

  std::ofstream out(outputPath, std::ios::trunc);
  if (!out)
    throw std::runtime_error("cannot open output");
  out << "schema=1\nsource=cpu\n"
      << "precision=float\ndimension=2\ngridDelta=0.5\n"
         "xExtent=10\nyExtent=10\nseed=42\nraysPerPoint=1\n"
         "maxReflections=0\nnormalization=source\n"
         "route=cpu_fallback_only\n";
  writeValues(out, "neutralFlux0", *neutral);
  writeValues(out, "ionFlux1", *ion);
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
    std::cerr << "usage: multi_particle_cpu_fixture output\n";
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
