# P5 device multi-bounce general queue contract

Status: design gate (`P5-RAY-MULTIBOUNCE-GENERAL-QUEUE-DESIGN`, 2026-08-13).
This document is a contract for the next implementation card; it is not
evidence that a general device transport queue exists.

## Card brief

**Milestone.** Freeze the bounded event-queue ABI and ownership rules that can
carry an arbitrary (but explicitly capacity-bounded) number of ray events while
ViennaRay and ViennaPS remain the semantic authority.

**Predecessor.** The [state map](p5-device-multibounce-state-map.md) and the
accepted [Slice-01](../../gpu/vulkan/ray/multibounce_event.hpp:19),
[Slice-02](../../gpu/vulkan/ray/multibounce_decision_producer.cpp:55),
[Slice-03](../../gpu/vulkan/ray/multibounce_hit_decision_chain_smoke.cpp:1),
and [Slice-04](../../gpu/vulkan/ray/multibounce_two_hit_chain_smoke.cpp:1)
fixtures provide narrow ABI, producer, one-hit, and explicitly unrolled
two-hit evidence only.

**Next unlock.** `P5-RAY-MULTIBOUNCE-GENERAL-QUEUE-IMPLEMENTATION` may own
the queue buffers, descriptors, shader transitions, and a paired CPU oracle
after this contract is reviewed. It does not unlock a `Process` route or a
model-matrix row.

**Global position (about 180 tokens).** P5 moves only proved compute to Vulkan.
The CPU `TraceKernel` remains authoritative for source/particle initialisation,
sampling, boundaries, materials, callbacks, reflection, roulette, normalisation,
and `Process` publication. This card freezes a finite deterministic queue for
device hit work and complete host decisions; it never translates C++ callbacks
to GLSL or presents a host bounce loop as device physics. Events retain stable
ray/particle identity, bounce, and sequence; successors obey explicit
reflection/boundary limits. Host owns semantic state and RNG. Device owns only
immutable geometry, in-flight buffers, hit results, decision bytes, and staged
accumulation/status. Validation failure, overflow, stale session, device loss,
or non-finite data leaves caller output byte-for-byte unchanged. The next card
must pass a paired raw-bit CPU fixture and a real-adapter run. Slice-01..04 and
throughput smokes are narrow evidence, not generic multi-bounce coverage;
`Process` integration remains gated by complete CPU/model/surface evidence.

## Authority and invariants

The unmodified tree at `D:\Codex_lib\code_reference\ViennaPS` is the CPU
authority. The checked-in ViennaRay headers under
`.cpm-cache/viennaray/0fe9` are the locked dependency used for line-level
ordering. The [program intent](vulkan-program-intent-framework.md) requires
Host Canonical state, per-stage fallback, and CPU reuse for all non-compute
semantics. These invariants are mandatory:

1. A queue record is an execution state, not a second model implementation.
2. Device work may intersect immutable geometry, classify hits, compact/sort
   records, apply a fully formed host decision, and accumulate a fixed numeric
   contribution. It may not call, emulate, or reinterpret a C++ callback.
3. A single original-ray RNG stream is preserved by an explicit host draw
   ordinal. Device scheduling order never defines random order.
4. Every accepted output is published transactionally only after device status
   and session generation checks pass.
5. Capacity and maximum-reflection/boundary limits are finite and explicit;
   overflow is a failure, never an implicit drop or wrap.
6. Deterministic ordering is part of the ABI and is tested independently of
   aggregate flux.

## CPU ordered state and event identity

The CPU loop in [`TraceKernel::apply`](../../.cpm-cache/viennaray/0fe9/include/viennaray/rayTraceKernel.hpp:117)
initialises a particle-specific RNG with `tea<3>(idx, runNumber+rngSeed)`,
then carries mutable `rayWeight`, `numReflections`, `boundaryHits`, origin,
direction, and `hitFromBack`. A general wire record must therefore contain,
at minimum:

| Field | Contract |
|---|---|
| `rayId`, `particleId` | Original source identity; never regenerated after compaction. |
| `bounce` | Monotonic event generation number. It is not `reflectionCount`. |
| `sequence` | Monotonic per-ray decision/event ordinal. The total order key is `(particleId, bounce, sequence)`; ties are rejected. |
| `reflectionCount`, `boundaryHits` | Separate counters matching the CPU limits. Increment/check order is preserved. |
| origin/direction, `tNear`, `tFar` | Current trace state; successor origin/direction comes only from a host decision. |
| initial/current/next weight | Initial weight is immutable; current and next weights retain FP32 bits. |
| RNG stream key and draw ordinal | Host-owned stream identity and observable number of draws consumed. No `(ray,bounce)` reseeding. |
| hit kind, primitive/material IDs, `t/u/v`, point, normal | Explicit miss/scatter/boundary/backface/surface classification and payload. Invalid IDs are terminal failures. |
| action, termination reason, status | `continue`, `terminate`, or `rouletteReject`; diagnostics are not inferred from inactive bits. |

