# P5 CPU Reference Conformance Inventory

- Card: `P5-CPU-REFERENCE-CONFORMANCE`
- Baseline: unmodified `D:\Codex_lib\code_reference\ViennaPS`
- Scope: CPU code reachable from the P5 ray Process route and its explicit
  Auto CPU fallback. This is an inventory, not a CPU or Vulkan implementation.
- Method: semantic comparison of the current headers against the baseline with
  line-ending differences ignored, followed by call-site tracing from
  `ray_flux_process_route_smoke`.

## Current disposition (2026-08-21)

The Release crash described below is a historical 2026-08-09 snapshot, not a
current blocker. The former ViennaCore `KDTree::traverseDown` optimization
failure was repaired at the dependency boundary by
`cmake/patches/viennacore-v2.2.1-kdtree-traversedown-nullcheck.patch` and
closed by the independent `P5-N2` Mod/reference matrix: Release OMP 1/2/4/8
all exit 0 with raw serialized equality and `max_ulp=0`. The bounded
one-reflection slice was accepted by its Process card; `P5-S1` accepted the
narrow NeutralTransport frontier and `P5-M0` consolidated those rows into the
matrix. This inventory still does not claim broad CPU parity, full
NeutralTransport semantics, or production Vulkan promotion.

## Reachable Route

The accepted route is:

`Process::calculateFlux/apply` -> `FluxProcessStrategy` -> injected
`VulkanRayFluxEngine` -> either the eligible Vulkan stage or its
`CPUTriangleEngine` fallback. CPU surface extraction and post-processing use
`CreateSurfaceMesh`, `ElementToPointData`, ViennaRay, and the model's surface
callbacks. The multi-step fallback can additionally reach `AdvectionHandler`;
surface-composition work can reach `NeutralTransportSurfaceModel` and
`CoverageManager`.

## Difference Inventory

