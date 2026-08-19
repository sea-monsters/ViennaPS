// Copyright 2026 ViennaPS
// SPDX-License-Identifier: MIT
//
// P5-MODEL-MATRIX row fallback contract smoke.
//
// This is deliberately a CPU-led contract test.  It does not promote any
// model: only the already evidenced SingleParticleProcess<float, 2> slice is
// eligible, while every other inventory row must stay on CPU in AUTO and fail
// closed for an explicit MANUAL Vulkan request.

#include "vulkan_ray_flux_engine.hpp"

#include "../runtime/deployment_compute_context.hpp"

#include <models/psCF4O2Etching.hpp>
#include <models/psFluorocarbonEtching.hpp>
#include <models/psIonBeamEtching.hpp>
#include <models/psMultiParticleProcess.hpp>
#include <models/psNeutralTransport.hpp>
#include <models/psOxideRegrowth.hpp>
#include <models/psSF6C4F8Etching.hpp>
#include <models/psSF6O2Etching.hpp>
#include <models/psSelectiveEpitaxy.hpp>
#include <models/psSingleParticleALD.hpp>
#include <models/psSingleParticleProcess.hpp>
#include <models/psTEOSDeposition.hpp>
#include <models/psTEOSPECVD.hpp>
#include <models/psWetEtching.hpp>
#include <process/psProcessContext.hpp>
#include <psUnits.hpp>

#include <array>
#include <functional>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

namespace {

using Model = viennaps::SmartPointer<viennaps::ProcessModelBase<float, 2>>;
using Factory = std::function<Model()>;

struct Row {
  const char *name;
  Factory make;
};

bool require(const bool condition, const char *what) {
  if (!condition)
    std::cerr << "model-matrix fallback FAIL: " << what << '\n';
  return condition;
}

viennaps::RayTracingParameters singleBounce() {
  viennaps::RayTracingParameters params;
  params.maxReflections = 0;
  params.rngSeed = 42;
  params.useRandomSeeds = false;
  return params;
}

Model makeMultiParticle() {
  auto model = viennaps::SmartPointer<viennaps::MultiParticleProcess<float, 2>>::New();
  model->addNeutralParticle(1.0F);
  model->addNeutralParticle(1.0F);
  return model;
}

Model makeIonBeam() {
  using Process = viennaps::IonBeamEtching<float, 2>;
  return viennaps::SmartPointer<Process>::New(Process::defaultParameters());
}

Model makeFluorocarbon() {
  viennaps::FluorocarbonParameters<float> params;
  viennaps::FluorocarbonParameters<float>::MaterialParameters material;
  material.id = viennaps::Material::Si;
  params.addMaterial(material);
  return viennaps::SmartPointer<viennaps::FluorocarbonEtching<float, 2>>::New(
      params);
}

Model makeNonFluxCpuRow(const char *name) {
  auto model = viennaps::SmartPointer<viennaps::ProcessModelBase<float, 2>>::New();
  model->setProcessName(name);
  return model;
}

std::vector<Row> fallbackRows() {
  using namespace viennaps;
  return {
      {"MultiParticleProcess", makeMultiParticle},
      {"IonBeamEtching", makeIonBeam},
      {"NeutralTransport", [] { return SmartPointer<NeutralTransport<float, 2>>::New(); }},
      {"CF4O2Etching", [] { return SmartPointer<CF4O2Etching<float, 2>>::New(); }},
      {"SF6O2Etching", [] { return SmartPointer<SF6O2Etching<float, 2>>::New(); }},
      {"SF6C4F8Etching", [] { return SmartPointer<SF6C4F8Etching<float, 2>>::New(); }},
      // The inventory groups the generic PlasmaEtching semantics with this
      // CPU-only fluorocarbon family; there is no standalone PlasmaEtching
      // ProcessModel class to instantiate.
      {"FluorocarbonEtching/PlasmaEtching", makeFluorocarbon},
      {"SingleParticleALD", [] {
         return SmartPointer<SingleParticleALD<float, 2>>::New(
             SingleParticleALDParams{});
       }},
      {"TEOSDeposition", [] {
         return SmartPointer<TEOSDeposition<float, 2>>::New(1.0F, 1.0F, 1.0F);
       }},
      {"TEOSPECVD", [] {
         return SmartPointer<TEOSPECVD<float, 2>>::New(1.0F, 1.0F, 1.0F, 1.0F);
       }},
      {"WetEtching", [] {
         return SmartPointer<WetEtching<float, 2>>::New(
             std::vector<std::pair<Material, float>>{{Material::Si, 1.0F}});
       }},
      {"SelectiveEpitaxy", [] { return SmartPointer<SelectiveEpitaxy<float, 2>>::New(); }},
      {"OxideRegrowth", [] {
         return SmartPointer<OxideRegrowth<float, 2>>::New(
             1.0F, 1.0F, 1.0F, 0.1F, 1.0F, 0.01F, 0.01F, 1.0F, 1.0F,
             1.0F, 1.0F);
       }},
      // Oxidation is a manages-own-physics CPU model, not a FluxEngine model.
      // Keep this row's routing assertion independent of its heavy LS solver
      // template instantiation; the production model remains CPU-authoritative.
      {"Oxidation", [] { return makeNonFluxCpuRow("Oxidation"); }},
  };
}

bool checkRow(const Row &row) {
  auto model = row.make();
  if (!require(model != nullptr, row.name))
    return false;

  auto params = singleBounce();
  viennaps::ProcessContext<float, 2> autoContext;
  autoContext.model = model;
  autoContext.rayTracingParams = params;
  viennaps::VulkanRayFluxSpirvPaths emptyPaths;
  auto deployment = std::make_shared<
      viennaps::vulkan::runtime::DeploymentComputeContext>();
  viennaps::VulkanRayFluxEngine<float, 2> autoEngine(deployment, emptyPaths,
                                                      true);
  // Level-set/analytic rows (WetEtching, SelectiveEpitaxy, OxideRegrowth,
  // and Oxidation) do not use a FluxEngine at all; their CPU ProcessModel is
  // already the AUTO route. Particle rows must pass the CPU engine admission
  // check before any Vulkan initialization is possible.
  const bool autoAccepted =
      !model->useFluxEngine() ||
      autoEngine.checkInput(autoContext) == viennaps::ProcessResult::SUCCESS;
  if (!require(autoAccepted, row.name))
    return false;

  viennaps::ProcessContext<float, 2> manualContext;
  manualContext.model = row.make();
  manualContext.rayTracingParams = params;
  viennaps::VulkanRayFluxEngine<float, 2> manualEngine(deployment, emptyPaths,
                                                        false);
  const auto result = manualEngine.checkInput(manualContext);
  return require(result == viennaps::ProcessResult::INVALID_INPUT, row.name) &&
         require(manualContext.diskMesh == nullptr, row.name);
}

} // namespace

