// CPU-authority fixture for the rank-10 TEOSPECVD fallback row.
// The ion particle remains an active CPU label in the production constructor;
// ionRate=0 freezes a radical-only reaction without inventing a new model.
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#ifndef VIENNAPS_TEOSPECVD_MOD
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
#include <models/psTEOSPECVD.hpp>
#include <process/psProcess.hpp>
#include <psDomain.hpp>

namespace {

using T = float;
constexpr int D = 2;
constexpr std::uint32_t kSeed = 42U;

constexpr std::array<const char *, 2> kLabels = {"radicalFlux", "ionFlux"};

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

  constexpr T radicalSticking = T(1);
  constexpr T radicalRate = T(1);
  constexpr T ionRate = T(0);
  constexpr T ionExponent = T(300);
  constexpr T ionSticking = T(1);
  constexpr T radicalOrder = T(1);
  constexpr T ionOrder = T(1);
  constexpr T ionMinAngle = T(85);

  auto domain = Domain<T, D>::New(T(.5), T(10), T(10));
  MakePlane<T, D>(domain, T(0)).apply();
  auto model = SmartPointer<TEOSPECVD<T, D>>::New(
      radicalSticking, radicalRate, ionRate, ionExponent, ionSticking,
      radicalOrder, ionOrder, ionMinAngle);

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
      throw std::runtime_error("TEOSPECVD CPU label sizes differ");
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
  out << "schema=1\nsource=cpu\nmodel=TEOSPECVD\n"
         "precision=float\ndimension=2\ngridDelta=0.5\n"
         "xExtent=10\nyExtent=10\nseed=42\nraysPerPoint=1\n"
         "maxReflections=0\nnormalization=source\n"
         "engine=CPU_TRIANGLE\nroute=cpu_fallback_only\n"
         "auto_route=CPU\nmanual_vulkan=fail_closed_no_publication\n"
         "radicalSticking=1\nradicalRate=1\nradicalOrder=1\n"
         "ionRate=0\nionSticking=1\nionExponent=300\nionOrder=1\n"
         "ionMinAngle=85\nreaction_mode=radical_only_ion_rate_disabled\n"
         "labels=radicalFlux,ionFlux\n";
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
    std::cerr << "usage: teospecvd_cpu_fixture output\n";
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
