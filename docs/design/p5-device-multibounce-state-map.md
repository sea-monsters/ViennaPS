# P5 device multi-bounce state map

Status: design/evidence map only (`P5-RAY-MULTIBOUNCE-STATE-MAP`,
2026-08-12). This file does not add a Vulkan route, change a public API, or
claim that device multi-bounce transport is implemented.

The governing invariant is the P5 split: Vulkan may replace a proved compute
operation, while ViennaPS `Process`, model callbacks, CPU ray sampling and
reflection semantics, surface physics, normalization, and level-set/advection
transactions remain the authority. The current negative contract is therefore
correct: [`DeviceRayFluxPipeline::runGpuPhysics`](../../gpu/vulkan/ray/device_ray_flux_pipeline.cpp:385)
returns before validation or dispatch, and
[`DeviceRayFluxPipeline::devicePhysicsGap`](../../gpu/vulkan/ray/device_ray_flux_pipeline.hpp:71)
requires an event queue, reflection, roulette, and material state.

## Evidence boundary

The map is derived from the following current sources; no behavior is inferred
from the existence of a CUDA implementation or from a throughput smoke.

| Concern | Current authoritative evidence |
|---|---|
| CPU Process order and transaction boundary | `FluxProcessStrategy::processTimeStep` prepares advection, updates the surface, calculates source/surface flux, applies coverage/diffusion/velocity, invokes pre/post callbacks, and finally calls `performAdvection` ([`psFluxProcessStrategy.hpp`](../../include/viennaps/process/psFluxProcessStrategy.hpp:309)). `Process::apply` publishes only after `strategy->execute` returns ([`psProcess.hpp`](../../include/viennaps/process/psProcess.hpp:204)). |
| CPU ray configuration | `CPUTriangleEngine::initialize` maps boundaries, chooses source direction, sets rays/limits/seeds/reflections, and accepts a custom source or primary direction ([`psCPUTriangleEngine.hpp`](../../include/viennaps/process/psCPUTriangleEngine.hpp:37)). |
| CPU geometry/material mapping | `CPUTriangleEngine::updateSurface` builds the surface mesh, converts 2-D lines to triangle ribbons, maps point material IDs to element IDs, and passes geometry/materials to ViennaRay ([`psCPUTriangleEngine.hpp`](../../include/viennaps/process/psCPUTriangleEngine.hpp:89)). |
| CPU event loop | ViennaRay `TraceKernel::apply` owns the per-ray state and `do/while` transition loop ([`rayTraceKernel.hpp`](../../.cpm-cache/viennaray/0fe9/include/viennaray/rayTraceKernel.hpp:117)). |
| CPU particle semantics | `AbstractParticle` defines `initNew`, `initNewWithDirection`, `surfaceCollision`, and `surfaceReflection` ([`rayParticle.hpp`](../../.cpm-cache/viennaray/0fe9/include/viennaray/rayParticle.hpp:22)). The concrete model callback is authoritative; for example `SingleParticle` uses diffuse reflection ([`psSingleParticleProcess.hpp`](../../include/viennaps/models/psSingleParticleProcess.hpp:50)) and `NeutralTransportParticle` derives sticking from material/coverage before diffuse reflection ([`psNeutralTransport.hpp`](../../include/viennaps/models/psNeutralTransport.hpp:612)). |
| Current device chain | `DeviceRayFluxPipeline::runGpu` records triangle hit, compaction, radix sort, and surface reduction into one command submission, then checks status before terminal readback ([`device_ray_flux_pipeline.cpp`](../../gpu/vulkan/ray/device_ray_flux_pipeline.cpp:193)). No event state is produced. |
| Existing CPU-only helper contracts | `ray_event_queue.hpp` (`RayEventQueue::push/pop`), `ray_reflection.hpp`, `ray_roulette.hpp`, and `ray_surface_response.hpp` are CPU contracts only; their acceptance explicitly says no device shaders or end-to-end multi-bounce validation ([status board](vulkan-compute-acceleration-status.md:2084)). The queue's `(particle,bounce,sequence)` ordering is a proposed deterministic wire order, not evidence that `TraceKernel` already uses a host queue. |

