# P5 Model Matrix Inventory

Status: inventory plus fallback-contract evidence (`P5-MODEL-MATRIX-ROW-01`,
2026-08-09; `P5-MODEL-MATRIX-ROW-02-MULTIPARTICLE-CPU-FALLBACK`, 2026-08-18;
`P5-MODEL-MATRIX-ROW-03-IONBEAM-CPU-FALLBACK`, 2026-08-18;
`P5-MODEL-MATRIX-ROW-04-CF4O2-CPU-FALLBACK`, 2026-08-18;
`P5-MODEL-MATRIX-ROW-05-SF6O2-CPU-FALLBACK`, 2026-08-18;
`P5-MODEL-MATRIX-ROW-06-SF6C4F8-CPU-FALLBACK`, 2026-08-18;
`P5-MODEL-MATRIX-ROW-07-FLUOROCARBON-CPU-FALLBACK`, 2026-08-18;
`P5-MODEL-MATRIX-ROW-08-SINGLEPARTICLEALD-CPU-FALLBACK`, 2026-08-18;
`P5-MODEL-MATRIX-ROW-09-TEOSPECVD-CPU-FALLBACK`, 2026-08-18;
`P5-MODEL-MATRIX-ROW-10-OXIDEREGROWTH-CPU-FALLBACK`, 2026-08-18),
with separately accepted narrow WetEtching, SelectiveEpitaxy, and
single-precursor TEOS numeric Process rows (`P5-WETETCH-PROCESS-ADAPTER`,
`P5-SELECTIVE-EPITAXY-PROCESS-ADAPTER`, and
`P5-TEOS-PROCESS-ADAPTER`, 2026-08-18). `P5-S1-SURFACE-INTEGRATION-GREEN`
(2026-08-20) additionally admits a narrow `NeutralTransport<float,2>` frontier
ray slice on the same CPU-first boundary. This file does not enable a generic
route or claim aggregate model support.

The governing boundary is [the Vulkan intent whitepaper](vulkan-program-intent-framework.md):
ViennaPS `Process`, strategy, model, surface, and level-set behavior remain
CPU-canonical; Vulkan replaces compute operations only. The current
[status board](vulkan-compute-acceleration-status.md) records a narrowly
accepted `P5-RAY-ROUTE` fixture and explicitly leaves surface integration and
the model matrix open. The [development report](vulkan-compute-acceleration-development-report.md)
is the release-slice context.

## Support-row contract

A row is supportable only when all of these fields have evidence for the
specific model configuration, numeric type, dimension, particle labels, and
ray/surface settings. A `getGPUModel()` method or a CUDA callable table is not
evidence of Vulkan support.

1. **CPU authority**: the production CPU model, particle callbacks, surface
   model, velocity field, and any advection/solver callback, linked to its
   header.
2. **Vulkan seam**: the exact compute bridge or `VulkanRayFluxEngine` stage
   that would be used. A seam for one velocity/coverage primitive is not a
   complete model route.
3. **Eligibility**: a closed predicate covering precision, dimension, labels,
   source, reflection/roulette, transport, and surface semantics.
4. **Missing semantics**: every behavior that is not represented by the seam.
5. **Fallback**: Auto must run the CPU authority; Manual Vulkan must fail
   closed without publishing flux, geometry, metadata, or partial state.
6. **CPU oracle**: a deterministic focused test comparing the candidate with
   the CPU result, including conservation and geometry criteria appropriate to
   the row.

The current Vulkan ray predicate is a three-row admission, not a generic model
hierarchy, and requires a non-empty `multibounceFrontierQueue` SPIR-V path for
any reflection-bearing route:
1. `SingleParticleProcess<float,2>`, one `SingleParticle`/`particleFlux` label,
   no custom source, and `maxReflections == 0`;
2. `SingleParticleProcess<float,2>`, one label, no custom source, no
   coverage/desorption, `maxReflections == 1`, and a non-empty
   `multibounceFrontierQueue` path (bounded one-reflection slice evidenced by
   `P5-RAY-MULTIBOUNCE-PROCESS-INTEGRATION`);
3. `NeutralTransport<float,2>`, default-constructed or equivalent single-label
   configuration, no custom source, `maxReflections <= 2`, and a non-empty
   `multibounceFrontierQueue` path (narrow frontier ray slice evidenced by
   `P5-S1-SURFACE-INTEGRATION-GREEN`; paired CPU record bit-exact, five-stage
   shared session, negative battery 4/4).
