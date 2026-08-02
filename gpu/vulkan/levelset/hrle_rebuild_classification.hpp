// Copyright 2026 ViennaPS
// SPDX-License-Identifier: MIT

#pragma once

#include "../runtime/compute_session.hpp"

#include <levelset/psHrleRebuildClassification.hpp>

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace viennaps::vulkan::levelset {

[[nodiscard]] bool classifyHrleRebuildFp32(
    runtime::ComputeSession &session, const runtime::SpirvProgram &program,
    std::span<const viennaps::levelset::HrleRebuildCandidateFp32> candidates,
    std::uint32_t dimensions, float cutoff,
    std::vector<viennaps::levelset::HrleRebuildDecisionFp32> &output,
    std::string &error);

} // namespace viennaps::vulkan::levelset
