#pragma once

#include <memory>
#include <string>
#include <string_view>

#include <models/psWetEtchingVelocityExecutor.hpp>

#include "../runtime/compute_session.hpp"

namespace viennaps::vulkan::levelset {

/// Vulkan implementation of the isolated WetEtching crystal-velocity
/// arithmetic seam.  It is deliberately not installed into Process or
/// TranslationField by this class; callers own CPU selection and publication.
class VulkanWetEtchVelocityExecutor {
public:
  using Executor = viennaps::WetEtchVelocityExecutor<float>;

  VulkanWetEtchVelocityExecutor();
  ~VulkanWetEtchVelocityExecutor();

  VulkanWetEtchVelocityExecutor(const VulkanWetEtchVelocityExecutor &) = delete;
  VulkanWetEtchVelocityExecutor &operator=(
      const VulkanWetEtchVelocityExecutor &) = delete;
  VulkanWetEtchVelocityExecutor(VulkanWetEtchVelocityExecutor &&) = delete;
  VulkanWetEtchVelocityExecutor &operator=(VulkanWetEtchVelocityExecutor &&) = delete;

  [[nodiscard]] bool initialize(std::string_view spirvPath, std::string &error);
  [[nodiscard]] bool initialize(runtime::ComputeSession &session,
                                std::string_view spirvPath,
                                std::string &error);
  void reset();
  [[nodiscard]] bool isInitialized() const;

  /// The returned callback retains the bridge state and is safe to install on
  /// a caller-owned backend-neutral seam.  It commits output only after the
  /// device result passes the independent CPU oracle and status checks.
  [[nodiscard]] Executor makeExecutor() const;

private:
  struct State;
  std::shared_ptr<State> state_;
};

} // namespace viennaps::vulkan::levelset
