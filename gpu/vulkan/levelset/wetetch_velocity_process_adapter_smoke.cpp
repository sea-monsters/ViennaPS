// Copyright 2026 ViennaPS
// SPDX-License-Identifier: MIT
//
// P5-WETETCH-PROCESS-ADAPTER.  This smoke exercises the production
// WetEtching model's opt-in numeric executor.  CPU formula evaluation remains
// inside the model; the Vulkan callback is only accepted after its independent
// CPU-oracle/status transaction succeeds.  Process, TranslationField,
// AnalyticProcessStrategy and LevelSet publication are otherwise unchanged.

#include "wetetch_velocity_executor.hpp"

#include <geometries/psMakePlane.hpp>
#include <models/psWetEtching.hpp>
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

#ifndef VIENNAPS_VULKAN_WETETCH_VELOCITY_SPV_PATH
#define VIENNAPS_VULKAN_WETETCH_VELOCITY_SPV_PATH ""
#endif

namespace {

using NumericType = float;
constexpr int Dimension = 2;
using Model = viennaps::WetEtching<NumericType, Dimension>;
using Work = viennaps::WetEtchVelocityWork<NumericType>;
using Executor = viennaps::WetEtchVelocityExecutor<NumericType>;

struct RunResult {
  std::vector<NumericType> values;
  std::size_t calls = 0U;
  std::size_t accepted = 0U;
  std::size_t rejected = 0U;
};

std::vector<NumericType>
snapshot(const viennacore::SmartPointer<viennaps::Domain<NumericType, Dimension>>
             &domain) {
  const auto &surface = domain->getSurface()->getDomain();
  std::vector<NumericType> values;
  for (unsigned index = 0U; index < surface.getNumberOfSegments(); ++index) {
    const auto &segment = surface.getDomainSegment(index);
    values.insert(values.end(), segment.definedValues.begin(),
                  segment.definedValues.end());
  }
  return values;
}

viennacore::SmartPointer<viennaps::Domain<NumericType, Dimension>>
makeDomain() {
  auto domain = viennaps::Domain<NumericType, Dimension>::New(1.0F, 4.0F,
                                                               4.0F);
  viennaps::MakePlane<NumericType, Dimension>(domain, 0.0F).apply();
  return domain;
}

RunResult runCpu() {
  auto domain = makeDomain();
  auto model = viennacore::SmartPointer<Model>::New(
      std::vector<std::pair<viennaps::Material, NumericType>>{
          {viennaps::Material::Si, 1.0F}});
  viennaps::Process<NumericType, Dimension> process(domain, model, 0.02);
  process.apply();
  if (process.getLastProcessResult() != viennaps::ProcessResult::SUCCESS)
    throw std::runtime_error("CPU WetEtching Process failed");
  return {snapshot(domain)};
}

RunResult runFallback() {
  auto domain = makeDomain();
  auto model = viennacore::SmartPointer<Model>::New(
      std::vector<std::pair<viennaps::Material, NumericType>>{
          {viennaps::Material::Si, 1.0F}});
  std::atomic<std::size_t> calls{0U};
  model->setVelocityExecutor([&calls](Work &, std::string &) {
    calls.fetch_add(1U, std::memory_order_relaxed);
    return false;
  });
  viennaps::Process<NumericType, Dimension> process(domain, model, 0.02);
  process.apply();
  if (process.getLastProcessResult() != viennaps::ProcessResult::SUCCESS)
    throw std::runtime_error("CPU fallback WetEtching Process failed");
  const auto callCount = calls.load(std::memory_order_relaxed);
  return {snapshot(domain), callCount, 0U, callCount};
}

RunResult runVulkan(const std::string &spirvPath) {
  auto bridge = std::make_shared<
      viennaps::vulkan::levelset::VulkanWetEtchVelocityExecutor>();
  std::string error;
  if (!bridge->initialize(spirvPath, error))
    throw std::runtime_error("WetEtching Vulkan executor initialization failed: " +
                             error);

  auto domain = makeDomain();
  auto model = viennacore::SmartPointer<Model>::New(
      std::vector<std::pair<viennaps::Material, NumericType>>{
          {viennaps::Material::Si, 1.0F}});
  const Executor deviceExecutor = bridge->makeExecutor();
  std::atomic<std::size_t> calls{0U};
  std::atomic<std::size_t> accepted{0U};
  std::atomic<std::size_t> rejected{0U};
  Executor guardedExecutor =
      [deviceExecutor, &calls, &accepted,
       &rejected](Work &work, std::string &callbackError) {
        calls.fetch_add(1U, std::memory_order_relaxed);
        const bool ok = deviceExecutor && deviceExecutor(work, callbackError);
        if (ok && work.complete && work.writtenCount == work.output.size())
          accepted.fetch_add(1U, std::memory_order_relaxed);
        else
          rejected.fetch_add(1U, std::memory_order_relaxed);
        return ok;
      };
  model->setVelocityExecutor(std::move(guardedExecutor));
  if (!model->hasVelocityExecutor())
    throw std::runtime_error("WetEtching executor was not retained");

  viennaps::Process<NumericType, Dimension> process(domain, model, 0.02);
  process.apply();
  if (process.getLastProcessResult() != viennaps::ProcessResult::SUCCESS)
    throw std::runtime_error("Vulkan WetEtching Process failed");
  return {snapshot(domain), calls.load(std::memory_order_relaxed),
          accepted.load(std::memory_order_relaxed),
          rejected.load(std::memory_order_relaxed)};
}

std::uint32_t orderedBits(const NumericType value) {
  const auto bits = std::bit_cast<std::uint32_t>(value);
  return (bits & 0x80000000U) != 0U ? ~bits : bits ^ 0x80000000U;
}

std::uint32_t ulpDistance(const NumericType left, const NumericType right) {
  const auto a = orderedBits(left);
  const auto b = orderedBits(right);
  return a >= b ? a - b : b - a;
}

} // namespace

