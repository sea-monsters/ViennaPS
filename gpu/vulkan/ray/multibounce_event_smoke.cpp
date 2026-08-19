// Copyright 2026 ViennaPS
// SPDX-License-Identifier: MIT
#include "multibounce_event.hpp"

#include <bit>
#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

#ifndef VIENNAPS_VULKAN_MULTIBOUNCE_EVENT_SPV_PATH
#define VIENNAPS_VULKAN_MULTIBOUNCE_EVENT_SPV_PATH "multibounce_event.comp.spv"
#endif

using namespace viennaps::vulkan::ray;
using viennaps::vulkan::runtime::ComputeSession;

int main() {
  std::string error;
  ComputeSession session;
  if (!session.initialize(error)) {
    std::cerr << "multibounce event Vulkan dispatch FAIL [session]: " << error
              << '\n';
    return 1;
  }
  DeviceMultibounceSlice slice;
  if (!slice.initialize(session, VIENNAPS_VULKAN_MULTIBOUNCE_EVENT_SPV_PATH,
                        error)) {
    std::cerr << "multibounce event Vulkan dispatch FAIL [initialize]: "
              << error << '\n';
    return 1;
  }
  constexpr std::uint32_t stride = 2U;
  std::vector<MultibounceEvent> input(3U);
  for (std::uint32_t i = 0U; i < input.size(); ++i) {
    input[i].particle = i;
    input[i].sequence = 100U + i;
    input[i].activeFlag = 1U;
    input[i].weight = i == 0U ? 1.0F : (i == 1U ? 0.05F : 0.05F);
    input[i].surfaceId = i == 2U ? 1U : 0U;
  }
  std::vector<MultibounceDecision> decisions(input.size() * stride);
  for (std::uint32_t i = 0U; i < input.size(); ++i) {
    auto &first = decisions[i * stride];
    first.particle = i;
    first.bounce = 0U;
    first.sequence = 100U + i;
    first.surfaceId = input[i].surfaceId;
    first.weight = input[i].weight;
    first.nextWeight = i == 2U ? 0.0F : input[i].weight * 0.5F;
    first.contribution = input[i].weight;
    first.successorOrigin[0] = 10.0F + static_cast<float>(i);
    first.successorOrigin[1] = 11.0F + static_cast<float>(i);
    first.successorOrigin[2] = 12.0F + static_cast<float>(i);
    first.successorOrigin[3] = 13.0F + static_cast<float>(i);
    first.successorDirection[0] = 1.0F + static_cast<float>(i);
    first.successorDirection[1] = 2.0F + static_cast<float>(i);
    first.successorDirection[2] = 3.0F + static_cast<float>(i);
    first.successorDirection[3] = 4.0F + static_cast<float>(i);
    first.action = i == 2U ? static_cast<std::uint32_t>(MultibounceAction::rouletteReject)
                           : static_cast<std::uint32_t>(MultibounceAction::continueRay);
    auto &second = decisions[i * stride + 1U];
    second.particle = i;
    second.bounce = 1U;
    second.sequence = 100U + i;
    second.surfaceId = input[i].surfaceId + 1U;
    second.weight = first.nextWeight;
    second.nextWeight = first.nextWeight;
    second.contribution = i == 1U ? first.nextWeight : 0.0F;
    second.successorOrigin[0] = 20.0F + static_cast<float>(i);
    second.successorOrigin[1] = 21.0F + static_cast<float>(i);
    second.successorOrigin[2] = 22.0F + static_cast<float>(i);
    second.successorOrigin[3] = 23.0F + static_cast<float>(i);
    second.successorDirection[0] = 5.0F + static_cast<float>(i);
    second.successorDirection[1] = 6.0F + static_cast<float>(i);
    second.successorDirection[2] = 7.0F + static_cast<float>(i);
    second.successorDirection[3] = 8.0F + static_cast<float>(i);
    second.action = i == 1U ? static_cast<std::uint32_t>(MultibounceAction::rouletteReject)
                            : static_cast<std::uint32_t>(MultibounceAction::terminate);
  }
  MultibounceSliceResult result;
  if (!slice.run(input, decisions, stride, 1U, result, error)) {
    std::cerr << "multibounce event Vulkan dispatch FAIL [run]: " << error
              << '\n';
    return 1;
  }
  if (result.events.size() != input.size() || result.accumulation.size() != 2U * input.size()) {
    std::cerr << "multibounce event Vulkan dispatch FAIL [shape]\n";
    return 1;
  }
  if (result.events[0].bounce != 2U || result.events[0].activeFlag != 0U ||
      result.events[1].bounce != 2U || result.events[1].activeFlag != 0U ||
      result.events[2].bounce != 1U || result.events[2].activeFlag != 0U ||
      std::bit_cast<std::uint32_t>(result.accumulation[0].weightBits) !=
          std::bit_cast<std::uint32_t>(1.0F) ||
      std::bit_cast<std::uint32_t>(result.accumulation[1].weightBits) !=
          std::bit_cast<std::uint32_t>(0.05F)) {
    std::cerr << "multibounce event Vulkan dispatch FAIL [oracle] "
              << result.events[0].bounce << "/" << result.events[0].activeFlag
              << " " << result.events[1].bounce << "/" << result.events[1].activeFlag
              << " " << result.events[2].bounce << "/" << result.events[2].activeFlag
              << " bits=" << std::hex << result.accumulation[0].weightBits
              << "/" << result.accumulation[1].weightBits << std::dec << '\n';
    return 1;
  }
  const auto expectBits = [](float actual, float expected) {
    return std::bit_cast<std::uint32_t>(actual) ==
           std::bit_cast<std::uint32_t>(expected);
  };
  if (!expectBits(result.events[0].origin[0], 20.0F) ||
      !expectBits(result.events[0].origin[3], 23.0F) ||
      !expectBits(result.events[0].direction[0], 5.0F) ||
      !expectBits(result.events[0].direction[3], 8.0F) ||
      !expectBits(result.events[1].origin[0], 21.0F) ||
      !expectBits(result.events[1].direction[3], 9.0F)) {
    std::cerr << "multibounce event Vulkan dispatch FAIL [successor-wire]\n";
    return 1;
  }
  // Malformed decisions are rejected before publication and do not overwrite
  // the caller's already-published result.
  auto malformed = decisions;
  malformed[0].particle = 99U;
  MultibounceSliceResult published = result;
  if (slice.run(input, malformed, stride, 1U, published, error) ||
      published.events.size() != result.events.size() ||
      published.events[0].sequence != result.events[0].sequence) {
    std::cerr << "multibounce event Vulkan dispatch FAIL [fail-closed]\n";
    return 1;
  }
  std::vector<MultibounceEvent> overflowInput(1025U, input.front());
  std::vector<MultibounceDecision> overflowDecisions(overflowInput.size() * stride,
                                                     decisions.front());
  published = result;
  if (slice.run(overflowInput, overflowDecisions, stride, 1U, published,
                error) ||
      published.events[0].sequence != result.events[0].sequence) {
    std::cerr << "multibounce event Vulkan dispatch FAIL [overflow]\n";
    return 1;
  }
  slice.reset();
  published = result;
  if (slice.run(input, decisions, stride, 1U, published, error) ||
      published.events[0].sequence != result.events[0].sequence) {
    std::cerr << "multibounce event Vulkan dispatch FAIL [session-loss]\n";
    return 1;
  }
  std::cout << "multibounce event Vulkan dispatch PASS\n";
  return 0;
}