int main() {
  viennacore::Logger::setLogLevel(viennacore::LogLevel::WARNING);
  // Plasma/fluorocarbon constructors validate units while building their CPU
  // particle and surface models.
  viennaps::units::Length::setUnit(viennaps::units::Length::MICROMETER);
  viennaps::units::Time::setUnit(viennaps::units::Time::SECOND);

  // Strict row remains the only eligible model/precision/dimension slice.
  viennaps::ProcessContext<float, 2> strictContext;
  strictContext.model =
      viennaps::SmartPointer<viennaps::SingleParticleProcess<float, 2>>::New();
  strictContext.rayTracingParams = singleBounce();
  viennaps::VulkanRayFluxSpirvPaths emptyPaths;
  auto deployment = std::make_shared<
      viennaps::vulkan::runtime::DeploymentComputeContext>();
  viennaps::VulkanRayFluxEngine<float, 2> strictEngine(deployment, emptyPaths,
                                                        false);
  if (!require(strictEngine.checkInput(strictContext) ==
                   viennaps::ProcessResult::SUCCESS,
               "strict SingleParticleProcess<float,2> remains eligible"))
    return 1;

  const auto rows = fallbackRows();
  if (!require(rows.size() == 14U, "14 non-eligible inventory rows"))
    return 1;
  for (const auto &row : rows)
    if (!checkRow(row))
      return 1;

  std::cout << "model-matrix fallback rows PASS (15 inventory rows; 1 strict, "
                "14 CPU fallback/unsupported)\n";
  return 0;
}
