// Copyright 2026 ViennaPS
// SPDX-License-Identifier: MIT
//
// P5-TEOS-PROCESS-ADAPTER.  This smoke exercises the production
// single-precursor TEOS surface model with an opt-in numeric executor.  CPU
// particle transport, sticking, coverage updates and Process ordering remain
// unchanged; callback failure must publish the exact CPU geometry.

#include "teos_velocity_executor.hpp"

#include <geometries/psMakePlane.hpp>
#include <models/psTEOSDeposition.hpp>
#include <process/psProcess.hpp>

#include <algorithm>
#include <atomic>
#include <bit>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#ifndef VIENNAPS_VULKAN_TEOS_VELOCITY_SPV_PATH
#define VIENNAPS_VULKAN_TEOS_VELOCITY_SPV_PATH ""
#endif

namespace {

using T = float;
constexpr int Dimension = 2;
using Model = viennaps::TEOSDeposition<T, Dimension>;
using Work = viennaps::TEOSVelocityWork<T>;
using Executor = viennaps::TEOSVelocityExecutor<T>;

std::vector<T> snapshot(
    const viennacore::SmartPointer<viennaps::Domain<T, Dimension>> &domain) {
  const auto &surface = domain->getSurface()->getDomain();
  std::vector<T> values;
  for (unsigned index = 0U; index < surface.getNumberOfSegments(); ++index) {
    const auto &segment = surface.getDomainSegment(index);
    values.insert(values.end(), segment.definedValues.begin(),
                  segment.definedValues.end());
  }
  return values;
}

viennacore::SmartPointer<viennaps::Domain<T, Dimension>> makeDomain() {
  auto domain = viennaps::Domain<T, Dimension>::New(1.0F, 4.0F, 4.0F);
  viennaps::MakePlane<T, Dimension>(domain, 0.0F).apply();
  return domain;
}

void configureDeterministic(viennaps::Process<T, Dimension> &process) {
  viennaps::RayTracingParameters parameters;
  parameters.raysPerPoint = 8U;
  parameters.useRandomSeeds = false;
  parameters.rngSeed = 42U;
  parameters.maxReflections = 0U;
  process.setParameters(parameters);
}

std::vector<T> runCpu() {
  auto domain = makeDomain();
  auto model = viennacore::SmartPointer<Model>::New(1.0F, 0.75F, 0.5F);
  viennaps::Process<T, Dimension> process(domain, model, 0.02F);
  configureDeterministic(process);
  process.apply();
  if (process.getLastProcessResult() != viennaps::ProcessResult::SUCCESS)
    throw std::runtime_error("CPU TEOS Process failed");
  return snapshot(domain);
}

struct RunResult {
  std::vector<T> values;
  std::size_t calls = 0U;
  std::size_t accepted = 0U;
  std::size_t rejected = 0U;
};

RunResult runFallback() {
  auto domain = makeDomain();
  auto model = viennacore::SmartPointer<Model>::New(1.0F, 0.75F, 0.5F);
  std::atomic<std::size_t> calls{0U};
  model->setVelocityExecutor([&calls](Work &, std::string &) {
    calls.fetch_add(1U, std::memory_order_relaxed);
    return false;
  });
  viennaps::Process<T, Dimension> process(domain, model, 0.02F);
  configureDeterministic(process);
  process.apply();
  if (process.getLastProcessResult() != viennaps::ProcessResult::SUCCESS)
    throw std::runtime_error("CPU fallback TEOS Process failed");
  const auto count = calls.load(std::memory_order_relaxed);
  return {snapshot(domain), count, 0U, count};
}

RunResult runVulkan(const std::string &spirvPath) {
  auto bridge = std::make_shared<
      viennaps::vulkan::levelset::VulkanTEOSVelocityExecutor>();
  std::string error;
  if (!bridge->initialize(spirvPath, error))
    throw std::runtime_error("TEOS Vulkan executor initialization failed: " +
                             error);
  auto domain = makeDomain();
  auto model = viennacore::SmartPointer<Model>::New(1.0F, 0.75F, 0.5F);
  const Executor deviceExecutor = bridge->makeExecutor();
  std::atomic<std::size_t> calls{0U};
  std::atomic<std::size_t> accepted{0U};
  std::atomic<std::size_t> rejected{0U};
  Executor guarded = [deviceExecutor, &calls, &accepted,
                      &rejected](Work &work, std::string &callbackError) {
    calls.fetch_add(1U, std::memory_order_relaxed);
    const bool ok = deviceExecutor && deviceExecutor(work, callbackError);
    if (ok && work.complete && work.writtenCount == work.output.size())
      accepted.fetch_add(1U, std::memory_order_relaxed);
    else
      rejected.fetch_add(1U, std::memory_order_relaxed);
    return ok;
  };
  model->setVelocityExecutor(std::move(guarded));
  if (!model->hasVelocityExecutor())
    throw std::runtime_error("TEOS executor was not retained");
  viennaps::Process<T, Dimension> process(domain, model, 0.02F);
  configureDeterministic(process);
  process.apply();
  if (process.getLastProcessResult() != viennaps::ProcessResult::SUCCESS)
    throw std::runtime_error("Vulkan TEOS Process failed");
  return {snapshot(domain), calls.load(std::memory_order_relaxed),
          accepted.load(std::memory_order_relaxed),
          rejected.load(std::memory_order_relaxed)};
}

std::uint32_t orderedBits(const T value) {
  const auto raw = std::bit_cast<std::uint32_t>(value);
  return (raw & 0x80000000U) != 0U ? ~raw : raw ^ 0x80000000U;
}

std::uint32_t ulpDistance(const T lhs, const T rhs) {
  const auto a = orderedBits(lhs);
  const auto b = orderedBits(rhs);
  return a >= b ? a - b : b - a;
}

} // namespace

