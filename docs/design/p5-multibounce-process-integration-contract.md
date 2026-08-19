# P5 bounded multi-bounce Process integration contract

Status: design gate plus the accepted narrow implementation
(`P5-RAY-MULTIBOUNCE-PROCESS-INTEGRATION-IMPLEMENTATION`, 2026-08-17). This
document still defines the boundary for generic Process integration: the
implemented route is only the separately admitted one-reflection slice below.
It does not claim generic multi-bounce Process or model support.

## Card brief

**Milestone.** Specify how an accepted bounded frontier queue could be injected
at the existing `FluxEngine` seam while ViennaPS `Process`, ViennaRay, model
callbacks, normalisation, and Level Set transactions remain CPU-authoritative.

**Predecessors.** The [program intent framework](vulkan-program-intent-framework.md),
[multi-bounce state map](p5-device-multibounce-state-map.md),
[general frontier-queue contract](p5-device-multibounce-general-queue.md), the
accepted strict-route/transaction evidence, and an independently built
reference-vs-Mod CPU oracle. The current frontier implementation is still a
bounded compute composition and is not by itself a Process predecessor.

**Next unlocked milestone.** A separately carded
`P5-RAY-MULTIBOUNCE-PROCESS-INTEGRATION-IMPLEMENTATION` may implement one
explicitly admitted Process row only after the gates in §9 pass. This document
does not unlock a model-matrix row, automatic promotion, or release readiness.

**Global position (about 180 tokens).** P5 replaces only proved compute with
Vulkan. The unmodified `D:\Codex_lib\code_reference\ViennaPS` tree remains the
CPU semantic authority for Process orchestration, ViennaRay source and
particle state, callbacks, RNG, boundaries, reflection, roulette, model and
surface semantics, normalisation, and Level Set/advection publication. The
bounded frontier queue may apply immutable geometry results and complete
host-produced decisions, but it may not invent a callback, consume a hidden
random draw, or make a host bounce loop look device-resident. Integration must
enter through the existing FluxEngine lifecycle, stage all output, and publish
only after queue status, session generation, capacity, raw-bit differential,
conservation, and geometry checks pass. Auto falls back to the CPU authority
per stage; Manual Vulkan fails closed with no publication. The present strict
route remains single-bounce and its model predicate is unchanged. A queue run
is therefore a bounded compute route, not generic multi-bounce Process
support.

**Invariants.** CPU state is canonical; every admitted event has stable
`(particleId,bounce,sequence)` identity; callbacks and RNG execute in CPU trace
order; device buffers are bounded, generation-owned, and non-aliasing; status
is checked before readback; and no failure may publish partial flux, mesh,
metadata, model, or Level Set state.

## 1. Current predicates and non-expansion rule

The current production baseline predicate is exact and must remain unchanged:

```text
NumericType == float
D == 2
model is SingleParticleProcess<float, 2>
exactly one particle type
exactly one particle-data label
model source == nullptr (the evidenced default source)
context.rayTracingParams.maxReflections == 0
```

This is the executable gate in
[`VulkanRayFluxEngine::checkInput`](../../gpu/vulkan/ray/vulkan_ray_flux_engine.cpp:542),
with the predicate at lines
[`551-563`](../../gpu/vulkan/ray/vulkan_ray_flux_engine.cpp:551). The public
header documents the same no-reflection boundary at
[`VulkanRayFluxEngine::devicePhysicsGap`](../../gpu/vulkan/ray/vulkan_ray_flux_engine.hpp:60).
The model inventory records the relation as a narrow flux-stage eligibility
statement, not complete model support
([inventory §Support-row contract](p5-model-matrix-inventory.md:36)).

The bounded extension is a separate, explicitly checked predicate: the same
`float/2D/SingleParticleProcess`/one-label/default-source row, exactly
`maxReflections == 1`, an available frontier SPIR-V path, and no coverages or
surface desorption. It does not admit `maxReflections > 1`, custom sources,
multiple particles or labels, double precision, 3-D, NeutralTransport, CUDA
model classes, or any other model-matrix row. The route's Auto/Manual relation
remains:

