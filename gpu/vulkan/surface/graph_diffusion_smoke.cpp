// Copyright 2026 ViennaPS
// SPDX-License-Identifier: MIT

#include "graph_diffusion.hpp"

#include <array>
#include <bit>
#include <cstdint>
#include <iostream>
#include <limits>
#include <string>
#include <vector>

namespace {

using Model = viennaps::vulkan::surface::SurfaceGraphDiffusionFp32;
using Buffer = viennaps::vulkan::runtime::HostVisibleBuffer;

[[nodiscard]] bool check(const bool condition, const char *message) {
  if (!condition) {
    std::cerr << "[graph-diffusion] " << message << '\n';
    return false;
  }
  return true;
}

[[nodiscard]] std::uint32_t ulpDistance(const float lhs, const float rhs) {
  const auto left = viennaps::vulkan::runtime::orderedFloatBits(lhs);
  const auto right = viennaps::vulkan::runtime::orderedFloatBits(rhs);
  return left > right ? left - right : right - left;
}

template <class T>
[[nodiscard]] bool write(Buffer &buffer, const std::vector<T> &values,
                         std::string &error) {
  return values.empty() ||
         buffer.write(values.data(), values.size() * sizeof(T), 0U, error);
}

template <class T>
[[nodiscard]] bool read(Buffer &buffer, std::vector<T> &values,
                        std::string &error) {
  return values.empty() ||
         buffer.read(values.data(), values.size() * sizeof(T), 0U, error);
}

} // namespace

