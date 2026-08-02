// Copyright 2026 ViennaPS
// SPDX-License-Identifier: MIT

#pragma once

#include "../primitives/reduction_scan_primitives.hpp"

#include <levelset/psHrleRebuildCompaction.hpp>

#include <cstdint>
#include <span>
#include <string>

namespace viennaps::vulkan::levelset {

[[nodiscard]] bool compactHrleRebuildDecisionsFp32(
    viennaps::vulkan::primitives::ReductionScanPrimitives &primitives,
    const std::span<const viennaps::levelset::HrleRebuildDecisionFp32>
        decisions,
    viennaps::levelset::HrleRebuildCompactionResultFp32 &output,
    std::string &error);

} // namespace viennaps::vulkan::levelset