The first implementation may extend the existing fixed-width
`MultibounceEvent`/`MultibounceDecision` records, but must preserve their
`static_assert` size/offset checks ([event ABI](../../gpu/vulkan/ray/multibounce_event.hpp:21)).
The existing 64-byte records and two-pass `DeviceMultibounceSlice::run`
([implementation](../../gpu/vulkan/ray/multibounce_event.cpp:108)) are a
narrow predecessor, not a general capacity or arbitrary-bounce contract.

### Normative CPU transition order

For each event, the semantic producer follows the locked ViennaRay order:

1. Trace and classify miss, positive-mean-free-path scatter, boundary,
   backface, or surface. The branch order is visible in
   [`rayTraceKernel.hpp`](../../.cpm-cache/viennaray/0fe9/include/viennaray/rayTraceKernel.hpp:153).
2. On a valid surface, call `surfaceCollision`, then `surfaceReflection`,
   update `weight -= weight * sticking`, and stage the callback successor.
   The callback declarations are in
   [`rayParticle.hpp`](../../.cpm-cache/viennaray/0fe9/include/viennaray/rayParticle.hpp:22);
   the current host producer preserves this order
   ([producer](../../gpu/vulkan/ray/multibounce_decision_producer.cpp:55)).
3. If the updated weight is non-positive, terminate immediately: no
   reflection-limit increment and no roulette draw.
4. Increment `reflectionCount`; if it exceeds `maxReflections`, terminate
   without roulette. This is the order in
   [`rayTraceKernel.hpp`](../../.cpm-cache/viennaray/0fe9/include/viennaray/rayTraceKernel.hpp:309).
5. Only for the low-weight branch, consume exactly one roulette draw using the
   locked `0.1 * initialWeight` / `0.3 * initialWeight` thresholds
   ([`rejectionControl`](../../.cpm-cache/viennaray/0fe9/include/viennaray/rayTraceKernel.hpp:380)).
   A branch not taken consumes no RNG value. A surviving successor receives the
   host callback's origin and direction bytes.

GPU hit dispatch may return a `TriangleHit`/classification and device-local
queue state. The host must reconstruct the CPU point/normal/material convention,
run the callback producer, and upload an immutable decision record. A shader
only validates IDs/weights/status, copies the supplied successor, applies the
action, and writes a staged contribution. It never owns `AbstractParticle`,
`PointData`, `Source`, `Boundary`, or RNG semantics.

## Queue topology, ownership, and synchronization

The implementation shall use bounded ping-pong storage buffers and an explicit
host/device frontier. A frontier is a stable, sorted batch of events that have
the same semantic handoff boundary:

```text
Host source/CPU state + complete decisions for frontier N
        upload (generation checked)
GPU round N: apply prior decisions; hit/classify/compact/sort/reduce
        barriers between every producer/consumer stage
        read back sorted surface frontier N (or terminal records)
Host TraceKernel-order callbacks/RNG for frontier N
        upload complete decisions and next frontier N+1
GPU round N+1 ... (bounded by total events, frontier size, and max rounds)
        terminal fence -> status readback -> publish staged output only on success
```

* Host owns source geometry upload lifetime, callback state, local/global
  `PointData`, RNG, model/material semantics, decision publication, and the
  caller's output/sentinel. Device owns only buffers created in the current
  `ComputeSession` generation.
* `EventsIn` and `EventsOut` must not alias in a dispatch. A device append uses
  an atomic count plus a capacity check; a rejected append sets overflow status
  and writes no usable successor. Counts, status, and queue records use fixed
  width integer fields and explicit little-endian host packing.
* Every dispatch declares storage-buffer barriers for queue writes, hit reads,
  scan/radix reads, accumulation writes, and status writes. Descriptor sets
  include a contract/version word, generation/session token, active count,
  capacity, and limit values; values affecting branch order are push/descriptors,
  never hidden in specialisation constants.
* For every surface frontier, host executes the CPU `TraceKernel` semantic
  decision in `(particleId,bounce,sequence)` order: callbacks, all required
  RNG draws, weight/limit/roulette branches, and successor construction. GPU
  hit/classify work is therefore batched between semantic frontiers; this is
  CPU orchestration around compute, not a host reimplementation of the device
  hit kernel. A host bounce loop may not be presented as device physics or used
  to claim device-resident transport.
* A submission may fuse hit, compact, sort, reduction, and application of
  already-uploaded decisions. It must stop before a surface bounce that needs a
  new host decision; no submission may cross that boundary. Only stages with no
  new callback/RNG decision may be merged. Each run has explicit finite limits:
  `maxTotalEvents`, `maxFrontierEvents`, and `maxRounds`; exceeding any limit
  sets status and fails closed rather than silently dropping or wrapping work.
* On device loss, reset, stale generation, fence timeout, malformed descriptor,
  or status non-zero, the API returns failure and leaves the caller's output
  sentinel unchanged. Temporary buffers are discarded after the failure; no
  partial queue is published.

