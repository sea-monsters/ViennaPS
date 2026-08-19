#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <limits>
#include <type_traits>

namespace viennaps_p5_trace_observer {

struct Record {
  std::uint32_t rayId{};
  std::uint32_t bounce{};
  std::uint32_t sequence{};
  std::uint32_t kind{};
  std::uint32_t primitive{};
  std::uint32_t material{};
  std::uint32_t action{};
  std::uint32_t callback{};
  std::uint32_t reflectionCount{};
  std::uint32_t rngStateKind{};
  std::uint32_t rngCounter[4]{};
  std::uint32_t rngKey[2]{};
  std::uint32_t rngOutputIndex{};
  std::uint32_t tBits{};
  std::uint32_t uBits{};
  std::uint32_t vBits{};
  std::uint32_t weightBits{};
  std::uint32_t nextWeightBits{};
  std::uint32_t originBits[3]{};
  std::uint32_t directionBits[3]{};
  std::uint32_t successorBits[3]{};
};

static_assert(sizeof(Record) == 31U * sizeof(std::uint32_t),
              "The raw trace record is a fixed cross-build ABI.");

class Sink {
public:
  explicit Sink(const char *path)
      : stream_(path, std::ios::binary | std::ios::out | std::ios::app) {}
  bool good() const { return stream_.good(); }
  void write(const Record &record) {
    const std::uint32_t magic = 0x50355452U;
    stream_.write(reinterpret_cast<const char *>(&magic), sizeof(magic));
    stream_.write(reinterpret_cast<const char *>(&record), sizeof(record));
  }

private:
  std::ofstream stream_;
};

inline Sink *&current() {
  static Sink *sink = nullptr;
  return sink;
}

class Scope {
public:
  explicit Scope(Sink &sink) : previous_(current()) { current() = &sink; }
  ~Scope() { current() = previous_; }

private:
  Sink *previous_;
};

inline std::uint32_t bits(float value) {
  std::uint32_t result{};
  std::memcpy(&result, &value, sizeof(result));
  return result;
}

// Philox is the locked P5 CPU RNG. The state record is emitted after the real
// callback/roulette branch has run; it observes the next RNG state without
// drawing or mutating it. Keep the fallback explicit so another configured RNG
// cannot be misreported as a comparable Philox stream.
template <class Rng> inline void captureRngState(Record &record, const Rng &rng) {
  if constexpr (requires { rng.get_state().counter; rng.get_state().key;
                           rng.get_state().output_index; }) {
    const auto state = rng.get_state();
    record.rngStateKind = 1U;
    for (std::size_t i = 0U; i < state.counter.size(); ++i)
      record.rngCounter[i] = state.counter[i];
    for (std::size_t i = 0U; i < state.key.size(); ++i)
      record.rngKey[i] = state.key[i];
    if (state.output_index <= std::numeric_limits<std::uint32_t>::max())
      record.rngOutputIndex = static_cast<std::uint32_t>(state.output_index);
    else
      record.rngStateKind = 0U;
  }
}

} // namespace viennaps_p5_trace_observer
