#include <fstream>
#include <iostream>
#include <string>

int main(int argc, char **argv) {
  if (argc != 3) {
    std::cerr << "usage: oxide_regrowth_cpu_checker reference mod\n";
    return 2;
  }
  std::ifstream reference(argv[1], std::ios::binary);
  std::ifstream mod(argv[2], std::ios::binary);
  if (!reference || !mod) {
    std::cerr << "cannot open paired output\n";
    return 2;
  }
  std::string referenceBytes((std::istreambuf_iterator<char>(reference)), {});
  std::string modBytes((std::istreambuf_iterator<char>(mod)), {});
  if (referenceBytes != modBytes) {
    std::cerr << "raw-byte mismatch: reference=" << referenceBytes.size()
              << " mod=" << modBytes.size() << '\n';
    return 1;
  }
  std::cout << "raw-byte exact: " << referenceBytes.size() << " bytes\n";
  return 0;
}
