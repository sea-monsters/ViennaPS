// Copyright 2026 ViennaPS
// SPDX-License-Identifier: MIT

#include "neutral_transport_surface.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <iostream>
#include <limits>
#include <string>
#include <vector>

namespace {

using Model = viennaps::vulkan::surface::NeutralTransportSurfaceModelFp32;
using Params = viennaps::vulkan::surface::NeutralTransportSurfaceParamsFp32;
using Buffer = viennaps::vulkan::runtime::HostVisibleBuffer;

[[nodiscard]] bool check(const bool condition, const char *message) {
  if (!condition) {
    std::cerr << "[surface] " << message << '\n';
    return false;
  }
  return true;
}

[[nodiscard]] bool writeFloats(Buffer &buffer, const std::vector<float> &data,
                               std::string &error) {
  return data.empty() ||
         buffer.write(data.data(), data.size() * sizeof(float), 0U, error);
}

[[nodiscard]] bool readFloats(Buffer &buffer, std::vector<float> &data,
                              std::string &error) {
  return data.empty() ||
         buffer.read(data.data(), data.size() * sizeof(float), 0U, error);
}

[[nodiscard]] std::uint32_t ulpDistance(const float lhs, const float rhs) {
  const auto leftBits = viennaps::vulkan::runtime::orderedFloatBits(lhs);
  const auto rightBits = viennaps::vulkan::runtime::orderedFloatBits(rhs);
  return leftBits > rightBits ? leftBits - rightBits : rightBits - leftBits;
}

} // namespace