int main() {
  try {
    const std::string spirvPath = VIENNAPS_VULKAN_TEOS_VELOCITY_SPV_PATH;
    if (spirvPath.empty()) {
      std::cerr << "TEOS Process adapter SKIP: no SPIR-V path\n";
      return 2;
    }
    const auto cpu = runCpu();
    const auto fallback = runFallback();
    const auto vulkan = runVulkan(spirvPath);
    if (cpu.size() != fallback.values.size() ||
        cpu.size() != vulkan.values.size() || fallback.calls == 0U ||
        fallback.rejected != fallback.calls || vulkan.calls == 0U ||
        vulkan.accepted == 0U || vulkan.rejected != 0U) {
      std::cerr << "TEOS Process adapter FAIL: incomplete executor use\n";
      return 1;
    }
    std::uint32_t maxUlp = 0U;
    std::uint32_t fallbackUlp = 0U;
    for (std::size_t index = 0U; index < cpu.size(); ++index) {
      if (!std::isfinite(cpu[index]) || !std::isfinite(fallback.values[index]) ||
          !std::isfinite(vulkan.values[index])) {
        std::cerr << "TEOS Process adapter FAIL: non-finite geometry\n";
        return 1;
      }
      fallbackUlp = std::max(fallbackUlp,
                             ulpDistance(cpu[index], fallback.values[index]));
      maxUlp = std::max(maxUlp,
                        ulpDistance(cpu[index], vulkan.values[index]));
    }
    if (fallbackUlp != 0U || maxUlp > 32U) {
      std::cerr << "TEOS Process adapter FAIL: geometry ULP=" << maxUlp
                << ", fallbackUlp=" << fallbackUlp << '\n';
      return 1;
    }
    std::cout << "TEOS Process adapter PASS (maxGeometryUlp=" << maxUlp
              << ", calls=" << vulkan.calls << ", accepted=" << vulkan.accepted
              << ", fallbackExactUlp=" << fallbackUlp << ")\n";
    return 0;
  } catch (const std::exception &exception) {
    std::cerr << "TEOS Process adapter FAIL: " << exception.what() << '\n';
    return 1;
  }
}