int main() {
  try {
    const std::string spirvPath = VIENNAPS_VULKAN_WETETCH_VELOCITY_SPV_PATH;
    if (spirvPath.empty()) {
      std::cerr << "wet-etch Process adapter SKIP: no SPIR-V path\n";
      return 2;
    }
    const auto cpu = runCpu();
    const auto fallback = runFallback();
    const auto vulkan = runVulkan(spirvPath);
    if (cpu.values.size() != fallback.values.size() ||
        cpu.values.size() != vulkan.values.size() || fallback.calls == 0U ||
        fallback.rejected != fallback.calls || vulkan.calls == 0U ||
        vulkan.accepted == 0U || vulkan.rejected != 0U) {
      std::cerr << "wet-etch Process adapter FAIL: incomplete executor use\n";
      return 1;
    }
    std::uint32_t maxUlp = 0U;
    std::uint32_t fallbackUlp = 0U;
    for (std::size_t index = 0U; index < cpu.values.size(); ++index) {
      if (!std::isfinite(cpu.values[index]) ||
          !std::isfinite(fallback.values[index]) ||
          !std::isfinite(vulkan.values[index])) {
        std::cerr << "wet-etch Process adapter FAIL: non-finite geometry\n";
        return 1;
      }
      fallbackUlp = std::max(fallbackUlp,
                             ulpDistance(cpu.values[index],
                                         fallback.values[index]));
      maxUlp = std::max(maxUlp, ulpDistance(cpu.values[index],
                                             vulkan.values[index]));
    }
    if (fallbackUlp != 0U || maxUlp > 1U) {
      std::cerr << "wet-etch Process adapter FAIL: geometry ULP=" << maxUlp
                << ", fallbackUlp=" << fallbackUlp << '\n';
      return 1;
    }
    std::cout << "wet-etch Process adapter PASS (maxGeometryUlp=" << maxUlp
              << ", calls=" << vulkan.calls
              << ", accepted=" << vulkan.accepted
              << ", fallbackExactUlp=" << fallbackUlp << ")\n";
    return 0;
  } catch (const std::exception &exception) {
    std::cerr << "wet-etch Process adapter FAIL: " << exception.what() << '\n';
    return 1;
  }
}
