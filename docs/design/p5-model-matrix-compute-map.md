# P5 Model-Matrix Compute Map

Status: design/inventory evidence only (`P5-MODEL-MATRIX-COMPUTE-MAP`,
2026-08-12). This document does not change routing, eligibility, or support
claims. It is a read-only comparison against the unmodified CPU tree at
`D:\Codex_lib\code_reference\ViennaPS` and the tracked model inventory.

## Boundary and common CPU call graph

The P5 contract is a compute substitution boundary. The CPU model remains the
definition of model parameters, particle labels, source sampling, collision,
reflection/roulette, coverages, material semantics, callbacks, and level-set
ordering. A Vulkan implementation may replace a named numeric operation only
after a row-specific CPU differential has been accepted.

The reference orchestration is the following:

```
Process::calculateFlux/apply
  -> FluxProcessStrategy::calculateFlux
     -> ray engine (particle source + surfaceCollision/surfaceReflection)
     -> SurfaceModel::updateCoverages
        -> optional getDiffusionCoefficients()/surface diffusion
     -> SurfaceModel::calculateVelocities
     -> optional AdvectionCallback pre/post hooks
     -> level-set advection and mesh/material update
```

The anchors are `process/psProcess.hpp:97,128`,
`process/psFluxProcessStrategy.hpp:54,323-367,400-432,496-550`, and
`process/psProcessModel.hpp:47-48,70-72`. `psALPStrategy.hpp:126-279,351-438`
is the alternate strategy with the same surface/diffusion seam. A model with
`managesOwnPhysics()==true` bypasses that generic chain through
`process/psOxidationStrategy.hpp:13-18`; it is not a normal ray row.

For the map below:

- **R (ray)** means source sampling, triangle hits, local labels, reflection,
  energy/roulette, and event ordering.
- **C (coverage/surface)** means `initialize*`, `updateCoverages`, desorption,
  and `calculateVelocities`.
- **D (diffusion)** means a model-local diffusion solver or the generic surface
  diffusion contract (`getDiffusionCoefficients`).
- **L (level set)** means the CPU velocity field, advection, material map, and
  geometry transaction.
- **M (managed)** means an advection callback or a model-owned PDE/geometry
  loop rather than the generic Process strategy.

Every row in this document is still **CPU fallback** for a complete model;
the separately accepted WetEtching, SelectiveEpitaxy, and single-precursor
TEOS entries are bounded numeric substages, not complete-model promotion:
Auto must execute the linked CPU model; an explicit Manual Vulkan request must
fail closed before publishing flux, metadata, velocity, or geometry. CUDA
`getGPUModel()` and CUDA callable tables are implementation evidence for CUDA
only, never Vulkan eligibility.

## Candidate compute map

The line references below point to the unmodified reference tree. They are
anchors for the CPU oracle, not permission to modify that tree.

