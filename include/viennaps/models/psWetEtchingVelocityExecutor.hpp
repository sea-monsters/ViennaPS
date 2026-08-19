#pragma once

// Backend-neutral, caller-owned numeric seam for the WetEtching crystal
// velocity operation.  The Process, TranslationField, material mapping and
// Level Set transaction remain CPU-owned; an executor only evaluates a
// validated batch of already selected points.

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <string>

namespace viennaps {

template <typename NumericType> struct WetEtchMaterialRate {
  std::int32_t materialId = 0;
  NumericType rate = NumericType(0);
};

template <typename NumericType> struct WetEtchVelocityParameters {
  std::array<NumericType, 3> direction100{};
  std::array<NumericType, 3> direction010{};
  NumericType r100 = NumericType(0);
  NumericType r110 = NumericType(0);
  NumericType r111 = NumericType(0);
  NumericType r311 = NumericType(0);
  std::span<const WetEtchMaterialRate<NumericType>> materialRates{};
};

/// A transactional batch request.  `cpuOracle` is produced by the caller's
/// unchanged CPU velocity field; the executor never treats a copied formula
/// as the semantic oracle.  Coordinates are carried for point identity and
/// future Process integration, although this isolated arithmetic stage only
/// consumes normals and material IDs.
template <typename NumericType> struct WetEtchVelocityWork {
  std::span<const std::array<NumericType, 3>> coordinates{};
  std::span<const std::array<NumericType, 3>> normals{};
  std::span<const std::int32_t> materialIds{};
  std::span<const NumericType> cpuOracle{};
  std::span<NumericType> output{};
  WetEtchVelocityParameters<NumericType> parameters{};
  std::size_t writtenCount = 0U;
  bool complete = false;
};

template <typename NumericType>
using WetEtchVelocityExecutor = std::function<
    bool(WetEtchVelocityWork<NumericType> &, std::string &)>;

} // namespace viennaps
