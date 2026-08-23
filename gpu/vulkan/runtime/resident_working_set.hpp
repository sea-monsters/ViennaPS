#pragma once

// P7-R1 resident working set: cross-step residency bookkeeping for device
// buffers that intentionally outlive a single Process step on one shared
// deployment session.
//
// Ownership stays with the existing RAII types - ResidentWorkingSet only
// tracks liveness and transfers metrics, so the E0 resource ledger still
// guarantees that no registered allocation survives a device reset.
// Invalidation is explicit (profile/model change) or automatic when the
// attached session generation dies.

#include "compute_session.hpp"
#include "vulkan_compute_runtime.hpp"

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace viennaps::vulkan::runtime {

struct ResidencyTelemetry {
  std::uint64_t registrations = 0;
  std::uint64_t invalidations = 0;
  std::uint64_t bytesRegistered = 0;
  std::uint64_t bytesInvalidated = 0;
};

struct ResidencyEntryInfo {
  std::string tag;
  std::uint64_t bytes = 0;
  bool valid = false;
};

class ResidentWorkingSet {
public:
  ResidentWorkingSet() = default;

  [[nodiscard]] bool attach(ComputeSession &session, std::string &error) {
    if (!session.isValid() || session.generation() == 0U) {
      error = "ResidentWorkingSet: session is not initialized.";
      return false;
    }
    std::lock_guard lock(mutex_);
    session_ = &session;
    generation_ = session.generation();
    return true;
  }

  /// Track an existing device buffer as resident. The buffer keeps its own
  /// RAII ownership; invalidation never destroys anything.
  [[nodiscard]] bool registerBuffer(DeviceBuffer &buffer,
                                    const std::string &tag,
                                    std::string &error) {
    if (!buffer.isValid()) {
      error = "ResidentWorkingSet: buffer is not valid.";
      return false;
    }
    std::lock_guard lock(mutex_);
    if (!attachedLocked(error))
      return false;
    entries_.push_back(Entry{&buffer, tag, static_cast<std::uint64_t>(buffer.size()),
                             true});
    ++telemetry_.registrations;
    telemetry_.bytesRegistered += static_cast<std::uint64_t>(buffer.size());
    return true;
  }

  /// Invalidate every entry (profile/model change); the counters increment per// entry. Reasons are
  /// retained for telemetry; buffers themselves remain untouched RAII husks.
  void invalidate(const std::string &reason) {
    std::lock_guard lock(mutex_);
    for (auto &entry : entries_) {
      if (entry.valid) {
        entry.valid = false;
        ++telemetry_.invalidations;
        telemetry_.bytesInvalidated += entry.bytes;
      }
    }
    lastInvalidationReason_ = reason;
  }

  /// A resident set is live while its session generation is alive and no
  /// invalidation happened. A dead generation auto-invalidates (the ledger
  /// sweep already freed the allocations before the device went away).
  [[nodiscard]] bool validate() {
    std::lock_guard lock(mutex_);
    if (session_ == nullptr || !session_->isValid() ||
        session_->generation() != generation_) {
      invalidateLocked("session generation changed");
      return false;
    }
    for (const auto &entry : entries_)
      if (!entry.valid)
        return false;
    return !entries_.empty();
  }

  [[nodiscard]] ResidencyTelemetry telemetry() const {
    std::lock_guard lock(mutex_);
    return telemetry_;
  }

  [[nodiscard]] std::vector<ResidencyEntryInfo> entries() const {
    std::lock_guard lock(mutex_);
    std::vector<ResidencyEntryInfo> info;
    info.reserve(entries_.size());
    for (const auto &e : entries_)
      info.push_back(ResidencyEntryInfo{e.tag, e.bytes, e.valid});
    return info;
  }

  [[nodiscard]] std::string lastInvalidationReason() const {
    std::lock_guard lock(mutex_);
    return lastInvalidationReason_;
  }

private:
  struct Entry {
    DeviceBuffer *buffer = nullptr;
    std::string tag;
    std::uint64_t bytes = 0;
    bool valid = false;
  };

  [[nodiscard]] bool attachedLocked(std::string &error) const {
    if (session_ == nullptr || generation_ == 0U) {
      error = "ResidentWorkingSet: not attached to a session.";
      return false;
    }
    if (!session_->isValid() || session_->generation() != generation_) {
      error = "ResidentWorkingSet: attached session is gone.";
      return false;
    }
    return true;
  }

  void invalidateLocked(const std::string &reason) {
    for (auto &entry : entries_) {
      if (entry.valid) {
        entry.valid = false;
        ++telemetry_.invalidations;
        telemetry_.bytesInvalidated += entry.bytes;
      }
    }
    lastInvalidationReason_ = reason;
  }

  ComputeSession *session_ = nullptr;
  std::uint64_t generation_ = 0;
  std::vector<Entry> entries_;
  ResidencyTelemetry telemetry_;
  std::string lastInvalidationReason_;
  mutable std::mutex mutex_;
};

} // namespace viennaps::vulkan::runtime