| Rank | CPU family and call graph (R/C/D/L/M) | Existing numeric seam | Missing Vulkan kernel/contract | CPU semantics that must remain authoritative | First executable candidate and oracle |
|---|---|---|---|---|---|
| S | **SingleParticleProcess<float,2> (baseline strict row)** — R: `impl::SingleParticle::surfaceCollision/surfaceReflection` (`psSingleParticleProcess.hpp:44-72`); C: `SingleParticleSurfaceModel::calculateVelocities` (`:15-42`); L: default Process. | `VulkanRayFluxEngine` triangle hit/compaction/reduction and the narrow Process route are locally evidenced for one `particleFlux` label, default/equivalent source, and `maxReflections==0`. | Multi-bounce event/reflection/roulette and broad model admission remain absent; CPU surface integration is still the semantic owner. | `stickingProbability_`, `sourceDistributionPower_`, `materialRates_`, label ordering, CPU source normalization, rollback, and default level-set ordering. | Existing fixed-seed CPU/Vulkan fixture and Intel Arc Process route are the baseline oracle (`20,000/20,000` hits; current narrow route reports `maxFluxUlp=0`, `totalFluxRelDiff=0`, `maxGeometryDelta=0`). This row does not promote any candidate below it. |
| 1 | **NeutralTransport** — R: `NeutralTransportParticle::surfaceCollision/surfaceReflection` (`psNeutralTransport.hpp:559-598`); C: `NeutralTransportSurfaceModel::updateCoverages/calculateVelocities/getDesorptionWeights` (`:188-291`); D: `buildDiffusionGraph/applySurfaceDiffusion` (`:367-538`); L: `DefaultVelocityField`; generic Process chain. | `NeutralTransportVelocityExecutor` is an explicit FP32 compute bridge in the Mod tree. It is only the velocity substage. | No Vulkan ballistic event queue, coverage/desorption state machine, material-dependent sticking, or graph/implicit diffusion route. A velocity executor cannot own the ray route. | Coverage clamp and steady-state/time-step equations, desorption labels, material IDs, surface-data lifetime, diffuse reflection, and Process ordering. | **Only feasible first substage, not a support row:** `P5-NEUTRAL-VELOCITY-SUBSTAGE` after surface integration. Oracle: raw-bit velocity equality for accepted N2 inputs, then CPU full-process flux/coverage/desorption/diffusion and final geometry/conservation. Release paired CPU oracle remains an evidence gap (`0xC0000005`), so no row unlock yet. |
| 2 | **MultiParticleProcess** — R: `IonParticle` collision/reflection/energy (`psMultiParticleProcess.hpp:67-167`) plus `DiffuseParticle` (`:170-208`); C: `MultiParticleSurfaceModel::calculateVelocities` (`:26-64`); L: default Process advection. | CUDA `gpu::MultiParticleProcess` has callable slots (`:211-464`), but no Vulkan species seam. | Per-species ray labels, arbitrary ion count, energy threshold/distribution, angle-dependent sticking, neutral material sticking, custom `rateFunction_`, and event/roulette state. | Particle insertion order and labels (`addIonParticle/addNeutralParticle`, `:526-572`), CPU callbacks, material mapping, and user rate function. | No complete row is presently executable. If a new Vulkan species seam is approved, start with one ion + one neutral and fixed labels. Oracle: per-label flux sums, total weight conservation, CPU velocity, and final geometry for one-ion, multi-ion, and mixed-neutral fixtures. |
| 3 | **IonBeamEtching** — R: `IBEIonWithRedeposition::surfaceCollision/surfaceReflection/initNew` (`psIonBeamEtching.hpp:82-244`); C: `IBESurfaceModel::calculateVelocities` (`:23-80`); L: default Process. | CUDA `gpu::IonBeamEtching` exists (`:268-350`); it is not a Vulkan seam. | Energy/angle-dependent yield, reflection threshold, redeposition weight propagation, two output labels (`ionFlux`, `redepositionFlux`), and material-plane masking. | `IBEParameters`, `cos4Yield/yieldFunction`, redeposition threshold/rate, rotating-wafer source, and callback ordering. | No complete row. First candidate would be zero-redeposition, single fixed-energy ion only after a Vulkan energy/reflection contract. Oracle: ion/redeposition mass balance, per-material velocity, and 2-D/3-D etched geometry. |
| 4 | **CF4O2Etching** — R: `CF4O2Ion`, `CF4O2Etchant`, `CF4O2Oxygen`, `CF4O2Polymer` callbacks (`psCF4O2Etching.hpp:223-525`); C: `CF4O2SurfaceModel::updateCoverages/calculateVelocities` (`:19-221`); L: default Process. | No Vulkan seam; CPU constructor inserts four species (`:527-596`). | Four-label transport, ion energy/yields, oxygen/polymer sticking, coupled three-coverage algebra, etch-stop and material-dependent chemistry. | Coverage equations (`eCoverage/oCoverage/cCoverage`), Si/SiGe/Mask densities and rates, units, labels, and stop-depth transaction. | No complete row. Smallest future row is ion + etchant with oxygen/polymer disabled, but it still needs a model-specific Vulkan ray and surface kernel. Oracle: all active labels, coverage bounds, species/flux conservation, per-material velocity, and geometry. |
| 5 | **SF6O2Etching** — CPU constructs `PlasmaEtchingIon` and two `PlasmaEtchingNeutral` particles (`psSF6O2Etching.hpp:238-266`), sharing `PlasmaEtchingSurfaceModel` from `psPlasmaEtching.hpp:19-206`; L: default Process. | CUDA `gpu::SF6O2Etching` (`psSF6O2Etching.hpp:21-120`) only. | Ion sputter/ion-enhanced/passivation yields, angle/energy reflection, etchant/passivation coverage coupling, and material/rate-factor semantics. | CPU parameter defaults, `ionFlux/etchantFlux/passivationFlux` labels, coverage branch handling for zero species, and stop depth. | No complete row. Candidate ordering follows the simpler zero-passivation SF6C4F8 row, but no Vulkan support is implied. Oracle: three labels, per-label conservation, coverage `[0,1]`, velocity, and final geometry. |
| 6 | **SF6C4F8Etching** — CPU constructs `PlasmaEtchingIon` + `PlasmaEtchingNeutral("etchantFlux")` (`psSF6C4F8Etching.hpp:227-250`); C: shared `PlasmaEtchingSurfaceModel`; L: default Process. | CUDA `gpu::SF6C4F8Etching` (`:21-116`) only. | Ion/etchant transport, polymer-material sputter/deposition response, angle/energy reflection, and the no-passivation branch as an explicit invariant. | Polymer density/yield, beta maps, `passivationFlux==0`, units, stop-depth, and CPU callback order. | **Lowest-complexity plasma candidate after a real ray seam**, but not currently executable. Oracle: ion/etchant labels, polymer mass balance, per-material velocity, non-negative deposition/etch, and geometry differential. |
| 7 | **FluorocarbonEtching / PlasmaEtching** — R: `FluorocarbonIon/FluorocarbonNeutral` (`psFluorocarbonEtching.hpp:344-483`) and `PlasmaEtchingIon/PlasmaEtchingNeutral` (`psPlasmaEtching.hpp:208-393`); C: `FluorocarbonSurfaceModel::calculateVelocities/updateCoverages` (`psFluorocarbonEtching.hpp:128-343`) or `PlasmaEtchingSurfaceModel::updateCoverages/calculateVelocities` (`psPlasmaEtching.hpp:19-206`); L: default Process. | No Vulkan transport or surface bridge; SF6 wrapper CUDA tables do not cover these CPU models generically. | Ion enhanced/sputter/chemical labels, two/three coverages, polymer deposition-versus-etch branch, Arrhenius chemistry, reflection/energy yields, material rates, and etch-stop. | Material-specific parameters, zero-flux branch behavior, coverage epsilon/NaN guards, unit conversion, labels (including `F_ev`), and CPU callback order. | No complete row. A single-species/zero-reflection proof must freeze which concrete family is being tested; disabling polymer/passivation is a different model contract. Oracle: active labels, coverage bounds, mass/flux conservation, velocity, and geometry for each concrete family. |
| 8 | **SingleParticleALD** — R: `SingleParticleALDParticle` (`psSingleParticleALD.hpp:159-202`); C: `SingleParticleALDSurfaceModel::updateCoverages/updateCoveragesFromDesorption/calculateVelocities` (`:38-157`); D: generic `getDiffusionCoefficients` (`:132-137`); L: default Process. | CUDA model is ballistic-only and explicitly changes positive mean free path to a warning/fallback (`:204-252`). | Coverage-dependent sticking, evaporation/desorption flux, re-adsorption, time-step scaling, and coverage diffusion; gas mean-free-path behavior must not be silently changed. | ALD cycle/time-step equations, clamp, desorption weights, coverage label, and generic diffusion timing. | No complete row. Smallest future row is ballistic, no-evaporation, no-diffusion, fixed coverage, but it still needs a Vulkan ray route. Oracle: cycle-by-cycle particle flux, coverage bounds, adsorption/desorption balance, and geometry. |
| 9 | **TEOSDeposition** — R: `SingleTEOSParticle`/`MultiTEOSParticle` (`psTEOSDeposition.hpp:96-182`); C: `SingleTEOSSurfaceModel` and `MultiTEOSSurfaceModel` (`:13-94`); L: default Process. | Opt-in `TEOSDeposition::setVelocityExecutor` for the single-precursor scalar reaction-power batch; no Vulkan transport seam. | Coverage-dependent reaction-order sticking, one/two precursor labels, multi-precursor ordering, and broad reaction-domain handling remain CPU-owned. | Exact reaction-order edge cases (`<1`, `==1`, `>1`), precursor labels, nonnegative deposition, and CPU surface model. | **Narrow row accepted** for `float,D=2`, one `SingleTEOSParticle`, nonnegative flux, one label, and finite nonnegative rate/order. The executor dispatches only `rate * pow(flux, order)` after CPU flux; paired CPU Process raw bits, Intel Arc geometry/fallback, and malformed/reset sentinels pass. This is not full TEOS support. |
| 10 | **TEOSPECVD** — R: `Ion` plus CPU `DiffuseParticle` radical (`psTEOSPECVD.hpp:50-96,162-185`); C: `PECVDSurfaceModel::calculateVelocities` (`:13-48`); L: default Process. | CUDA `gpu::TEOSPECVD` and callable map (`:99-159`) only. | Radical/ion transport, ion angle reflection, two flux labels, independent reaction orders, and CPU radical sticking semantics. | Radical/ion insertion order, `ionMinAngle`, reaction powers, labels, and CPU collision/reflection callbacks. | No complete row. Candidate starts with radical-only, no ion reflection, if a transport seam exists. Oracle: radical/ion labels, coupled flux conservation, velocity, and geometry. |
| 11 | **WetEtching** — R/C remain CPU model logic; `WetEtchingVelocityField::getScalarVelocity` computes crystal-direction rate from normals (`psWetEtching.hpp:14-92`); L is the generic level-set advection path. | Opt-in `WetEtching::setVelocityExecutor` is a narrow Vulkan velocity seam; no generic stage/profile binding exists. | D=3, multiple materials/species, non-analytic callbacks, automatic deployment policy, broader surface physics and aggregate model semantics remain absent. | `r100/r110/r111/r311`, direction construction, material-rate map, CPU-first fallback, and zero velocity for non-etching materials. | **Narrow production row accepted** for `WetEtching<float,2>`, one Si rate and the existing analytic Process fixture. Paired CPU Process raw-exact plus Intel Arc geometry/fallback evidence is recorded in [`p5-wetetch-process-adapter.md`](p5-wetetch-process-adapter.md); this does not promote the general family. |
| 12 | **SelectiveEpitaxy** — C is the default empty surface model; `EpitaxyVelocityField::getScalarVelocity` (`psSelectiveEpitaxy.hpp:12-49`); M/L: `initialize` builds mask via BooleanOperation and stencil (`:84-131`), `finalize` restores domain (`:135-141`). | Opt-in `SelectiveEpitaxyVelocityExecutor` plus Vulkan scalar kernel; no Vulkan Boolean/stencil/domain seam. | Multi-level-set mask construction, Boolean complements, stencil preparation/finalization, material gating, and domain rollback remain CPU-owned. | `domainCopy`, top-material validation, at-least-two-level-set precondition, restored topology, custom materials and D=3. | **Narrow row accepted** for a float/D=2 two-built-in-rate analytic fixture. Paired CPU Process raw-exact, CPU fallback exact, and Intel Arc geometry/callback evidence are recorded in `p5-selective-epitaxy-velocity-substage.md`; this is not complete SelectiveEpitaxy support. |
| 13 | **OxideRegrowth** — L: `SelectiveEtchingVelocityField` (`psOxideRegrowth.hpp:16-39`); M: `ByproductDynamics::applyPreAdvect/applyPostAdvect` (`:98-199`), `diffuseByproducts` (`:201-306`); generic Process advection callback. | No Vulkan callback, dense-cell-set, or redeposition seam. | Disk-mesh sampling, neighbor transport, redeposition advection, diffusion, sink/convection, filling fractions, and event order. | Callback timing, GAS/material filtering, `byproductSum`, nonnegative concentrations, threshold/time interval, and level-set transaction. | No complete row. The velocity field alone is insufficient. Oracle: callback event order, byproduct mass balance, nonnegative concentration, redeposition labels, and final geometry. |
| 14 | **Oxidation** — M: `managesOwnPhysics/applyModel` (`psOxidation.hpp:55-166`), `run` creates/normalizes level sets and calls `ls::Oxidation::applyCFLLimited` (`:532-750`); CPU diffusion/mechanics/contact coupling; no R/C. | ViennaLS exposes a `GpuMode`/BiCGSTAB path (`psOxidation.hpp:323-328,659-710`), but it is not a Vulkan ViennaPS seam and may require FP64. | Oxidant diffusion, stress-coupled mechanics, contact/mask bending, CFL substeps, convergence/rollback, and native-oxide/domain creation. | Deal-Grove rates, orientation, material-index selection, solve bounds, coupling tolerances, and the whole managed loop. | No P5 row. This is a P6-class managed solver candidate only after an explicit Vulkan ViennaLS contract. Oracle: CPU residual/convergence history, Deal-Grove thickness, field/material differential, and final geometry; devices without required precision remain CPU per stage. |

