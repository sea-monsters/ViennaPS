#pragma once

#include <cstddef>
#include <functional>
#include <span>
#include <string>

namespace viennaps {

/// Parameters for the narrow single-precursor TEOS reaction arithmetic.
/// Particle transport, sticking, coverage updates and Process ordering remain
/// CPU-owned; this type carries only the already accumulated particle flux.
template <typename NumericType> struct TEOSVelocityParameters {
  NumericType depositionRate{};
  NumericType reactionOrder{};
};

/// Caller-owned work packet for an optional TEOS velocity executor.
template <typename NumericType> struct TEOSVelocityWork {
  std::span<const NumericType> particleFlux{};
  std::span<const NumericType> cpuOracle{};
  std::span<NumericType> output{};
  TEOSVelocityParameters<NumericType> parameters{};
  std::size_t writtenCount = 0U;
  bool complete = false;
};

/// A backend-neutral numeric seam.  The caller computes cpuOracle with the
/// reference CPU formula before invoking the optional backend.  Returning
/// false, incomplete output, or a malformed result keeps the CPU result.
template <typename NumericType>
using TEOSVelocityExecutor = std::function<bool(TEOSVelocityWork<NumericType> &,
                                                std::string &)>;

} // namespace viennaps
