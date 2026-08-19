// Copyright 2026 ViennaPS
// SPDX-License-Identifier: MIT
#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "../runtime/compute_session.hpp"
#include "multibounce_event.hpp"

namespace viennaps::vulkan::ray {

struct MultibounceFrontier {
  std::vector<MultibounceEvent> events;
  std::vector<MultibounceDecision> decisions;
  std::uint32_t decisionStride{0U};
};

struct MultibounceFrontierLimits {
  std::uint32_t maxTotalEvents{0U};
  std::uint32_t maxFrontierEvents{0U};
  std::uint32_t maxRounds{0U};
  std::uint32_t maxReflections{0U};
};

struct MultibounceFrontierResult {
  std::vector<MultibounceEvent> terminalEvents;
  std::vector<MultibounceAccumulation> accumulation;
  std::uint32_t rounds{0U};
  std::uint32_t totalEvents{0U};
};

// Runs bounded host/device frontiers. Host supplies complete CPU semantic
// decisions for each frontier; this class performs only one real device
// apply/classify transition per frontier and publishes transactionally.
class MultibounceFrontierQueue {
public:
  MultibounceFrontierQueue() = default;
  ~MultibounceFrontierQueue();
  MultibounceFrontierQueue(const MultibounceFrontierQueue &) = delete;
  MultibounceFrontierQueue &operator=(const MultibounceFrontierQueue &) = delete;

  [[nodiscard]] bool initialize(runtime::ComputeSession &session,
                                std::string_view spirv, std::string &error);
  void reset();
  [[nodiscard]] bool isInitialized() const;

  [[nodiscard]] bool run(std::span<const MultibounceFrontier> frontiers,
                         const MultibounceFrontierLimits &limits,
                         MultibounceFrontierResult &output,
                         std::string &error);

private:
  runtime::ComputeSession *session_{nullptr};
  std::uint64_t sessionGeneration_{0U};
  runtime::ShaderModule shader_{};
  runtime::DescriptorSetLayout layout_{};
  runtime::PipelineLayout pipelineLayout_{};
  runtime::ComputePipeline pipeline_{};
  runtime::DescriptorPool pool_{};
  VkDescriptorSet descriptor_{VK_NULL_HANDLE};
  VkCommandBuffer commandBuffer_{VK_NULL_HANDLE};
  runtime::Fence fence_{};
  runtime::DeviceBuffer eventsIn_{};
  runtime::DeviceBuffer eventsOut_{};
  runtime::DeviceBuffer decisions_{};
  runtime::DeviceBuffer accumulation_{};
  runtime::DeviceBuffer status_{};
};

} // namespace viennaps::vulkan::ray