The `S` baseline plus ranks 1--14 remain the inventory identities. Ranks 9,
11, and 12 contain separately accepted narrow numeric subrows; ranks 2--8,
10, and 13 now also have explicit CPU/fallback differentials for bounded
configurations. Rank 14 (`Oxidation`) is deliberately deferred to the P6
managed-solver phase. The broader numbered families remain candidates and are
deliberately not eligible merely because a CUDA class, `getGPUModel()`, a
surface callback, or a generic executor exists.

## Ordered implementation recommendation

1. **Do not widen the current ray predicate.** Keep
   `SingleParticleProcess<float,2>`/one label/zero reflection as the only
   complete Vulkan ray row until a row-specific CPU oracle closes.
2. **Close `NeutralTransportVelocityExecutor` as a compute-only substage**
   (rank 1) only after the Release paired CPU oracle and shared surface
   integration are available. This unlocks a reusable numeric operation, not
   the `NeutralTransport` model row.
3. **Keep rank 11 narrow.** The accepted WetEtching adapter is a low-coupling
   non-ray row with an opt-in callback and CPU-first fallback. Keep domain
   transforms, generic deployment policy, and all broader WetEtching geometry
   semantics on CPU.
4. **Keep rank 12 narrow.** The accepted SelectiveEpitaxy adapter replaces only
   the scalar velocity candidate for one FP32/D=2 analytic fixture. Mask
   BooleanOperations, stencil setup/finalization and domain restoration stay
   CPU-owned; no Vulkan Boolean/stencil support is implied.
