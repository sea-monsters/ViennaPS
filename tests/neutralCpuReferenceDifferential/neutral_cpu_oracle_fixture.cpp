#include "neutral_cpu_oracle_process.hpp"

#include <exception>
#include <iostream>

int main(int argc, char **argv) {
  if (argc != 2) {
    std::cerr << "usage: neutral_cpu_oracle_fixture output\n";
    return 2;
  }
  try {
    writeNeutralCpuOracleFixture(argv[1]);
  } catch (const std::exception &error) {
    std::cerr << "fixture failure: " << error.what() << '\n';
    return 1;
  }
  return 0;
}
