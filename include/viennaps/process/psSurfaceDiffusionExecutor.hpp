#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <string>

namespace viennaps {

enum class ProcessResult;

/// A synchronous, type-erased surface-diffusion dispatch request.
///
/// Every span is valid only for the duration of the executor call. An executor
/// must fill the complete output span, set writtenCount to output.size(), and
/// set complete before returning SUCCESS; callers commit the output only after
/// all three conditions hold. NumericType is preserved exactly; installing a
/// double executor never implies an FP32 conversion.
template <typename NumericType> struct SurfaceDiffusionWork {
  std::span<const std::uint32_t> rowOffsets;
  std::span<const std::uint32_t> columnIndices;
  std::span<const NumericType> weights;
  std::span<const NumericType> field;
  std::span<NumericType> output;
  NumericType diffusionStep{};
  std::size_t writtenCount = 0U;
  bool complete = false;
};

template <typename NumericType>
using SurfaceDiffusionExecutor = std::function<
    ProcessResult(SurfaceDiffusionWork<NumericType> &, std::string &)>;

} // namespace viennaps