## CPU ordered event state

The following fields are the minimum state needed to replay the CPU loop. A
field is not optional merely because a current fixture does not use it.

| Field | CPU source and meaning | Device representation requirement |
|---|---|---|
| `rayId` / `particleId` | `idx` identifies the source ray; `particleIdx` is the model particle loop index. The CPU loop runs each ray to termination before the next ray ([`rayTraceKernel.hpp`](../../.cpm-cache/viennaray/0fe9/include/viennaray/rayTraceKernel.hpp:117)). | `uint` fields; preserve ray identity through every queue, compaction, sort, and reduction record. |
| `bounce` / `reflectionCount` | `numReflections` starts at zero and is incremented after surface response, before the max-reflection check ([`rayTraceKernel.hpp`](../../.cpm-cache/viennaray/0fe9/include/viennaray/rayTraceKernel.hpp:124)). | Separate `bounce` (event generation order) and `reflectionCount` (CPU limit); do not use one counter for both. |
| `boundaryHits` | Incremented on each front-face boundary hit; exceeding `maxBoundaryHits` terminates the ray ([`rayTraceKernel.hpp`](../../.cpm-cache/viennaray/0fe9/include/viennaray/rayTraceKernel.hpp:205)). | `uint`, checked before enqueueing a successor. |
| `origin`, `direction`, `tNear`, `tFar` | Source and every successor update the origin/direction; each trace resets `tfar`, while `fillRayPosition` supplies the CPU epsilon ([`rayTraceKernel.hpp`](../../.cpm-cache/viennaray/0fe9/include/viennaray/rayTraceKernel.hpp:135)). | Two `vec4` values (`originNear`, `directionFar`) with FP32 bit-preserving validation. |
| `initialWeight`, `weight` | Initial value comes from `Source::getInitialRayWeight`; current weight is reduced by sticking before reflection/roulette ([`rayTraceKernel.hpp`](../../.cpm-cache/viennaray/0fe9/include/viennaray/rayTraceKernel.hpp:123)). | Store both as `uint` bit patterns or validated `float`; never reconstruct `initialWeight` from the mutable weight. |
| `rngSeed`, `rngCounter` / draw ordinal | CPU derives one independent stream with `tea<3>(idx, runNumber + rngSeed)` ([`rayTraceKernel.hpp`](../../.cpm-cache/viennaray/0fe9/include/viennaray/rayTraceKernel.hpp:100)). | A per-ray stream key plus an explicit draw ordinal. A single shared device RNG is not compatible. |
| `hitKind` and hit payload | Miss, scattering event, boundary, backface, and surface hit take different branches. Surface hits need `t`, primitive ID, barycentrics, hit point, and geometric normal ([`rayTraceKernel.hpp`](../../.cpm-cache/viennaray/0fe9/include/viennaray/rayTraceKernel.hpp:171)). | `uint hitKind`, `uint primitiveId`, `float t/u/v`, `vec3 hitPoint`, `vec3 normal`; invalid/miss IDs must be explicit. |
| `materialId` | Surface callbacks receive the primitive material ID ([`rayParticle.hpp`](../../.cpm-cache/viennaray/0fe9/include/viennaray/rayParticle.hpp:45)). | Device material buffer plus the material ID copied into each event; out-of-range IDs fail closed. |
| `surface/global/local` state | `surfaceCollision` updates per-particle local data; `surfaceReflection` reads model/global data and returns sticking plus a new direction ([`rayParticle.hpp`](../../.cpm-cache/viennaray/0fe9/include/viennaray/rayParticle.hpp:44)). | Model-specific read-only parameter/coverage buffers and per-particle accumulation buffers. Arbitrary C++ callbacks cannot be silently translated. |
| `meanFreePath` and `hitFromBack` | A positive mean free path inserts a scattering branch before boundary/surface handling; disk backface state is separate from triangle backface rejection ([`rayTraceKernel.hpp`](../../.cpm-cache/viennaray/0fe9/include/viennaray/rayTraceKernel.hpp:178)). | Carry both values even if the first accepted slice fixes `meanFreePath < 0` and triangle geometry. |
| `sequence` / `termination` | CPU helper `EventQueue` orders `(particle,bounce,sequence)`; the trace loop also needs an explicit termination reason for diagnostics and fail-closed status. | Use a bounded 32-bit event ordinal (or a two-word 64-bit value) and a status/termination enum; the current CPU `uint64_t` sequence cannot be copied to GLSL as an implicit scalar. |

