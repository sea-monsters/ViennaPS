// Independent CPU oracle fixture for the isolated WetEtching velocity
// operation.  The same source is compiled once against the Mod headers and
// once against D:\Codex_lib\code_reference\ViennaPS.
#include <bit>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#ifndef VIENNAPS_WETETCH_ORACLE_MOD
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

#include <models/psWetEtching.hpp>

namespace {
using T = float;
using Point = viennacore::Vec3D<T>;

std::uint32_t bits(const T value) { return std::bit_cast<std::uint32_t>(value); }

void write(const std::string &path) {
  const Point direction100{0.0F, 1.0F, 0.0F};
  const Point direction010{1.0F, 0.0F, -1.0F};
  constexpr T r100 = 0.0166666666667F;
  constexpr T r110 = 0.0309166666667F;
  constexpr T r111 = 0.000121666666667F;
  constexpr T r311 = 0.0300166666667F;
  const std::vector<std::pair<viennaps::Material, T>> materials{
      {viennaps::Material::Si, 1.0F}};
  viennaps::impl::WetEtchingVelocityField<T, 2> field(
      direction100, direction010, r100, r110, r111, r311, materials);
  const std::vector<Point> coordinates{{-1.0F, 0.0F, 0.0F},
                                       {-0.5F, 0.5F, 0.0F},
                                       {0.0F, 1.0F, 0.0F},
                                       {0.5F, 0.866025388F, 0.0F},
                                       {1.0F, 0.0F, 0.0F},
                                       {1.5F, 0.0F, 0.0F},
                                       {2.0F, 0.0F, 0.0F},
                                       {2.5F, 0.0F, 0.0F}};
  const std::vector<Point> normals{{0.0F, 1.0F, 0.0F},
                                   {1.0F, 0.0F, 0.0F},
                                   {0.707106769F, 0.707106769F, 0.0F},
                                   {0.5F, 0.866025388F, 0.0F},
                                   {0.0F, -1.0F, 0.0F},
                                   {0.0F, 1.0F, 0.0F},
                                   {0.0F, 1.0F, 0.0F},
                                   {0.0F, 1.0F, 0.0F}};
  const std::vector<int> materialIds{10, 10, 10, 10, 10, 10, 0, 6};

  std::ofstream output(path, std::ios::trunc);
  if (!output)
    throw std::runtime_error("cannot open oracle output");
  output << "schema=1\nprecision=float\ndimension=2\ncount="
         << normals.size() << "\nvalues=" << std::hex << std::setfill('0');
  for (std::size_t index = 0U; index < normals.size(); ++index) {
    const auto value = field.getScalarVelocity(coordinates[index],
                                               materialIds[index],
                                               normals[index], index);
    output << std::setw(8) << bits(value) << ' ';
  }
  output << std::dec << '\n';
}
} // namespace

int main(int argc, char **argv) {
  if (argc != 2) {
    std::cerr << "usage: wetetch_velocity_cpu_fixture output\n";
    return 2;
  }
  try {
    write(argv[1]);
  } catch (const std::exception &error) {
    std::cerr << "fixture failure: " << error.what() << '\n';
    return 1;
  }
  return 0;
}