The eligibility gate rejects every other `ProcessModelCPU` before device
initialization. See
[`VulkanRayFluxEngine::checkInput`](../../gpu/vulkan/ray/vulkan_ray_flux_engine.cpp)
and the documented reflection boundary in
[`vulkan_ray_flux_engine.hpp`](../../gpu/vulkan/ray/vulkan_ray_flux_engine.hpp).

`P5-SURFACE-INTEGRATION` is still a prerequisite for a complete Process row.
Until it is accepted, even the eligible ray stage below is only a flux-stage
eligibility statement; the CPU surface update remains authoritative.

## Matrix

The expected fallback is part of every status below: **Eligible now** still
falls back to the linked CPU model in Auto when any predicate or deployment
gate is false; **CPU fallback** always selects that CPU model in Auto and
returns a non-throwing, no-publication failure for Manual Vulkan. No row
silently substitutes a CUDA model.

| Family (CPU source) | Vulkan compute seam | Current eligibility | Missing semantics and required CPU oracle |
|---|---|---|---|
| [SingleParticleProcess](../../include/viennaps/models/psSingleParticleProcess.hpp) | `VulkanRayFluxEngine` device triangle hit/compaction/reduction; CPU source setup and normalization are reused. | **Eligible now, narrowly**: only `SingleParticleProcess<float,2>`, one `SingleParticle`/`particleFlux` label, evidenced default or equivalent source, `maxReflections == 0`, and a valid strict deployment profile. A bounded `maxReflections == 1` slice with no coverage/desorption and a non-empty `multibounceFrontierQueue` path is separately evidenced by `P5-RAY-MULTIBOUNCE-PROCESS-INTEGRATION`; Row-01 itself remains limited to `maxReflections == 0`. This is not full model support until surface integration. | No reflection/roulette/multi-bounce and no general particle/model admission yet. Freeze the fixed-seed `CPU_TRIANGLE` differential (20,000/20,000 hits, total relative difference 0.29%, max 2.07% in the accepted fixture), then require unchanged CPU velocity/geometry after surface integration. |
| [MultiParticleProcess](../../include/viennaps/models/psMultiParticleProcess.hpp) (ion and neutral species) | No Vulkan species/energy/label seam. Existing GPU conversion is a CUDA model and warns that only one ion is converted. | **CPU fallback** for the complete process; no Vulkan row may be selected. | Per-species labels, ion energy distribution/threshold, angle-dependent sticking, neutral material sticking, custom rate function, and multiple particle ordering are absent from the ray engine. Oracle: per-label flux conservation and CPU surface velocity/geometry differential for one-ion, multi-ion, and mixed-neutral fixtures. |
| [IonBeamEtching](../../include/viennaps/models/psIonBeamEtching.hpp) | No Vulkan IonBeam transport or redeposition seam; the `ProcessModelGPU` class is CUDA-only. | **CPU fallback** in Auto; Manual Vulkan **unsupported/fail closed**. | Ion energy/angle response, reflection, redeposition flux, and `IBESurfaceModel` coupling are not represented. Oracle: CPU ion flux plus redeposition labels, mass/flux conservation, and final etched geometry for 2-D and 3-D fixtures. |
| [NeutralTransport](../../include/viennaps/models/psNeutralTransport.hpp) | `NeutralTransportVelocityExecutor` has an explicit FP32 Vulkan bridge; the frontier ballistic event queue is implemented in the narrow slice below. | **Narrow frontier ray slice accepted**: `NeutralTransport<float,2>`, default-constructed or equivalent single-label configuration, no custom source, `maxReflections <= 2`, and a non-empty `multibounceFrontierQueue` path (`P5-S1-SURFACE-INTEGRATION-GREEN`: paired CPU record bit-exact on Release/Intel Arc, five-stage shared session, negative battery 4/4). The rest of the model remains CPU fallback. | The frontier slice covers only the ballistic event queue and surface-label reduction; coverage update, desorption, surface diffusion, material IDs, custom source/labels, and the full callback lifecycle remain CPU fallback. Oracle: raw-bit equality of the paired CPU record for the admitted slice (`P5-S1`), then CPU full-process coverage/surface-data/velocity and geometry differential with conservation for any broader configuration. |
| [CF4O2Etching](../../include/viennaps/models/psCF4O2Etching.hpp) | No Vulkan seam; CPU model owns ion, etchant, oxygen, polymer particles and surface chemistry. | **CPU fallback**; no `getGPUModel()`-based promotion. | Four species, ion-enhanced oxidation/sputtering, polymer/oxygen coupling, coverage and material response are missing. Oracle: all four flux labels, per-material velocity, non-negative/conserved species totals, and final geometry. |
| [SF6O2Etching](../../include/viennaps/models/psSF6O2Etching.hpp) | CUDA `ProcessModelGPU` callable model only; no Vulkan plasma transport or surface bridge. | **CPU fallback**; CUDA class is not Vulkan evidence. | Ion/etchant/oxygen callables, plasma surface chemistry, coverage, and material response are missing. Oracle: three species labels, flux/velocity conservation, and CPU geometry differential. |
| [SF6C4F8Etching](../../include/viennaps/models/psSF6C4F8Etching.hpp) | CUDA `ProcessModelGPU` callable model only; it reuses the plasma surface model but has no Vulkan route. | **CPU fallback**. | Ion/etchant/polymer transport, polymer deposition/etch competition, and plasma surface response are missing. Oracle: all species labels, polymer mass balance, per-material velocity, and geometry differential. |
| [FluorocarbonEtching](../../include/viennaps/models/psFluorocarbonEtching.hpp) / [PlasmaEtching](../../include/viennaps/models/psPlasmaEtching.hpp) | CPU particle callbacks and `SurfaceModel` only; the SF6 wrappers' CUDA path does not supply a Vulkan seam for these semantics. | **CPU fallback**. | Ion and neutral reflection, coverage, surface data, etch/deposition competition, and custom plasma rates require a model-specific transport/surface contract. Oracle: species flux labels, coverage bounds, mass/flux conservation, and geometry differential. |
| [SingleParticleALD](../../include/viennaps/models/psSingleParticleALD.hpp) | CUDA GPU model is ballistic-only and rejects positive mean free path; no Vulkan ALD transport/coverage seam. | **CPU fallback**. | Adsorption/desorption fluxes, coverage evolution, evaporation, and ALD cycle semantics are missing. Oracle: particle flux, coverage bounds, adsorption/desorption balance, and cycle-by-cycle CPU geometry. |
| [TEOSDeposition](../../include/viennaps/models/psTEOSDeposition.hpp) | Opt-in `TEOSDeposition::setVelocityExecutor` replaces only the single-precursor `rate * pow(flux, order)` batch; CPU particles, sticking, coverage and Process order remain authoritative. | **Narrow row accepted** only for `float,D=2`, one `SingleTEOSParticle`, one label, nonnegative flux and finite nonnegative rate/order; all multi-precursor and broader TEOS inputs remain CPU fallback/Manual fail-closed. | Coverage-dependent sticking, precursor ordering, multi-label reaction rates and generic deployment are absent. Oracle: paired CPU Process raw bits, Intel Arc rate/geometry differential, nonnegative output, and malformed/reset/fallback sentinels. |
| [TEOSPECVD](../../include/viennaps/models/psTEOSPECVD.hpp) | CUDA GPU callable model for radical and ion particles; no Vulkan transport or PECVD surface bridge. | **CPU fallback**. | Radical/ion coupling, reflection, reaction-order surface velocity, and species conservation are missing. Oracle: radical and ion labels, coupled rate-law/flux conservation, and CPU geometry differential. |
| [WetEtching](../../include/viennaps/models/psWetEtching.hpp) | Opt-in `WetEtching::setVelocityExecutor` numeric seam for the strict adapter row; CPU crystal-direction `VelocityField` remains authoritative and generic deployment has no binding. | **Narrow row accepted** only for `WetEtching<float,2>`, one Si material-rate entry and the existing analytic Process fixture; all other WetEtching inputs remain CPU fallback/Manual fail-closed. | The accepted row covers one-point candidate replacement with exact CPU fallback and full fixture geometry. D=3, multiple materials/species, non-analytic callbacks, automatic stage/profile promotion, broader surface physics and aggregate support remain absent. |
| [SelectiveEpitaxy](../../include/viennaps/models/psSelectiveEpitaxy.hpp) | Opt-in `SelectiveEpitaxy::setVelocityExecutor` numeric seam for scalar crystal velocity; CPU `initialize`/`finalize` still transform the domain and own the stencil. | **Narrow row accepted** only for `SelectiveEpitaxy<float,2>`, two built-in material rates and the bounded analytic Process fixture; all other inputs remain CPU fallback/Manual fail-closed. | D=3, custom materials, Boolean/stencil/device domain ownership, generic deployment/profile binding and aggregate semantics remain absent. Oracle: paired CPU Process raw bits, pre/post level-set topology/material IDs, exact callback fallback and final geometry. |
| [OxideRegrowth](../../include/viennaps/models/psOxideRegrowth.hpp) | No Vulkan seam; CPU `ByproductDynamics` advection callback performs redeposition, diffusion, convection, sink, and material updates. | **CPU fallback**; unsupported for Vulkan Process routing. | Callback ordering, dense-cell-set neighbor transport, redeposition, byproduct conservation, and rollback semantics are missing. Oracle: callback event order, byproduct mass balance, non-negative concentrations, and final geometry. |
| [Oxidation](../../include/viennaps/models/psOxidation.hpp) | P6 linear-algebra/oxidation coupling is not complete; the Process uses ViennaLS oxidation diffusion/deformation on CPU. | **CPU fallback**; Vulkan oxidation is not eligible in P5 (especially without FP64 evidence). | Oxidant diffusion, mechanics/deformation, coupling iterations, convergence/residual history, and non-convergence rollback are missing. Oracle: residual/convergence history plus CPU field, material, and geometry differential; a device without required FP64 must remain CPU per stage. |