A possible std430 wire record for the first device slice is therefore:

```text
uvec4 originNearBits;       // xyz = origin, w = tNear
uvec4 directionFarBits;     // xyz = direction, w = tFar
uvec4 identity;             // rayId, particleId, bounce, sequence
uvec4 counters;             // reflectionCount, boundaryHits, hitKind, status
uvec4 weights;              // initialWeightBits, weightBits, rngKey, rngDraw
uvec4 hit;                  // primitiveId, materialId, tBits, barycentricBits
vec4  hitPoint;
vec4  normal;
```

The exact ABI must be frozen in a shader/header differential test before a
production implementation. The existing [`RayRecord`](../../gpu/vulkan/ray/ray_record_compaction.hpp:18)
is only a 16-byte `(rayId,surfaceId,weightBits,reserved)` reduction record and
must not be reused as an event record.

## Ordered transitions

The order below is normative. A device implementation may fuse stages, but it
must produce the same accepted/rejected event sequence and the same per-ray RNG
draw order.

| Step | CPU transition | Device work that is safe to migrate | Must remain host/model-owned for the first slice |
|---|---|---|---|
| 0. Seed/init | Compute `seed = runNumber + rngSeed`, then `tea<3>(rayId, seed)`, call `particle->initNew`, `initNewWithDirection`, and `source->getOriginAndDirection` ([`rayTraceKernel.hpp`](../../.cpm-cache/viennaray/0fe9/include/viennaray/rayTraceKernel.hpp:100)). | Upload a deterministic event seed/initial ray buffer; later, a device counter-based stream can be added only with a paired bitwise draw fixture. | Arbitrary source and particle virtual calls; custom source, primary direction, and model initialization. |
| 1. Trace | Intersect current ray with boundary and geometry; no hit terminates ([`rayTraceKernel.hpp`](../../.cpm-cache/viennaray/0fe9/include/viennaray/rayTraceKernel.hpp:153)). | Triangle/BVH traversal, hit selection, and miss classification. The existing `triangle_hit_device.comp` proves only one hit from immutable input rays ([`triangle_hit_device.comp`](../../gpu/vulkan/ray/shaders/triangle_hit_device.comp:52)). | General boundary geometry until its condition/ID/orientation contract is encoded and differentially tested. |
| 2. Scatter | If `lambda > 0`, draw a random value before boundary/surface handling; a scatter changes origin and direction and re-enters the trace loop without a reflection increment ([`rayTraceKernel.hpp`](../../.cpm-cache/viennaray/0fe9/include/viennaray/rayTraceKernel.hpp:178)). | A dedicated scatter transition can run on device once RNG and mean-free-path arithmetic have an exact fixture. | First slice fixes `lambda < 0`; no scatter is admitted. |
| 3. Boundary | Increment `boundaryHits`; `Boundary::processHit` performs pass-through, reflective, periodic, or ignore behavior ([`rayBoundary.hpp`](../../.cpm-cache/viennaray/0fe9/include/viennaray/rayBoundary.hpp:29)). | A bounded analytic boundary kernel with explicit condition/normal/axis buffers. | First slice uses a source/geometry fixture with no boundary hit, or host boundary reconstruction as the current route does ([`vulkan_ray_flux_engine.cpp`](../../gpu/vulkan/ray/vulkan_ray_flux_engine.cpp:281)). |
| 4. Backface | Triangle backfaces terminate; disk backfaces can pass once and then terminate on the second back hit ([`rayTraceKernel.hpp`](../../.cpm-cache/viennaray/0fe9/include/viennaray/rayTraceKernel.hpp:223)). | Triangle front-face predicate for the first slice. | Disk-specific `hitFromBack` and general 3-D/2-D matrix remain outside the first slice. |
| 5. Surface collision | Call `surfaceCollision` before calculating reflection. The local contribution uses the current weight and primitive/material ID ([`rayTraceKernel.hpp`](../../.cpm-cache/viennaray/0fe9/include/viennaray/rayTraceKernel.hpp:301)). | Device append/aggregate of a fixed model's local contribution. | The CPU model callback and arbitrary `PointData` layout; first slice may upload a precomputed callback response. |
| 6. Surface response | Call `surfaceReflection`; it returns sticking and successor direction. Then update `weight -= weight * sticking` ([`rayTraceKernel.hpp`](../../.cpm-cache/viennaray/0fe9/include/viennaray/rayTraceKernel.hpp:309)). | FP32 weight update, successor origin at hit point, and a fixed reflection law after its shader/CPU differential. | Model-specific sticking, coverage lookup, and reflection sampling remain CPU-canonical. `NeutralTransportParticle` demonstrates why material and global coverage are semantic inputs ([`psNeutralTransport.hpp`](../../include/viennaps/models/psNeutralTransport.hpp:619)). |
| 7. Limit/roulette | If weight is non-positive, terminate; increment reflections and terminate when `numReflections > maxReflections`; only then run roulette ([`rayTraceKernel.hpp`](../../.cpm-cache/viennaray/0fe9/include/viennaray/rayTraceKernel.hpp:315)). | Device counter checks and the `0.1 * initialWeight` / `0.3 * initialWeight` roulette arithmetic. | RNG stream and model callback must be supplied by the paired oracle until exact draw ordering is proven. |
| 8. Enqueue successor | If roulette continues, write hit-point origin and reflected direction and repeat the `do/while` ([`rayTraceKernel.hpp`](../../.cpm-cache/viennaray/0fe9/include/viennaray/rayTraceKernel.hpp:325)). | Device event queue append/consume, bounded capacity, stable ordering, and barriers. | Queue overflow must fail closed; no host loop may masquerade as device multi-bounce. |
| 9. Aggregate/normalize | CPU merges local data logs, normalizes by source/area, and post-processes element-to-point data ([`psCPUTriangleEngine.hpp`](../../include/viennaps/process/psCPUTriangleEngine.hpp:266)). | Associative per-surface accumulation only after an accepted ordering/FP32 policy. | `Process`, `SurfaceModel`, normalization policy, and advection transaction remain host-owned. |

