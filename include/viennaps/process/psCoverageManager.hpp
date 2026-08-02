#pragma once

#include "psCoverageDeltaExecutor.hpp"
#include "psProcessContext.hpp"

#include <cstddef>
#include <string>
#include <utility>
#include <vector>

namespace viennaps {

VIENNAPS_TEMPLATE_ND(NumericType, D) class CoverageManager {
  std::ofstream covMetricFile_;
  SmartPointer<PointData<NumericType>> previousCoverages_;
  CoverageDeltaExecutor<NumericType> coverageDeltaExecutor_;

public:
  CoverageManager() = default;
  ~CoverageManager() {
    if (covMetricFile_.is_open())
      covMetricFile_.close();
  }

  bool
  initializeCoverages(ProcessContext<NumericType, D> const &context) const {
    // Initialize coverage information based on the current context
    auto surfaceModel = context.model->getSurfaceModel();
    assert(surfaceModel != nullptr);
    assert(context.diskMesh != nullptr);
    assert(context.diskMesh->getNodes().size() > 0);

    surfaceModel->initializeCoverages(context.diskMesh->getNodes().size());

    return surfaceModel->getCoverages() != nullptr;
  }

  void saveCoverages(ProcessContext<NumericType, D> const &context) {
    previousCoverages_ = SmartPointer<PointData<NumericType>>::New(
        *context.model->getSurfaceModel()->getCoverages());
  }

  void setCoverageDeltaExecutor(CoverageDeltaExecutor<NumericType> executor) {
    coverageDeltaExecutor_ = std::move(executor);
  }

  [[nodiscard]] CoverageDeltaExecutor<NumericType>
  getCoverageDeltaExecutor() const {
    return coverageDeltaExecutor_;
  }

  void clearCoverageDeltaExecutor() { coverageDeltaExecutor_ = {}; }

  bool
  checkCoveragesConvergence(ProcessContext<NumericType, D> const &context) {

    auto coverages = context.model->getSurfaceModel()->getCoverages();
    assert(previousCoverages_ != nullptr);
    const auto cpuMetric =
        calculateCoverageDeltaMetric(coverages, previousCoverages_);
    auto deltaMetric = runCoverageDeltaExecutor(coverages, previousCoverages_,
                                                cpuMetric);
    assert(
        deltaMetric.size() ==
        context.model->getSurfaceModel()->getCoverages()->getScalarDataSize());

    if (Logger::hasInfo()) {
      logMetric(deltaMetric);
      std::stringstream stream;
      stream << std::setprecision(4) << std::fixed;
      stream << "Coverage delta metric: ";
      for (int i = 0; i < coverages->getScalarDataSize(); i++) {
        stream << coverages->getScalarDataLabel(i) << ": " << deltaMetric[i]
               << "\t";
      }
      VIENNACORE_LOG_INFO(stream.str());
    }

    for (auto val : deltaMetric) {
      if (val > context.coverageParams.tolerance)
        return false;
    }
    return true;
  }

private:
  std::vector<NumericType> runCoverageDeltaExecutor(
      SmartPointer<PointData<NumericType>> updated,
      SmartPointer<PointData<NumericType>> previous,
      const std::vector<NumericType> &cpuMetric) const {
    if (!coverageDeltaExecutor_)
      return cpuMetric;

    std::vector<NumericType> updatedValues;
    std::vector<NumericType> previousValues;
    std::vector<std::size_t> channelOffsets{0U};
    const auto channelCount = updated->getScalarDataSize();
    for (int i = 0; i < channelCount; ++i) {
      const auto label = updated->getScalarDataLabel(i);
      const auto updatedData = updated->getScalarData(label);
      const auto previousData = previous->getScalarData(label);
      if (updatedData->size() != previousData->size())
        return cpuMetric;
      updatedValues.insert(updatedValues.end(), updatedData->begin(),
                           updatedData->end());
      previousValues.insert(previousValues.end(), previousData->begin(),
                            previousData->end());
      channelOffsets.push_back(updatedValues.size());
    }

    std::vector<NumericType> candidate(channelCount, NumericType(0));
    CoverageDeltaWork<NumericType> work{
        std::span<const NumericType>(updatedValues),
        std::span<const NumericType>(previousValues),
        std::span<const std::size_t>(channelOffsets),
        std::span<NumericType>(candidate),
        static_cast<std::size_t>(channelCount), 0U, false};
    std::string error;
    try {
      if (coverageDeltaExecutor_(work, error) && work.complete &&
          work.writtenCount == work.channelCount)
        return candidate;
    } catch (...) {
      // Optional backends are fail-closed; preserve the canonical CPU result.
    }
    return cpuMetric;
  }

  static std::vector<NumericType>
  calculateCoverageDeltaMetric(SmartPointer<PointData<NumericType>> updated,
                               SmartPointer<PointData<NumericType>> previous) {

    assert(updated->getScalarDataSize() == previous->getScalarDataSize());
    std::vector<NumericType> delta(updated->getScalarDataSize(), 0.);

#pragma omp parallel for
    for (int i = 0; i < updated->getScalarDataSize(); i++) {
      auto label = updated->getScalarDataLabel(i);
      auto updatedData = updated->getScalarData(label);
      auto previousData = previous->getScalarData(label);
      for (size_t j = 0; j < updatedData->size(); j++) {
        auto diff = updatedData->at(j) - previousData->at(j);
        delta[i] += diff * diff;
      }

      delta[i] /= updatedData->size();
    }

    return delta;
  }

  void logMetric(const std::vector<NumericType> &metric) {
    if (!Logger::hasDebug())
      return;

    if (!covMetricFile_.is_open()) {
      covMetricFile_.open("coverage_metrics.txt");
    }
    assert(covMetricFile_.is_open());

    for (auto val : metric) {
      covMetricFile_ << val << ";";
    }
    covMetricFile_ << "\n";
  }
};

} // namespace viennaps
