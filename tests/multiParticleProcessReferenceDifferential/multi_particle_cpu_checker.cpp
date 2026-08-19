#include <fstream>
#include <iostream>
#include <iterator>
#include <vector>

namespace {
std::vector<char> read(const char *path) {
  std::ifstream input(path, std::ios::binary);
  if (!input)
    return {};
  return {std::istreambuf_iterator<char>(input),
          std::istreambuf_iterator<char>()};
}
} // namespace

int main(int argc, char **argv) {
  if (argc != 3) {
    std::cerr << "usage: multi_particle_cpu_checker reference mod\n";
    return 2;
  }
  const auto reference = read(argv[1]);
  const auto mod = read(argv[2]);
  if (reference.empty() || mod.empty()) {
    std::cerr << "multi-particle CPU differential FAIL: empty output\n";
    return 1;
  }
  if (reference != mod) {
    std::cerr << "multi-particle CPU differential FAIL: raw mismatch "
              << "reference=" << reference.size() << " mod=" << mod.size()
              << '\n';
    return 1;
  }
  std::cout << "multi-particle CPU reference differential PASS raw_bytes="
            << mod.size() << '\n';
  return 0;
}
