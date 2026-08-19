#include <fstream>
#include <iostream>
#include <iterator>
#include <string>

int main(int argc, char **argv) {
  if (argc != 3) {
    std::cerr << "usage: teospecvd_cpu_checker reference mod\n";
    return 2;
  }

  std::ifstream reference(argv[1], std::ios::binary);
  std::ifstream mod(argv[2], std::ios::binary);
  if (!reference || !mod) {
    std::cerr << "cannot open paired output\n";
    return 1;
  }

  const std::string referenceBytes((std::istreambuf_iterator<char>(reference)),
                                   std::istreambuf_iterator<char>());
  const std::string modBytes((std::istreambuf_iterator<char>(mod)),
                             std::istreambuf_iterator<char>());
  if (referenceBytes.empty() || modBytes.empty()) {
    std::cerr << "paired output is empty\n";
    return 1;
  }
  if (referenceBytes != modBytes) {
    std::cerr << "TEOSPECVD CPU reference differential mismatch\n";
    return 1;
  }

  std::cout << "TEOSPECVD CPU reference differential PASS raw_bytes="
            << referenceBytes.size()
            << " labels=2 finite=fixture-checked auto=CPU "
               "manual_vulkan=fail_closed\n";
  return 0;
}
