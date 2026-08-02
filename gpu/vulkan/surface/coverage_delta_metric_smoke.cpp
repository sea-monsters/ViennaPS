// Copyright 2026 ViennaPS
// SPDX-License-Identifier: MIT

#include "coverage_delta_metric.hpp"

#include <array>
#include <bit>
#include <cfloat>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <limits>
#include <string>
#include <vector>

namespace {

using Model = viennaps::vulkan::surface::CoverageDeltaMetricFp32;
using Buffer = viennaps::vulkan::runtime::HostVisibleBuffer;
using DeviceBuffer = viennaps::vulkan::runtime::DeviceBuffer;
using Session = viennaps::vulkan::runtime::ComputeSession;

[[nodiscard]] bool check(const bool condition, const char *message) {
  if (!condition)
    std::cerr << "[coverage-delta] " << message << '\n';
  return condition;
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

[[nodiscard]] std::vector<float>
oracle(const std::vector<float> &updated, const std::vector<float> &previous,
       const std::size_t channels, const std::size_t points) {
  std::vector<float> result(channels);
  for (std::size_t channel = 0U; channel < channels; ++channel) {
    volatile float sum = 0.0F;
    for (std::size_t point = 0U; point < points; ++point) {
      volatile float difference =
          updated[channel * points + point] - previous[channel * points + point];
      volatile float square = difference * difference;
      sum = sum + square;
    }
    volatile float mean = sum / static_cast<float>(points);
    result[channel] = mean;
  }
  return result;
}

[[nodiscard]] bool exact(const float lhs, const float rhs) {
  return std::bit_cast<std::uint32_t>(lhs) == std::bit_cast<std::uint32_t>(rhs);
}

[[nodiscard]] bool runCase(Model &model, const std::size_t channels,
                           const std::size_t points, std::string &error) {
  const auto total = channels * points;
  std::vector<float> updated(total);
  std::vector<float> previous(total);
  for (std::size_t channel = 0U; channel < channels; ++channel) {
    for (std::size_t point = 0U; point < points; ++point) {
      const auto index = channel * points + point;
      updated[index] = 1.0F + static_cast<float>((channel + point) % 7U) * 0.03125F;
      previous[index] = 0.5F + static_cast<float>((channel * 3U + point) % 5U) * 0.015625F;
    }
  }
  const auto expected = oracle(updated, previous, channels, points);
  constexpr float sentinel = -12345.25F;
  std::vector<float> output(channels + 3U, sentinel);
  Buffer updatedBuffer;
  Buffer previousBuffer;
  Buffer outputBuffer;
  if (!model.createFloatBuffer(updated.size(), updatedBuffer, error) ||
      !model.createFloatBuffer(previous.size(), previousBuffer, error) ||
      !model.createFloatBuffer(output.size(), outputBuffer, error) ||
      !write(updatedBuffer, updated, error) || !write(previousBuffer, previous, error) ||
      !write(outputBuffer, output, error) ||
      !model.evaluate(updatedBuffer, updated.size(), previousBuffer,
                      previous.size(), outputBuffer, channels, points,
                      output.size(), error) ||
      !read(outputBuffer, output, error))
    return false;
  for (std::size_t channel = 0U; channel < channels; ++channel) {
    if (!check(exact(output[channel], expected[channel]),
               "CPU/Vulkan metric mismatch"))
      return false;
  }
  for (std::size_t index = channels; index < output.size(); ++index) {
    if (!check(exact(output[index], sentinel), "metric output tail was overwritten"))
      return false;
  }

  // Invalid input must leave every output slot untouched.
  output.assign(channels + 3U, sentinel);
  if (!write(outputBuffer, output, error) ||
      model.evaluate(updatedBuffer, updated.size() - 1U, previousBuffer,
                     previous.size(), outputBuffer, channels, points,
                     output.size(), error) ||
      !read(outputBuffer, output, error))
    return false;
  for (const auto value : output)
    if (!check(exact(value, sentinel), "invalid shape modified output"))
      return false;

  if (!check(!model.evaluate(updatedBuffer, updated.size(), previousBuffer,
                             previous.size(), updatedBuffer, channels, points,
                             updated.size(), error),
             "aliased coverage buffers were accepted"))
    return false;

  output.assign(channels + 3U, sentinel);
  if (!write(outputBuffer, output, error))
    return false;
  const float old = updated.front();
  updated[0] = std::numeric_limits<float>::quiet_NaN();
  if (!write(updatedBuffer, updated, error) ||
      model.evaluate(updatedBuffer, updated.size(), previousBuffer,
                     previous.size(), outputBuffer, channels, points,
                     output.size(), error) ||
      !read(outputBuffer, output, error))
    return false;
  updated[0] = old;
  if (!check(exact(output.front(), sentinel), "non-normal input modified output"))
    return false;
  if (!write(updatedBuffer, updated, error))
    return false;

  output.assign(channels + 3U, sentinel);
  if (!write(outputBuffer, output, error))
    return false;
  updated[0] = std::numeric_limits<float>::denorm_min();
  if (!write(updatedBuffer, updated, error) ||
      model.evaluate(updatedBuffer, updated.size(), previousBuffer,
                     previous.size(), outputBuffer, channels, points,
                     output.size(), error) ||
      !read(outputBuffer, output, error))
    return false;
  updated[0] = old;
  if (!check(exact(output.front(), sentinel), "subnormal input modified output"))
    return false;
  return write(updatedBuffer, updated, error);
}

[[nodiscard]] bool runDeviceCase(Model &model, Session &session,
                                 const std::size_t channels,
                                 const std::size_t points,
                                 std::string &error) {
  const auto total = channels * points;
  std::vector<float> updated(total);
  std::vector<float> previous(total);
  for (std::size_t channel = 0U; channel < channels; ++channel) {
    for (std::size_t point = 0U; point < points; ++point) {
      const auto index = channel * points + point;
      updated[index] =
          1.0F + static_cast<float>((channel + point) % 7U) * 0.03125F;
      previous[index] =
          0.5F + static_cast<float>((channel * 3U + point) % 5U) * 0.015625F;
    }
  }
  const auto expected = oracle(updated, previous, channels, points);
  constexpr float sentinel = -999.5F;
  std::vector<float> output(channels + 2U, sentinel);
  DeviceBuffer updatedBuffer;
  DeviceBuffer previousBuffer;
  DeviceBuffer outputBuffer;
  const auto updatedBytes = static_cast<VkDeviceSize>(updated.size() *
                                                       sizeof(float));
  const auto previousBytes = static_cast<VkDeviceSize>(previous.size() *
                                                        sizeof(float));
  const auto outputBytes =
      static_cast<VkDeviceSize>(output.size() * sizeof(float));
  if (!updatedBuffer.create(session, updatedBytes, error) ||
      !previousBuffer.create(session, previousBytes, error) ||
      !outputBuffer.create(session, outputBytes, error) ||
      !updatedBuffer.upload(session, updated.data(), updatedBytes, 0U, error) ||
      !previousBuffer.upload(session, previous.data(), previousBytes, 0U,
                             error) ||
      !outputBuffer.upload(session, output.data(), outputBytes, 0U, error) ||
      !model.evaluateDevice(updatedBuffer, updated.size(), previousBuffer,
                            previous.size(), outputBuffer, channels, points,
                            output.size(), updated, previous, error) ||
      !outputBuffer.download(session, output.data(), outputBytes, 0U, error))
    return false;
  for (std::size_t channel = 0U; channel < channels; ++channel) {
    if (!check(exact(output[channel], expected[channel]),
               "device CPU/Vulkan metric mismatch"))
      return false;
  }
  for (std::size_t index = channels; index < output.size(); ++index) {
    if (!check(exact(output[index], sentinel),
               "device metric output tail was overwritten"))
      return false;
  }
  return check(!model.evaluateDevice(updatedBuffer, updated.size(),
                                     previousBuffer, previous.size(),
                                     updatedBuffer, channels, points,
                                     updated.size(), updated, previous, error),
               "aliased device coverage buffers were accepted");
}

} // namespace

int main() {
  std::string error;
  Session session;
  Model model;
  if (!check(session.initialize(error), "failed to initialize compute session") ||
      !check(model.initialize(session,
                              VIENNAPS_VULKAN_COVERAGE_DELTA_METRIC_SPV_PATH,
                              error),
             "failed to initialize coverage delta metric") ||
      !error.empty()) {
    if (!error.empty())
      std::cerr << error << '\n';
    return 1;
  }
  for (const auto points : std::array<std::size_t, 3U>{1U, 16U, 257U}) {
    if (!runCase(model, 3U, points, error)) {
      std::cerr << "coverage delta case failed: " << error << '\n';
      return 1;
    }
  }
  if (!runDeviceCase(model, session, 3U, 257U, error)) {
    std::cerr << "device coverage delta case failed: " << error << '\n';
    return 1;
  }

  // N=0 and an FP32 overflow are rejected before dispatch and preserve output.
  Buffer emptyUpdated;
  Buffer emptyPrevious;
  Buffer emptyOutput;
  std::vector<float> sentinel{77.0F, 88.0F};
  if (!model.createFloatBuffer(1U, emptyUpdated, error) ||
      !model.createFloatBuffer(1U, emptyPrevious, error) ||
      !model.createFloatBuffer(sentinel.size(), emptyOutput, error) ||
      !write(emptyOutput, sentinel, error) ||
      model.evaluate(emptyUpdated, 0U, emptyPrevious, 0U, emptyOutput, 1U, 0U,
                     sentinel.size(), error) ||
      !read(emptyOutput, sentinel, error) || !check(exact(sentinel[0], 77.0F),
                                                   "N=0 modified output"))
    return 1;

  std::vector<float> overflow{FLT_MAX};
  std::vector<float> zero{0.0F};
  sentinel.assign(2U, 91.0F);
  if (!write(emptyUpdated, overflow, error) || !write(emptyPrevious, zero, error) ||
      !write(emptyOutput, sentinel, error) ||
      model.evaluate(emptyUpdated, 1U, emptyPrevious, 1U, emptyOutput, 1U, 1U,
                     sentinel.size(), error) ||
      !read(emptyOutput, sentinel, error) ||
      !check(exact(sentinel[0], 91.0F), "overflow modified output"))
    return 1;

  std::cout << "[CoverageDeltaMetric] CPU/Vulkan exact PASS for N=1,16,257\n";
  return 0;
}
