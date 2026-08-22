#pragma once

// P6-A1 Jacobi preconditioner: inverse-diagonal application with a typed
// error for singular (zero) diagonal entries.

#include <cmath>
#include <cstddef>
#include <stdexcept>
#include <string>
#include <vector>

#include "csrMatrix.hpp"

namespace viennaps::la {

class JacobiPreconditioner {
public:
  [[nodiscard]] bool build(const CsrMatrix &A, std::string &error) {
    error.clear();
    if (!A.isValid() || A.rows() != A.cols()) {
      error = "JacobiPreconditioner: matrix must be valid and square";
      return false;
    }
    const auto &rp = A.rowPtr();
    const auto &ci = A.colIdx();
    const auto &va = A.values();
    invDiag_.assign(A.rows(), 0.0);
    for (std::size_t i = 0; i < A.rows(); ++i) {
      double diag = 0.0;
      for (std::size_t k = rp[i]; k < rp[i + 1]; ++k)
        if (ci[k] == i)
          diag = va[k];
      if (diag == 0.0 || !std::isfinite(diag)) {
        error = "JacobiPreconditioner: zero or non-finite diagonal at row " +
                std::to_string(i);
        return false;
      }
      invDiag_[i] = 1.0 / diag;
    }
    return true;
  }

  // z = M^-1 * r
  void apply(const std::vector<double> &r, std::vector<double> &z) const {
    z.resize(r.size());
    for (std::size_t i = 0; i < r.size(); ++i)
      z[i] = invDiag_[i] * r[i];
  }

private:
  std::vector<double> invDiag_;
};

} // namespace viennaps::la
