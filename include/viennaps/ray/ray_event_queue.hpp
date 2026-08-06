#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <queue>
#include <tuple>
#include <vector>

namespace viennaps::ray {

/// A single ray event in a deterministic event-driven simulation loop.
/// Events are ordered by (particle index, bounce index) so that CPU and GPU
/// replay produce the same processing order when fed the same seed/counter
/// population. An internal monotonic sequence number breaks ties for the rare
/// case where two events share the same (particle, bounce) key.
struct RayEvent {
  std::array<float, 3> origin = {0.0f, 0.0f, 0.0f};
  std::array<float, 3> direction = {0.0f, 0.0f, 1.0f};
  float weight = 1.0f;
  std::uint32_t particle = 0;
  std::uint32_t bounce = 0;
  std::uint64_t sequence = 0;
};

namespace impl {
struct RayEventOrdering {
  [[nodiscard]] bool operator()(const RayEvent &lhs,
                                const RayEvent &rhs) const {
    return std::tie(lhs.particle, lhs.bounce, lhs.sequence) >
           std::tie(rhs.particle, rhs.bounce, rhs.sequence);
  }
};
} // namespace impl

/// Deterministic min-heap event queue ordered by (particle, bounce, sequence).
class EventQueue {
public:
  [[nodiscard]] bool empty() const { return heap_.empty(); }
  [[nodiscard]] std::size_t size() const { return heap_.size(); }

  void push(const RayEvent &event) {
    RayEvent ordered = event;
    ordered.sequence = nextSequence_++;
    heap_.push(ordered);
  }

  void push(RayEvent &&event) {
    event.sequence = nextSequence_++;
    heap_.push(std::move(event));
  }

  RayEvent pop() {
    RayEvent event = heap_.top();
    heap_.pop();
    return event;
  }

  [[nodiscard]] const RayEvent &top() const { return heap_.top(); }

  void clear() {
    while (!heap_.empty()) {
      heap_.pop();
    }
    nextSequence_ = 0;
  }

private:
  std::uint64_t nextSequence_ = 0;
  std::priority_queue<RayEvent, std::vector<RayEvent>, impl::RayEventOrdering>
      heap_;
};

} // namespace viennaps::ray