## Executable first row: `P5-MODEL-MATRIX-ROW-01`

This is the first serial model-matrix task. It admits only the already
evidenced `SingleParticleProcess<float, 2>` ray slice; the separately carded
strict WetEtching and SelectiveEpitaxy adapters are later narrow exceptions,
while every other family remains an explicit CPU-fallback row; TEOS is a
separate narrow numeric exception with the same CPU-first boundary. This card is an
execution contract, not a support or release claim.

**Predecessor and next unlock.** Start after `P5-RAY-APPLY-TRANSACTION` and
the `P5-SURFACE-INTEGRATION-NONACCEPTANCE` coverage are present. At the time
this card was drafted, the complete row was blocked by a reference-identical
KDTree failure handed to `P5-CPU-KDTREE-ROOTCAUSE`; that dependency boundary
was later closed by `P5-N1H/N2`. A passing row unlocks the next model-matrix
row and its aggregate review, never `P5-DEPLOYMENT-EXIT` by itself.

**Owned boundary and invariants.** Own the one eligible ray row, its CPU-led
differential/conservation/geometry evidence, and the fallback assertions for
the rows below. Preserve CPU setup, source normalization, surface callbacks,
Process ordering, and rollback. Do not edit production headers, tests, CMake,
CUDA code, or `D:\Codex_lib\code_reference\ViennaPS`; do not infer Vulkan
support from `getGPUModel()`, a CUDA callable table, a surface smoke, or a CPU
crash.

