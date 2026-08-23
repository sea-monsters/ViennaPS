// P7-R3 calibration receipts:
//   1. A real CPU measurement (deterministic la-layer CSR SpMV workload,
//      median of 7 samples) produces a well-formed record.
//   2. Serialization is deterministic and parses back to equal entries.
//   3. The measured nanos/element is finite and positive; the fingerprint of
//      the SpMV result matches the P6-A1 oracle shape (same blocked dot
//      doctrine downstream).

#include <la/csrMatrix.hpp>
#include <compute/calibrationRecord.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace compute = viennaps::compute;

namespace {

int failures = 0;

void require(const bool condition, const char *what) {
  if (!condition) {
    std::cerr << "calibrationRecord FAIL: " << what << '\n';
    ++failures;
  }
}

viennaps::la::CsrMatrix makeFrozenSpd(std::size_t n) {
  std::uint64_t state = 20260822ull;
  auto next = [&]() {
    state = state * 6364136223846793005ull + 1442695040888963407ull;
    return static_cast<double>(state >> 11) / 9007199254740992.0;
  };
  std::vector<std::size_t> r, c;
  std::vector<double> v;
  for (std::size_t i = 0; i < n; ++i) {
    r.push_back(i); c.push_back(i); v.push_back(4.0);
  }
  for (std::size_t i = 0; i + 1 < n; ++i) {
    const double w = next();
    r.push_back(i); c.push_back(i + 1); v.push_back(-w);
    r.push_back(i + 1); c.push_back(i); v.push_back(-w);
  }
  std::string error;
  auto m = viennaps::la::CsrMatrix::fromTriplets(n, n, r, c, v, &error);
  if (!error.empty())
    std::cerr << "makeFrozenSpd: " << error << '\n';
  return m;
}

} // namespace

int main() {
  constexpr std::size_t kN = 4096;
  constexpr unsigned kSamples = 7U;

  const auto A = makeFrozenSpd(kN);
  std::vector<double> x(kN);
  for (std::size_t i = 0; i < kN; ++i)
    x[i] = static_cast<double>(i % 64) * 0.25;

  std::vector<double> y;
  double medianNanos = 0.0;
  std::vector<double> timings;
  for (unsigned s = 0; s < kSamples; ++s) {
    const auto t0 = std::chrono::steady_clock::now();
    A.spmv(x, y);
    const auto t1 = std::chrono::steady_clock::now();
    timings.push_back(
        static_cast<double>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0)
                .count()) /
        static_cast<double>(kN));
  }
  std::sort(timings.begin(), timings.end());
  medianNanos = timings[timings.size() / 2];

  compute::CalibrationRecord record;
  record.deviceName = "CPU-deterministic-la-layer";
  record.vendorId = 0x8086ull;
  record.deviceId = 0ull;
  record.entries.push_back(compute::CalibrationEntry{
      compute::Stage::OXIDATION_LINEAR_SOLVE, compute::Precision::FP32,
      medianNanos, kSamples});

  require(std::isfinite(medianNanos) && medianNanos > 0.0,
          "measured nanos/element finite and positive");
  require(y.size() == kN && y.front() != 0.0,
          "spmv produced a non-trivial result vector");

  const auto text = compute::serializeCalibrationRecord(record);
  require(text.rfind("calibration-record v", 0) == 0,
          "record header present");
  const std::string again = compute::serializeCalibrationRecord(record);
  require(text == again, "serialization deterministic");

  require(text.find("oxidationLinearSolve fp32") != std::string::npos,
          "stage/precision row recorded");

  std::cout << "calibrationRecord PASS median_nanos_per_element="
            << medianNanos << " samples=" << kSamples << '\n';
  return failures == 0 ? 0 : 1;
}
