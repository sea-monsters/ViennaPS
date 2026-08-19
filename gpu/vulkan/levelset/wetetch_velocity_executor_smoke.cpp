#include "wetetch_velocity_executor.hpp"

#include <materials/psMaterial.hpp>
#include <models/psWetEtching.hpp>

#include <bit>
#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <span>
#include <string>
#include <vector>

#ifndef VIENNAPS_VULKAN_WETETCH_VELOCITY_SPV_PATH
#error "VIENNAPS_VULKAN_WETETCH_VELOCITY_SPV_PATH must be defined"
#endif

namespace {

using T = float;
using Point = std::array<T, 3>;
using Rate = viennaps::WetEtchMaterialRate<T>;
using Params = viennaps::WetEtchVelocityParameters<T>;
using Work = viennaps::WetEtchVelocityWork<T>;

std::uint32_t bits(const T value) { return std::bit_cast<std::uint32_t>(value); }

std::uint32_t orderedBits(const T value) {
  const auto raw = bits(value);
  return (raw & 0x80000000U) != 0U ? ~raw : raw ^ 0x80000000U;
}

std::uint32_t ulpDistance(const T lhs, const T rhs) {
  const auto a = orderedBits(lhs);
  const auto b = orderedBits(rhs);
  return a >= b ? a - b : b - a;
}

bool allEqual(const std::vector<T> &lhs, const std::vector<T> &rhs,
              std::uint32_t &maxUlp) {
  if (lhs.size() != rhs.size())
    return false;
  maxUlp = 0U;
  for (std::size_t index = 0U; index < lhs.size(); ++index)
    maxUlp = std::max(maxUlp, ulpDistance(lhs[index], rhs[index]));
  return maxUlp <= 32U;
}

bool sentinel(const std::vector<T> &values, const T expected) {
  for (const auto value : values)
    if (bits(value) != bits(expected))
      return false;
  return true;
}

} // namespace