* unsupported input in `checkInput` returns the CPU engine's check in Auto and
  `INVALID_INPUT` in Manual
  ([engine](../../gpu/vulkan/ray/vulkan_ray_flux_engine.cpp:569));
* pipeline setup failure falls back to `CPUTriangleEngine` in Auto and returns
  `FAILURE` in Manual;
* a bounded frontier/session/dispatch failure discards staged output and
  retries the same request through the initialized CPU engine in Auto, while
  Manual returns `FAILURE` without publication;
* the current device-physics entry point remains an unconditional,
  transaction-safe rejection
  ([`runGpuPhysics`](../../gpu/vulkan/ray/device_ray_flux_pipeline.cpp:385)).

Later Process integration may add a separately evidenced row only by a new
card, a new closed predicate, and a new model-matrix entry. It must not infer
support from the existence of `MultibounceFrontierQueue`, a shader, a
throughput smoke, or a CUDA `getGPUModel()`.

## 2. Authority and injection seam

The only permitted production seam is the existing `FluxEngine` interface:
`checkInput`, `initialize`, `updateSurface`, `calculateSourceFluxes`, and
`calculateSurfaceFluxes`
([interface](../../include/viennaps/process/psFluxEngine.hpp:10)). A deployment
may install a pre-built engine with `setFluxEngineOverride`; the override is
consumed by the next `apply()` or `calculateFlux()` call
([injection](../../include/viennaps/process/psProcess.hpp:111)). No public
Vulkan type or second Process loop may be introduced.

`Process::apply()` checks and updates context, finds the existing strategy,
creates/consumes the injected engine, executes the strategy, and records the
terminal `ProcessResult`
([transaction boundary](../../include/viennaps/process/psProcess.hpp:204)).
`calculateFlux()` creates the Flux strategy, runs its diagnostic one-pass
calculation, and returns the disk mesh without advancing process time
([diagnostic path](../../include/viennaps/process/psProcess.hpp:242)). The
frontier implementation must therefore be an implementation detail of the
engine, never a replacement for `Process` or `FluxProcessStrategy`.

## 3. CPU semantic order and bounded frontier handoff

`FluxProcessStrategy::processTimeStep` is a fixed transaction order:

1. prepare advection and update the surface;
2. call `FluxEngine::updateSurface`;
3. calculate source fluxes, then optional desorption;
4. apply diffusion and coverage updates;
5. calculate surface-model velocities and prepare the velocity field;
6. run pre-advection callback and coverage transfer;
7. perform advection;
8. update post-advection coverages and run the post-advection callback.

The current implementation is anchored at
[`processTimeStep`](../../include/viennaps/process/psFluxProcessStrategy.hpp:309)
and the reference tree has the same ordering at
`D:\Codex_lib\code_reference\ViennaPS\include\viennaps\process\psFluxProcessStrategy.hpp:302`.
No frontier submission may reorder, skip, or duplicate these stages.

The permitted handoff is:

```text
CPU source/particle state + frontier N hit inputs
  -> device hit/classification/compact/sort/apply of complete decisions
  -> status + session check
  -> host reconstructs CPU point/normal/material convention
  -> CPU callbacks and RNG in (particleId,bounce,sequence) order
  -> upload complete decisions for frontier N+1
  -> repeat under finite limits
  -> host normalises and stages PointData/mesh/logs
  -> FluxProcessStrategy continues its unchanged order
```

[`MultibounceFrontierQueue::run`](../../gpu/vulkan/ray/multibounce_frontier_queue.hpp:36)
accepts `events` plus `decisions`; its implementation validates them, applies
one bounded device round per supplied frontier, checks status before readback,
and only then moves staged output to the caller
([run](../../gpu/vulkan/ray/multibounce_frontier_queue.cpp:114)). It does not
trace a ray, call `AbstractParticle`, choose a source, consume RNG, or create a
successor decision. A host loop that supplies each next frontier is CPU
orchestration around compute and must never be reported as device-resident
transport.

