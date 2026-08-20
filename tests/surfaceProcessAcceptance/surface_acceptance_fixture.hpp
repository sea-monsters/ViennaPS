// Copyright 2026 ViennaPS
// SPDX-License-Identifier: MIT
//
// Shared acceptance fixture for the P5-S0 surface-process route.  Used by both
// the CPU-only acceptance executable and the Vulkan composition smoke test.

#pragma once

#include <geometries/psMakePlane.hpp>
#include <models/psNeutralTransport.hpp>
#include <process/psProcess.hpp>
#include <psDomain.hpp>

#include <bit>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iostream>
#include <limits>
#include <sstream>
#include <string>
#include <vector>

namespace surface_process_acceptance {

using namespace viennacore;

// Surface model wrapper that seeds coverage to a deterministic initial value.
// This is required because the full Process route reinitializes coverages
// during setup; subclassing keeps the dynamic_cast to the concrete neutral
// surface model working for the Vulkan velocity binding.
template <typename T, int D>
class InitialCoverageNeutralSurfaceModel
    : public viennaps::impl::NeutralTransportSurfaceModel<T, D> {
  using Base = viennaps::impl::NeutralTransportSurfaceModel<T, D>;
  viennaps::NeutralTransportParameters<T> paramsCopy_;
  T initialCoverage_;

public:
  InitialCoverageNeutralSurfaceModel(
      const viennaps::NeutralTransportParameters<T> &params, T initialCoverage)
      : Base(params), paramsCopy_(params), initialCoverage_(initialCoverage) {}

  void initializeCoverages(unsigned numGeometryPoints) override {
    Base::initializeCoverages(numGeometryPoints);
    auto coverage = this->getCoverages()->getScalarData(paramsCopy_.coverageLabel);
    if (coverage != nullptr) {
      std::fill(coverage->begin(), coverage->end(), initialCoverage_);
    }
  }
};

// Records every advection callback invocation so the fixture can verify
// callback ordering and derive iteration/process-time statistics.
template <typename T, int D>
class RecordingAdvectionCallback : public viennaps::AdvectionCallback<T, D> {
public:
  bool applyPreAdvect(const T processTime) override {
    preCalls_++;
    lastPreTime_ = processTime;
    sequence_.push_back("pre:" + std::to_string(processTime));
    return true;
  }

  bool applyPostAdvect(const T advectionTime) override {
    postCalls_++;
    lastPostTime_ = advectionTime;
    sequence_.push_back("post:" + std::to_string(advectionTime));
    return true;
  }

