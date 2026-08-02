#pragma once

#include <cstddef>
#include <functional>
#include <span>
#include <string>

namespace viennaps {

/// Work handed to an optional coverage delta backend.
///
/// The scalar samples are flattened channel-major. channelOffsets has one
/// entry per channel plus a terminal end offset, so a backend need not know
/// anything about PointData or a particular execution API.
template <typename NumericType> struct CoverageDeltaWork {
  std::span<const NumericType> updated;
  std::span<const NumericType> previous;
  std::span<const std::size_t> channelOffsets;
  std::span<NumericType> output;
  std::size_t channelCount = 0;
  std::size_t writtenCount = 0;
  bool complete = false;
};

template <typename NumericType>
using CoverageDeltaExecutor =
    std::function<bool(CoverageDeltaWork<NumericType> &, std::string &)>;

} // namespace viennaps
