// Copyright 2026 ViennaPS
// SPDX-License-Identifier: MIT
#include "ray_reducer.hpp"
#include "ray_surface_reduction.hpp"

#include <array>
#include <bit>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <span>
#include <vector>

#ifndef VIENNAPS_VULKAN_RAY_SURFACE_SEGMENTS_SPV_PATH
#define VIENNAPS_VULKAN_RAY_SURFACE_SEGMENTS_SPV_PATH                          \
  "ray_surface_segments.comp.spv"
#endif
#ifndef VIENNAPS_VULKAN_RAY_SURFACE_REDUCE_SPV_PATH
#define VIENNAPS_VULKAN_RAY_SURFACE_REDUCE_SPV_PATH                            \
  "ray_surface_reduce.comp.spv"
#endif
#ifndef VIENNAPS_VULKAN_REDUCTION_SCAN_SPV_PATH
#define VIENNAPS_VULKAN_REDUCTION_SCAN_SPV_PATH "reduction_scan.comp.spv"
#endif

using namespace viennaps::vulkan::ray;
using viennaps::vulkan::runtime::ComputeSession;
using viennaps::vulkan::runtime::DeviceBuffer;

int main() {
  std::string error;
  ComputeSession session;
  DeviceRaySurfaceReducer reducer;
  if (!session.initialize(error) ||
      !reducer.initialize(session,
                          VIENNAPS_VULKAN_RAY_SURFACE_SEGMENTS_SPV_PATH,
                          VIENNAPS_VULKAN_RAY_SURFACE_REDUCE_SPV_PATH,
                          VIENNAPS_VULKAN_REDUCTION_SCAN_SPV_PATH, error)) {
    std::cerr << error << '\n';
    return 1;
  }
  constexpr std::size_t capacity = 8U;
  constexpr std::uint32_t active = 7U;
  const std::array<RayRecord, capacity> records{{
      {0U, 1U, 0x80000000U, 0U},
      {3U, 1U, std::bit_cast<std::uint32_t>(1.5F), 0U},
      {1U, 2U, std::bit_cast<std::uint32_t>(2.0F), 0U},
      {4U, 3U, 0x80000000U, 0U},
      {2U, 4U, std::bit_cast<std::uint32_t>(1.0e20F), 0U},
      {5U, 4U, std::bit_cast<std::uint32_t>(-1.0e20F), 0U},
      {7U, 4U, std::bit_cast<std::uint32_t>(0.25F), 0U},
      {0xdecafbadU, 0xabcdef01U, 0x13579bdfU, 0x2468ace0U},
  }};
  std::vector<std::uint32_t> rayIds(active), surfaceIds(active);
  std::vector<float> weights(active);
  for (std::size_t i = 0U; i < active; ++i) {
    rayIds[i] = records[i].rayId;
    surfaceIds[i] = records[i].surfaceId;
    weights[i] = std::bit_cast<float>(records[i].weightBits);
  }
  std::array<std::uint32_t, capacity> expectedSurface{};
  std::array<float, capacity> expectedWeight{};
  RayReduction expected{expectedSurface, expectedWeight, 0U};
  const RayRecordSoA oracle{rayIds, surfaceIds, weights, {}};
  if (!reduceCpu(oracle, 5U, expected, error)) {
    std::cerr << error << '\n';
    return 1;
  }
  DeviceBuffer inputRecords, inputCount, outputSurface, outputWeight;
  if (!inputRecords.create(session, sizeof(records), error) ||
      !inputCount.create(session, sizeof(active), error) ||
      !outputSurface.create(session, capacity * sizeof(std::uint32_t), error) ||
      !outputWeight.create(session, capacity * sizeof(float), error) ||
      !inputRecords.upload(session, records.data(), sizeof(records), 0U,
                           error) ||
      !inputCount.upload(session, &active, sizeof(active), 0U, error)) {
    std::cerr << error << '\n';
    return 1;
  }
  const std::array<std::uint32_t, capacity> surfaceSentinel{
      0xdecafbadU, 0xdecafbadU, 0xdecafbadU, 0xdecafbadU,
      0xdecafbadU, 0xdecafbadU, 0xdecafbadU, 0xdecafbadU};
  const std::array<std::uint32_t, capacity> weightSentinel{
      0xabcdef01U, 0xabcdef01U, 0xabcdef01U, 0xabcdef01U,
      0xabcdef01U, 0xabcdef01U, 0xabcdef01U, 0xabcdef01U};
  if (!outputSurface.upload(session, surfaceSentinel.data(),
                            sizeof(surfaceSentinel), 0U, error) ||
      !outputWeight.upload(session, weightSentinel.data(),
                           sizeof(weightSentinel), 0U, error)) {
    std::cerr << error << '\n';
    return 1;
  }
  DeviceRaySurfaceReductionOutput reduction;
  if (!reducer.reduce(inputRecords, inputCount, capacity, outputSurface,
                      outputWeight, capacity, reduction, error)) {
    std::cerr << error << '\n';
    return 1;
  }
  std::uint32_t actualCount = 0U;
  std::array<std::uint32_t, capacity> actualSurface{};
  std::array<std::uint32_t, capacity> actualWeight{};
  std::array<std::uint32_t, capacity> flags{};
  std::array<std::uint32_t, capacity> offsets{};
  if (!reduction.count.download(session, &actualCount, sizeof(actualCount), 0U,
                                error) ||
      !outputSurface.download(session, actualSurface.data(),
                              sizeof(actualSurface), 0U, error) ||
      !outputWeight.download(session, actualWeight.data(), sizeof(actualWeight),
                             0U, error) ||
      !reduction.flags.download(session, flags.data(), sizeof(flags), 0U,
                                error) ||
      !reduction.offsets.download(session, offsets.data(), sizeof(offsets), 0U,
                                  error)) {
    std::cerr << error << '\n';
    return 1;
  }
  if (actualCount != expected.count)
    return 1;
  for (std::size_t i = 0U; i < expected.count; ++i)
    if (actualSurface[i] != expectedSurface[i] ||
        actualWeight[i] != std::bit_cast<std::uint32_t>(expectedWeight[i]))
      return 1;
  for (std::size_t i = expected.count; i < capacity; ++i)
    if (actualSurface[i] != surfaceSentinel[i] ||
        actualWeight[i] != weightSentinel[i])
      return 1;
  constexpr std::array<std::uint32_t, capacity> expectedFlags{1U, 0U, 1U, 1U,
                                                              1U, 0U, 0U, 0U};
  constexpr std::array<std::uint32_t, capacity> expectedOffsets{0U, 1U, 1U, 2U,
                                                                3U, 4U, 4U, 4U};
  if (flags != expectedFlags || offsets != expectedOffsets)
    return 1;
  if (std::bit_cast<std::uint32_t>(expectedWeight[2]) != 0x80000000U)
    return 1;

  std::string rejected;
  if (reducer.reduce(inputRecords, inputCount, capacity, inputRecords,
                     outputWeight, capacity, reduction, rejected) ||
      reducer.reduce(inputRecords, inputCount, capacity, outputSurface,
                     outputWeight, capacity - 1U, reduction, rejected) ||
      !reducer.reduce(inputRecords, inputCount, 0U, outputSurface, outputWeight,
                      0U, reduction, rejected))
    return 1;
  ComputeSession foreign;
  DeviceBuffer foreignOutput;
  if (!foreign.initialize(error) ||
      !foreignOutput.create(foreign, capacity * sizeof(std::uint32_t), error) ||
      reducer.reduce(inputRecords, inputCount, capacity, foreignOutput,
                     outputWeight, capacity, reduction, rejected))
    return 1;
  std::array<std::uint32_t, capacity> afterRejected{};
  if (!outputSurface.download(session, afterRejected.data(),
                              sizeof(afterRejected), 0U, error) ||
      std::memcmp(afterRejected.data(), actualSurface.data(),
                  sizeof(actualSurface)) != 0) {
    std::cerr << "rejected surface reduction modified output\n";
    return 1;
  }
  std::cout << "ray surface reduction Vulkan dispatch PASS\n";
  return 0;
}