int main() {
  using Field = viennaps::impl::WetEtchingVelocityField<T, 2>;
  const Point direction100{0.0F, 1.0F, 0.0F};
  const Point direction010{1.0F, 0.0F, -1.0F};
  constexpr T r100 = 0.0166666666667F;
  constexpr T r110 = 0.0309166666667F;
  constexpr T r111 = 0.000121666666667F;
  constexpr T r311 = 0.0300166666667F;
  const std::vector<std::pair<viennaps::Material, T>> cpuMaterials{
      {viennaps::Material::Si, 1.0F}};
  Field cpuField(direction100, direction010, r100, r110, r111, r311,
                cpuMaterials);

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
  const std::vector<std::int32_t> materialIds{10, 10, 10, 10, 10, 10, 0,
                                               6};
  std::vector<T> cpuOracle;
  cpuOracle.reserve(normals.size());
  for (std::size_t index = 0U; index < normals.size(); ++index)
    cpuOracle.push_back(cpuField.getScalarVelocity(
        coordinates[index], materialIds[index], normals[index], index));

  const std::vector<Rate> rates{{10, 1.0F}, {0, 0.0F}};
  const Params params{direction100, direction010, r100, r110, r111, r311,
                      std::span<const Rate>(rates)};
  std::vector<T> output(normals.size(), 17.25F);
  Work work{std::span<const Point>(coordinates), std::span<const Point>(normals),
            std::span<const std::int32_t>(materialIds),
            std::span<const T>(cpuOracle), std::span<T>(output), params};

  viennaps::vulkan::levelset::VulkanWetEtchVelocityExecutor executor;
  std::string error;
  if (!executor.initialize(VIENNAPS_VULKAN_WETETCH_VELOCITY_SPV_PATH, error)) {
    std::cerr << "wet-etch velocity initialization failed: " << error << '\n';
    return 1;
  }
  auto invoke = executor.makeExecutor();
  if (!invoke(work, error) || !work.complete ||
      work.writtenCount != output.size()) {
    std::cerr << "wet-etch velocity dispatch failed: " << error << '\n';
    return 1;
  }
  std::uint32_t maxUlp = 0U;
  if (!allEqual(output, cpuOracle, maxUlp)) {
    std::cerr << "wet-etch velocity CPU oracle mismatch maxUlp=" << maxUlp
              << '\n';
    return 1;
  }
  if (bits(output[6]) != bits(0.0F) || bits(output[7]) != bits(0.0F)) {
    std::cerr << "wet-etch non-etching material did not produce zero\n";
    return 1;
  }

  // Malformed material IDs must fail before dispatch and leave the staged
  // output untouched.
  auto malformedIds = materialIds;
  malformedIds[0] = 1000;
  std::vector<T> malformedOutput(normals.size(), -9.5F);
  Work malformed{std::span<const Point>(coordinates),
                 std::span<const Point>(normals),
                 std::span<const std::int32_t>(malformedIds),
                 std::span<const T>(cpuOracle), std::span<T>(malformedOutput),
                 params};
  if (invoke(malformed, error) || !sentinel(malformedOutput, -9.5F)) {
    std::cerr << "wet-etch malformed-input sentinel failed\n";
    return 1;
  }

  // NaN normals are also rejected without publishing a partial vector.
  auto malformedNormals = normals;
  malformedNormals[1][0] = std::numeric_limits<T>::quiet_NaN();
  std::vector<T> nanOutput(normals.size(), 6.75F);
  Work malformedNormal{std::span<const Point>(coordinates),
                       std::span<const Point>(malformedNormals),
                       std::span<const std::int32_t>(materialIds),
                       std::span<const T>(cpuOracle), std::span<T>(nanOutput),
                       params};
  if (invoke(malformedNormal, error) || !sentinel(nanOutput, 6.75F)) {
    std::cerr << "wet-etch NaN-input sentinel failed\n";
    return 1;
  }

  executor.reset();
  std::vector<T> resetOutput(normals.size(), 3.5F);
  Work afterReset{std::span<const Point>(coordinates),
                  std::span<const Point>(normals),
                  std::span<const std::int32_t>(materialIds),
                  std::span<const T>(cpuOracle), std::span<T>(resetOutput),
                  params};
  if (invoke(afterReset, error) || !sentinel(resetOutput, 3.5F)) {
    std::cerr << "wet-etch reset sentinel failed\n";
    return 1;
  }

  // A caller-owned session reset must make the retained callback fail closed
  // without destroying stale Vulkan handles through the new device.
  viennaps::vulkan::runtime::ComputeSession externalSession;
  if (!externalSession.initialize(error)) {
    std::cerr << "wet-etch external session initialization failed: " << error
              << '\n';
    return 1;
  }
  viennaps::vulkan::levelset::VulkanWetEtchVelocityExecutor externalExecutor;
  if (!externalExecutor.initialize(
          externalSession, VIENNAPS_VULKAN_WETETCH_VELOCITY_SPV_PATH, error)) {
    std::cerr << "wet-etch external executor initialization failed: " << error
              << '\n';
    return 1;
  }
  auto externalInvoke = externalExecutor.makeExecutor();
  std::vector<T> externalOutput(normals.size(), 4.25F);
  Work externalWork{std::span<const Point>(coordinates),
                    std::span<const Point>(normals),
                    std::span<const std::int32_t>(materialIds),
                    std::span<const T>(cpuOracle),
                    std::span<T>(externalOutput), params};
  if (!externalInvoke(externalWork, error) ||
      !allEqual(externalOutput, cpuOracle, maxUlp)) {
    std::cerr << "wet-etch external-session dispatch failed: " << error
              << '\n';
    return 1;
  }
  externalSession.reset();
  std::fill(externalOutput.begin(), externalOutput.end(), 4.25F);
  Work staleWork{std::span<const Point>(coordinates),
                 std::span<const Point>(normals),
                 std::span<const std::int32_t>(materialIds),
                 std::span<const T>(cpuOracle),
                 std::span<T>(externalOutput), params};
  if (externalInvoke(staleWork, error) || !sentinel(externalOutput, 4.25F)) {
    std::cerr << "wet-etch stale-session sentinel failed\n";
    return 1;
  }
  externalExecutor.reset();

  std::cout << "wet-etch velocity Vulkan dispatch PASS (maxUlp=" << maxUlp
            << ", malformed/reset/stale-session sentinels PASS)\n";
  return 0;
}
