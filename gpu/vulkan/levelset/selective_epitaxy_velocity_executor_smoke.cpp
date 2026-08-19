// Copyright 2026 ViennaPS
// SPDX-License-Identifier: MIT
//
// P5-SELECTIVE-EPITAXY-VELOCITY-SUBSTAGE.  The unchanged CPU
// EpitaxyVelocityField computes the oracle.  Vulkan is admitted only as a
// transactional numeric candidate for the already selected point; model
// masking, stencil ownership, Process ordering and publication stay CPU-side.

#include "selective_epitaxy_velocity_executor.hpp"

#include <models/psSelectiveEpitaxy.hpp>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <span>
#include <string>
#include <vector>

#ifndef VIENNAPS_VULKAN_SELECTIVE_EPITAXY_VELOCITY_SPV_PATH
#error "VIENNAPS_VULKAN_SELECTIVE_EPITAXY_VELOCITY_SPV_PATH must be defined"
#endif

namespace {

using T = float;
using Point = std::array<T, 3>;
using Rate = viennaps::SelectiveEpitaxyMaterialRate<T>;
using Params = viennaps::SelectiveEpitaxyVelocityParameters<T>;
using Work = viennaps::SelectiveEpitaxyVelocityWork<T>;

std::uint32_t bits(const T value) {
  return std::bit_cast<std::uint32_t>(value);
}

std::uint32_t orderedBits(const T value) {
  const auto raw = bits(value);
  return (raw & 0x80000000U) != 0U ? ~raw : raw ^ 0x80000000U;
}

std::uint32_t ulpDistance(const T lhs, const T rhs) {
  const auto a = orderedBits(lhs);
  const auto b = orderedBits(rhs);
  return a >= b ? a - b : b - a;
}

bool sentinel(const std::vector<T> &values, const T expected) {
  for (const auto value : values)
    if (bits(value) != bits(expected))
      return false;
  return true;
}

bool oracleEqual(const std::vector<T> &lhs, const std::vector<T> &rhs,
                 std::uint32_t &maxUlp) {
  if (lhs.size() != rhs.size())
    return false;
  maxUlp = 0U;
  for (std::size_t index = 0U; index < lhs.size(); ++index)
    maxUlp = std::max(maxUlp, ulpDistance(lhs[index], rhs[index]));
  return maxUlp <= 32U;
}

} // namespace

