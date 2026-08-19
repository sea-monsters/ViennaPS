#pragma once

#include <memory>
#include <string>
#include <string_view>

#include <models/psTEOSVelocityExecutor.hpp>

#include "../runtime/compute_session.hpp"

namespace viennaps::vulkan::levelset {

/// Vulkan implementation of the narrow single-precursor TEOS reaction-power
/// arithmetic.  Particle transport, sticking and coverage remain on the CPU.
class VulkanTEOSVelocityExecutor {
public:
  using Executor = viennaps::TEOSVelocityExecutor<float>;

  VulkanTEOSVelocityExecutor();
  ~VulkanTEOSVelocityExecutor();

  VulkanTEOSVelocityExecutor(const VulkanTEOSVelocityExecutor &) = delete;
  VulkanTEOSVelocityExecutor &operator=(const VulkanTEOSVelocityExecutor &) =
      delete;

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