## 4. Callback and RNG sequencing

The CPU producer remains authoritative for every admitted surface event. The
current producer follows the locked ViennaRay order:

1. `surfaceCollision`;
2. `surfaceReflection`;
3. `nextWeight = weight - weight * sticking`;
4. terminate when `nextWeight <= 0`;
5. increment/check reflection limit;
6. only then perform the low-weight roulette draw, using the locked
   `0.1 * initialWeight` and `0.3 * initialWeight` thresholds;
7. publish the successor origin/direction and action.

See the producer's ordering comment and calls
([`produce`](../../gpu/vulkan/ray/multibounce_decision_producer.cpp:60)) and
the ViennaRay callback declarations
([`AbstractParticle`](../../.cpm-cache/viennaray/0fe9/include/viennaray/rayParticle.hpp:22)).
The CPU trace loop's state and branch order are also fixed by
[`TraceKernel::apply`](../../.cpm-cache/viennaray/0fe9/include/viennaray/rayTraceKernel.hpp:117).

The integration contract requires one per-ray RNG stream and explicit draw
ordinals. A non-taken roulette branch consumes no draw; survival and rejection
consume exactly one. Device scheduling, event sorting, or `(rayId,bounce)`
reseeding must not alter the CPU stream. Arbitrary C++ callbacks, model/global
`PointData`, and material-dependent reflection remain host-owned; shaders may
only validate and apply the uploaded decision bytes.

## 5. Flux injection, normalisation, conservation, and geometry

The engine must keep `updateSurface` as a geometry-input phase and
`calculateSourceFluxes` as the only source-to-surface flux publication point.
The current Vulkan engine intentionally keeps a CPU engine in sync for
desorption and fallback
([surface sync](../../gpu/vulkan/ray/vulkan_ray_flux_engine.cpp:658)). Its
device path accumulates per-triangle hits, normalises with the CPU triangle
contract, writes element labels, and runs the existing element-to-point
post-processing
([normalisation and staging](../../gpu/vulkan/ray/vulkan_ray_flux_engine.cpp:734)).
The reference CPU engine performs the corresponding per-particle normalisation
and data-log merge at
`D:\Codex_lib\code_reference\ViennaPS\include\viennaps\process\psCPUTriangleEngine.hpp:266`.

A future frontier engine must stage, but not publish, all of the following:

* per-label `PointData` fluxes and element arrays;
* particle data-log updates;
* triangle/disk mesh metadata and material IDs;
* any queue terminal records and conservation counters.

Acceptance must compare the admitted CPU row for source-weight conservation,
sticking/reflection/termination accounting, per-label totals, finite values,
and the exact normalisation denominator. The final `Process::apply()` gate must
also compare Level Set width/topology, material geometry, process time, and
callback order against the CPU authority. Ray output alone is not geometry or
surface-physics acceptance: `SurfaceModel::calculateVelocities` and
`viennals::Advect` remain in the strategy path
([velocity/advection](../../include/viennaps/process/psFluxProcessStrategy.hpp:340)).

## 6. Transaction, rollback, and publication

The queue must use an output sentinel and a staged result. Before any caller
mutation, reject malformed events/decisions, non-finite or subnormal values,
duplicate or mismatched identities, capacity overflow, stale session
generation, device loss/reset, timeout, and non-zero device status. The general
queue contract requires status-before-download and byte-for-byte unchanged
output on all such failures
([status contract](p5-device-multibounce-general-queue.md:183)).

For `calculateFlux()`, a failed frontier run must leave the prior mesh, flux
labels, output count, and model logs unchanged; only a fully staged successful
result may be appended to the diagnostic disk mesh. For `apply()`, a failed
source-flux stage must return `FAILURE` through `PROCESS_CHECK` before velocity
or advection publication. Any later Level Set failure must retain the existing
strict rollback contract: the pre-transaction sparse topology, values, width,
and point data are restored in place
([accepted rollback evidence](vulkan-compute-acceleration-status.md:945)).
The existing `void apply()` API remains unchanged; callers inspect
`getLastProcessResult()` after the call ([result](../../include/viennaps/process/psProcess.hpp:200)).