int main() {
  using Field = viennaps::impl::EpitaxyVelocityField<T, 2>;
  const viennaps::Vec3D<T> normalFactors{0.5F, 1.0F, 0.0F};
  viennaps::MaterialValueMap<T> cpuMaterials;
  cpuMaterials.set(viennaps::Material::Si, 1.0F);
  cpuMaterials.set(viennaps::Material::SiGe, 0.75F);
  Field cpuField(cpuMaterials, 0.5F, 1.0F, normalFactors);

  const std::vector<Point> coordinates{{-1.0F, 0.0F, 0.0F},
                                       {-0.5F, 0.5F, 0.0F},
                                       {0.0F, 1.0F, 0.0F},
                                       {0.5F, 0.5F, 0.0F},
                                       {1.0F, 0.0F, 0.0F},
                                       {1.5F, 0.0F, 0.0F}};
  const std::vector<Point> normals{{0.0F, 1.0F, 0.0F},
                                   {1.0F, 0.0F, 0.0F},
                                   {0.707106769F, 0.707106769F, 0.0F},
                                   {0.5F, 0.866025388F, 0.0F},
                                   {0.0F, -1.0F, 0.0F},
                                   {0.0F, 1.0F, 0.0F}};
  const std::vector<std::int32_t> materialIds{10, 13, 10, 13, 0, 6};
  std::vector<T> cpuOracle;
  cpuOracle.reserve(normals.size());
  for (std::size_t index = 0U; index < normals.size(); ++index)
    cpuOracle.push_back(cpuField.getScalarVelocity(
        coordinates[index], materialIds[index], normals[index], index));

  const std::vector<Rate> rates{{10, 1.0F}, {13, 0.75F}, {0, 0.0F},
                                {6, 0.0F}};
  Params params;
  params.normalFactors = {normalFactors[0], normalFactors[1], normalFactors[2]};
  params.rate111 = 0.5F;
  params.rate100 = 1.0F;
  params.materialRates = std::span<const Rate>(rates);
  std::vector<T> output(normals.size(), 17.25F);
  Work work{std::span<const Point>(coordinates), std::span<const Point>(normals),
            std::span<const std::int32_t>(materialIds),
            std::span<const T>(cpuOracle), std::span<T>(output), params};

  viennaps::vulkan::levelset::VulkanSelectiveEpitaxyVelocityExecutor executor;
  std::string error;
  if (!executor.initialize(
          VIENNAPS_VULKAN_SELECTIVE_EPITAXY_VELOCITY_SPV_PATH, error)) {
    std::cerr << "selective-epitaxy velocity initialization failed: " << error
              << '\n';
    return 1;
  }
  auto invoke = executor.makeExecutor();
  if (!invoke(work, error) || !work.complete ||
      work.writtenCount != output.size()) {
    std::cerr << "selective-epitaxy velocity dispatch failed: " << error
              << '\n';
    return 1;
  }
  std::uint32_t maxUlp = 0U;
  if (!oracleEqual(output, cpuOracle, maxUlp)) {
    std::cerr << "selective-epitaxy CPU oracle mismatch maxUlp=" << maxUlp
              << '\n';
    return 1;
  }
  if (bits(output[4]) != bits(0.0F) || bits(output[5]) != bits(0.0F)) {
    std::cerr << "selective-epitaxy non-epitaxy material did not produce zero\n";
    return 1;
  }

  auto malformedIds = materialIds;
  malformedIds[0] = 1000;
  std::vector<T> malformedOutput(normals.size(), -9.5F);
  Work malformed{std::span<const Point>(coordinates),
                 std::span<const Point>(normals),
                 std::span<const std::int32_t>(malformedIds),
                 std::span<const T>(cpuOracle), std::span<T>(malformedOutput),
                 params};
  if (invoke(malformed, error) || !sentinel(malformedOutput, -9.5F)) {
    std::cerr << "selective-epitaxy malformed-input sentinel failed\n";
    return 1;
  }

  auto malformedNormals = normals;
  malformedNormals[1][0] = std::numeric_limits<T>::quiet_NaN();
  std::vector<T> nanOutput(normals.size(), 6.75F);
  Work malformedNormal{std::span<const Point>(coordinates),
                       std::span<const Point>(malformedNormals),
                       std::span<const std::int32_t>(materialIds),
                       std::span<const T>(cpuOracle), std::span<T>(nanOutput),
                       params};
  if (invoke(malformedNormal, error) || !sentinel(nanOutput, 6.75F)) {
    std::cerr << "selective-epitaxy NaN-input sentinel failed\n";
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
    std::cerr << "selective-epitaxy reset sentinel failed\n";
    return 1;
  }

  // Verify the production model retains the callback and falls back to the
  // exact CPU result when the callback reports a failed transaction.
  auto model = viennacore::SmartPointer<viennaps::SelectiveEpitaxy<T, 2>>::New(
      std::vector<std::pair<viennaps::Material, T>>{{viennaps::Material::Si,
                                                     1.0F}},
      0.5F, 1.0F);
  std::size_t callbackCalls = 0U;
  model->setVelocityExecutor([&callbackCalls](Work &, std::string &) {
    ++callbackCalls;
    return false;
  });
  const viennaps::Vec3D<T> modelCoordinate{0.0F, 0.0F, 0.0F};
  const viennaps::Vec3D<T> modelNormal{0.0F, 1.0F, 0.0F};
  const T modelVelocity = model->getVelocityField()->getScalarVelocity(
      modelCoordinate, 10, modelNormal, 0U);
  const T modelCpuVelocity = cpuField.getScalarVelocity(
      modelCoordinate, 10, modelNormal, 0U);
  if (callbackCalls != 1U || bits(modelVelocity) != bits(modelCpuVelocity) ||
      !model->hasVelocityExecutor()) {
    std::cerr << "selective-epitaxy production CPU fallback failed\n";
    return 1;
  }

  // A caller-owned session reset must invalidate the retained callback.
  viennaps::vulkan::runtime::ComputeSession externalSession;
  if (!externalSession.initialize(error)) {
    std::cerr << "selective-epitaxy external session initialization failed: "
              << error << '\n';
    return 1;
  }
  viennaps::vulkan::levelset::VulkanSelectiveEpitaxyVelocityExecutor
      externalExecutor;
  if (!externalExecutor.initialize(
          externalSession, VIENNAPS_VULKAN_SELECTIVE_EPITAXY_VELOCITY_SPV_PATH,
          error)) {
    std::cerr << "selective-epitaxy external executor initialization failed: "
              << error << '\n';
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
      !oracleEqual(externalOutput, cpuOracle, maxUlp)) {
    std::cerr << "selective-epitaxy external-session dispatch failed: "
              << error << '\n';
    return 1;
  }
  externalSession.reset();
  std::fill(externalOutput.begin(), externalOutput.end(), 4.25F);
  Work staleWork{std::span<const Point>(coordinates),
                 std::span<const Point>(normals),
                 std::span<const std::int32_t>(materialIds),
                 std::span<const T>(cpuOracle), std::span<T>(externalOutput),
                 params};
  if (externalInvoke(staleWork, error) || !sentinel(externalOutput, 4.25F)) {
    std::cerr << "selective-epitaxy stale-session sentinel failed\n";
    return 1;
  }
  externalExecutor.reset();

  std::cout << "selective-epitaxy velocity Vulkan dispatch PASS (maxUlp="
            << maxUlp << ", malformed/reset/stale-session sentinels PASS)\n";
  return 0;
}
