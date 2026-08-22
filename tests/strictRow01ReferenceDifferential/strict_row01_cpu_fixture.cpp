// Caller-owned CPU reference fixture for the strict P5 Row-01 predicate.
// It uses no Vulkan, no executor override, and no model-specific shortcut.
#include <bit>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#ifndef VIENNAPS_STRICT_ROW_MOD
// Parse-only no-VTK definitions for the unmodified reference include closure.
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
#include <models/psSingleParticleProcess.hpp>
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
  for (const auto &point : points) {
    out << std::setw(8) << bits(point[0]) << ':' << std::setw(8)
        << bits(point[1]) << ':' << std::setw(8) << bits(point[2]) << ' ';
  }
  out << std::dec << '\n';
}

void run(const std::string &outputPath) {
  using namespace viennaps;
  using viennacore::SmartPointer;
  units::Length::setUnit(units::Length::METER);
  units::Time::setUnit(units::Time::SECOND);

  auto domain = Domain<T, D>::New(T(.5), T(10), T(10));
  MakePlane<T, D>(domain, T(0)).apply();
  auto model = SmartPointer<SingleParticleProcess<T, D>>::New(T(1), T(1),
                                                                T(1));
  Process<T, D> process(domain, model, T(0));
  process.setFluxEngineType(FluxEngineType::CPU_TRIANGLE);
  RayTracingParameters params;
  params.rngSeed = 42U;
  params.useRandomSeeds = false;
  params.raysPerPoint = 1U;
  params.maxReflections = 0U;
  params.normalizationType = viennaray::NormalizationType::SOURCE;
  process.setParameters(params);

  const auto diskMesh = process.calculateFlux();
  if (!diskMesh)
    throw std::runtime_error("calculateFlux returned null");
  const auto flux = diskMesh->getCellData().getScalarData("particleFlux", true);
  if (!flux || flux->empty())
    throw std::runtime_error("particleFlux output missing");
  const auto surface = domain->getSurfaceMesh();
  if (!surface)
    throw std::runtime_error("surface mesh missing");

  std::ofstream output(outputPath, std::ios::trunc);
  if (!output)
    throw std::runtime_error("cannot open output");
  output << "schema=1\nprecision=float\ndimension=2\ngridDelta=0.5\n"
            "xExtent=10\nyExtent=10\nseed=42\nraysPerPoint=1\n"
            "normalization=source\nmaxReflections=0\nfluxLabel=particleFlux\n";
  writeValues(output, "flux", *flux);
  writePoints(output, "geometry.nodes", surface->nodes);
  output << "geometry.lines.count=" << surface->lines.size() << '\n';
  output << "geometry.lines=";
  for (const auto &line : surface->lines)
    output << line[0] << ':' << line[1] << ' ';
  output << '\n';
  output << "process.levelsets=" << domain->getNumberOfLevelSets() << '\n';
}

} // namespace

int main(int argc, char **argv) {
  if (argc != 2) {
    std::cerr << "usage: strict_row01_cpu_fixture output\n";
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
