// CPU-authority fixture for the rank-13 OxideRegrowth fallback row.
// This exercises the existing Process/ByproductDynamics callback path on a
// small deterministic stack. It does not add a Vulkan or solver seam.
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

#ifndef VIENNAPS_OXIDEREGROWTH_MOD
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

#include <geometries/psMakeStack.hpp>
#include <models/psOxideRegrowth.hpp>
#include <process/psProcess.hpp>
#include <psDomain.hpp>

namespace {

using T = float;
constexpr int D = 2;
constexpr std::uint32_t kSeed = 42U;
constexpr std::array<const char *, 3> kCellLabels = {
    "byproductSum", "Material", "fillingFractions"};

std::uint32_t bits(const T value) { return std::bit_cast<std::uint32_t>(value); }

void writeValues(std::ofstream &out, const char *name,
                 const std::vector<T> &values) {
  out << name << ".count=" << values.size() << '\n';
  out << name << '=' << std::hex << std::setfill('0');
  for (const auto value : values)
    out << std::setw(8) << bits(value) << ' ';
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

template <typename CellSet> void writeCellGeometry(std::ofstream &out,
                                                    const CellSet &cellSet) {
  const auto &nodes = cellSet.getNodes();
  const auto &elements = cellSet.getElements();
  if (nodes.empty() || elements.empty())
    throw std::runtime_error("cell geometry is empty");
  out << "cell.nodes.count=" << nodes.size() << '\n';
  out << "cell.nodes=" << std::hex << std::setfill('0');
  for (const auto &point : nodes) {
    for (int axis = 0; axis < 3; ++axis) {
      if (!std::isfinite(point[axis]))
        throw std::runtime_error("cell geometry is not finite");
      out << std::setw(8) << bits(point[axis]) << (axis == 2 ? ' ' : ':');
    }
  }
  out << std::dec << '\n';
  out << "cell.elements.count=" << elements.size() << '\n';
  out << "cell.elements=";
  for (const auto &element : elements) {
    for (std::size_t i = 0; i < element.size(); ++i)
      out << element[i] << (i + 1 == element.size() ? ' ' : ':');
  }
  out << '\n';
}

void run(const std::string &outputPath) {
  using namespace viennaps;
  using viennacore::SmartPointer;

  units::Length::setUnit(units::Length::METER);
  units::Time::setUnit(units::Time::SECOND);

  constexpr T gridDelta = T(.5);
  constexpr T xExtent = T(10);
  constexpr T yExtent = T(10);
  constexpr int numLayers = 2;
  constexpr T layerHeight = T(1);
  constexpr T substrateHeight = T(1);
  constexpr T trenchWidth = T(4);
  constexpr T processDuration = T(.1);

  auto domain = Domain<T, D>::New(gridDelta, xExtent, yExtent);
  MakeStack<T, D>(domain, numLayers, layerHeight, substrateHeight, T(0),
                  trenchWidth, T(0))
      .apply();
  domain->duplicateTopLevelSet(Material::Polymer);
  domain->generateCellSet(substrateHeight + numLayers * layerHeight + T(4),
                          Material::GAS, true);
  auto &cellSet = domain->getCellSet();
  if (!cellSet)
    throw std::runtime_error("cell set was not generated");
  cellSet->addScalarData("byproductSum", T(0));
  cellSet->buildNeighborhood();

  // Positive diffusion and stream rates keep the production callback on its
  // ordinary finite path; the long redeposition interval leaves the fixture
  // deterministic without inventing byproduct production.
  constexpr T nitrideEtchRate = T(.05);
  constexpr T oxideEtchRate = T(0);
  constexpr T redepositionRate = T(.01);
  constexpr T redepositionThreshold = T(.05);
  constexpr T redepositionTimeInt = T(60);
  constexpr T diffusionCoefficient = T(.5);
  constexpr T sinkStrength = T(.01);
  constexpr T scallopVelocity = T(.1);
  constexpr T centerVelocity = T(.1);
  constexpr T topHeight = T(3);
  constexpr T centerWidth = T(4);
  constexpr T timeStabilityFactor = T(.245);

  auto model = SmartPointer<OxideRegrowth<T, D>>::New(
      nitrideEtchRate, oxideEtchRate, redepositionRate,
      redepositionThreshold, redepositionTimeInt, diffusionCoefficient,
      sinkStrength, scallopVelocity, centerVelocity, topHeight, centerWidth,
      timeStabilityFactor);

  AdvectionParameters advectionParameters;
  advectionParameters.ignoreVoids = true;
  Process<T, D> process(domain, model, processDuration, advectionParameters);
  process.apply();

  const auto byproductSum = cellSet->getScalarData("byproductSum");
  const auto material = cellSet->getScalarData("Material");
  const auto fillingFractions = cellSet->getFillingFractions();
  if (!byproductSum || !material || !fillingFractions)
    throw std::runtime_error("required cell data is missing");
  requireFiniteNonnegative("byproductSum", *byproductSum);
  requireFiniteNonnegative("Material", *material);
  requireFiniteNonnegative("fillingFractions", *fillingFractions);
  if (byproductSum->size() != material->size() ||
      byproductSum->size() != fillingFractions->size())
    throw std::runtime_error("cell data sizes differ");

  T byproductMass = T(0);
  for (const auto value : *byproductSum)
    byproductMass += value;
  if (!std::isfinite(byproductMass) || byproductMass < T(0))
    throw std::runtime_error("byproduct mass conservation check failed");

  std::ofstream out(outputPath, std::ios::trunc);
  if (!out)
    throw std::runtime_error("cannot open output");
  out << "schema=1\nsource=cpu\nmodel=OxideRegrowth\n"
         "precision=float\ndimension=2\n"
         "gridDelta=0.5\nxExtent=10\nyExtent=10\n"
         "seed=42\nraysPerPoint=1\nmaxReflections=0\n"
         "normalization=source\nengine=CPU_CALLBACK\n"
         "route=cpu_fallback_only\nauto_route=CPU\n"
         "manual_vulkan=fail_closed_no_publication\n"
         "numLayers=2\nlayerHeight=1\nsubstrateHeight=1\n"
         "trenchWidth=4\nprocessDuration=0.1\n"
         "nitrideEtchRate=0.05\noxideEtchRate=0\n"
         "redepositionRate=0.01\nredepositionThreshold=0.05\n"
         "redepositionTimeInt=60\ndiffusionCoefficient=0.5\n"
         "sinkStrength=0.01\nscallopVelocity=0.1\ncenterVelocity=0.1\n"
         "topHeight=3\ncenterWidth=4\ntimeStabilityFactor=0.245\n"
         "labels=byproductSum,Material,fillingFractions\n"
         "callback=ByproductDynamics.applyPreAdvect_then_applyPostAdvect\n"
         "initialByproductMass=0\n";
  writeValues(out, kCellLabels[0], *byproductSum);
  writeValues(out, kCellLabels[1], *material);
  writeValues(out, kCellLabels[2], *fillingFractions);
  out << "finalByproductMass=" << std::setprecision(9) << byproductMass
      << '\n';
  out << "byproductMassResidual=" << byproductMass << '\n';
  writeCellGeometry(out, *cellSet);
  out << "levelsets=" << domain->getNumberOfLevelSets() << '\n';
}

} // namespace

int main(int argc, char **argv) {
  if (argc != 2) {
    std::cerr << "usage: oxide_regrowth_cpu_fixture output\n";
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