int main() {
  std::string error;

  Model invalidModel;
  if (!check(!invalidModel.initialize("missing-neutral-transport.spv", error),
             "missing SPIR-V must fail initialization")) {
    return 1;
  }

  Model model;
  if (!model.initialize(VIENNAPS_VULKAN_NEUTRAL_TRANSPORT_SPV_PATH, error)) {
    std::cerr << "initialize failed: " << error << '\n';
    return 1;
  }

  const Params params{3.5F, 1.66e-5F, 8.3e4F, 2.0F, 1.0e-9F, 10U};
  constexpr std::array<std::size_t, 5U> lengths{0U, 1U, 16U, 257U, 1025U};
  std::uint32_t maxUlp = 0U;
  std::size_t exactValues = 0U;
  std::size_t comparedValues = 0U;
  for (const auto count : lengths) {
    Buffer coverage;
    Buffer materialIds;
    Buffer velocity;
    if (!model.createFloatBuffer(count, coverage, error) ||
        !model.createFloatBuffer(count, materialIds, error) ||
        !model.createFloatBuffer(count + 3U, velocity, error)) {
      std::cerr << "buffer allocation failed: " << error << '\n';
      return 1;
    }

    std::vector<float> coverageValues(count);
    std::vector<float> materialValues(count);
    std::vector<float> expected(count);
    for (std::size_t i = 0; i < count; ++i) {
      coverageValues[i] = static_cast<float>((i % 7U) + 1U) / 8.0F;
      materialValues[i] = (i % 3U == 0U) ? 10.0F : 11.0F;
      expected[i] =
          Model::cpuVelocity(coverageValues[i], materialValues[i], params);
    }
    if (count > 0U) {
      coverageValues[0] = 0.0F;
      expected[0] =
          Model::cpuVelocity(coverageValues[0], materialValues[0], params);
    }
    constexpr float sentinel = -12345.25F;
    std::vector<float> output(count + 3U, sentinel);
    if (!writeFloats(coverage, coverageValues, error) ||
        !writeFloats(materialIds, materialValues, error) ||
        !writeFloats(velocity, output, error)) {
      std::cerr << "input write failed: " << error << '\n';
      return 1;
    }
    if (!model.evaluate(coverage, count, materialIds, count, velocity, count,
                        params, error)) {
      std::cerr << "Vulkan evaluation failed for N=" << count << ": " << error
                << '\n';
      return 1;
    }
    if (!readFloats(velocity, output, error)) {
      std::cerr << "output read failed: " << error << '\n';
      return 1;
    }
    for (std::size_t i = 0; i < count; ++i) {
      const auto ulp = ulpDistance(output[i], expected[i]);
      maxUlp = std::max(maxUlp, ulp);
      ++comparedValues;
      if (ulp == 0U) {
        ++exactValues;
      }
      if (!check(ulp == 0U, "CPU/Vulkan velocity is not bit-exact")) {
        std::cerr << "index=" << i << " output=" << output[i]
                  << " expected=" << expected[i] << " outputBits=0x" << std::hex
                  << std::bit_cast<std::uint32_t>(output[i])
                  << " expectedBits=0x"
                  << std::bit_cast<std::uint32_t>(expected[i]) << std::dec
                  << " ulp=" << ulp << '\n';
        return 1;
      }
    }
    for (std::size_t i = count; i < output.size(); ++i) {
      if (!check(
              viennaps::vulkan::runtime::exactlyEqualFloat(output[i], sentinel),
              "tail guard was overwritten")) {
        return 1;
      }
    }

    std::fill(output.begin(), output.end(), sentinel);
    if (!writeFloats(velocity, output, error)) {
      std::cerr << "sentinel write failed: " << error << '\n';
      return 1;
    }
    if (model.evaluate(coverage, count, materialIds, count, velocity,
                       count + 1U, params, error) ||
        !readFloats(velocity, output, error)) {
      std::cerr << "invalid length was accepted or output read failed\n";
      return 1;
    }
    if (!check(viennaps::vulkan::runtime::exactlyEqualFloat(output.front(),
                                                            sentinel),
               "invalid input overwrote output")) {
      return 1;
    }
  }

  Buffer coverage;
  Buffer materialIds;
  Buffer velocity;
  if (!model.createFloatBuffer(16U, coverage, error) ||
      !model.createFloatBuffer(16U, materialIds, error) ||
      !model.createFloatBuffer(16U, velocity, error)) {
    std::cerr << "branch buffer allocation failed: " << error << '\n';
    return 1;
  }
  std::vector<float> coverageValues(16U, 0.5F);
  std::vector<float> materialValues(16U, 10.0F);
  std::vector<float> output(16U, 77.0F);
  if (!writeFloats(coverage, coverageValues, error) ||
      !writeFloats(materialIds, materialValues, error) ||
      !writeFloats(velocity, output, error)) {
    std::cerr << "branch input write failed: " << error << '\n';
    return 1;
  }
  Params zeroDensity = params;
  zeroDensity.siliconDensity = 0.0F;
  if (!model.evaluate(coverage, 16U, materialIds, 16U, velocity, 16U,
                      zeroDensity, error) ||
      !readFloats(velocity, output, error)) {
    std::cerr << "zero-density branch failed: " << error << '\n';
    return 1;
  }
  for (const auto value : output) {
    if (!check(viennaps::vulkan::runtime::exactlyEqualFloat(value, 0.0F),
               "non-positive density branch mismatch")) {
      return 1;
    }
  }

  const Params strictParams{1.0F, 1.0F, 1.0F, 1.0F, 1.0F, 10U};
  for (std::size_t i = 0; i < coverageValues.size(); ++i) {
    coverageValues[i] = static_cast<float>(i + 1U) / 32.0F;
    materialValues[i] = 10.0F;
    output[i] = 19.0F;
  }
  if (!writeFloats(coverage, coverageValues, error) ||
      !writeFloats(materialIds, materialValues, error) ||
      !writeFloats(velocity, output, error) ||
      !model.evaluate(coverage, 16U, materialIds, 16U, velocity, 16U,
                      strictParams, error) ||
      !readFloats(velocity, output, error)) {
    std::cerr << "strict bit-exact submatrix dispatch failed: " << error
              << '\n';
    return 1;
  }
  for (std::size_t i = 0; i < output.size(); ++i) {
    const auto expected =
        Model::cpuVelocity(coverageValues[i], materialValues[i], strictParams);
    if (!check(
            viennaps::vulkan::runtime::exactlyEqualFloat(output[i], expected),
            "strict bit-exact submatrix mismatch")) {
      return 1;
    }
  }

  const std::array<Params, 3U> divisionMatrix{
      Params{0.125F, 3.25F, 7.0F, 0.75F, 1.5F, 10U},
      Params{-2.5F, 0.75F, 2.25F, -1.25F, -0.5F, 10U},
      Params{1.0F, 1.75F, 0.375F, 2.0F, 4.0F, 10U}};
  for (const auto &matrixParams : divisionMatrix) {
    for (std::size_t i = 0U; i < coverageValues.size(); ++i) {
      coverageValues[i] = static_cast<float>(static_cast<int>(i) - 4) / 8.0F;
      materialValues[i] = 10.0F;
      output[i] = 31.0F;
    }
    if (!writeFloats(coverage, coverageValues, error) ||
        !writeFloats(materialIds, materialValues, error) ||
        !writeFloats(velocity, output, error) ||
        !model.evaluate(coverage, 16U, materialIds, 16U, velocity, 16U,
                        matrixParams, error) ||
        !readFloats(velocity, output, error)) {
      std::cerr << "division matrix dispatch failed: " << error << '\n';
      return 1;
    }
    for (std::size_t i = 0U; i < output.size(); ++i) {
      if (!check(viennaps::vulkan::runtime::exactlyEqualFloat(
                     output[i],
                     Model::cpuVelocity(coverageValues[i], materialValues[i],
                                        matrixParams)),
                 "division matrix bit-exact mismatch")) {
        return 1;
      }
    }
  }

  const std::array<float, 6U> boundaryCoverage{
      -std::numeric_limits<float>::max(),
      -std::numeric_limits<float>::min(),
      -0.0F,
      0.0F,
      std::numeric_limits<float>::min(),
      std::numeric_limits<float>::max()};
  std::fill(coverageValues.begin(), coverageValues.end(), 0.0F);
  std::fill(materialValues.begin(), materialValues.end(), 10.0F);
  std::fill(output.begin(), output.end(), 23.0F);
  std::copy(boundaryCoverage.begin(), boundaryCoverage.end(),
            coverageValues.begin());
  if (!writeFloats(coverage, coverageValues, error) ||
      !writeFloats(materialIds, materialValues, error) ||
      !writeFloats(velocity, output, error) ||
      !model.evaluate(coverage, boundaryCoverage.size(), materialIds,
                      boundaryCoverage.size(), velocity,
                      boundaryCoverage.size(), strictParams, error) ||
      !readFloats(velocity, output, error)) {
    std::cerr << "boundary bit-exact dispatch failed: " << error << '\n';
    return 1;
  }
  for (std::size_t i = 0U; i < boundaryCoverage.size(); ++i) {
    if (!check(viennaps::vulkan::runtime::exactlyEqualFloat(
                   output[i], Model::cpuVelocity(boundaryCoverage[i], 10.0F,
                                                 strictParams)),
               "boundary bit-exact mismatch")) {
      return 1;
    }
  }
  for (std::size_t i = boundaryCoverage.size(); i < output.size(); ++i) {
    if (!check(viennaps::vulkan::runtime::exactlyEqualFloat(output[i], 23.0F),
               "boundary tail guard was overwritten")) {
      return 1;
    }
  }

  constexpr float sentinel = -91.0F;
  std::fill(coverageValues.begin(), coverageValues.end(), 0.5F);
  coverageValues[0] = std::numeric_limits<float>::denorm_min();
  std::fill(output.begin(), output.end(), sentinel);
  if (!writeFloats(coverage, coverageValues, error) ||
      !writeFloats(materialIds, materialValues, error) ||
      !writeFloats(velocity, output, error) ||
      model.evaluate(coverage, 16U, materialIds, 16U, velocity, 16U,
                     strictParams, error) ||
      !readFloats(velocity, output, error)) {
    std::cerr << "subnormal coverage was accepted or output read failed\n";
    return 1;
  }
  for (const auto value : output) {
    if (!check(viennaps::vulkan::runtime::exactlyEqualFloat(value, sentinel),
               "subnormal rejection overwrote output")) {
      return 1;
    }
  }

  std::fill(coverageValues.begin(), coverageValues.end(), 0.5F);
  std::fill(output.begin(), output.end(), sentinel);
  Params nanParams = strictParams;
  nanParams.kEtch = std::numeric_limits<float>::quiet_NaN();
  if (!writeFloats(coverage, coverageValues, error) ||
      !writeFloats(velocity, output, error) ||
      model.evaluate(coverage, 16U, materialIds, 16U, velocity, 16U, nanParams,
                     error) ||
      !readFloats(velocity, output, error)) {
    std::cerr << "non-finite parameter was accepted or output read failed\n";
    return 1;
  }
  for (const auto value : output) {
    if (!check(viennaps::vulkan::runtime::exactlyEqualFloat(value, sentinel),
               "non-finite rejection overwrote output")) {
      return 1;
    }
  }

  std::cout << "[NeutralTransportSurface] lengths=0,1,16,257,1025 "
            << "CPU/Vulkan bit-exact PASS; exact=" << exactValues << "/"
            << comparedValues << " maxULP=" << maxUlp
            << "; strict/boundary bit-exact PASS; nonnormal rejection PASS; "
               "device="
            << model.device().selection().properties.deviceName << '\n';
  return 0;
}