| Symbol/file | Evidence against reference | Classification | Required boundary/differential |
|---|---|---|---|
| `process/psCPUTriangleEngine.hpp` | Semantic content identical; only working-copy line endings differ. Its `initialize`, `updateSurface`, `runRayTracer`, and normalization calls are unchanged. | No CPU regression. | Keep `CPU_TRIANGLE` as the oracle; compare fixed-seed labels, counts, and flux values before any route claim. |
| `psElementToPointData.hpp` | Semantic content identical. It is used by both `CPUTriangleEngine` and the Vulkan engine's final conversion. | No CPU regression. | Preserve the reference conversion radius, KD-tree lookup, and label ordering. |
| `process/psFluxEngine.hpp`, `process/psCPUDiskEngine.hpp`, `process/psProcessModel.hpp`, `models/psSingleParticleProcess.hpp` | Semantic content identical. | No CPU regression. | These remain the model/base CPU authority for all fallback rows. |
| `process/psProcess.hpp` (`setFluxEngineOverride`, `createFluxEngine`, result tracking; lines 46-47, 113-122, 204-263, 326-345) | Adds an injected-engine seam and status reporting. Existing CPU factory branches still construct `CPUDiskEngine`/`CPUTriangleEngine` (lines 337-342). | P5 orchestration, not CPU algorithm. | Auto fallback must call the same CPU engine; Manual Vulkan failure must publish no mesh/metadata/flux. The override is consumed once, so repeated calls need an explicit fresh engine. |
| `process/psProcessContext.hpp` (lines 24-56) | Adds Level Set, coverage, and surface-diffusion executor fields and a failure policy; no ray or CPU formula changed. | P5 orchestration, not CPU algorithm. | Empty callbacks must leave the reference CPU path unchanged. Executor-enabled behavior requires a separate callback differential. |
| `process/psFluxProcessStrategy.hpp` (lines 122-126, 525-626) | Binds an optional coverage executor and dispatches optional surface-diffusion work; the original solver remains the no-executor branch. | P5 compute seam. | Run CPU/no-executor and executor-failure cases against the reference; executor failure must return/fallback without partial publication. This is not evidence of a changed CPU model. |
| `process/psAdvectionHandler.hpp` (lines 60-68, 117-218) | Adds executor wiring and fail-closed validation. The no-executor branch at lines 208-216 matches the reference implementation's time update and `SUCCESS` result (reference lines 108-128). | P5 orchestration when an executor is active; reference CPU semantics when empty. | Differential with both executor fields empty is mandatory. Do not treat executor-active strict validation as a new CPU algorithm. |
| `models/psNeutralTransport.hpp` (`NeutralTransportSurfaceModel`, lines 166-209) | The canonical CPU velocity loop is retained as `computeCpuVelocity`; an optional `VelocityExecutor` may supply a candidate and falls back to that loop on failure. | P5 compute seam, not a CPU algorithm replacement. | With no executor, compare velocity bit-for-bit to reference. With an executor, require completion/count checks and CPU fallback. Do not use the optional executor as evidence that the CPU model changed. |
| `process/psCoverageManager.hpp` (lines 59-126) | Always computes `cpuMetric` first; an optional executor can replace it only after complete/count validation, otherwise the CPU metric is returned. | P5 compute seam, not a CPU algorithm replacement. | Differential no executor, successful executor, malformed executor, and thrown executor. Coverage route remains separate from ray transport. |
| `psUtil.hpp` (`FluxEngineType::VULKAN_RAY`, lines 16-24, 81-97, 145-164) | Adds enum/string conversion only. `Process::createFluxEngine` has no `VULKAN_RAY` factory branch; P5 uses `setFluxEngineOverride`. | Non-CPU orchestration metadata; not a route acceptance signal. | Do not claim selecting this enum creates a Vulkan engine. Use the explicit override and eligibility/fallback checks. |
| `psDomain.hpp::saveVolumeMesh` (lines 643-661) | Adds a `VIENNALS_USE_VTK` guard and diagnostic. | Pre-existing/non-P5 build portability. | No ray or CPU numerical impact; exclude from P5 parity claims. |
| `models/psOxidation.hpp` diagnostic text only (line 647) | Replaces a non-ASCII `<=` display character with ASCII `<=`. | Pre-existing/non-P5. | No behavioral differential required. |
| `levelset/psHrleRebuild{Classification,Compaction,SparseReconstruction}.hpp` (called by `gpu/vulkan/levelset/viennals_rebuild_executor.hpp`) | New local helper headers with no reference counterpart. They are reachable only when the Level Set rebuild executor is explicitly installed, not from the CPU ray algorithm. | P5 Level Set compute seam outside P5-RAY-ROUTE CPU semantics. | Keep the frozen `hrleRebuildCpuFixture` differential against the canonical CPU rebuild path; do not treat helper presence as ray CPU parity. |
| `ray/ray_{event_queue,reflection,roulette,surface_response}.hpp` | New local headers with no reference counterpart; current consumers are `tests/rayPhysics`, not the Process ray route. | P5-RAY-PHYSICS host contracts, not P5-RAY-ROUTE CPU implementation. | Keep reflection/roulette/multi-bounce outside the current Vulkan predicate; each future route needs a reference CPU differential. |

## Vulkan Host Replica Boundary

`gpu/vulkan/ray/vulkan_ray_flux_engine.cpp` is new P5 orchestration code, not a
replacement CPU engine. It delegates `checkInput`, initialization, surface
updates, and all source/surface flux work to `CPUTriangleEngine` when Vulkan is
not selected. The 2026-08-06 round-2 audit below records the original
zero-reflection differential and remains a historical boundary snapshot. The
current `checkInput` predicate has three separately evidenced narrow slices:
zero-reflection `SingleParticleProcess`, bounded one-reflection
`SingleParticleProcess`, and the `NeutralTransport<float,2>` frontier slice.
Together they still do not prove general source, boundary, coverage, logging,
multi-particle, desorption, or 3-D equivalence.

Therefore:

1. Keep the explicit eligibility predicate in the current `checkInput`; the
   model-matrix inventory is the canonical row-by-row record.
2. Auto must call the unchanged CPU engine for every rejected predicate or
   unavailable Vulkan deployment.
3. Manual Vulkan must return a failure before publishing flux, geometry, or
   metadata when the predicate/deployment is not satisfied.