5. **Keep rank 9 narrow.** The accepted TEOS adapter replaces only the
   single-precursor `pow` batch after CPU ray/coverage work; multi-precursor
   labels, sticking, and ordering remain CPU-owned.
6. **For a future full ray row, prefer the least semantic surface coupling**:
   zero-passivation SF6C4F8 (rank 6), but only after adding and validating its
   missing transport seam.
7. **Defer MultiParticle, IonBeam, CF4O2, Fluorocarbon/Plasma, ALD, and
   TEOSPECVD** until device event/reflection/roulette/material state exists.
   Defer OxideRegrowth and Oxidation until their callback or managed PDE
   contracts have an explicit device design.

No row in this map unlocks `P5-DEPLOYMENT-EXIT`. A row can move from CPU
fallback only when it has (a) a closed eligibility predicate, (b) a paired
reference-CPU differential, (c) conservation and geometry evidence, (d) Auto
CPU fallback, and (e) Manual Vulkan fail-closed/no-publication evidence.

## Source comparison and integrity notes

- The CPU symbols and formulas cited above were inspected in the unmodified
  `D:\Codex_lib\code_reference\ViennaPS` headers. No reference file was
  edited.
- Existing Mod-side CUDA conversion is intentionally treated as non-Vulkan
  evidence. In particular, `MultiParticleProcess`, `IonBeamEtching`,
  `SF6O2Etching`, `SF6C4F8Etching`, `SingleParticleALD`, and `TEOSPECVD`
  expose CUDA paths while their CPU semantic surfaces remain unimplemented on
  Vulkan.
- The current P5 board's `P5-NEUTRAL-CPU-ORACLE` Release optimization crash is
  an evidence gap, not permission to weaken the oracle or promote the neutral
  row. The same rule applies to any future paired CPU crash.
