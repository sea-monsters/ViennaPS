#include "neutral_cpu_oracle_process.hpp"

#ifdef VIENNAPS_NEUTRAL_ORACLE_EXPLICIT_KDTREE
#include "neutral_cpu_oracle_kdtree_specialization.hpp"
#endif

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
#include <models/psNeutralTransport.hpp>
#include <process/psProcess.hpp>
#include <psDomain.hpp>

#include <bit>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <stdexcept>

namespace {

using T = float;
constexpr int D = 2;
using Model = viennaps::impl::NeutralTransportSurfaceModel<T, D>;

std::uint32_t bits(const T value) { return std::bit_cast<std::uint32_t>(value); }

void writeValues(std::ofstream &out, const char *name,
                 const std::vector<T> &values) {
  out << name << ".count=" << values.size() << '\n';
  out << name << "=" << std::hex << std::setfill('0');
  for (const auto value : values)
    out << std::setw(8) << bits(value) << ' ';
  out << std::dec << '\n';
}

template <typename Point>
void writePoints(std::ofstream &out, const char *name,
                 const std::vector<Point> &points) {
  out << name << ".count=" << points.size() << '\n';
  out << name << "=" << std::hex << std::setfill('0');
  for (const auto &point : points) {
    out << std::setw(8) << bits(point[0]) << ':' << std::setw(8)
        << bits(point[1]) << ':' << std::setw(8) << bits(point[2]) << ' ';
  }
  out << std::dec << '\n';
}

std::vector<T> calculateVelocities(
    Model &model,
    const viennacore::SmartPointer<viennacore::PointData<T>> &fluxes,
    const std::vector<viennacore::Vec3D<T>> &coords,
    const std::vector<T> &materials) {
  auto values = model.calculateVelocities(fluxes, coords, materials);
  return *values;
}

void installActiveAdapter(Model &model) {
#ifdef VIENNAPS_NEUTRAL_ORACLE_MOD
  model.setVelocityExecutor([](auto &work, std::string &) {
    for (std::size_t i = 0; i < work.output.size(); ++i) {
      const bool isEtchFront = work.materialIds[i] ==
                               static_cast<T>(work.parameters.etchFrontMaterialId);
      // The frozen fixture parameters reduce the canonical formula to
      // -coverage in SI units (2 * 3 / 6, time/length conversion = 1).
      work.output[i] = isEtchFront ? -work.coverage[i] : T(0);
    }
    work.writtenCount = work.output.size();
    work.complete = true;
    return true;
  });
#else
  static_cast<void>(model);
#endif
}

} // namespace

void writeNeutralCpuOracleFixture(const std::string &path) {
  std::ofstream out(path, std::ios::trunc);
  if (!out)
    throw std::runtime_error("cannot open oracle output");

  viennaps::units::Length::setUnit(viennaps::units::Length::METER);
  viennaps::units::Time::setUnit(viennaps::units::Time::SECOND);

  viennaps::NeutralTransportParameters<T> params;
  params.kEtch = T(2);
  params.surfaceSiteDensity = T(3);
  params.siliconDensity = T(6);
  params.etchFrontMaterial = viennaps::Material::Si;
  params.fluxLabel = "neutralFlux";
  params.coverageLabel = "neutralCoverage";

  constexpr std::size_t count = 6;
  const std::vector<T> coverageValues{T(.125), T(.25), T(.5), T(.75), T(1),
                                      T(.375)};
  const std::vector<T> materialValues{
      static_cast<T>(viennaps::Material::Si.legacyId()), T(1),
      static_cast<T>(viennaps::Material::Si.legacyId()), T(1),
      static_cast<T>(viennaps::Material::Si.legacyId()), T(1)};
  const std::vector<T> fluxValues{T(1.25), T(.5), T(2.75), T(0), T(3.5),
                                  T(4.25)};
  const std::vector<viennacore::Vec3D<T>> coordinates{
      {T(-1.5), T(0), T(0)}, {T(-.5), T(0), T(0)}, {T(.5), T(0), T(0)},
      {T(1.5), T(0), T(0)},  {T(2.5), T(0), T(0)}, {T(3.5), T(0), T(0)}};

  auto fluxes = viennacore::PointData<T>::New();
  fluxes->insertNextScalarData(fluxValues, params.fluxLabel);

  Model model(params);
  model.initializeCoverages(static_cast<unsigned>(count));
  *model.getCoverages()->getScalarData(params.coverageLabel) = coverageValues;
  const auto empty = calculateVelocities(model, fluxes, coordinates,
                                         materialValues);
  installActiveAdapter(model);
  const auto active = calculateVelocities(model, fluxes, coordinates,
                                          materialValues);

  auto domain = viennaps::Domain<T, D>::New(T(.5), T(4), T(4));
  viennaps::MakePlane<T, D>(domain, T(0)).apply();
  auto processModel =
      viennacore::SmartPointer<viennaps::NeutralTransport<T, D>>::New(params);
  viennaps::Process<T, D> process(domain, processModel, T(0));
  process.setFluxEngineType(viennaps::FluxEngineType::CPU_TRIANGLE);
  viennaps::RayTracingParameters rayParams;
  rayParams.rngSeed = 42;
  rayParams.useRandomSeeds = false;
  rayParams.raysPerPoint = 1;
  rayParams.maxReflections = 0;
  process.setParameters(rayParams);
  const auto fluxMesh = process.calculateFlux();
  if (!fluxMesh)
    throw std::runtime_error("calculateFlux returned null");
  const auto *flux = fluxMesh->getCellData().getScalarData(params.fluxLabel);
  if (!flux)
    throw std::runtime_error("neutralFlux output missing");

  const auto geometry = domain->getSurfaceMesh();
  out << "schema=1\nsource="
#ifdef VIENNAPS_NEUTRAL_ORACLE_MOD
      << "mod\n"
#else
      << "reference\n"
#endif
      << "precision=float\ndimension=2\ngridDelta=0.5\n"
         "xExtent=4\nyExtent=4\nseed=42\nraysPerPoint=1\n"
         "maxReflections=0\nactive_scope=velocity_adapter_only\n";
  writeValues(out, "empty.velocity", empty);
  writeValues(out, "active.velocity", active);
  writeValues(out, "flux", *flux);
  writePoints(out, "geometry.nodes", geometry->nodes);
  out << "geometry.lines.count=" << geometry->lines.size() << '\n';
  out << "geometry.lines=";
  for (const auto &line : geometry->lines)
    out << line[0] << ':' << line[1] << ' ';
  out << '\n';
  out << "process.levelsets=" << domain->getLevelSets().size() << '\n';
  out << "process.metadata_level="
      << static_cast<int>(domain->getMetaDataLevel()) << '\n';
  out << "process.flux_cells="
      << fluxMesh->lines.size() + fluxMesh->triangles.size() << '\n';
}
