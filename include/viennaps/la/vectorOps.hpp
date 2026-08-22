#pragma once

// P6-A1 deterministic double-precision vector operations.
// Dot products use fixed index-based blocks: the block partition, each
// block's sequential accumulation, and the sequential block combine are all
// independent of OpenMP scheduling, so results are bit-identical for any
// thread count. No omp reduction/simd pragmas are used anywhere in this
// layer on purpose.

#include <cmath>
#include <cstddef>
#include <vector>

namespace viennaps::la {

inline constexpr std::size_t kDeterministicBlock = 4096;

void axpy(double alpha, const std::vector<double> &x,
          std::vector<double> &y) {
  const std::size_t n = x.size();
  for (std::size_t i = 0; i < n; ++i)
    y[i] += alpha * x[i];
}

void scale(double alpha, std::vector<double> &x) {
  for (auto &v : x)
    v *= alpha;
}

double dot(const std::vector<double> &a, const std::vector<double> &b) {
  const std::size_t n = a.size();
  if (n != b.size() || n == 0)
    return 0.0;
  const std::size_t blocks = (n + kDeterministicBlock - 1) / kDeterministicBlock;
  std::vector<double> partial(blocks, 0.0);
#ifdef _OPENMP
#pragma omp parallel for schedule(static)
#endif
  for (long long blk = 0; blk < static_cast<long long>(blocks); ++blk) {
    const std::size_t begin = static_cast<std::size_t>(blk) * kDeterministicBlock;
    const std::size_t end =
        begin + kDeterministicBlock < n ? begin + kDeterministicBlock : n;
    double s = 0.0;
    for (std::size_t i = begin; i < end; ++i)
      s += a[i] * b[i];
    partial[static_cast<std::size_t>(blk)] = s;
  }
  double total = 0.0;
  for (std::size_t k = 0; k < blocks; ++k)
    total += partial[k];
  return total;
}

double norm2(const std::vector<double> &x) { return std::sqrt(dot(x, x)); }

double infNorm(const std::vector<double> &x) {
  double m = 0.0;
  for (const auto v : x) {
    const double a = v < 0.0 ? -v : v;
    if (a > m)
      m = a;
  }
  return m;
}

} // namespace viennaps::la
