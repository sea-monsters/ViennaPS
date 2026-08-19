#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

int main(int argc, char **argv) {
  if (argc != 3) {
    std::cerr << "usage: wetetch_velocity_cpu_checker reference mod\n";
    return 2;
  }
  std::ifstream reference(argv[1], std::ios::binary);
  std::ifstream mod(argv[2], std::ios::binary);
  std::ostringstream referenceText;
  std::ostringstream modText;
  referenceText << reference.rdbuf();
  modText << mod.rdbuf();
  if (!reference || !mod || referenceText.str() != modText.str()) {
    std::cerr << "wetetch CPU reference differential RED\n";
    return 1;
  }
  std::cout << "wetetch CPU reference differential PASS raw_exact\n";
  return 0;
}