## 7. Resource caps, session loss, and status

Every integration call must carry finite, explicit `maxTotalEvents`,
`maxFrontierEvents`, and `maxRounds`, plus the existing reflection and boundary
limits. Reject zero/over-limit values, stride/size multiplication overflow,
invalid IDs, stale generation, and non-finite input before dispatch. During
dispatch, status must distinguish malformed wire, identity mismatch, invalid
hit, decision mismatch, limit violation, queue overflow, non-finite arithmetic,
session loss, and timeout. Queue counts and records remain fixed-width and
non-aliasing; temporary buffers belong to the current `ComputeSession` only.

On any non-zero status, fence timeout, device reset/loss, or generation mismatch:

1. stop before terminal output download/publication;
2. discard temporary frontiers and staged accumulations;
3. preserve caller sentinels and Process/Level Set state;
4. let Auto select the CPU stage for the same request, or let Manual return a
   non-throwing explicit failure.

The current queue's finite shape and status checks are the implementation
baseline ([limits and validation](../../gpu/vulkan/ray/multibounce_frontier_queue.cpp:120),
[status-before-publish](../../gpu/vulkan/ray/multibounce_frontier_queue.cpp:215)).

## 8. Auto, Manual, and matrix boundary

Auto is allowed to use a frontier only when the full row predicate, deployment
profile, CPU differential, resource budget, and session are valid. Any stage
failure or unsupported semantic falls back to the linked CPU engine before
publication. Manual Vulkan is an explicit request: unsupported predicates,
missing profile/session, queue status, or incomplete model semantics return a
diagnosable failure and publish no flux, geometry, metadata, or partial state.
Manual CPU remains a direct CPU bypass.

The bounded implementation now exercises the runtime rule: after a successful
Vulkan step, a test callback resets the deployment session; the stale frontier
fails before publication and Auto retries through CPU. The focused Intel Arc
smoke reports `totalRelDiff=0,maxRelDiff=0`, preserves the Manual unprepared
sentinel, and its paired route CTest passes. The baseline single-bounce
dispatch path also uses the same staged CPU retry when `allowCpuFallback` is
true; Manual remains fail-closed.

This is the same fallback rule used by the inventory
([fallback rules](p5-model-matrix-inventory.md:154)); it does not turn a
successful compute primitive into a model row. The only currently eligible
model row remains the strict single-bounce row described in §1; the bounded
one-reflection route is an implementation slice over that same row, not a new
model row. All other inventory families remain CPU fallback/Manual fail-closed,
including `MultiParticleProcess`,
`NeutralTransport`, ion/plasma families, ALD/TEOS/PECVD, wet etch, selective
epitaxy, oxide regrowth, and oxidation
([matrix](p5-model-matrix-inventory.md:56)). The strict baseline and bounded
one-reflection extension are narrow ray slices, not complete model support;
surface integration remains blocked by the Release paired NeutralTransport
CPU oracle.

## 9. True acceptance gates

The generic integration card is not accepted until every gate below is
independently green. The bounded one-reflection implementation is accepted
narrowly; these gates remain the non-expansion and aggregate-support barrier:

1. **Independent CPU oracle.** Build a test-only emitter separately against
   `D:\Codex_lib\code_reference\ViennaPS` and this Mod tree. It must record at
   least three ordered frontiers, callback order, hit identity/geometry,
   successor vectors, weights, termination reason, RNG stream/draw ordinals,
   per-label accumulation, and status. Fixed FP32 fields compare by raw bits;
   conservation and geometry compare under a named, row-specific tolerance.
   The accepted narrow oracle exercises high-weight continuation, roulette
   survival and rejection, reflection-limit termination without a roulette
   draw, and miss/backface/boundary terminals through the authorized default-off
   observer seam ([reference oracle](vulkan-compute-acceleration-status.md:3045));
   no same-header pseudo-pair or CPU reimplementation is acceptable. A future
   aggregate model row still needs its own independent CPU oracle.