### Reflection and roulette ordering rule

The random stream is per original ray, not per queue worker. For a fixed
`(runNumber, rngSeed, rayId)`, the device must consume random values in this
exact sequence: particle initialization, source sampling, optional scattering,
surface-collision callback, surface-reflection callback, then roulette. A
reflection event must not reseed from `(rayId,bounce)` and must not consume a
roulette draw before the max-reflection check. If a branch is not taken, its
draw is not consumed. This follows the CPU statements at
[`rayTraceKernel.hpp`](../../.cpm-cache/viennaray/0fe9/include/viennaray/rayTraceKernel.hpp:119),
[`rayTraceKernel.hpp`](../../.cpm-cache/viennaray/0fe9/include/viennaray/rayTraceKernel.hpp:178),
and [`rayTraceKernel.hpp`](../../.cpm-cache/viennaray/0fe9/include/viennaray/rayTraceKernel.hpp:309).

Because `std::uniform_real_distribution` and ViennaRay's RNG state are part of
the CPU contract, a device counter RNG is not equivalent merely because it is
statistically similar. Until a raw-bit draw fixture passes, the first slice
must upload host-produced callback/reflection/roulette decisions and use the
device only for event-state transitions and bounded queue processing.

## Device migration partition

### Candidate for device residency

1. Current ray/event buffers and immutable triangle/BVH geometry.
2. One trace/classify dispatch per active event batch.
3. Stable compaction and `(surfaceId, rayId, sequence)` ordering using the
   existing scan/radix primitives. The current shaders already establish the
   descriptor style: triangle hit uses four storage buffers and two push words
   ([`triangle_hit_device.cpp`](../../gpu/vulkan/ray/triangle_hit_device.cpp:52));
   compaction uses five storage buffers and three push words
   ([`ray_record_compaction.cpp`](../../gpu/vulkan/ray/ray_record_compaction.cpp:48));
   sort uses three storage buffers and one push word
   ([`ray_record_sort.cpp`](../../gpu/vulkan/ray/ray_record_sort.cpp:47)).