**Eligible row and oracles.** The predicate is `float`, `D == 2`, one
`SingleParticle`/`particleFlux` label, evidenced default (or equivalent)
source, `maxReflections == 0`, and a valid strict-FP32 deployment profile.
The engine predicate has since been extended to admit a bounded
`SingleParticleProcess` one-reflection slice and a `NeutralTransport<float,2>`
frontier ray slice, but Row-01 evidence remains limited to `maxReflections == 0`.
Use the unmodified reference tree as the CPU authority. Freeze the fixed-seed
`CPU_TRIANGLE` comparison at 20,000/20,000 hits (existing fixture limits:
`totalRelDiff <= 0.29%`, `maxRelDiff <= 2.07%`), verify hit/flux conservation,
then compare CPU velocity and final geometry after surface integration. The
existing Intel Arc Vulkan evidence (`gridDelta=0.5`, zero reported relative
difference) is admissible only for this predicate and does not replace the
CPU surface oracle.

**Ordered exits.**

1. Recheck the predicate and reject every out-of-predicate input before device
   initialization.
2. Run the deterministic reference-CPU flux/label differential and
   conservation checks.
3. Run the strict-profile Vulkan ray fixture and compare the same flux labels
   and totals; publish no partial result on failure.
4. For every fallback row below, assert Auto executes the linked CPU model and
   Manual Vulkan returns a non-throwing fail-closed result with no flux,
   metadata, velocity, or geometry publication.
5. Run the CPU surface velocity/geometry differential. If a new Mod/reference
   failure appears, stop as a reference-precondition blocker and do not mark
   this row accepted; the former N1/N2 KDTree failure is already closed and is
   not a current acceptance condition.

**Explicit CPU-fallback rows.** These families remain CPU-authoritative in
Auto and unsupported/fail-closed in Manual Vulkan until a separate row card
supplies its own seam and oracle:

