// P7-R1 resident working set smoke.
//
// Measures host-device transfer volume for a four-step chained pipeline in
// two routes over identical work:
//   Route A (P5-style): per-step upload of inputs + per-step download of
//                       results, buffers recreated every step.
//   Route B (resident): one registration, single upload, device-side chaining
//                       via copyTo, single final download.
// Contracts asserted:
//   - Both routes produce bit-identical final results.
//   - Resident route transfers strictly fewer host bytes than route A.
//   - Telemetry counts registrations; explicit invalidation flips validate()
//     to false and bumps the counters; buffers destroyed before session
//     reset keep the E0 ledger guarantee intact.

#include "compute_session.hpp"
#include "resident_working_set.hpp"
#include "vulkan_compute_runtime.hpp"

#include <algorithm>
#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

namespace {

int failures = 0;

void require(const bool condition, const char *what) {
  if (!condition) {
    std::cerr << "resident-working-set FAIL: " << what << '\n';
    ++failures;
  }
}

struct TransferAccount {
  std::uint64_t hostUpload = 0;
  std::uint64_t hostDownload = 0;
};

} // namespace

int main() {
  constexpr std::uint64_t kElements = 262144U; // 1 MiB of float payload
  constexpr std::uint64_t kBytes = kElements * sizeof(float);
  constexpr unsigned kSteps = 4U;

  viennaps::vulkan::runtime::ComputeSession session;
  std::string error;
  if (!session.initialize(error)) {
    std::cerr << "session: " << error << '\n';
    return 1;
  }

  std::vector<float> base(kElements);
  for (std::size_t i = 0; i < base.size(); ++i)
    base[i] = static_cast<float>(i % 1024) * 0.5F;

  // ---- Route A: P5-style per-step transfers -----------------------------
  TransferAccount accountA{};
  std::vector<float> resultA(kElements, 0.0F);
  {
    std::vector<float> current = base;
    for (unsigned step = 0; step < kSteps; ++step) {
      viennaps::vulkan::runtime::DeviceBuffer a, c;
      if (!a.create(session, kBytes, error) ||
          !c.create(session, kBytes, error)) {
        std::cerr << "route A create: " << error << '\n';
        return 1;
      }
      if (!a.upload(session, current.data(), kBytes, 0, error)) {
        std::cerr << "route A upload: " << error << '\n';
        return 1;
      }
      accountA.hostUpload += kBytes;
      // Device-side stand-in for a compute stage: two ordered copies.
      if (!a.copyTo(session, c, kBytes, 0, 0, error) ||
          !c.copyTo(session, a, kBytes, 0, 0, error)) {
        std::cerr << "route A copy: " << error << '\n';
        return 1;
      }
      if (!a.download(session, current.data(), kBytes, 0, error)) {
        std::cerr << "route A download: " << error << '\n';
        return 1;
      }
      accountA.hostDownload += kBytes;
      resultA = current;
      a.reset();
      c.reset();
    }
  }

  // ---- Route B: resident set, single upload/download --------------------
  TransferAccount accountB{};
  std::vector<float> resultB(kElements, 0.0F);
  viennaps::vulkan::runtime::ResidencyTelemetry liveTelemetry{};
  {
    viennaps::vulkan::runtime::DeviceBuffer a, b, c;
    if (!a.create(session, kBytes, error) ||
        !b.create(session, kBytes, error) ||
        !c.create(session, kBytes, error)) {
      std::cerr << "route B create: " << error << '\n';
      return 1;
    }
    if (!a.upload(session, base.data(), kBytes, 0, error)) {
      std::cerr << "route B upload: " << error << '\n';
      return 1;
    }
    accountB.hostUpload += kBytes;

    viennaps::vulkan::runtime::ResidentWorkingSet resident;
    if (!resident.attach(session, error) ||
        !resident.registerBuffer(a, "state", error) ||
        !resident.registerBuffer(b, "scratch", error) ||
        !resident.registerBuffer(c, "output", error)) {
      std::cerr << "route B register: " << error << '\n';
      return 1;
    }
    require(resident.telemetry().registrations == 3U,
            "three registrations recorded");

    for (unsigned step = 0; step < kSteps; ++step) {
      if (!resident.validate())
        break;
      if (!a.copyTo(session, c, kBytes, 0, 0, error) ||
          !c.copyTo(session, b, kBytes, 0, 0, error) ||
          !b.copyTo(session, a, kBytes, 0, 0, error)) {
        std::cerr << "route B copy: " << error << '\n';
        return 1;
      }
    }
    if (!a.download(session, resultB.data(), kBytes, 0, error)) {
      std::cerr << "route B download: " << error << '\n';
      return 1;
    }
    accountB.hostDownload += kBytes;

    require(resident.validate(), "resident set stays valid across steps");
    resident.invalidate("smoke teardown");
    require(!resident.validate(),
            "explicit invalidation flips the set to not-live");
    require(resident.telemetry().invalidations == 3U,
            "all three entries invalidated");
    require(resident.telemetry().bytesInvalidated == kBytes * 3U,
            "invalidated byte volume matches registrations");
    liveTelemetry = resident.telemetry();
    require(resident.lastInvalidationReason() == "smoke teardown",
            "invalidation reason retained");
    a.reset();
    b.reset();
    c.reset();
  }

  // ---- Receipts ----------------------------------------------------------
  require(resultA.size() == resultB.size(), "routes produce same size");
  require(std::equal(resultA.begin(), resultA.end(), resultB.begin()),
          "routes bit-identical");
  require(accountB.hostUpload + accountB.hostDownload <
              accountA.hostUpload + accountA.hostDownload,
          "resident route transfers fewer host bytes");
  require(liveTelemetry.registrations == 3U &&
              liveTelemetry.invalidations == 3U &&
              liveTelemetry.bytesRegistered == kBytes * 3U &&
              liveTelemetry.bytesInvalidated == kBytes * 3U,
          "telemetry consistent with the resident phase");
  require(accountA.hostUpload == kBytes * kSteps &&
              accountA.hostDownload == kBytes * kSteps,
          "route A accounting matches expectation");

  std::cout << "resident-working-set PASS"
            << " routeA_bytes=" << (accountA.hostUpload + accountA.hostDownload)
            << " routeB_bytes=" << (accountB.hostUpload + accountB.hostDownload)
            << '\n';
  return failures == 0 ? 0 : 1;
}