4. Surface-event accumulation and strict-FP32 status propagation, reusing the
   current status-before-readback transaction pattern
   ([`ray_surface_reduce.comp`](../../gpu/vulkan/ray/shaders/ray_surface_reduce.comp:23)).

### Host/model ownership that must not be silently migrated

1. `Process`/`FluxProcessStrategy` setup, callbacks, coverages, diffusion,
   velocity calculation, level-set update, and advection. The CPU loop's
   ordering is explicit at [`psFluxProcessStrategy.hpp`](../../include/viennaps/process/psFluxProcessStrategy.hpp:309).
2. Arbitrary `AbstractParticle` virtual calls, custom `Source`, particle data
   logs, and model-specific surface/coverage/material equations.
3. CPU normalization and element-to-point post-processing until a paired
   differential proves the device accumulation order and geometry convention.
4. Fail-closed publication and rollback: an unsupported event, queue overflow,
   non-normal FP32 value, stale descriptor/session, or status bit must leave
   caller output and `Process` state unchanged.

## SPIR-V and descriptor contract needed for implementation

The first implementation card must freeze these contracts before shader work:

- **Event input/output:** two non-aliasing storage buffers (`EventsIn`,
  `EventsOut`) with a device `uint` active count and a separate overflow/status
  word. `EventsOut` capacity is bounded before dispatch; append uses an atomic
  count and rejects overflow without partial publication.
- **Geometry:** immutable triangle/BVH buffers and a per-primitive material ID
  buffer. Normals may be supplied by geometry or derived only if the CPU
  normal convention is bitwise covered.
- **Surface/model inputs:** fixed-slice sticking/reflection/coverage buffers,
  particle parameters, and per-particle local-data output. Arbitrary pointers
  or callable addresses are prohibited in the SPIR-V ABI.
- **Randomness:** a per-ray stream key and draw ordinal/counter buffer. The
  descriptor must make the consumed draw count observable in the oracle.
- **Push constants:** `activeCount`, `eventCapacity`, `triangleCount`,
  `bounce`, `maxReflections`, `maxBoundaryHits`, `dimension`, `mode`, and a
  contract/version word. Values affecting branch order must be explicit, not
  hidden in specialization constants.
- **Barriers/submission:** every dispatch declares storage-buffer barriers for
  queue writes, scan/radix reads, and status writes. A one-submit composition is
  desirable but not an acceptance criterion until queue overflow and status
  rollback are covered.
- **ABI validation:** `spirv-val --target-env vulkan1.2`, C++ `static_assert`
  sizes/offsets, and a shader/header byte-layout test are mandatory. The
  existing 16-byte `RayRecord` ABI cannot represent this state.

## Strict first slice and CPU oracle fixture

The first implementation slice should be **device-level and deliberately
narrow**, not a Process eligibility expansion:

`P5-RAY-MULTIBOUNCE-SLICE-01`

- Fixed FP32, fixed dimension, one particle, one particle-data label, no
  positive mean free path, no custom source, no coverage callback, and a finite
  `maxReflections` of one. Keep Process routing on the existing CPU fallback
  until this slice and the later model/surface cards pass.
