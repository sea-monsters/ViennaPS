// Copyright 2026 ViennaPS
// SPDX-License-Identifier: MIT

#include "levelset_deployment_session.hpp"
#include "../surface/process_deployment_binding.hpp"

#include <memory>

int main() {
  using SurfaceBinding =
      viennaps::vulkan::surface::ProcessDeploymentBinding<2>;
  using LevelSetSession = viennaps::vulkan::levelset::LevelSetDeploymentSession<2>;
  using Context = viennaps::vulkan::runtime::DeploymentComputeContext;
  static_assert(std::is_same_v<
                decltype(std::declval<const SurfaceBinding &>().sharedContext()),
                std::shared_ptr<Context>>);
  static_assert(std::is_same_v<
                decltype(std::declval<LevelSetSession &>().configure(
                    std::declval<LevelSetSession::ProcessType &>(),
                    std::declval<std::shared_ptr<Context>>())),
                LevelSetSession::Controller::Result>);
  return 0;
}
