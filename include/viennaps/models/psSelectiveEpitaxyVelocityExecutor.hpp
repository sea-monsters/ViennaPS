#pragma once

// Backend-neutral, caller-owned numeric seam for the selective-epitaxy
// velocity operation.  Domain masking, stencil preparation/finalization,
// Process ordering and Level Set publication remain CPU-owned.

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <string>

namespace viennaps {

template <typename NumericType> struct SelectiveEpitaxyMaterialRate {
  std::int32_t materialId = 0;
  NumericType rate = NumericType(0);
};

template <typename NumericType> struct SelectiveEpitaxyVelocityParameters {
  std::array<NumericType, 3> normalFactors{};
  NumericType rate111 = NumericType(0.5);
  NumericType rate100 = NumericType(1.0);
  std::span<const SelectiveEpitaxyMaterialRate<NumericType>> materialRates{};
};

/// Transactional batch request. `cpuOracle` is produced by the unchanged
/// CPU velocity field; the executor never becomes the semantic authority.
template <typename NumericType> struct SelectiveEpitaxyVelocityWork {
  std::span<const std::array<NumericType, 3>> coordinates{};
  std::span<const std::array<NumericType, 3>> normals{};
  std::span<const std::int32_t> materialIds{};
  std::span<const NumericType> cpuOracle{};
  std::span<NumericType> output{};
  SelectiveEpitaxyVelocityParameters<NumericType> parameters{};
  std::size_t writtenCount = 0U;
  bool complete = false;
};

template <typename NumericType>
using SelectiveEpitaxyVelocityExecutor = std::function<
    bool(SelectiveEpitaxyVelocityWork<NumericType> &, std::string &)>;

} // namespace viennaps
