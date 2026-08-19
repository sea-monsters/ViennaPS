#include "p5_trace_observer.hpp"

#include <bit>
#include <cstring>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>

namespace {

using Record = viennaps_p5_trace_observer::Record;
constexpr std::uint32_t kMagic = 0x50355452U;

bool readRecords(const std::vector<char> &bytes, std::vector<Record> &records) {
  constexpr std::size_t kWireSize = sizeof(kMagic) + sizeof(Record);
  if (bytes.empty() || bytes.size() % kWireSize != 0U)
    return false;
  for (std::size_t offset = 0U; offset < bytes.size(); offset += kWireSize) {
    std::uint32_t magic{};
    std::memcpy(&magic, bytes.data() + offset, sizeof(magic));
    if (magic != kMagic)
      return false;
    Record record{};
    std::memcpy(&record, bytes.data() + offset + sizeof(magic),
                sizeof(record));
    records.push_back(record);
  }
  return true;
}

bool sameRngState(const Record &left, const Record &right) {
  return left.rngStateKind == right.rngStateKind &&
         std::memcmp(left.rngCounter, right.rngCounter,
                     sizeof(left.rngCounter)) == 0 &&
         std::memcmp(left.rngKey, right.rngKey, sizeof(left.rngKey)) == 0 &&
         left.rngOutputIndex == right.rngOutputIndex;
}

bool validateTraceShape(const std::vector<Record> &records) {
  unsigned hitCount = 0U;
  unsigned collisionCount = 0U;
  unsigned reflectionCount = 0U;
  unsigned weightCount = 0U;
  unsigned rouletteCount = 0U;
  unsigned limitCount = 0U;
  bool sawRngDraw = false;
  for (std::size_t i = 0U; i < records.size(); ++i) {
    const auto &record = records[i];
    hitCount += record.kind == 1U;
    collisionCount += record.kind == 4U;
    reflectionCount += record.kind == 5U;
    weightCount += record.kind == 6U;
    rouletteCount += record.kind == 2U;
    limitCount += record.kind == 7U;
    if (record.kind != 6U || i + 1U == records.size())
      continue;

    const auto &next = records[i + 1U];
    if (next.kind == 7U) {
      if (next.action != 0U || !sameRngState(record, next))
        return false; // The reflection limit terminates without roulette.
      continue;
    }
    if (next.kind != 2U)
      return false;
    const float nextWeight =
        std::bit_cast<float>(record.nextWeightBits);
    if (nextWeight >= 0.1F) {
      if (next.action != 1U || !sameRngState(record, next))
        return false; // High-weight continuation does not draw roulette RNG.
    } else {
      if (next.action != 1U && next.action != 2U)
        return false;
      sawRngDraw |= !sameRngState(record, next);
    }
  }
  return hitCount == 4U && collisionCount == 4U && reflectionCount == 4U &&
         weightCount == 4U && rouletteCount == 3U && limitCount == 1U &&
         sawRngDraw;
}

} // namespace

int main(int argc, char **argv) {
  if (argc != 3) {
    std::cerr << "usage: trace_checker reference mod\n";
    return 2;
  }
  std::ifstream reference(argv[1], std::ios::binary);
  std::ifstream mod(argv[2], std::ios::binary);
  if (!reference || !mod) {
    std::cerr << "RED: trace output missing\n";
    return 1;
  }
  const std::vector<char> referenceBytes((std::istreambuf_iterator<char>(reference)),
                                         std::istreambuf_iterator<char>());
  const std::vector<char> modBytes((std::istreambuf_iterator<char>(mod)),
                                   std::istreambuf_iterator<char>());
  if (referenceBytes.empty() || modBytes.empty()) {
    std::cerr << "RED: trace output empty\n";
    return 1;
  }
  if (referenceBytes != modBytes) {
    const auto n = std::min(referenceBytes.size(), modBytes.size());
    std::size_t first = 0U;
    while (first < n && referenceBytes[first] == modBytes[first]) ++first;
    std::cerr << "first raw divergence byte=" << first
              << " reference_size=" << referenceBytes.size()
              << " mod_size=" << modBytes.size() << '\n';
    return 1;
  }
  std::vector<Record> records;
  if (!readRecords(referenceBytes, records) || !validateTraceShape(records)) {
    std::cerr << "RED: trace record contract mismatch\n";
    return 1;
  }
  std::cout << "multibounce frontier reference differential PASS raw_bytes="
            << referenceBytes.size() << "\n";
  return 0;
}
