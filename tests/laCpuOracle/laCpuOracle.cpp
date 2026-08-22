// P6-A1 acceptance fixture: four facets in one focused executable.
// 1. Stencil assembly matches hand-computed references (1D tridiagonal,
//    2D five-point Laplacian).
// 2. Frozen SPD system: BiCGSTAB converges with identical iterate sequences
//    at OpenMP 1/2/4/8 (bit-level, via residual-history fingerprints).
// 3. Singular/inconsistent inputs fail with typed statuses, never hang.
// 4. CSR spmv equals a dense reference product exactly on a small case.

#include <la/bicgstabSolver.hpp>
#include <la/csrMatrix.hpp>
#include <la/jacobiPreconditioner.hpp>
#include <la/vectorOps.hpp>

#ifdef _OPENMP
#include <omp.h>
#endif

#include <cmath>
#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

namespace {

int failures = 0;

void require(const bool condition, const char *what) {
  if (!condition) {
    std::cerr << "laCpuOracle FAIL: " << what << '\n';
    ++failures;
  }
}

std::uint64_t fingerprintVector(const std::vector<double> &v) {
  const std::uint64_t fnvPrime = 1099511628211ull;
  std::uint64_t hash = 1469598103934665603ull;
  for (const auto d : v) {
    const auto bits =
        static_cast<const unsigned char *>(static_cast<const void *>(&d));
    for (std::size_t i = 0; i < sizeof(double); ++i) {
      hash ^= bits[i];
      hash *= fnvPrime;
    }
  }
  return hash;
}

// Deterministic LCG so the frozen system is identical on every machine.
class Lcg {
public:
  explicit Lcg(std::uint64_t seed) : state_(seed) {}
  double nextUniform() { // [0, 1)
    state_ = state_ * 6364136223846793005ull + 1442695040888963407ull;
    return static_cast<double>(state_ >> 11) / 9007199254740992.0;
  }

private:
  std::uint64_t state_;
};

// Facet 1a: 1D tridiagonal(-1, 2, -1), N = 6, hand-checked.
void testAssembly1D() {
  const std::size_t n = 6;
  std::vector<std::size_t> r, c;
  std::vector<double> v;
  for (std::size_t i = 0; i < n; ++i) {
    r.push_back(i); c.push_back(i); v.push_back(2.0);
    if (i > 0) { r.push_back(i); c.push_back(i - 1); v.push_back(-1.0); }
    if (i + 1 < n) { r.push_back(i); c.push_back(i + 1); v.push_back(-1.0); }
  }
  std::string error;
  const auto A = viennaps::la::CsrMatrix::fromTriplets(n, n, r, c, v, &error);
  require(error.empty(), "1D assembly produced no error");
  require(A.nonZeros() == 3 * n - 2, "1D assembly nonzero count");
  const auto &rp = A.rowPtr();
  const auto &ci = A.colIdx();
  const auto &va = A.values();
  bool ok = rp[0] == 0;
  std::size_t k = 0;
  for (std::size_t i = 0; i < n; ++i) {
    ok = ok && rp[i + 1] - rp[i] == (i == 0 || i + 1 == n ? 2 : 3);
    for (std::size_t t = rp[i]; t < rp[i + 1]; ++t, ++k)
      ok = ok && va[t] == ((ci[t] == i) ? 2.0 : -1.0);
  }
  require(ok && k == A.nonZeros(), "1D hand-computed entries");
}

// Facet 1b: 2D five-point Laplacian on a 3x3 grid: diag 4, off -1.
void testAssembly2D() {
  const std::size_t g = 3, n = g * g;
  auto idx = [&](std::size_t x, std::size_t y) { return y * g + x; };
  std::vector<std::size_t> r, c;
  std::vector<double> v;
  for (std::size_t y = 0; y < g; ++y)
    for (std::size_t x = 0; x < g; ++x) {
      const auto i = idx(x, y);
      r.push_back(i); c.push_back(i); v.push_back(4.0);
      if (x > 0) { r.push_back(i); c.push_back(idx(x - 1, y)); v.push_back(-1.0); }
      if (x + 1 < g) { r.push_back(i); c.push_back(idx(x + 1, y)); v.push_back(-1.0); }
      if (y > 0) { r.push_back(i); c.push_back(idx(x, y - 1)); v.push_back(-1.0); }
      if (y + 1 < g) { r.push_back(i); c.push_back(idx(x, y + 1)); v.push_back(-1.0); }
    }
  std::string error;
  const auto A = viennaps::la::CsrMatrix::fromTriplets(n, n, r, c, v, &error);
  require(error.empty(), "2D assembly produced no error");
  // Center node (1,1): four off-diagonals plus diagonal.
  const auto center = idx(1, 1);
  const auto &rp = A.rowPtr();
  bool ok = rp[center + 1] - rp[center] == 5;
  const auto &ci = A.colIdx();
  const auto &va = A.values();
  double diagSum = 0.0;
  for (std::size_t t = rp[center]; t < rp[center + 1]; ++t) {
    if (ci[t] == center) diagSum += va[t];
    else ok = ok && va[t] == -1.0;
  }
  require(ok && diagSum == 4.0, "2D hand-computed center stencil");
}

// Deterministic diagonally dominant symmetric SPD system of size n.
viennaps::la::CsrMatrix makeFrozenSpd(std::size_t n) {
  Lcg rng{20260821ull};
  std::vector<std::size_t> r, c;
  std::vector<double> v;
  for (std::size_t i = 0; i < n; ++i) {
    r.push_back(i); c.push_back(i); v.push_back(4.0);
  }
  for (std::size_t i = 0; i + 1 < n; ++i) {
    const double w = rng.nextUniform();
    r.push_back(i);     c.push_back(i + 1); v.push_back(-w);
    r.push_back(i + 1); c.push_back(i);     v.push_back(-w);
  }
  std::string error;
  auto A = viennaps::la::CsrMatrix::fromTriplets(n, n, r, c, v, &error);
  if (!error.empty())
    std::cerr << "makeFrozenSpd error: " << error << '\n';
  return A;
}

// Facet 2: convergence + bit-identical iterate sequences across OMP counts.
void testFrozenSpdCrossThread() {
  const std::size_t n = 1200;
  const auto A = makeFrozenSpd(n);

  std::vector<double> b(n);
  Lcg rhs{987654321ull};
  for (auto &x : b)
    x = rhs.nextUniform();

  viennaps::la::JacobiPreconditioner M;
  std::string error;
  require(M.build(A, error), "Jacobi build on frozen SPD");
#ifdef _OPENMP
  const int counts[] = {1, 2, 4, 8};
  std::uint64_t firstSolutionHash = 0;
  std::uint64_t firstHistoryHash = 0;
  for (const int threads : counts) {
    omp_set_num_threads(threads);
#endif
    viennaps::la::SolveOptions options;
    options.maxIterations = 400;
    options.relativeTol = 1e-12;
    std::vector<double> x(n, 0.0);
    const auto result =
        viennaps::la::solveBiCgStab(A, M, b, x, options);
    require(result.status == viennaps::la::SolveStatus::Converged,
            "frozen SPD converged");
    require(result.residualHistory.back() <=
                options.relativeTol * viennaps::la::norm2(b),
            "frozen SPD final relative residual");
    const auto solutionHash = fingerprintVector(x);
    const auto historyHash =
        viennaps::la::fingerprintHistory(result.residualHistory);
#ifdef _OPENMP
    if (threads == 1) {
      firstSolutionHash = solutionHash;
      firstHistoryHash = historyHash;
      std::cout << "OMP=1 iterations=" << result.iterations
                << " solutionFingerprint=0x" << std::hex << solutionHash
                << " historyFingerprint=" << historyHash << std::dec
                << '\n';
    } else {
      require(solutionHash == firstSolutionHash,
              "solution bits identical across OMP counts");
      require(historyHash == firstHistoryHash,
              "residual history bits identical across OMP counts");
    }
  }
  omp_set_num_threads(1);
#else
  std::cout << "solutionFingerprint=0x" << std::hex << solutionHash
            << " historyFingerprint=" << historyHash << std::dec
            << " (single-threaded build)\n";
#endif
}

// Facet 3: typed failures for singular / inconsistent inputs.
void testTypedFailures() {
  // Zero diagonal -> Jacobi reports a typed error naming the row.
  std::vector<std::size_t> r{0}, c{0};
  std::vector<double> v{0.0};
  std::string error;
  const auto singular = viennaps::la::CsrMatrix::fromTriplets(1, 1, r, c, v, &error);
  viennaps::la::JacobiPreconditioner M;
  require(!M.build(singular, error), "zero diagonal rejected");
  require(error.find("row 0") != std::string::npos,
          "singular diagnostic names the row");

  // Structurally rank-deficient system (last row dropped entirely) must
  // terminate with a breakdown status instead of converging or looping.
  const std::size_t n = 50;
  const auto A50 = makeFrozenSpd(n);
  std::vector<std::size_t> zr, zc;
  std::vector<double> zv;
  {
    const auto &rpA = A50.rowPtr();
    const auto &ciA = A50.colIdx();
    const auto &vaA = A50.values();
    for (std::size_t i = 0; i + 1 < n; ++i)
      for (std::size_t k = rpA[i]; k < rpA[i + 1]; ++k)
        if (ciA[k] < n - 1) { // drop column n-1 as well -> zero last row/col
          zr.push_back(i); zc.push_back(ciA[k]); zv.push_back(vaA[k]);
        }
  }
  const auto broken = viennaps::la::CsrMatrix::fromTriplets(
      n, n, zr, zc, zv, &error);
  require(broken.isValid(), "rank-deficient matrix assembled");
  require(M.build(A50, error), "preconditioner from healthy matrix");
  viennaps::la::SolveOptions options;
  options.maxIterations = 60;
  options.absoluteTol = 0.0;
  options.relativeTol = 0.0; // force full iteration budget / breakdown path
  std::vector<double> x(n, 0.0);
  std::vector<double> b(n, 1.0);
  const auto result = viennaps::la::solveBiCgStab(broken, M, b, x, options);
  require(result.status != viennaps::la::SolveStatus::Converged,
          "rank-deficient system never reports Converged");
  require(result.iterations <= options.maxIterations,
          "breakdown terminates within budget");
}

// Facet 4: spmv equals dense reference exactly.
void testSpmvDenseReference() {
  const std::size_t n = 40;
  Lcg rng{424242ull};
  std::vector<double> dense(n * n, 0.0);
  std::vector<std::size_t> r, c;
  std::vector<double> v;
  for (std::size_t k = 0; k < 6 * n; ++k) {
    const auto i = static_cast<std::size_t>(rng.nextUniform() * n) % n;
    const auto j = static_cast<std::size_t>(rng.nextUniform() * n) % n;
    const double val = rng.nextUniform() - 0.25;
    dense[i * n + j] += val;
    r.push_back(i); c.push_back(j); v.push_back(val);
  }
  std::string error;
  const auto A = viennaps::la::CsrMatrix::fromTriplets(n, n, r, c, v, &error);
  std::vector<double> x(n);
  for (auto &e : x) e = rng.nextUniform();
  std::vector<double> y;
  A.spmv(x, y);
  bool ok = y.size() == n;
  for (std::size_t i = 0; i < n && ok; ++i) {
    double ref = 0.0;
    for (std::size_t j = 0; j < n; ++j)
      ref += dense[i * n + j] * x[j];
    ok = ok && y[i] == ref; // identical accumulation order per row
  }
  require(ok, "CSR spmv equals dense reference bit-for-bit");
}

} // namespace

int main() {
  testAssembly1D();
  testAssembly2D();
  testFrozenSpdCrossThread();
  testTypedFailures();
  testSpmvDenseReference();

  if (failures != 0) {
    std::cerr << "laCpuOracle FAILED with " << failures << " failure(s)\n";
    return 1;
  }
  std::cout << "laCpuOracle PASS (assembly, cross-thread determinism, "
               "typed failures, spmv reference)\n";
  return 0;
}
