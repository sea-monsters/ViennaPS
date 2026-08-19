#pragma once

#include <array>
#include <vcKDTree.hpp>

// N1 test-only code-generation candidate. The implementation and data layout
// remain ViennaCore-owned; this declaration only centralizes the existing
// float/3-D template specialization in one object file.
extern template class viennacore::KDTree<float, std::array<float, 3>>;