int main() {
  std::string error;
  Model model;
  if (!check(model.initialize(VIENNAPS_VULKAN_GRAPH_DIFFUSION_SPV_PATH, error),
             "failed to initialize graph diffusion model")) {
    std::cerr << error << '\n';
    return 1;
  }

  const std::vector<std::uint32_t> rowOffsets{0U, 2U, 5U, 7U, 9U, 11U};
  const std::vector<std::uint32_t> columns{0U, 1U, 0U, 1U, 2U, 1U,
                                           2U, 2U, 3U, 3U, 4U};
  const std::vector<float> weights{0.25F,  -0.25F, 0.5F,    0.25F,
                                   -0.75F, 0.125F, -0.125F, 0.75F,
                                   -0.75F, 0.5F,   -0.5F};
  const std::vector<float> field{1.0F, -2.0F, 3.0F, 4.0F, -5.0F};
  constexpr float step = 0.125F;
  std::vector<float> expected(field.size());
  for (std::size_t i = 0U; i < field.size(); ++i) {
    volatile float laplacian = 0.0F;
    for (std::size_t edge = rowOffsets[i]; edge < rowOffsets[i + 1U]; ++edge) {
      volatile float product = weights[edge] * field[columns[edge]];
      laplacian = laplacian + product;
    }
    volatile float scaled = step * laplacian;
    expected[i] = field[i] + scaled;
  }

  Buffer rowBuffer;
  Buffer columnBuffer;
  Buffer weightBuffer;
  Buffer fieldBuffer;
  Buffer outputBuffer;
  if (!model.createIndexBuffer(rowOffsets.size(), rowBuffer, error) ||
      !model.createIndexBuffer(columns.size(), columnBuffer, error) ||
      !model.createFloatBuffer(weights.size(), weightBuffer, error) ||
      !model.createFloatBuffer(field.size(), fieldBuffer, error) ||
      !model.createFloatBuffer(field.size() + 3U, outputBuffer, error) ||
      !write(rowBuffer, rowOffsets, error) ||
      !write(columnBuffer, columns, error) ||
      !write(weightBuffer, weights, error) ||
      !write(fieldBuffer, field, error)) {
    std::cerr << "buffer setup failed: " << error << '\n';
    return 1;
  }

  constexpr float sentinel = -12345.25F;
  std::vector<float> output(field.size() + 3U, sentinel);
  if (!write(outputBuffer, output, error) ||
      !model.evaluate(rowBuffer, rowOffsets.size(), columnBuffer,
                      columns.size(), weightBuffer, weights.size(), fieldBuffer,
                      field.size(), outputBuffer, output.size(), step, error) ||
      !read(outputBuffer, output, error)) {
    std::cerr << "evaluation failed: " << error << '\n';
    return 1;
  }
  for (std::size_t i = 0U; i < field.size(); ++i) {
    if (!check(ulpDistance(output[i], expected[i]) == 0U,
               "CPU/Vulkan graph diffusion mismatch")) {
      std::cerr << "index=" << i << " expected=" << expected[i]
                << " actual=" << output[i] << '\n';
      return 1;
    }
  }
  for (std::size_t i = field.size(); i < output.size(); ++i) {
    if (!check(
            viennaps::vulkan::runtime::exactlyEqualFloat(output[i], sentinel),
            "tail guard was overwritten")) {
      return 1;
    }
  }

  // A row-sum-zero CSR matrix must preserve a constant field exactly.
  const std::vector<float> constantField(field.size(), 3.25F);
  std::vector<float> constantOutput(output.size(), sentinel);
  if (!write(fieldBuffer, constantField, error) ||
      !write(outputBuffer, constantOutput, error) ||
      !model.evaluate(rowBuffer, rowOffsets.size(), columnBuffer,
                      columns.size(), weightBuffer, weights.size(), fieldBuffer,
                      constantField.size(), outputBuffer, constantOutput.size(),
                      step, error) ||
      !read(outputBuffer, constantOutput, error)) {
    std::cerr << "constant-field evaluation failed: " << error << '\n';
    return 1;
  }
  for (std::size_t i = 0U; i < constantField.size(); ++i) {
    if (!check(viennaps::vulkan::runtime::exactlyEqualFloat(constantOutput[i],
                                                            constantField[i]),
               "constant field was not preserved")) {
      return 1;
    }
  }
  for (std::size_t i = constantField.size(); i < constantOutput.size(); ++i) {
    if (!check(viennaps::vulkan::runtime::exactlyEqualFloat(constantOutput[i],
                                                            sentinel),
               "constant-field tail guard was overwritten")) {
      return 1;
    }
  }

  // This three-node symmetric stencil checks peak smoothing, conservation,
  // and the unaligned dispatch tail in one compact case.
  const std::vector<std::uint32_t> peakRows{0U, 2U, 5U, 7U};
  const std::vector<std::uint32_t> peakColumns{0U, 1U, 0U, 1U, 2U, 1U, 2U};
  const std::vector<float> peakWeights{-.5F, .5F, .5F, -1.0F, .5F, .5F, -.5F};
  const std::vector<float> peakField{0.0F, 1.0F, 0.0F};
  constexpr float peakStep = 0.25F;
  const std::vector<float> peakExpected{0.125F, 0.75F, 0.125F};
  Buffer peakRowBuffer;
  Buffer peakColumnBuffer;
  Buffer peakWeightBuffer;
  Buffer peakFieldBuffer;
  Buffer peakOutputBuffer;
  std::vector<float> peakOutput(peakField.size() + 5U, sentinel);
  if (!model.createIndexBuffer(peakRows.size(), peakRowBuffer, error) ||
      !model.createIndexBuffer(peakColumns.size(), peakColumnBuffer, error) ||
      !model.createFloatBuffer(peakWeights.size(), peakWeightBuffer, error) ||
      !model.createFloatBuffer(peakField.size(), peakFieldBuffer, error) ||
      !model.createFloatBuffer(peakOutput.size(), peakOutputBuffer, error) ||
      !write(peakRowBuffer, peakRows, error) ||
      !write(peakColumnBuffer, peakColumns, error) ||
      !write(peakWeightBuffer, peakWeights, error) ||
      !write(peakFieldBuffer, peakField, error) ||
      !write(peakOutputBuffer, peakOutput, error) ||
      !model.evaluate(peakRowBuffer, peakRows.size(), peakColumnBuffer,
                      peakColumns.size(), peakWeightBuffer, peakWeights.size(),
                      peakFieldBuffer, peakField.size(), peakOutputBuffer,
                      peakOutput.size(), peakStep, error) ||
      !read(peakOutputBuffer, peakOutput, error)) {
    std::cerr << "peak evaluation failed: " << error << '\n';
    return 1;
  }
  float beforeMass = 0.0F;
  float afterMass = 0.0F;
  for (std::size_t i = 0U; i < peakField.size(); ++i) {
    beforeMass += peakField[i];
    afterMass += peakOutput[i];
    if (!check(ulpDistance(peakOutput[i], peakExpected[i]) == 0U,
               "symmetric peak result is not bit-exact")) {
      return 1;
    }
  }
  if (!check(afterMass == beforeMass,
             "symmetric graph did not conserve mass") ||
      !check(peakOutput[1U] < peakField[1U],
             "symmetric graph did not smooth the peak")) {
    return 1;
  }
  for (std::size_t i = peakField.size(); i < peakOutput.size(); ++i) {
    if (!check(viennaps::vulkan::runtime::exactlyEqualFloat(peakOutput[i],
                                                            sentinel),
               "peak tail guard was overwritten")) {
      return 1;
    }
  }

  std::fill(output.begin(), output.end(), sentinel);
  if (!write(outputBuffer, output, error) ||
      model.evaluate(rowBuffer, rowOffsets.size() - 1U, columnBuffer,
                     columns.size(), weightBuffer, weights.size(), fieldBuffer,
                     field.size(), outputBuffer, output.size(), step, error) ||
      !read(outputBuffer, output, error)) {
    std::cerr << "invalid row-offset length was accepted\n";
    return 1;
  }
  if (!check(viennaps::vulkan::runtime::exactlyEqualFloat(output.front(),
                                                          sentinel),
             "invalid CSR input overwrote output")) {
    return 1;
  }

  std::vector<std::uint32_t> invalidColumns = columns;
  invalidColumns[0U] = static_cast<std::uint32_t>(field.size());
  std::fill(output.begin(), output.end(), sentinel);
  if (!write(columnBuffer, invalidColumns, error) ||
      !write(outputBuffer, output, error) ||
      model.evaluate(rowBuffer, rowOffsets.size(), columnBuffer, columns.size(),
                     weightBuffer, weights.size(), fieldBuffer, field.size(),
                     outputBuffer, output.size(), step, error) ||
      !read(outputBuffer, output, error)) {
    std::cerr << "out-of-range column was accepted\n";
    return 1;
  }
  if (!check(viennaps::vulkan::runtime::exactlyEqualFloat(output.front(),
                                                          sentinel),
             "out-of-range column overwrote output")) {
    return 1;
  }
  if (!write(columnBuffer, columns, error)) {
    std::cerr << "failed to restore CSR columns: " << error << '\n';
    return 1;
  }

  std::vector<float> invalidWeights = weights;
  invalidWeights[0U] = std::numeric_limits<float>::quiet_NaN();
  std::fill(output.begin(), output.end(), sentinel);
  if (!write(weightBuffer, invalidWeights, error) ||
      !write(outputBuffer, output, error) ||
      model.evaluate(rowBuffer, rowOffsets.size(), columnBuffer, columns.size(),
                     weightBuffer, weights.size(), fieldBuffer, field.size(),
                     outputBuffer, output.size(), step, error) ||
      !read(outputBuffer, output, error)) {
    std::cerr << "non-finite weight was accepted\n";
    return 1;
  }
  if (!check(viennaps::vulkan::runtime::exactlyEqualFloat(output.front(),
                                                          sentinel),
             "non-finite weight overwrote output")) {
    return 1;
  }
  if (!write(weightBuffer, weights, error)) {
    std::cerr << "failed to restore CSR weights: " << error << '\n';
    return 1;
  }

  std::fill(output.begin(), output.end(), sentinel);
  if (!write(outputBuffer, output, error) ||
      model.evaluate(rowBuffer, rowOffsets.size(), columnBuffer, columns.size(),
                     weightBuffer, weights.size(), fieldBuffer, field.size(),
                     outputBuffer, field.size() - 1U, step, error) ||
      !read(outputBuffer, output, error)) {
    std::cerr << "undersized output was accepted\n";
    return 1;
  }
  if (!check(viennaps::vulkan::runtime::exactlyEqualFloat(output.front(),
                                                          sentinel),
             "undersized output overwrote output")) {
    return 1;
  }

  // Empty CSR is a valid no-op and must leave the caller's output untouched.
  Buffer emptyRows;
  Buffer emptyColumns;
  Buffer emptyWeights;
  Buffer emptyField;
  Buffer emptyOutput;
  std::vector<std::uint32_t> emptyRowValues{0U};
  std::vector<float> emptySentinel{sentinel};
  if (!model.createIndexBuffer(emptyRowValues.size(), emptyRows, error) ||
      !model.createIndexBuffer(0U, emptyColumns, error) ||
      !model.createFloatBuffer(0U, emptyWeights, error) ||
      !model.createFloatBuffer(0U, emptyField, error) ||
      !model.createFloatBuffer(emptySentinel.size(), emptyOutput, error) ||
      !write(emptyRows, emptyRowValues, error) ||
      !write(emptyOutput, emptySentinel, error) ||
      !model.evaluate(emptyRows, 1U, emptyColumns, 0U, emptyWeights, 0U,
                      emptyField, 0U, emptyOutput, 1U, step, error) ||
      !read(emptyOutput, emptySentinel, error)) {
    std::cerr << "empty CSR evaluation failed: " << error << '\n';
    return 1;
  }
  if (!check(viennaps::vulkan::runtime::exactlyEqualFloat(emptySentinel[0U],
                                                          sentinel),
             "empty CSR wrote output")) {
    return 1;
  }

  std::cout << "[GraphDiffusion] CPU/Vulkan exact PASS, N=" << field.size()
            << " nnz=" << columns.size() << '\n';
  return 0;
}