| Family | Required status in this card |
|---|---|
| `MultiParticleProcess` | CPU fallback; no Vulkan species/energy route. |
| `NeutralTransport` | Narrow frontier ray slice see the matrix row above; full model CPU fallback. |
| `IonBeamEtching` | CPU fallback; Manual Vulkan fail closed. |
| `CF4O2Etching`, `SF6O2Etching`, `SF6C4F8Etching` | CPU fallback; CUDA is not Vulkan evidence. |
| `FluorocarbonEtching`, `PlasmaEtching` | CPU fallback; no transport/surface contract. |
| `SingleParticleALD`, `TEOSPECVD` | CPU fallback; no Vulkan ALD/precursor/PECVD route. |
| `TEOSDeposition` outside its separately accepted single-precursor numeric row | CPU fallback; multi-precursor transport, sticking and coverage semantics remain CPU-owned. |
| `WetEtching` outside its separately accepted strict adapter row, `SelectiveEpitaxy` outside its separately accepted strict adapter row | CPU fallback; CPU level-set/domain transforms remain authoritative. |
| `OxideRegrowth`, `Oxidation` | CPU fallback; callback/P6 physics and FP64 requirements remain unmet. |

The handoff records the exact predicate, oracle output, fallback diagnostics,
and any CPU-surface blocker for each row. The rank-2
`P5-MODEL-MATRIX-ROW-02-MULTIPARTICLE-CPU-FALLBACK` and rank-3
`P5-MODEL-MATRIX-ROW-03-IONBEAM-CPU-FALLBACK` and rank-4
`P5-MODEL-MATRIX-ROW-04-CF4O2-CPU-FALLBACK` and rank-5
`P5-MODEL-MATRIX-ROW-05-SF6O2-CPU-FALLBACK` and rank-6
`P5-MODEL-MATRIX-ROW-06-SF6C4F8-CPU-FALLBACK` and rank-7 concrete
`P5-MODEL-MATRIX-ROW-07-FLUOROCARBON-CPU-FALLBACK` and rank-8
`P5-MODEL-MATRIX-ROW-08-SINGLEPARTICLEALD-CPU-FALLBACK` and rank-10
`P5-MODEL-MATRIX-ROW-09-TEOSPECVD-CPU-FALLBACK` and rank-13
`P5-MODEL-MATRIX-ROW-10-OXIDEREGROWTH-CPU-FALLBACK` evidence is
CPU/fallback only;
subsequent rows must not widen the predicate or weaken the oracle.

### Row-01 fallback smoke

`viennaps-vulkan-model-matrix-fallback-smoke` is the machine-checkable
inventory gate. It constructs one CPU model for each of the 14 non-eligible
rows (the combined `FluorocarbonEtching`/`PlasmaEtching` inventory row is
represented by its concrete CPU model) with an empty `multibounceFrontierQueue`
path, then checks that AUTO accepts the CPU engine contract while MANUAL Vulkan
returns `INVALID_INPUT` before any disk-mesh output exists. A separate strict
check confirms that only `SingleParticleProcess<float,2>` with zero reflections
remains eligible under that empty-path configuration; it does not exercise the
bounded one-reflection or `NeutralTransport` frontier slices, which require a
non-empty `multibounceFrontierQueue` path and are evidenced separately. This
smoke is routing evidence only; it does not close the pending Release CPU
surface oracle or claim Vulkan support for any additional row.

## Fallback and dependency rules

- Auto selection may use a Vulkan stage only when the row predicate and its
  CPU differential are accepted. Otherwise it records the reason and executes
  the linked CPU authority.
- Manual Vulkan is an explicit request, not permission to reinterpret a CUDA
  model. An unsupported row returns a non-throwing failure and publishes no
  flux or geometry update.
- A complete row depends on `P5-RAY-ELIGIBILITY-GATE` and
  `P5-SURFACE-INTEGRATION`; the current inventory intentionally precedes the
  `P5-MODEL-MATRIX` implementation card. The accepted ray fixture alone does
  not unlock any other family.
- Focused tests must be deterministic and CPU-led: compare flux labels and
  surface fields, then check model-specific conservation and geometry. A
  compile, a CUDA `getGPUModel()`, or a device smoke without that oracle cannot
  change a row to supported.

## Source-link resolution

All named families resolve to tracked headers in
`include/viennaps/models/`: `psSingleParticleProcess.hpp`,
`psMultiParticleProcess.hpp`, `psIonBeamEtching.hpp`,
`psNeutralTransport.hpp`, `psCF4O2Etching.hpp`, `psSF6O2Etching.hpp`,
`psSF6C4F8Etching.hpp`, `psFluorocarbonEtching.hpp`, `psPlasmaEtching.hpp`,
`psSingleParticleALD.hpp`, `psTEOSDeposition.hpp`, `psTEOSPECVD.hpp`,
`psWetEtching.hpp`, `psSelectiveEpitaxy.hpp`, `psOxideRegrowth.hpp`, and
`psOxidation.hpp`.