- Use an explicit two-surface triangle fixture with a deterministic source ray
  that produces a first surface hit and a reflected successor hit. Run one
  high-weight case (roulette not entered) and one low-weight case (roulette
  entered) with a fixed seed. Material IDs must be uploaded and echoed in the
  event trace even if the first particle law is material-independent; a
  material-dependent model is a separate acceptance gate.
- Preserve CPU ViennaRay sampling and callback decisions in the first cut. The
  device must execute event state transitions, queue append/consume, hit
  classification, reflection-count ordering, application of the host-produced
  roulette/reflection decisions, and surface accumulation. It must not sample
  or reinterpret a C++ callback on its own. This satisfies the P5 principle
  that only a suitable computation moves while CPU semantics remain
  authoritative.
- The oracle is a paired reference/Mod fixture, not a final-flux-only smoke.
  Start from the fixed-seed `maxReflections=1` CPU oracle already present in
  [`ray_flux_process_route_smoke.cpp`](../../gpu/vulkan/ray/ray_flux_process_route_smoke.cpp:250),
  whose CPU result is also used to verify Auto fallback
  ([`ray_flux_process_route_smoke.cpp`](../../gpu/vulkan/ray/ray_flux_process_route_smoke.cpp:296)).
  Extend the test-only fixture to emit raw FP32 bits for every event state,
  draw ordinal, termination reason, per-surface contribution, and final
  normalized flux. Compare the unmodified reference tree and Mod executable;
  do not weaken the oracle to accommodate the device.
- Acceptance requires: identical event count and ordered `(rayId,bounce,
  sequence)` keys; identical seed/draw ordinals; identical hit primitive and
  material IDs; exact FP32 bits for fixed arithmetic and final flux; no queue
  overflow/status bit; unchanged output sentinels on any rejected request; and
  a real Vulkan adapter run. A CPU/no-SDK pass is not hardware acceptance.

This slice does **not** unlock `VulkanRayFluxEngine::checkInput`,
`P5-MODEL-MATRIX`, automatic backend promotion, or a release claim. Those
remain gated by the existing route predicate, surface/model oracle, and
deployment cards.

## Risks and next implementation card

| Risk | Why it is real | Required response |
|---|---|---|
| RNG drift | CPU uses `tea<3>` plus a mutable `std::uniform_real_distribution`; device statistical equivalence is insufficient. | Freeze draw order/bits first; keep host decisions in Slice-01 if needed. |
| Queue growth/DoS | Boundary and reflection loops are unbounded in the logical model and bounded only by CPU limits. | Precompute a conservative capacity, use atomic overflow status, and fail closed before publication. |
| Callback mismatch | `surfaceReflection` and `surfaceCollision` are virtual C++ model code, not a generic scalar formula. | Admit only one explicitly encoded particle law; all other models stay CPU fallback. |
| FP32 accumulation order | CPU thread-local data is merged after tracing; device atomics or reordered events can change bits. | Compare event order and raw bits; do not claim aggregate parity from a tolerant norm. |
| Boundary/backface divergence | Current Vulkan route reconstructs only a host lateral box boundary and supports a narrow triangle slice. | Keep boundary/no-scatter/no-disk assumptions explicit in Slice-01. |
| Process rollback | `Process::apply` and advection publish state after a sequence of callbacks. | Keep device output transactional and leave Process/Advect host-owned. |

Recommended next card: **`P5-RAY-MULTIBOUNCE-SLICE-01-IMPLEMENTATION`**. It owns
only the new event-state shader/API and its focused device-level paired oracle;
it must not edit `Process`, `FluxProcessStrategy`, CPU ray helpers, CMake model
eligibility, or the model matrix. Its exits are: freeze the std430 ABI and RED
oracle, implement bounded queue + one-reflection transition, run GREEN on a
real Vulkan adapter, run the paired reference/Mod raw-bit differential, and
demonstrate fail-closed overflow/status rollback. A second failed correction is
reclaimed to the mainline; no Process-route expansion is unlocked by a mere
throughput pass.
