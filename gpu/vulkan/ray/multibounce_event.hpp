// Copyright 2026 ViennaPS
// SPDX-License-Identifier: MIT
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "../runtime/compute_session.hpp"

namespace viennaps::vulkan::ray {

enum class MultibounceAction : std::uint32_t {
  terminate = 0U,
  continueRay = 1U,
  rouletteReject = 2U,
};

// Fixed std430 wire records. The host produces these records from the
// authoritative ViennaRay callbacks; the shader only applies their decisions.
struct alignas(16) MultibounceEvent {
  float origin[4]{};
  float direction[4]{};
  std::uint32_t particle{0U};
  std::uint32_t bounce{0U};
  std::uint32_t sequence{0U};
  std::uint32_t activeFlag{0U};
  std::uint32_t surfaceId{0U};
  float weight{0.0F};
  float nextWeight{0.0F};
  std::uint32_t action{0U};
};

struct alignas(16) MultibounceDecision {
  std::uint32_t particle{0U};
  std::uint32_t bounce{0U};
  std::uint32_t sequence{0U};
  std::uint32_t surfaceId{0U};
  float weight{0.0F};
  float nextWeight{0.0F};
  float contribution{0.0F};
  std::uint32_t action{0U};
  float successorOrigin[4]{};
  float successorDirection[4]{};
};

struct alignas(16) MultibounceAccumulation {
  std::uint32_t surfaceId{0U};
  std::uint32_t weightBits{0U};
  std::uint32_t active{0U};
  std::uint32_t sequence{0U};
};

static_assert(sizeof(MultibounceEvent) == 64U);
static_assert(sizeof(MultibounceDecision) == 64U);
static_assert(sizeof(MultibounceAccumulation) == 16U);

struct MultibounceSliceResult {
  std::vector<MultibounceEvent> events;
  std::vector<MultibounceAccumulation> accumulation;
};

class DeviceMultibounceSlice {
public:
  DeviceMultibounceSlice() = default;
  ~DeviceMultibounceSlice();
  DeviceMultibounceSlice(const DeviceMultibounceSlice &) = delete;
  DeviceMultibounceSlice &operator=(const DeviceMultibounceSlice &) = delete;

  [[nodiscard]] bool initialize(runtime::ComputeSession &session,
                                std::string_view spirv, std::string &error);
  void reset();
  [[nodiscard]] bool isInitialized() const;

  // Runs two device queue transitions. Decisions are indexed by
  // lane*decisionStride + event.bounce. No CPU event loop is used.
  [[nodiscard]] bool run(std::span<const MultibounceEvent> input,
                         std::span<const MultibounceDecision> decisions,
                         std::uint32_t decisionStride,
                         std::uint32_t maxReflections,
                         MultibounceSliceResult &output, std::string &error);

private:
  runtime::ComputeSession *session_{nullptr};
  runtime::ShaderModule shader_{};
  runtime::DescriptorSetLayout layout_{};
  runtime::PipelineLayout pipelineLayout_{};
  runtime::ComputePipeline pipeline_{};
  runtime::DescriptorPool pool_{};
  VkDescriptorSet descriptors_[2]{VK_NULL_HANDLE, VK_NULL_HANDLE};
  VkCommandBuffer commandBuffer_{VK_NULL_HANDLE};
  runtime::Fence fence_{};
  runtime::DeviceBuffer events_[2]{};
  runtime::DeviceBuffer decisions_{};
  runtime::DeviceBuffer accumulation_{};
  runtime::DeviceBuffer status_{};
};

} // namespace viennaps::vulkan::ray