4. Do not widen the predicate from this inventory. New source, boundary,
   reflection, coverage, or model behavior requires its own reference CPU
   differential.

## Validation Evidence

### Historical P5-NEUTRAL-CPU-ORACLE paired differential (2026-08-09)

`tests/neutralCpuReferenceDifferential/` provides a same-source, controlled
reference/Mod fixture and an independent bit-pattern checker. The default
MSVC objects were linked against the same local Release Embree closure. The
reference executable used only
`D:\Codex_lib\code_reference\ViennaPS` headers; the Mod executable enabled
`VIENNAPS_NEUTRAL_ORACLE_MOD` solely for the backend-neutral velocity adapter.
The checker compared empty-executor and active velocity, CPU-triangle flux,
plane geometry, and process-state records.

```text
paired neutral CPU differential PASS empty=exact active=exact flux=exact geometry=exact process=exact max_ulp=0
```

The active result is adapter-only evidence. It does not establish complete
NeutralTransport physics or any Vulkan/model support claim.

At this historical snapshot, the required `/O2 /Ob2 /openmp:llvm` gate was
blocked: both executables compiled and linked but exited `-1073741819`
(`0xC0000005`) during CPU `calculateFlux()` setup. No optimization workaround
was applied in that snapshot; the boundary was later closed by `P5-N1H/N2`.

### Historical deployment-only blocker (superseded by P5-N1H/N2)

The root-cause probe ran the composed
`.tmp_p5_route_20260805/gpu/vulkan/ray/viennaps-vulkan-levelset-surface-ray-deployment-smoke.exe`
with O2/Ob2/OpenMP and observed exit `0xC0000005` at module offset
`0x525f2`. Disassembly maps the instruction to
`KDTree<float,std::array<float,3>>::traverseDown` (`movsxd rbx, dword ptr [rdi+18h]`);
repeated filter output shows parallel callers. The same process/model/grid in
standalone `p5_mod_singleparticle_oracle` and
`p5_reference_singleparticle_oracle` both exit 0 with a mesh, as does the
route-smoke CPU oracle. The deployment executable fails before
`Composition.configure`, so no callback or deployment mutation is involved.
This evidence narrows the blocker to the composed deployment binary's
link/layout/runtime interaction. It does not justify changing CPU headers or
widening Vulkan eligibility.

Read-only checks performed for this card:

```text
git diff --no-index --ignore-space-at-eol \
  D:\Codex_lib\code_reference\ViennaPS\include\viennaps\process\psCPUTriangleEngine.hpp \
  include\viennaps\process\psCPUTriangleEngine.hpp
  -> no semantic diff

git diff --no-index --ignore-space-at-eol \
  D:\Codex_lib\code_reference\ViennaPS\include\viennaps\psElementToPointData.hpp \
  include\viennaps\psElementToPointData.hpp
  -> no semantic diff
```

The existing `p5-cpu-reuse-ray-audit-round2.md` records the fixed-seed Intel
Arc route differential and explicitly leaves the host replicas `PARTIAL`
outside the accepted predicate. The predecessor surface failure is now bounded
by the root-cause probe: standalone `p5_mod_singleparticle_oracle` and the
ray-route CPU oracle pass, while the composed deployment target crashes in
`viennacore::KDTree<float,std::array<float,3>>::traverseDown` during
`CPUTriangleEngine<float,2>::updateSurface` -> `PointToElementDataSingle::apply`
-> `ProcessContext::getPointKdTree`. This is a deployment link/layout or
ODR/runtime interaction around the KD-tree, not a changed SingleParticle or
CPUTriangle algorithm. No production fix or narrower pointer-state cause has
been proven; do not convert this crash into CPU-semantic or Vulkan evidence.

## Handoff

No CPU algorithm source drift was found in the canonical ray engine or
postprocessor. Conditional executor behavior remains an orchestration seam.
The former composed Release KD-tree failure was repaired at the ViennaCore
dependency boundary and the required paired oracle is now closed by `P5-N2`.
The Vulkan host replica must nevertheless remain behind its current narrow
eligibility predicate; `P5-MODEL-MATRIX-ROW-01` and `P5-DEPLOYMENT-EXIT` still
must not claim broader CPU parity or full model support.