## Determinism and accumulation

The queue scheduler may use stable compaction or radix sorting, but it must
preserve the key `(particleId, bounce, sequence)` and reject duplicate keys.
Per-ray callback decisions remain in CPU trace order. If events from different
rays are accumulated together, the implementation must either impose an
explicit stable reduction key (for example `(surfaceId, particleId, bounce,
sequence)`) or document the exact tolerance instead of claiming raw-bit parity.
For a fixed FP32 operation sequence and stable key, the oracle requires raw
`uint32` bit equality for event weights, successor vectors, contributions, and
status. Any relaxed aggregate tolerance must be written in the card and paired
with conservation and geometry checks; statistical similarity is not parity.

## Capacity and status contract

Before dispatch, reject zero or over-limit values for `maxTotalEvents`,
`maxFrontierEvents`, or `maxRounds`, capacity above the configured finite bound,
inconsistent decision stride, stale generation, non-finite input, and
counter/size multiplication overflow. During dispatch, status bits must cover
at least: malformed wire, duplicate/mismatched identity, invalid hit, decision
mismatch, reflection/boundary-limit violation, queue overflow, non-finite
arithmetic, session loss, and timeout. Status is device-local until a terminal
fence; output download is ordered **after** status readback. Any non-zero bit
causes fail-closed return with byte-for-byte unchanged output and no `Process`
publication. A successful run returns the complete staged event/accumulation
set and a deterministic terminal count.

## Paired CPU oracle (RED before implementation)

Freeze a test-only fixture before shader changes. Run the same source and
parameters once against the unmodified reference tree
`D:\Codex_lib\code_reference\ViennaPS` and once against the Mod CPU helper.
The fixture must contain at least three ordered hits/bounces and these branches:

* one high-weight continuation;
* one low-weight roulette survival and one low-weight rejection with fixed
  seeds;
* one reflection-limit termination and one miss/backface or boundary terminal.

Capture and compare, in order: event keys and count, hit `t/index/u/v`,
primitive/material IDs, callback sequence, successor origin/direction, action,
initial/current/next weight, contribution, termination reason, RNG stream key,
draw ordinals and next-draw state, and final per-surface accumulation. Fixed
FP32 fields compare by raw bits. The fixture must explicitly assert that
pre-limit and non-low-weight branches consume no roulette draw, while survival
and rejection consume exactly one. Invalid input, overflow, stale session, and
lost-session cases must preserve sentinels in both CPU and device-facing APIs.

After RED passes, run the same fixture through the real Vulkan adapter and
compare device event/decision application against the CPU trace. A CPU/no-SDK
run is only a control result, never hardware evidence. The oracle must remain a
test fixture; it must not replace production ViennaRay callbacks.

## Process integration prerequisites (not unlocked by this card)

`Process` integration is blocked until all of the following are independently
accepted:

1. The generic queue ABI, shader transitions, capacity/status rollback, and
   paired raw-bit CPU differential pass on a real adapter.
2. CPU decision production covers every admitted particle/source/boundary/
   material predicate, with a Release reference-vs-Mod oracle. Unsupported
   predicates remain CPU fallback or explicit Manual failure.
3. `FluxProcessStrategy::processTimeStep` ordering and `Process::apply`
   publication remain unchanged ([strategy](../../include/viennaps/process/psFluxProcessStrategy.hpp:309),
   [transaction](../../include/viennaps/process/psProcess.hpp:204)); device output
   is injected only at the existing FluxEngine seam.
4. SurfaceModel callbacks, coverage, diffusion, neutral velocity, normalization,
   LevelSet/Advect, and material mapping have their own CPU/conservation/geometry
   evidence. No shader callback translation is accepted as a substitute.
5. The strict eligibility predicate, CPU fallback, Manual fail-closed path,
   model matrix, deployment profile, and remote/release gates remain closed
   until their cards pass. This design does not alter any of them.

## Scope, ordered exits, and handoff

**Owned by this card:** this contract document and its links. A line correction
is allowed only if the mainline review identifies a concrete evidence mismatch.

**Prohibited:** production ray/Process/model/header/reference edits, CMake or
shader changes, generic route/eligibility promotion, and a host bounce loop
disguised as device dispatch.

**Ordered exits for the implementation card:**

1. Review this contract and freeze the std430 ABI, status bits, limits, and RED
   fixture.
2. Build the smallest queue implementation with transactional sentinels and
   no Process route changes.
3. Run paired reference/Mod CPU raw-bit oracle, malformed/overflow/lost-session
   negatives, `spirv-val`, focused CTest, and a real Intel Arc adapter run.
4. Only after those exits may the coordinator create a separate Process/model
   integration card; this card itself remains design-only.

Validation for this document is limited to Markdown link resolution and
`git diff --check`. Mainline acceptance must record exact commands and preserve
the residual statement: Slice-01..04 are narrow evidence, not generic queue or
multi-bounce Process support.