2. **Real adapter.** Run the same fixture through a Release Vulkan build on a
   real Intel Arc adapter, compare the device-applied records with the CPU
   oracle, and record the adapter identity and run evidence. CPU/no-SDK output is
   only a control result, never hardware acceptance.
3. **Process seam.** The narrow implementation exercises
   `Process::calculateFlux()` through the existing override seam and the
   runtime Auto retry through `Process::apply()` after session loss. Verify
   unchanged strategy ordering, callback/RNG order, process time,
   normalisation, conservation, Level Set geometry, and model logs for the one
   explicitly admitted row; a future surface/model card must repeat this for
   its own semantics.
4. **Failure transaction.** Prove malformed input, queue overflow, stale or
   lost session, timeout, non-finite arithmetic, and Manual unsupported input
   preserve bytewise flux/mesh/metadata/Level Set sentinels. Prove Auto executes
   the CPU authority for each case where fallback is permitted.
5. **Matrix non-expansion.** Run the model-matrix fallback smoke and confirm
   exactly the previously admitted strict row is eligible; every other row
   remains CPU fallback/Manual fail-closed. No route or matrix update is implied
   by this document.

## 10. Scope, ordered exits, and handoff

### Narrow implementation checkpoint (2026-08-17)

The separately admitted `float/2D/SingleParticleProcess` one-reflection route
is implemented in `gpu/vulkan/ray/vulkan_ray_flux_engine.cpp` and exercised by
`gpu/vulkan/ray/multibounce_process_route_smoke.cpp`. It preserves CPU
particle/RNG/callback decisions, uses device triangle-hit plus frontier
application only, stages output, checks session generation, and retries the
same request through CPU in Auto after a callback-triggered session reset. The
Release Intel Arc executable reports `totalRelDiff=0,maxRelDiff=0`; the paired
route CTest passes. This checkpoint does not change the generic `runGpuPhysics`
fail-closed contract, does not add a model row, and does not satisfy the
Release NeutralTransport oracle or surface/model/deployment exit.

**Owned by this card:** this Markdown contract and its source links only.

**Prohibited:** edits to production ray/Process/model headers, CMake, shaders,
CPU helpers, reference sources, status board, model inventory, or tests; changes
to the strict predicate; generic route promotion; and presenting host frontier
orchestration as device physics.

**Ordered exits for the later implementation card:**

1. Review and freeze the admitted row, frontier ABI/status bits, resource caps,
   CPU callback/RNG order, and independent RED oracle.
2. Implement the smallest FluxEngine injection that stages completed decisions
   and leaves Process/FluxStrategy order unchanged.
3. Run the paired reference-vs-Mod CPU oracle, focused malformed/overflow/
   session-loss negatives, SPIR-V validation, and the focused CTest gate.
4. Run the Release Intel Arc Process `calculateFlux()`/`apply()` differential,
   conservation and geometry checks; record Auto fallback and Manual failure.
5. Only after all gates pass may a coordinator create a separate model-row or
   deployment-promotion card. This contract itself never changes support.

**Acceptance scenario.** On the one separately admitted row, a fixed-seed
`Process::calculateFlux()` and one `Process::apply()` run first complete the
independent reference/Mod CPU trace, then execute the same bounded frontiers on
Release Intel Arc. Raw event/decision fields, callback and RNG ordinals,
normalised per-label flux, conservation totals, and final geometry match the
named CPU criteria. Repeating the run with malformed input, overflow, stale
session, or device loss leaves all sentinels unchanged; Auto produces the
linked CPU result and Manual returns an explicit failure with no publication.

Validation for this design document is limited to Markdown link resolution and
scoped `git diff --check`. Mainline handoff must preserve the residual claim:
the current queue applies completed host decisions only; it does not promote
generic multi-bounce Process support or change the model matrix.
