// Copyright 2026 ViennaPS
// SPDX-License-Identifier: MIT
//
// P5-S0 CPU-only surface process acceptance executable.  Emits a fixed-schema
// record that the Vulkan smoke can ingest for bitwise comparison.

#include "surface_acceptance_fixture.hpp"

#include <fstream>
#include <iostream>
#include <string>

int main(int argc, char *argv[]) {
  using T = float;
  constexpr int D = 2;

  const std::string outputPath = (argc > 1) ? argv[1] : std::string{};

  surface_process_acceptance::AcceptanceConfig<T> config;
  auto [domain, model] =
      surface_process_acceptance::makeAcceptanceDomainAndModel<T, D>(config);

  viennaps::Process<T, D> process(domain, model);
  process.setFluxEngineType(viennaps::FluxEngineType::CPU_TRIANGLE);

  const auto record =
      surface_process_acceptance::runAcceptance<T, D>("cpu", domain, process,
                                                      model, config);

  if (!outputPath.empty()) {
    std::ofstream out(outputPath, std::ios::trunc);
    if (!out) {
      std::cerr << "failed to open output file: " << outputPath << '\n';
      return 2;
    }
    surface_process_acceptance::serializeRecord(record, out);
  } else {
    surface_process_acceptance::serializeRecord(record, std::cout);
  }

  std::ostringstream selfCheckErrors;
  if (!surface_process_acceptance::selfCheck(record, selfCheckErrors)) {
    std::cerr << selfCheckErrors.str();
    return 1;
  }

  return 0;
}
