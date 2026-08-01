// Copyright 2026 ViennaPS
// SPDX-License-Identifier: MIT
//
// RAII wrapper around a reusable Vulkan compute session (instance + device +
// queue family + command pool) for future primitive composition.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>

#include "vulkan_compute_runtime.hpp"

namespace viennaps::vulkan::runtime {

struct ComputeSessionOptions {
  std::uint32_t manualDeviceIndex = std::numeric_limits<std::uint32_t>::max();
  std::string_view manualDeviceName;
  std::string_view manualDeviceUuid;
};

class ComputeSession {
public:
  ComputeSession() = default;
  ~ComputeSession();
  ComputeSession(const ComputeSession &) = delete;
  ComputeSession &operator=(const ComputeSession &) = delete;

  ComputeSession(ComputeSession &&other) noexcept;
  ComputeSession &operator=(ComputeSession &&other) noexcept;

  [[nodiscard]] bool initialize(std::string &error,
                                const ComputeSessionOptions &options = {});
  void reset();
  [[nodiscard]] bool isValid() const;

  [[nodiscard]] VulkanInstance &instance();
  [[nodiscard]] const VulkanInstance &instance() const;
  [[nodiscard]] VulkanDevice &device();
  [[nodiscard]] const VulkanDevice &device() const;
  [[nodiscard]] CommandContext &commandContext();
  [[nodiscard]] const CommandContext &commandContext() const;
  [[nodiscard]] const ComputeDeviceSelection &selection() const;

  [[nodiscard]] VkInstance instanceHandle() const;
  [[nodiscard]] VkDevice deviceHandle() const;

private:
  [[nodiscard]] bool optionsMatch(const ComputeSessionOptions &options) const;
  [[nodiscard]] bool
  selectComputeDevice(std::string &error, const ComputeSessionOptions &options,
                      ComputeDeviceSelection &selection) const;

  VulkanInstance instance_{};
  VulkanDevice device_{};
  CommandContext commandContext_{};
  ComputeDeviceSelection selection_{};
  std::uint32_t configuredDeviceIndex_ =
      std::numeric_limits<std::uint32_t>::max();
  std::string configuredDeviceName_{};
  std::string configuredDeviceUuid_{};
  bool initialized_{false};
};

} // namespace viennaps::vulkan::runtime
