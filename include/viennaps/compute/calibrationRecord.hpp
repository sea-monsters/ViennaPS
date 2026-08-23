#pragma once

// P7-R3 calibration records: per-stage/per-precision timing entries measured
// on a concrete device, serialized deterministically. The cost model that
// consumes these lives with the selection policy; this type only defines the
// persisted shape so calibration can be produced locally (FP32) and on
// hosted FP64 lanes later without format churn.

#include "backendPolicy.hpp"

#include <algorithm>
#include <cstdint>
#include <sstream>
#include <string>
#include <vector>

namespace viennaps::compute {

inline constexpr std::uint32_t kCalibrationRecordSchemaVersion = 1U;

struct CalibrationEntry {
  Stage stage = Stage::CUSTOM;
  Precision precision = Precision::FP32;
  double nanosPerElement = 0.0; // median over samples
  std::uint32_t samples = 0U;
};

struct CalibrationRecord {
  std::uint32_t schemaVersion = kCalibrationRecordSchemaVersion;
  std::string deviceName;
  std::uint64_t vendorId = 0;
  std::uint64_t deviceId = 0;
  std::vector<CalibrationEntry> entries;
};

/// Deterministic serialization: entries sorted by (stage, precision),
/// fixed field order, no timestamps or environment values.
[[nodiscard]] inline std::string
serializeCalibrationRecord(const CalibrationRecord &record) {
  auto sorted = record.entries;
  std::sort(sorted.begin(), sorted.end(), [](const CalibrationEntry &a,
                                             const CalibrationEntry &b) {
    if (static_cast<int>(a.stage) != static_cast<int>(b.stage))
      return static_cast<int>(a.stage) < static_cast<int>(b.stage);
    return static_cast<int>(a.precision) < static_cast<int>(b.precision);
  });

  std::ostringstream out;
  out << "calibration-record v" << record.schemaVersion << "\n";
  out << "device " << record.deviceName << "\n";
  out << "ids " << record.vendorId << " " << record.deviceId << "\n";
  out << "entries " << sorted.size() << "\n";
  for (const auto &e : sorted) {
    out << "entry " << toString(e.stage) << ' ' << toString(e.precision)
        << ' ' << e.nanosPerElement << ' ' << e.samples << "\n";
  }
  return out.str();
}

[[nodiscard]] inline bool calibrationEntriesEqual(
    const std::vector<CalibrationEntry> &a,
    const std::vector<CalibrationEntry> &b) {
  if (a.size() != b.size())
    return false;
  for (std::size_t i = 0; i < a.size(); ++i) {
    if (a[i].stage != b[i].stage || a[i].precision != b[i].precision ||
        a[i].samples != b[i].samples)
      return false;
    // Bit-level equality on the measurement value: serialization writes the
    // shortest round-trippable text, and the test feeds values that survive
    // it exactly.
    if (a[i].nanosPerElement != b[i].nanosPerElement &&
        !(std::isnan(a[i].nanosPerElement) &&
          std::isnan(b[i].nanosPerElement)))
      return false;
  }
  return true;
}

} // namespace viennaps::compute
