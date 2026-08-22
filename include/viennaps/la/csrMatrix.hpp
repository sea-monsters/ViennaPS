#pragma once

// P6-A1 linear-algebra layer: deterministic double-precision CSR matrix.
// Pure library component  no ViennaPS Process/model types, no Vulkan types.
// Every traversal is index-ordered so results are bit-reproducible across
// OpenMP thread counts (intent-framework invariant: CPU oracle authority).

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <stdexcept>
#include <string>
#include <vector>

namespace viennaps::la {

class CsrMatrix {
public:
  CsrMatrix() = default;

  static CsrMatrix fromTriplets(std::size_t rows, std::size_t cols,
                                const std::vector<std::size_t> &r,
                                const std::vector<std::size_t> &c,
                                const std::vector<double> &v,
                                std::string *error = nullptr) {
    if (r.size() != c.size() || r.size() != v.size()) {
      if (error)
        *error = "fromTriplets: triplet arrays have mismatched lengths";
      return CsrMatrix{};
    }
    for (std::size_t k = 0; k < r.size(); ++k) {
      if (r[k] >= rows || c[k] >= cols) {
        if (error)
          *error = "fromTriplets: index out of range";
        return CsrMatrix{};
      }
    }
    std::vector<std::size_t> order(r.size());
    for (std::size_t k = 0; k < order.size(); ++k)
      order[k] = k;
    // Row-major then column-major ordering makes assembly order-independent.
    std::sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) {
      if (r[a] != r[b])
        return r[a] < r[b];
      return c[a] < c[b];
    });
    CsrMatrix m;
    m.rows_ = rows;
    m.cols_ = cols;
    m.rowPtr_.assign(rows + 1, 0);
    for (std::size_t k = 0; k < order.size();) {
      const std::size_t i = r[order[k]];
      const std::size_t j = c[order[k]];
      double sum = 0.0;
      std::size_t next = k;
      while (next < order.size() && r[order[next]] == i &&
             c[order[next]] == j) {
        sum += v[order[next]];
        ++next;
      }
      m.colIdx_.push_back(j);
      m.values_.push_back(sum);
      ++m.rowPtr_[i + 1];
      k = next;
    }
    for (std::size_t i = 0; i < rows; ++i)
      m.rowPtr_[i + 1] += m.rowPtr_[i];
    return m;
  }

  std::size_t rows() const { return rows_; }
  std::size_t cols() const { return cols_; }
  std::size_t nonZeros() const { return values_.size(); }
  const std::vector<std::size_t> &rowPtr() const { return rowPtr_; }
  const std::vector<std::size_t> &colIdx() const { return colIdx_; }
  const std::vector<double> &values() const { return values_; }

  bool isValid() const { return rowPtr_.size() == rows_ + 1; }

  // y = A * x, sequential row traversal (deterministic accumulation).
  void spmv(const std::vector<double> &x, std::vector<double> &y) const {
    assert(isValid());
    assert(x.size() == cols_);
    y.assign(rows_, 0.0);
    for (std::size_t i = 0; i < rows_; ++i) {
      double sum = 0.0;
      for (std::size_t k = rowPtr_[i]; k < rowPtr_[i + 1]; ++k)
        sum += values_[k] * x[colIdx_[k]];
      y[i] = sum;
    }
  }

private:
  std::size_t rows_{0};
  std::size_t cols_{0};
  std::vector<std::size_t> rowPtr_;
  std::vector<std::size_t> colIdx_;
  std::vector<double> values_;
};

} // namespace viennaps::la
