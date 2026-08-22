#pragma once

// P6-A1 BiCGSTAB solver with left Jacobi preconditioning and honest residual
// history: history[0] is ||b - A x0||_2 before iterating, entry k (k >= 1) is
// the true two-norm recomputed after update k. All reductions flow through
// the deterministic blocked dot, so iterate sequences and fingerprints are
// identical for any OpenMP thread count.

#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

#include "csrMatrix.hpp"
#include "jacobiPreconditioner.hpp"
#include "vectorOps.hpp"

namespace viennaps::la {

enum class SolveStatus {
  Converged,
  MaxIterations,
  BreakdownRhoZero,
  BreakdownAlphaZero,
  BreakdownOmegaOutOfRange,
  IncompatibleSizes
};

[[nodiscard]] inline const char *toString(SolveStatus s) {
  switch (s) {
  case SolveStatus::Converged:
    return "Converged";
  case SolveStatus::MaxIterations:
    return "MaxIterations";
  case SolveStatus::BreakdownRhoZero:
    return "BreakdownRhoZero";
  case SolveStatus::BreakdownAlphaZero:
    return "BreakdownAlphaZero";
  case SolveStatus::BreakdownOmegaOutOfRange:
    return "BreakdownOmegaOutOfRange";
  }
  return "IncompatibleSizes";
}

struct SolveOptions {
  std::size_t maxIterations = 500;
  double relativeTol = 1e-10;
  double absoluteTol = 0.0;
};

struct SolveResult {
  SolveStatus status = SolveStatus::IncompatibleSizes;
  std::size_t iterations = 0;
  std::vector<double> residualHistory; // true ||r||_2 per checkpoint
};

// FNV-1a fingerprint over residual-history bit patterns (diagnostic anchor
// for cross-thread-count equality checks).
[[nodiscard]] inline std::uint64_t
fingerprintHistory(const std::vector<double> &history) {
  const std::uint64_t fnvPrime = 1099511628211ull;
  std::uint64_t hash = 1469598103934665603ull;
  auto mixDouble = [&](double d) {
    const auto bits = static_cast<const unsigned char *>(
        static_cast<const void *>(&d));
    for (std::size_t i = 0; i < sizeof(double); ++i) {
      hash ^= bits[i];
      hash *= fnvPrime;
    }
  };
  const auto sizeBits = static_cast<std::uint64_t>(history.size());
  const auto sizeBytes =
      static_cast<const unsigned char *>(static_cast<const void *>(&sizeBits));
  for (std::size_t i = 0; i < sizeof(std::uint64_t); ++i) {
    hash ^= sizeBytes[i];
    hash *= fnvPrime;
  }
  for (const auto v : history)
    mixDouble(v);
  return hash;
}

inline SolveResult solveBiCgStab(const CsrMatrix &A,
                                 const JacobiPreconditioner &M,
                                 const std::vector<double> &b,
                                 std::vector<double> &x,
                                 const SolveOptions &options = {}) {
  SolveResult result;
  if (!A.isValid() || A.rows() != A.cols() || b.size() != A.rows() ||
      x.size() != A.rows()) {
    result.status = SolveStatus::IncompatibleSizes;
    return result;
  }

  std::vector<double> r, Ax;
  A.spmv(x, Ax);
  r.resize(A.rows());
  for (std::size_t i = 0; i < A.rows(); ++i)
    r[i] = b[i] - Ax[i];

  const double bNorm = norm2(b);
  double trueResidual = norm2(r);
  result.residualHistory.push_back(trueResidual);

  const double threshold =
      options.absoluteTol +
      options.relativeTol * (bNorm > 0.0 ? bNorm : 1.0);
  if (trueResidual <= threshold) {
    result.status = SolveStatus::Converged;
    return result;
  }

  std::vector<double> rHat = r;       // shadow residual (unmodified)
  std::vector<double> rhoVec(b.size());
  std::vector<double> vVec(b.size()), pVec(b.size()), yVec(b.size());
  std::vector<double> sVec(b.size()), zVec(b.size()), tVec(b.size());
  double rho = 1.0, alpha = 1.0, omega = 1.0;

  for (std::size_t iter = 1; iter <= options.maxIterations; ++iter) {
    result.iterations = iter;
    const double rhoNext = dot(rHat, r);
    if (rhoNext == 0.0 || !std::isfinite(rhoNext)) {
      result.status = SolveStatus::BreakdownRhoZero;
      return result;
    }
    double beta = (rhoNext / rho) * (alpha / omega);
    rho = rhoNext;

    for (std::size_t i = 0; i < A.rows(); ++i)
      pVec[i] = r[i] + beta * (pVec[i] - omega * vVec[i]);

    M.apply(pVec, yVec);
    A.spmv(yVec, vVec);
    alpha = rho / dot(rHat, vVec);
    if (alpha == 0.0 || !std::isfinite(alpha)) {
      result.status = SolveStatus::BreakdownAlphaZero;
      return result;
    }

    sVec.resize(A.rows());
    for (std::size_t i = 0; i < A.rows(); ++i)
      sVec[i] = r[i] - alpha * vVec[i];

    const double sNorm = norm2(sVec);
    if (sNorm <= threshold) {
      axpy(alpha, pVec, x);
      trueResidual = sNorm;
      result.residualHistory.push_back(trueResidual);
      result.status = SolveStatus::Converged;
      return result;
    }

    M.apply(sVec, zVec);
    A.spmv(zVec, tVec);
    const double tt = dot(tVec, tVec);
    if (tt == 0.0 || !std::isfinite(tt)) {
      result.status = SolveStatus::BreakdownOmegaOutOfRange;
      return result;
    }
    omega = dot(tVec, sVec) / tt;
    if (!(std::abs(omega) > 0.0) || !std::isfinite(omega)) {
      result.status = SolveStatus::BreakdownOmegaOutOfRange;
      return result;
    }

    axpy(alpha, pVec, x);
    axpy(omega, zVec, x);

    for (std::size_t i = 0; i < A.rows(); ++i)
      r[i] = sVec[i] - omega * tVec[i];
    trueResidual = norm2(r);
    result.residualHistory.push_back(trueResidual);

    if (trueResidual <= threshold) {
      result.status = SolveStatus::Converged;
      return result;
    }
    if (omega == 0.0) {
      result.status = SolveStatus::BreakdownOmegaOutOfRange;
      return result;
    }
  }
  result.status = SolveStatus::MaxIterations;
  return result;
}

} // namespace viennaps::la
