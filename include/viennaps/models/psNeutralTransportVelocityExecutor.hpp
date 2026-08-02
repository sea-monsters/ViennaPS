#pragma once

#include <cstddef>
#include <functional>
#include <span>
#include <string>

namespace viennaps {

/// Parameters needed by a neutral-transport velocity executor.
///
/// The representation deliberately contains no Vulkan or process-context
/// types, so an executor can be implemented by any backend.
template <typename NumericType> struct NeutralTransportVelocityParameters {
  NumericType kEtch{};
  NumericType surfaceSiteDensity{};
  NumericType siliconDensity{};
  NumericType timeToSecond{};
  NumericType lengthToMeter{};
  int etchFrontMaterialId = 0;
};

/// A transactional neutral-transport velocity request.
///
/// An executor may inspect the input spans and must write the complete output
/// span before setting `complete` and `writtenCount`. The caller commits the
/// output only when the executor returns true and both completion markers are
/// valid; otherwise it keeps the canonical CPU result.
template <typename NumericType> struct NeutralTransportVelocityWork {
  std::span<const NumericType> coverage;
  std::span<const NumericType> materialIds;
  std::span<NumericType> output;
  NeutralTransportVelocityParameters<NumericType> parameters;
  std::size_t writtenCount = 0U;
  bool complete = false;
};

template <typename NumericType>
using NeutralTransportVelocityExecutor = std::function<
    bool(NeutralTransportVelocityWork<NumericType> &, std::string &)>;

} // namespace viennaps
