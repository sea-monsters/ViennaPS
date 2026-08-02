// Copyright 2026 ViennaPS
// SPDX-License-Identifier: MIT

#pragma once

#include "../primitives/reduction_scan_primitives.hpp"
#include "hrle_rebuild_classification.hpp"

#include <levelset/psHrleRebuildCompaction.hpp>

#include <cstdint>
#include <span>
#include <string>

namespace viennaps::vulkan::levelset {

struct HrleRebuildCompactionDeviceFp32 {
  runtime::DeviceBuffer flags{};
  runtime::DeviceBuffer offsets{};
  runtime::DeviceBuffer compacted{};
  runtime::DeviceBuffer count{};
  std::uint32_t candidateCount{0U};
  std::uint64_t sessionGeneration{0U};
};

// `primitives` must have been initialized with `session` and kept alive for
// the duration of this call. Reset/reinitialize of either object is not
// concurrent with this pipeline; the generation token is checked at entry.
[[nodiscard]] bool compactHrleRebuildDecisionsFp32Device(
    runtime::ComputeSession &session, const runtime::SpirvProgram &flagsProgram,
    const runtime::SpirvProgram &compactProgram,
    viennaps::vulkan::primitives::ReductionScanPrimitives &primitives,
    const HrleRebuildClassificationDeviceFp32 &classification,
    HrleRebuildCompactionDeviceFp32 &output, std::string &error);

[[nodiscard]] bool materializeHrleRebuildCompactionFp32Device(
    runtime::ComputeSession &session,
    const HrleRebuildClassificationDeviceFp32 &classification,
    const HrleRebuildCompactionDeviceFp32 &deviceOutput,
    viennaps::levelset::HrleRebuildCompactionResultFp32 &output,
    std::string &error);

[[nodiscard]] bool compactHrleRebuildDecisionsFp32(
    viennaps::vulkan::primitives::ReductionScanPrimitives &primitives,
    const std::span<const viennaps::levelset::HrleRebuildDecisionFp32>
        decisions,
    viennaps::levelset::HrleRebuildCompactionResultFp32 &output,
    std::string &error);

} // namespace viennaps::vulkan::levelset
