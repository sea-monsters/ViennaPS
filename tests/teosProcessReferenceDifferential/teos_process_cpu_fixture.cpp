// Independent CPU Process oracle for the narrow single-precursor TEOS row.
// This source is compiled once against the Mod headers and once against the
// unmodified D:\Codex_lib\code_reference\ViennaPS tree.  No Vulkan executor
// is installed; CPU particle/surface semantics remain the oracle.

#include <bit>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <stdexcept>
#include <string>
#include <vector>

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

#include <geometries/psMakePlane.hpp>
#include <models/psTEOSDeposition.hpp>
#include <process/psProcess.hpp>

namespace {
using T = float;
constexpr int D = 2;

std::uint32_t bits(const T value) { return std::bit_cast<std::uint32_t>(value); }

void write(const std::string &path) {
  auto domain = viennaps::Domain<T, D>::New(1.0F, 4.0F, 4.0F);
  viennaps::MakePlane<T, D>(domain, 0.0F).apply();
  auto model = viennacore::SmartPointer<viennaps::TEOSDeposition<T, D>>::New(
      1.0F, 0.75F, 0.5F);
  viennaps::Process<T, D> process(domain, model, 0.02F);
  viennaps::RayTracingParameters parameters;
  parameters.raysPerPoint = 8U;
  parameters.useRandomSeeds = false;
  parameters.rngSeed = 42U;
  parameters.maxReflections = 0U;
  process.setParameters(parameters);
  process.apply();

  const auto &surface = domain->getSurface()->getDomain();
  std::vector<T> values;
  for (unsigned index = 0U; index < surface.getNumberOfSegments(); ++index) {
    const auto &segment = surface.getDomainSegment(index);
    values.insert(values.end(), segment.definedValues.begin(),
                  segment.definedValues.end());
  }
  std::ofstream output(path, std::ios::trunc);
  if (!output)
    throw std::runtime_error("cannot open TEOS Process oracle output");
  output << "schema=1\nprecision=float\ndimension=2\ncount=" << values.size()
         << "\nvalues=" << std::hex << std::setfill('0');
  for (const T value : values)
    output << std::setw(8) << bits(value) << ' ';
  output << std::dec << '\n';
}
} // namespace

int main(int argc, char **argv) {
  if (argc != 2)
    return 2;
  try {
    write(argv[1]);
  } catch (...) {
    return 1;
  }
  return 0;
}