  unsigned preCalls() const { return preCalls_; }
  unsigned postCalls() const { return postCalls_; }
  T lastPreTime() const { return lastPreTime_; }
  T lastPostTime() const { return lastPostTime_; }
  const std::vector<std::string> &sequence() const { return sequence_; }

private:
  unsigned preCalls_ = 0U;
  unsigned postCalls_ = 0U;
  T lastPreTime_ = T(0);
  T lastPostTime_ = T(0);
  std::vector<std::string> sequence_;
};

// Fixed-schema record used for deterministic CPU/Vulkan comparison.
template <typename T, int D> struct AcceptanceRecord {
  static constexpr int kSchemaVersion = 1;

  std::string routeTag;

  // Parameters mirrored into the record so consumers can reason about the
  // fixture without re-parsing CMake or source files.
  T gridDelta = T(0);
  T xExtent = T(0);
  T yExtent = T(0);
  T processDuration = T(0);
  unsigned rngSeed = 0U;
  unsigned raysPerPoint = 0U;
  int maxReflections = 0;
  T initialCoverageValue = T(0);

  // Flux observation from calculateFlux().
  std::vector<T> fluxCellData;
  T fluxCellDataSum = T(0);

  // Process outcome.
  viennaps::ProcessResult processResult = viennaps::ProcessResult::SUCCESS;

  // Initial coverages immediately after calculateFlux() initializes them.
  std::vector<T> initialCoverages;

  // Final surface/coverage state after apply().
  std::vector<Vec3D<T>> finalSurfacePoints;
  std::vector<T> finalSurfaceMaterialIds;
  std::vector<T> finalCoverages;
  bool topLevelSetValid = false;

  // Timing and callback statistics.
  T processTime = T(0);
  unsigned advectionIterationCount = 0U;
  std::vector<std::string> callbackSequence;
};

// Configuration knob bundle used by runAcceptance().
template <typename T> struct AcceptanceConfig {
  T gridDelta = T(0.5);
  T xExtent = T(4);
  T yExtent = T(4);
  T processDuration = T(0.1);
  unsigned rngSeed = 42U;
  unsigned raysPerPoint = 1U;
  int maxReflections = 2;
  T initialCoverage = T(0.5);
  unsigned coverageMaxIterations = 10U;
  T coverageTolerance = T(1e-4);
  viennaps::MetaDataLevel metaDataLevel = viennaps::MetaDataLevel::PROCESS;
};

namespace detail {

template <typename T> std::uint32_t floatBits(const T value) {
  static_assert(sizeof(T) == sizeof(std::uint32_t),
                "floatBits supports 32-bit floating point only");
  return std::bit_cast<std::uint32_t>(value);
}

template <typename T>
void writeScalar(std::ostream &out, const char *name, const T value) {
  out << name << "=" << std::hex << std::setfill('0') << std::setw(8)
      << floatBits(value) << std::dec << '\n';
}

template <typename T>
bool readScalar(std::istream &in, const char *expectedName, T &value) {
  std::string line;
  if (!std::getline(in, line))
    return false;
  const std::string prefix = std::string(expectedName) + "=";
  if (line.find(prefix) != 0)
    return false;
  const auto bits = static_cast<std::uint32_t>(
      std::stoul(line.substr(prefix.size()), nullptr, 16));
  value = std::bit_cast<T>(bits);
  return true;
}

template <typename T>
void writeValues(std::ostream &out, const char *name, const std::vector<T> &values) {
  out << name << ".count=" << values.size() << '\n';
  out << name << "=" << std::hex << std::setfill('0');
  for (const auto value : values)
    out << std::setw(8) << floatBits(value) << ' ';
  out << std::dec << '\n';
}

template <typename T>
void writePoints(std::ostream &out, const char *name,
                 const std::vector<Vec3D<T>> &points) {
  out << name << ".count=" << points.size() << '\n';
  out << name << "=" << std::hex << std::setfill('0');
  for (const auto &point : points) {
    out << std::setw(8) << floatBits(point[0]) << ':' << std::setw(8)
        << floatBits(point[1]) << ':' << std::setw(8) << floatBits(point[2])
        << ' ';
  }
  out << std::dec << '\n';
}

template <typename T>
bool readValues(std::istream &in, const char *expectedName, std::vector<T> &values) {
  std::string line;
  if (!std::getline(in, line))
    return false;
  const std::string prefix = std::string(expectedName) + ".count=";
  if (line.find(prefix) != 0)
    return false;
  const auto count =
      static_cast<std::size_t>(std::stoull(line.substr(prefix.size())));
  if (!std::getline(in, line))
    return false;
  const std::string valuePrefix = std::string(expectedName) + "=";
  if (line.find(valuePrefix) != 0)
    return false;
  std::string rest = line.substr(valuePrefix.size());
  values.clear();
  values.reserve(count);
  std::istringstream tokens(rest);
  std::string token;
  for (std::size_t i = 0; i < count; ++i) {
    if (!(tokens >> token))
      return false;
    const auto bits = static_cast<std::uint32_t>(std::stoul(token, nullptr, 16));
    values.push_back(std::bit_cast<T>(bits));
  }
  return true;
}

template <typename T>
bool readPoints(std::istream &in, const char *expectedName,
                std::vector<Vec3D<T>> &points) {
  std::string line;
  if (!std::getline(in, line))
    return false;
  const std::string prefix = std::string(expectedName) + ".count=";
  if (line.find(prefix) != 0)
    return false;
  const auto count =
      static_cast<std::size_t>(std::stoull(line.substr(prefix.size())));
  if (!std::getline(in, line))
    return false;
  const std::string valuePrefix = std::string(expectedName) + "=";
  if (line.find(valuePrefix) != 0)
    return false;
  std::string rest = line.substr(valuePrefix.size());
  points.clear();
  points.reserve(count);
  std::istringstream tokens(rest);
  std::string token;
  for (std::size_t i = 0; i < count; ++i) {
    if (!(tokens >> token))
      return false;
    Vec3D<T> point{};
    for (int j = 0; j < 3; ++j) {
      const auto bits = static_cast<std::uint32_t>(std::stoul(token, nullptr, 16));
      point[j] = std::bit_cast<T>(bits);
      if (j < 2) {
        if (token.size() < 9 || token[8] != ':')
          return false;
        token = token.substr(9);
      }
    }
    points.push_back(point);
  }
  return true;
}

} // namespace detail

template <typename T, int D>
void serializeRecord(const AcceptanceRecord<T, D> &record, std::ostream &out) {
  out << "schema=" << record.kSchemaVersion << '\n';
  out << "route=" << record.routeTag << '\n';
  out << "precision=float\ndimension=" << D << '\n';
  detail::writeScalar(out, "gridDelta", record.gridDelta);
  detail::writeScalar(out, "xExtent", record.xExtent);
  detail::writeScalar(out, "yExtent", record.yExtent);
  detail::writeScalar(out, "processDuration", record.processDuration);
  out << "rngSeed=" << record.rngSeed << '\n';
  out << "raysPerPoint=" << record.raysPerPoint << '\n';
  out << "maxReflections=" << record.maxReflections << '\n';
  detail::writeScalar(out, "initialCoverage", record.initialCoverageValue);

  detail::writeValues(out, "flux", record.fluxCellData);
  detail::writeScalar(out, "flux.sum", record.fluxCellDataSum);

  out << "processResult=" << static_cast<int>(record.processResult) << '\n';

  detail::writeValues(out, "initialCoverages", record.initialCoverages);
  detail::writePoints(out, "finalSurfacePoints", record.finalSurfacePoints);
  detail::writeValues(out, "finalSurfaceMaterialIds", record.finalSurfaceMaterialIds);
  detail::writeValues(out, "finalCoverages", record.finalCoverages);
  out << "topLevelSetValid=" << (record.topLevelSetValid ? "true" : "false")
      << '\n';

  detail::writeScalar(out, "processTime", record.processTime);
  out << "advectionIterationCount=" << record.advectionIterationCount << '\n';
  out << "callbackSequence.count=" << record.callbackSequence.size() << '\n';
  out << "callbackSequence=";
  for (const auto &entry : record.callbackSequence)
    out << entry << ' ';
  out << '\n';
  out.flush();
}

template <typename T, int D>
bool deserializeRecord(AcceptanceRecord<T, D> &record, std::istream &in) {
  std::string line;
  auto readLine = [&](std::istream &stream, std::string &dest) -> bool {
    if (!std::getline(stream, dest))
      return false;
    // Records are written in text mode; strip a trailing CR so CRLF files
    // parse identically to LF files.
    if (!dest.empty() && dest.back() == '\r')
      dest.pop_back();
    return true;
  };
  auto expectPrefix = [&](const std::string &dest,
                          const std::string &prefix) -> bool {
    return dest.find(prefix) == 0;
  };
  auto after = [&](const std::string &dest, const std::string &prefix) {
    return dest.substr(prefix.size());
  };

  if (!readLine(in, line) || !expectPrefix(line, "schema="))
    return false;
  if (std::stoi(after(line, "schema=")) != AcceptanceRecord<T, D>::kSchemaVersion)
    return false;
  if (!readLine(in, line) || !expectPrefix(line, "route="))
    return false;
  record.routeTag = after(line, "route=");
  if (!readLine(in, line) || !expectPrefix(line, "precision=float"))
    return false;
  if (!readLine(in, line) || !expectPrefix(line, "dimension="))
    return false;
  if (!detail::readScalar(in, "gridDelta", record.gridDelta))
    return false;
  if (!detail::readScalar(in, "xExtent", record.xExtent))
    return false;
  if (!detail::readScalar(in, "yExtent", record.yExtent))
    return false;
  if (!detail::readScalar(in, "processDuration", record.processDuration))
    return false;
  if (!readLine(in, line) || !expectPrefix(line, "rngSeed="))
    return false;
  record.rngSeed = static_cast<unsigned>(std::stoul(after(line, "rngSeed=")));
  if (!readLine(in, line) || !expectPrefix(line, "raysPerPoint="))
    return false;
  record.raysPerPoint =
      static_cast<unsigned>(std::stoul(after(line, "raysPerPoint=")));
  if (!readLine(in, line) || !expectPrefix(line, "maxReflections="))
    return false;
  record.maxReflections = std::stoi(after(line, "maxReflections="));
  if (!detail::readScalar(in, "initialCoverage", record.initialCoverageValue))
    return false;

  if (!detail::readValues(in, "flux", record.fluxCellData))
    return false;
  if (!detail::readScalar(in, "flux.sum", record.fluxCellDataSum))
    return false;

  if (!readLine(in, line) || !expectPrefix(line, "processResult="))
    return false;
  record.processResult =
      static_cast<viennaps::ProcessResult>(std::stoi(after(line, "processResult=")));

  if (!detail::readValues(in, "initialCoverages", record.initialCoverages))
    return false;
  if (!detail::readPoints(in, "finalSurfacePoints", record.finalSurfacePoints))
    return false;
  if (!detail::readValues(in, "finalSurfaceMaterialIds",
                          record.finalSurfaceMaterialIds))
    return false;
  if (!detail::readValues(in, "finalCoverages", record.finalCoverages))
    return false;
  if (!readLine(in, line) || !expectPrefix(line, "topLevelSetValid="))
    return false;
  record.topLevelSetValid = after(line, "topLevelSetValid=") == "true";

  if (!detail::readScalar(in, "processTime", record.processTime))
    return false;
  if (!readLine(in, line) || !expectPrefix(line, "advectionIterationCount="))
    return false;
  record.advectionIterationCount =
      static_cast<unsigned>(std::stoul(after(line, "advectionIterationCount=")));
  if (!readLine(in, line) ||
      !expectPrefix(line, "callbackSequence.count="))
    return false;
  const auto callbackCount = static_cast<std::size_t>(
      std::stoull(after(line, "callbackSequence.count=")));
  if (!readLine(in, line) || !expectPrefix(line, "callbackSequence="))
    return false;
  std::string rest = after(line, "callbackSequence=");
  record.callbackSequence.clear();
  record.callbackSequence.reserve(callbackCount);
  std::istringstream tokens(rest);
  std::string token;
  for (std::size_t i = 0; i < callbackCount; ++i) {
    if (!(tokens >> token))
      return false;
    record.callbackSequence.push_back(token);
  }
  return true;
}

// Builds the canonical P5-S0 NeutralTransport parameter block with full
// surface physics enabled: non-unit sticking for multi-bounce re-emission,
// positive desorption, and positive surface diffusion.
template <typename T>
viennaps::NeutralTransportParameters<T> makeAcceptanceParameters() {
  viennaps::NeutralTransportParameters<T> params;
  params.incomingFlux = T(1);
  params.zeroCoverageSticking = T(0.5);
  params.etchFrontSticking = T(0.8);
  params.desorptionRate = T(0.1);
  params.desorptionMaterial = viennaps::Material::Mask;
  params.kEtch = T(2);
  params.surfaceSiteDensity = T(3);
  params.siliconDensity = T(6);
  params.coverageTimeStep = T(0.01);
  params.useSteadyStateCoverage = true;
  params.surfaceDiffusionCoefficient = T(1e-4);
  params.surfaceDiffusionMaterial = viennaps::Material::Si;
  params.surfaceDiffusionSolverTolerance = T(1e-6);
  params.surfaceDiffusionMaxIterations = 500U;
  params.etchFrontMaterial = viennaps::Material::Si;
  params.fluxLabel = "neutralFlux";
  params.coverageLabel = "neutralCoverage";
  return params;
}

// Creates a 2D plane domain and a NeutralTransport model whose surface coverage
// is initialized to a deterministic value.
template <typename T, int D>
std::pair<SmartPointer<viennaps::Domain<T, D>>,
          SmartPointer<viennaps::NeutralTransport<T, D>>>
makeAcceptanceDomainAndModel(const AcceptanceConfig<T> &config) {
  auto domain = viennaps::Domain<T, D>::New(config.gridDelta, config.xExtent,
                                            config.yExtent);
  viennaps::MakePlane<T, D>(domain, T(0)).apply();

  auto params = makeAcceptanceParameters<T>();
  auto model = SmartPointer<viennaps::NeutralTransport<T, D>>::New(params);
  auto surfaceModel = SmartPointer<InitialCoverageNeutralSurfaceModel<T, D>>::New(
      model->getParameters(), config.initialCoverage);
  model->setSurfaceModel(surfaceModel);

  return {domain, model};
}

// Runs the acceptance sequence on an already-configured Process handle.  The
// caller is responsible for any Vulkan composition / CPU flux-engine selection.
template <typename T, int D>
AcceptanceRecord<T, D>
runAcceptance(const std::string &routeTag,
              SmartPointer<viennaps::Domain<T, D>> domain,
              viennaps::Process<T, D> &process,
              SmartPointer<viennaps::ProcessModelBase<T, D>> model,
              const AcceptanceConfig<T> &config,
              std::function<void(viennaps::Process<T, D> &)> preApplyHook =
                  {}) {
  AcceptanceRecord<T, D> record;
  record.routeTag = routeTag;
  record.gridDelta = config.gridDelta;
  record.xExtent = config.xExtent;
  record.yExtent = config.yExtent;
  record.processDuration = config.processDuration;
  record.rngSeed = config.rngSeed;
  record.raysPerPoint = config.raysPerPoint;
  record.maxReflections = config.maxReflections;
  record.initialCoverageValue = config.initialCoverage;

  const auto params = makeAcceptanceParameters<T>();

  viennaps::units::Length::setUnit(viennaps::units::Length::METER);
  viennaps::units::Time::setUnit(viennaps::units::Time::SECOND);

  process.setDomain(domain);
  process.setProcessModel(model);
  process.setProcessDuration(static_cast<double>(config.processDuration));

  viennaps::RayTracingParameters rayParams;
  rayParams.rngSeed = config.rngSeed;
  rayParams.useRandomSeeds = false;
  rayParams.raysPerPoint = config.raysPerPoint;
  rayParams.maxReflections = config.maxReflections;
  process.setParameters(rayParams);

  viennaps::AdvectionParameters advectionParams;
  advectionParams.temporalScheme = viennals::TemporalSchemeEnum::FORWARD_EULER;
  advectionParams.timeStepRatio = 0.4999;
  advectionParams.checkDissipation = true;
  process.setParameters(advectionParams);

  viennaps::CoverageParameters coverageParams;
  coverageParams.maxIterations = config.coverageMaxIterations;
  coverageParams.tolerance = static_cast<double>(config.coverageTolerance);
  process.setParameters(coverageParams);

  viennaps::SurfaceDiffusionParameters diffusionParams;
  diffusionParams.stabilityFactor = T(1);
  process.setParameters(diffusionParams);

  auto callback = SmartPointer<RecordingAdvectionCallback<T, D>>::New();
  model->setAdvectionCallback(callback);

  if (domain != nullptr) {
    domain->enableMetaData(config.metaDataLevel);
  }

  const auto fluxMesh = process.calculateFlux();
  if (fluxMesh != nullptr) {
    const auto *flux = fluxMesh->getCellData().getScalarData(params.fluxLabel);
    if (flux != nullptr) {
      record.fluxCellData = *flux;
      T sum = T(0);
      for (const auto value : record.fluxCellData)
        sum += value;
      record.fluxCellDataSum = sum;
    }

    const auto surfaceModel = model->getSurfaceModel();
    if (surfaceModel != nullptr && surfaceModel->getCoverages() != nullptr) {
      const auto *coverage =
          surfaceModel->getCoverages()->getScalarData(params.coverageLabel);
      if (coverage != nullptr)
        record.initialCoverages = *coverage;
    }
  }

  if (preApplyHook)
    preApplyHook(process);

  process.apply();
  record.processResult = process.getLastProcessResult();

  if (domain != nullptr) {
    if (!domain->getLevelSets().empty()) {
      record.topLevelSetValid =
          domain->getLevelSets().back()->getNumberOfPoints() > 0;
    }

    const auto diskMesh = domain->getDiskMesh();
    if (diskMesh != nullptr) {
      record.finalSurfacePoints = diskMesh->nodes;
      const auto *materialIds = diskMesh->getMaterialIds();
      if (materialIds != nullptr)
        record.finalSurfaceMaterialIds = *materialIds;
    }

    const auto metaData = domain->getMetaData();
    const auto timeIt = metaData.find("ProcessTime");
    if (timeIt != metaData.end() && !timeIt->second.empty()) {
      record.processTime = static_cast<T>(timeIt->second.front());
    }
  }

  const auto surfaceModel = model->getSurfaceModel();
  if (surfaceModel != nullptr && surfaceModel->getCoverages() != nullptr) {
    const auto *coverage =
        surfaceModel->getCoverages()->getScalarData(params.coverageLabel);
    if (coverage != nullptr)
      record.finalCoverages = *coverage;
  }

  record.advectionIterationCount = callback->preCalls();
  record.callbackSequence = callback->sequence();

  return record;
}

// Self-check used by the CPU route.  Failures are surfaced as a non-zero return
// code; error details are written to the provided stream.
template <typename T, int D>
bool selfCheck(const AcceptanceRecord<T, D> &record, std::ostream &errorStream) {
  bool ok = true;

  if (!std::isfinite(record.fluxCellDataSum)) {
    errorStream << "self-check: flux sum is not finite\n";
    ok = false;
  }
  for (const auto value : record.fluxCellData) {
    if (!std::isfinite(value)) {
      errorStream << "self-check: flux cell value is not finite\n";
      ok = false;
      break;
    }
  }

  for (const auto value : record.initialCoverages) {
    if (!std::isfinite(value) || value < T(0) || value > T(1)) {
      errorStream << "self-check: initial coverage out of physical bounds\n";
      ok = false;
      break;
    }
  }
  for (const auto value : record.finalCoverages) {
    if (!std::isfinite(value) || value < T(0) || value > T(1)) {
      errorStream << "self-check: final coverage out of physical bounds\n";
      ok = false;
      break;
    }
  }

  if (record.processResult != viennaps::ProcessResult::SUCCESS) {
    errorStream << "self-check: process did not succeed\n";
    ok = false;
  }

  if (!record.topLevelSetValid) {
    errorStream << "self-check: top level set is invalid\n";
    ok = false;
  }

  if (record.finalSurfacePoints.empty()) {
    errorStream << "self-check: final surface is empty\n";
    ok = false;
  }

  if (record.processTime <= T(0)) {
    errorStream << "self-check: process time did not advance\n";
    ok = false;
  }

  if (record.advectionIterationCount == 0U) {
    errorStream << "self-check: advection callback was never invoked\n";
    ok = false;
  }

  // Conservation residual: total coverage should remain bounded in [0,1] and
  // the final surface must have physically moved from the initial plane.
  if (!record.finalSurfacePoints.empty()) {
    bool anyMoved = false;
    for (const auto &point : record.finalSurfacePoints) {
      if (!std::isfinite(point[0]) || !std::isfinite(point[1]) ||
          !std::isfinite(point[2])) {
        errorStream << "self-check: final surface point is not finite\n";
        ok = false;
        break;
      }
      if (std::abs(point[1]) > T(1e-6)) {
        anyMoved = true;
      }
    }
    if (!anyMoved) {
      errorStream << "self-check: surface did not advance from y=0 plane\n";
      ok = false;
    }
  }

  return ok;
}

} // namespace surface_process_acceptance
