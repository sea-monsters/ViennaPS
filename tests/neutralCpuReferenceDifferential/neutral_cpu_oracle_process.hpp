#pragma once

#include <string>

// Test-only entry point. The implementation intentionally lives in a
// separate translation unit so the paired Release oracle can exercise the
// unchanged Process/CPU path without making the fixture's main translation
// unit also own every heavy ViennaPS template instantiation.
void writeNeutralCpuOracleFixture(const std::string &path);
