#pragma once

#include <memory>
#include <string>
#include <string_view>

#include <models/psSelectiveEpitaxyVelocityExecutor.hpp>

#include "../runtime/compute_session.hpp"

namespace viennaps::vulkan::levelset {

/// Vulkan implementation of the narrow SelectiveEpitaxy velocity arithmetic
/// seam. Domain masking and Level Set publication stay on the CPU.
class VulkanSelectiveEpitaxyVelocityExecutor {
public:
  using Executor = viennaps::SelectiveEpitaxyVelocityExecutor<float>;

  VulkanSelectiveEpitaxyVelocityExecutor();
  ~VulkanSelectiveEpitaxyVelocityExecutor();

  VulkanSelectiveEpitaxyVelocityExecutor(
      const VulkanSelectiveEpitaxyVelocityExecutor &) = delete;
  VulkanSelectiveEpitaxyVelocityExecutor &operator=(
      const VulkanSelectiveEpitaxyVelocityExecutor &) = delete;

  [[nodiscard]] bool initialize(std::string_view spirvPath,
                                std::string &error);
  [[nodiscard]] bool initialize(runtime::ComputeSession &session,
                                std::string_view spirvPath,
                                std::string &error);
  void reset();
  [[nodiscard]] bool isInitialized() const;
  [[nodiscard]] Executor makeExecutor() const;

private:
  struct State;
  std::shared_ptr<State> state_;
};

} // namespace viennaps::vulkan::levelset
