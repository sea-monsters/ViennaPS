#include <fstream>
#include <iostream>
#include <iterator>
#include <string>

int main(int argc, char **argv) {
  if (argc != 3)
    return 2;
  std::ifstream left(argv[1], std::ios::binary);
  std::ifstream right(argv[2], std::ios::binary);
  if (!left || !right)
    return 3;
  const std::string lhs((std::istreambuf_iterator<char>(left)), {});
  const std::string rhs((std::istreambuf_iterator<char>(right)), {});
  if (lhs != rhs) {
    std::cerr << "TEOS CPU Process oracle mismatch\n";
    return 1;
  }
  std::cout << "TEOS CPU Process oracle PASS\n";
  return 0;
}
