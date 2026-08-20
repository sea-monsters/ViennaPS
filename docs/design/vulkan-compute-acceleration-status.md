# Vulkan compute acceleration implementation status

This file records verified implementation slices. It complements, and does not
replace, the accepted development report and ADR.

## Hard requirement: reuse CPU path outside compute

Vulkan migration accelerates **compute ops only**. Non-compute behavior must
**reuse or remain compatible** with the pre-migration CPU route
(`code_reference/ViennaPS`) so numerical consistency and backward compatibility
hold. Empty executors / Manual CPU must match the original CPU path. Intentional
departures need a card ID, a documented diff vs the reference tree, and
regression tests.

Authoritative wording:
[intent whitepaper §2.2 item 8](vulkan-program-intent-framework.md) and
[development report §12](vulkan-compute-acceleration-development-report.md).

### P0–P4 / PD0–PD4 compliance audit — Round 1 (2026-08-05)

**Formal ledger:**
[p0-p4-cpu-reuse-audit-round1.md](p0-p4-cpu-reuse-audit-round1.md)
(`RECORDED`; R1-F1 / R1-F2 closed by 2026-08-05 remediation and local CPU/Vulkan smoke revalidation, R1-F3 remains a
recommendation).

| Area | Verdict | Note |
|---|---|---|
| Control plane, runtime, primitives | Compliant | Selection + kernels; no Process fork |
| Surface coverage/diffusion/neutral seams | Compliant | Managers/strategies reused; empty executor = CPU |
| `psCPU*Engine` / default Advect | Compliant | Unchanged production defaults |
| LS update via Advect executors | Mostly compliant | Empty = original Advect |
| HRLE rebuild ports (`psHrleRebuild*`) | **Closed** | Frozen semantic mirror of ViennaLS 5.8.5 `rebuildLS`; `hrleRebuildCpuFixture` provides Advect-CPU differential oracle |
| P4 ray device chain | Partial (acceptable) | Kernel `runCpu` only; production flux still `psCPU*` |
| **P3K `AdvectionHandler::performAdvection`** | **Closed** | Executor-active-only fail-closed; empty executor / Manual CPU restored to reference 4.6.2 behavior |

Follow-ups (tracked as R1-F1…F3 in the Round 1 ledger):

- **R1-F1** — Closed. HRLE rebuild files carry frozen-mirror labels; differential
  fixture `hrleRebuildCpuFixture` executes the mirror through the Advect rebuild
  callback, asserts that callback dispatch occurred, compares canonical HRLE and
  PointData output with the unmodified CPU path, and must be run in CI and
  re-baselined on intentional upstream `rebuildLS` changes.
- **R1-F2** — Closed. `AdvectionHandler::performAdvection` and the ViennaLS patch
  gate P3K fail-closed behavior on `hasLevelSetExecutors()`; no-executor integration
  keeps the original update/rebuild sequence, while executor-active time errors roll
  the complete multi-step snapshot back before returning failure. Legacy tests assert
  reference CPU semantics.
- **R1-F3** — Remains an open recommendation for P5+ ray Process routing: prefer
  ViennaRay / `CPUTriangleEngine` host helpers to reduce normalization duplicates.

## S1: capability gate, real dispatch, and CPU oracle

- Status: accepted locally
- Date: 2026-08-01
- Scope: P1A plus the independently testable part of P1B
- Git baseline: recorded by the commit containing this status entry

### Delivered contracts

- CUDA and Vulkan are independent build switches.
- Vulkan SDK absence produces a runnable diagnostic probe stub instead of a
  configure or compile failure.
- A real Vulkan device probe records device and driver UUIDs, the selected
  compute queue, device limits, precision features, ray tiers, and extensions.
- Auto/manual backend policy rejects unknown memory budgets, rejects silent ray
  tier downgrade in manual mode, and falls back only the FP64 stage when Vulkan
  FP64 is unavailable.
- Capability records have a versioned schema, strict required fields, corrupt
  input diagnostics, hardware fingerprint invalidation, and temporary-file
  replacement on write.
- A deterministic two-dimensional ViennaLS circle-advection CPU oracle is
  frozen for FP32 and FP64. The higher-level trench/process oracle remains a P3
  integration gate because it also requires ViennaCS.
- An existing VTK source tree can be supplied through the
  `VIENNAPS_VTK_SOURCE_DIR` environment variable. No machine-specific SDK or
  library path is committed.

### Verified local device facts

| Fact | Result |
|---|---|
| Device | Intel Arc integrated GPU |
| Vulkan API | 1.4.348 |
| Selected compute queue | family 1, dedicated compute |
| Max workgroup invocations | 1024 |
| `shaderFloat64` | false |
| Ray query / RT pipeline | true / true |
| Policy consequence | Vulkan FP32 eligible after suite/budget gates; FP64 oxidation stays on CPU |

### Acceptance evidence

| Gate | Result |
|---|---|
| Vulkan compute smoke | 16 FP32 values for `y = 2x + 1`; all exact, 0 ULP |
| Full device probe | pass; non-empty device/driver UUID; dedicated queue selected |
| No-SDK probe | builds and exits 0 with `status=disabled` |
| Backend policy | 11 focused cases pass under MSVC C++20 `/W4` |
| Capability profile I/O | 7 focused cases pass, including round-trip and malformed input |
| CPU Level Set FP32 | 92 active points, 68 surface nodes/lines, fingerprint `0x51052040fecec990` |
| CPU Level Set FP64 | 92 active points, 68 surface nodes/lines, fingerprint `0x6af00ac25d8971d0` |
| Local VTK source override | root configure recognizes the local source and ViennaLS reuses its targets |
| Local ViennaLS source override | root configure consumes `VIENNAPS_VIENNALS_SOURCE_DIR` through CPM without tracking its resolved path |

The machine-local build uses environment variables, for example:

```bat
set VULKAN_SDK=<local Vulkan SDK>
set VIENNAPS_VTK_SOURCE_DIR=<local VTK source tree>
set VIENNAPS_VIENNALS_SOURCE_DIR=<local ViennaLS source tree>
set CPM_SOURCE_CACHE=<local dependency cache>
cmake -S . -B build -G Ninja -DVIENNAPS_ENABLE_VULKAN=ON
```

These values live only in the process environment and ignored CMake cache.
The same rule applies to external comparison trees used during development:
automation may read them through environment variables such as
`VIENNAPS_MPROCESS_SOURCE_DIR`, but neither their resolved value nor a
machine-local fallback path may enter tracked CMake, presets, tests, or docs.
The normal ViennaLS dependency remains the versioned remote declared in
`CMakeLists.txt`; `VIENNAPS_VIENNALS_SOURCE_DIR` only supplies an optional
local-development override through the ignored build cache. An explicit
`CPM_ViennaLS_SOURCE` CMake argument retains higher priority for CI and
packaging.

### Open exits before S2

- Bootstrap the exact CPM version requested by ViennaHRLE without relying on a
  blocked network path.
- Supply or fetch ViennaCS 2.0.1, then run the root CTest targets and the
  higher-level trench/process CPU oracle.
- Convert raw probe output into the persisted capability record and add safe
  working-set calibration. Until that succeeds, Auto fails closed because the
  Vulkan memory budget is unknown.
- Expand the single compute smoke into the P2 primitive differential suite:
  fill/copy, reduction, scan, compact, radix sort, gather/scatter, histogram,
  and deterministic RNG.

## S2A: reusable runtime and deployment profile decision

- Status: accepted locally
- Date: 2026-08-01
- Scope: reusable Vulkan resource/dispatch runtime and persisted-profile
  selection; primitive algorithms remain outside this slice
- Git baseline: recorded by the commit containing this status entry

### Delivered contracts

- The Vulkan runtime owns instance/device/queue, host-visible buffers, shader
  modules, descriptors, compute pipelines, command buffers, fences, and their
  destruction through move-only RAII types.
- Device selection prefers a dedicated compute queue and exposes the selected
  queue family to diagnostics.
- The parent Vulkan CMake target builds the runtime whenever Vulkan support is
  enabled. `VIENNAPS_BUILD_VULKAN_SMOKE` controls only the smoke executable.
- Deployment profile lookup uses caller configuration first, then
  `VIENNAPS_DEVICE_PROFILE_DIR`, then the relative
  `.viennaps-device-profiles` directory. No absolute SDK, library, or profile
  location is stored in source.
- Missing, corrupt, unknown-schema, or hardware/driver-stale profiles require a
  new probe and fail closed to CPU. A fresh matching profile can feed Auto
  selection without probing again; Manual selection remains authoritative and
  unsupported Manual Vulkan requests return an explicit failure.

### Acceptance evidence

| Gate | Result |
|---|---|
| Parent Vulkan configure/build | runtime library and both smoke executables build with SDK supplied through `VULKAN_SDK` |
| Reusable runtime smoke | Intel Arc queue family 1, dedicated; 16 `y = 2x + 1` FP32 values exact; exit 0 |
| Original compute smoke regression | all 16 values exact, 0 ULP; exit 0 |
| No-SDK regression | probe stub builds and reports `status=disabled`; exit 0 |
| Capability/deployment profile | 13 focused cases pass under MSVC C++20 `/W4` without warnings |
| Fixed-path audit | no local Vulkan SDK, VTK, or reference-tree absolute path in the changed source |

### Open exits before S2B

- Remove the bounded 256-block ceiling described below, then add compaction,
  sort, gather/scatter, histogram, and deterministic RNG.

## S2A-runtime-parameters: production pipeline parameter support

- Status: accepted locally
- Date: 2026-08-01
- Scope: reusable runtime API required by production primitive pipelines

`PipelineLayout` now accepts validated push-constant ranges, while preserving
the original no-push-constant overload. `ComputePipeline` now accepts an entry
point and validated specialization-constant map/data through
`ComputePipelineOptions`, while preserving its original `main` entry overload.
`DescriptorPool` also accepts an explicit descriptors-per-set count while its
two-descriptor compatibility overload remains unchanged; multi-binding
primitive layouts therefore allocate exactly one set with sufficient entries.
The runtime smoke uses a specialization constant for the multiplier and a push
constant for the bias, then differentially validates the same 16-value
`y = 2x + 1` CPU oracle on the Intel Arc device. All values remain bit-exact,
the dedicated compute queue remains selected, and both runtime targets build
under MSVC C++20 `/W4` without code warnings.

## S2A-runtime-lifecycle: fence destruction correction

- Status: accepted locally
- Date: 2026-08-01
- Scope: shared runtime resource-lifetime correction

Fence rearming and object destruction now have separate operations. Dispatch
continues to call `vkResetFences` after a completed wait, while destructors,
move assignment, and owning primitive `reset()` paths call `vkDestroyFence`
before the logical device is released. Runtime, elementwise, RNG, and
gather/scatter/histogram smokes all rebuild under MSVC C++20 `/W4 /WX` and
pass on the real device after this change.

## S2B-profile: deployment probe-to-profile persistence

- Status: accepted locally
- Date: 2026-08-01
- Scope: explicit deployment-time generation and validation of the cached
  capability profile; suite promotion remains a separate gate

### Delivered contracts

- The probe keeps its schema-1 diagnostic JSON output compatible and adds the
  explicit `--write-deployment-profile <path>` operation. The resulting
  schema-2 `CapabilityProfileRecord` is written through the existing temporary
  file plus atomic replacement path and can be read back with
  `--validate-profile`.
- Hardware and driver UUIDs, vendor/device identity, compute and ray features,
  and `shaderFloat64` are copied into the deployment profile. Missing identity
  fields reject profile adaptation.
- Safe working-set derivation considers only device-local heaps and uses their
  current `heapBudget - heapUsage`. It selects the largest available heap,
  reserves 64 MiB plus ten percent, rejects results below 128 MiB, and caps the
  persisted result at 4 GiB. If `VK_EXT_memory_budget` is unavailable, the
  budget remains zero so Auto fails closed.
- Primitive and FP64 suite flags always remain false at probe time. A probe
  without a compute queue cannot mark Vulkan available. A build without the
  Vulkan SDK still emits the disabled diagnostic, but an attempted deployment
  profile write fails explicitly and does not create a Vulkan-eligible cache.
- The output path is supplied by the caller or the deployment profile path
  resolver. No machine-local SDK, library, or cache path is compiled into the
  program.

### Acceptance evidence

| Gate | Result |
|---|---|
| Adapter and budget policy | 11 focused cases pass under MSVC C++20 `/W4 /WX` |
| Real device profile | Intel Arc schema 2 profile writes atomically and validates through the production parser |
| Real device gates | compute true; `shaderFloat64`, primitive suite, and FP64 suite false; safe budget capped at 4 GiB |
| No-SDK diagnostic | normal probe exits 0 with `status=disabled` |
| No-SDK profile write | exits nonzero and reports that no Vulkan deployment profile is available |
| Fixed-path audit | local SDK and reference-source paths are absent from tracked source and documentation |

## S2B-primitive-1: parallel primitive differential slice

- Status: accepted as a bounded parallel implementation, not the complete
  production primitive suite
- Date: 2026-08-01
- Scope: optional developer smoke target for the first five primitive
  contracts; this does not set `vulkanPrimitiveSuitePass`

### Delivered contracts

- The parent and standalone Vulkan builds expose
  `VIENNAPS_BUILD_VULKAN_PRIMITIVES_SMOKE`. When the SDK is unavailable, CMake
  skips the target cleanly and keeps the diagnostic probe usable.
- Fill, copy, affine transform, sum/min/max reduction, and exclusive signed
  32-bit scan are compared against CPU results at lengths 0, 1, 16, 257, and
  65,535.
- The checks include output mismatch counts, floating-point absolute/relative/
  ULP error, and guard-region validation around every logical buffer.
- Fill, copy, and transform dispatch one invocation per element. Reduction uses
  shared-memory block reduction followed by a second shared-memory reduction of
  partial sum/min/max values; it does not require floating-point atomics.
- Scan uses block-local Blelloch exclusive scan, a scan of block sums, and a
  parallel block-offset add. The current implementation supports at most 256
  blocks, or 65,536 elements at the fixed 256-thread workgroup size. Larger
  inputs must be rejected or routed to CPU until recursive block-sum scan is
  implemented.

### Acceptance evidence

| Gate | Result |
|---|---|
| Parent Vulkan configure/build | primitive shader and smoke executable build with the SDK supplied through `VULKAN_SDK` |
| Differential cases | five operations at five lengths pass, including 65,535 elements across 256 workgroups; zero mismatches |
| Guard regions | unchanged for all operations and lengths |
| No-SDK regression | primitive target is skipped; diagnostic probe remains buildable and exits 0 |
| Warning gate | primitive host target builds under MSVC C++20 `/W4` without code warnings |
| Fixed-path audit | source and documentation contain environment-variable names only, not machine-local SDK or library paths |

### Required promotion work

- Generalize reduction and block-sum scan recursively beyond 256 workgroups;
  preserve the exact zero-length, sum/min/max, and exclusive-scan contracts.
- Run the same CPU differential matrix across multiple workgroup boundaries and
  randomized deterministic inputs before setting the corresponding persisted
  profile suite gates.
- Complete the remaining primitive list and safe memory-budget calibration.

## S2B-primitive-2: reusable production elementwise primitives

- Status: accepted locally
- Date: 2026-08-01
- Scope: reusable fill, copy, and affine-transform API built on the shared
  Vulkan runtime; this slice alone does not set `vulkanPrimitiveSuitePass`

### Delivered contracts

- `ElementwisePrimitives` owns only composition-level state and reuses the S2A
  runtime for instance, device, queue, buffers, shaders, descriptors,
  pipelines, command buffers, and fences. Public consumers link the runtime
  transitively instead of duplicating Vulkan lifetimes.
- One specialization-driven shader exposes fill, copy, and affine transform.
  Push constants carry the active element count and scalar operands.
- Dispatch validates buffer sizes and the device workgroup-count limit, handles
  zero length without dispatch, preserves inactive tails, and rejects in-place
  copy/affine unless the caller opts in explicitly.
- Host-visible non-coherent memory is flushed before device access and
  invalidated before CPU readback. Aliasing dispatches use one combined buffer
  barrier, and destruction follows Vulkan dependency order.
- SDK discovery and shader compilation remain driven by `VULKAN_SDK`; no
  resolved SDK or external source path is part of the target interface or
  tracked source.

### Acceptance evidence

| Gate | Result |
|---|---|
| Warning gate | library and production smoke build under MSVC C++20 `/W4 /WX` |
| CPU differential | fill, copy, and affine at lengths 0, 1, 16, 257, and 65,535 are bit-exact on Intel Arc |
| Bounds and tails | inactive output tails retain sentinels for every tested length |
| Alias policy | default in-place copy/affine rejection and explicit opt-in execution both pass |
| Repeatability | complete production smoke passes twice consecutively with one reused command buffer/fence |
| Existing regressions | bounded primitive smoke and reusable runtime smoke both remain passing |
| No-SDK regression | primitives are skipped cleanly when `VULKAN_SDK` is absent |
| Fixed-path audit | no local SDK, VTK, or comparison-tree absolute path occurs in tracked text |

## S2B-primitive-3: deterministic counter-based RNG

- Status: accepted locally
- Date: 2026-08-01
- Scope: reusable deterministic uint32 and FP32 `[0, 1)` generation; this
  slice alone does not set `vulkanPrimitiveSuitePass`

The RNG maps `(seed, uint32 counter)` through a fixed xorshift32 transform.
Counter offsets make chunked dispatches reproduce the corresponding range of a
single dispatch, and overflow is rejected before submission. FP32 conversion
uses the upper 24 random bits times `2^-24`, avoiding the possible `1.0` caused
by rounding a full uint32 to float. Length, buffer, uint32 counter-range, device
workgroup-count, and zero-length contracts are checked before GPU execution.

| Gate | Result |
|---|---|
| Warning gate | RNG library and smoke build under MSVC C++20 `/W4 /WX` |
| CPU differential | uint32 and FP32 exact at 0, 1, 16, 257, 65,535, and 1,000,003 elements |
| Determinism | repeated seed/counter dispatches are exact; seed variation and nonzero offset cases pass |
| Range and guards | all FP32 values are in `[0, 1)` and inactive output tails remain unchanged |
| Rejection paths | buffer/length, counter overflow, and device dispatch limits fail before submission |
| Repeatability | full Intel Arc smoke passes twice consecutively |
| No-SDK regression | primitive targets remain cleanly skipped without `VULKAN_SDK` |

## S2B-primitive-4: gather, scatter, and histogram

- Status: accepted locally
- Date: 2026-08-01
- Scope: reusable FP32 gather/scatter and uint32 histogram primitives; this
  slice alone does not set `vulkanPrimitiveSuitePass`

The API validates logical buffer lengths, device dispatch limits, aliases,
indices, histogram values, and bin counts before submission. Gather and
scatter reject out-of-range indices. Scatter rejects duplicate destinations
by default; an explicit option applies a deterministic host-side last-write-
wins compaction before a race-free GPU dispatch. Histogram clears only its
declared bins and uses device atomics after host validation guarantees every
input is in range.

Unused descriptor bindings are backed by live one-element dummy buffers with
ranges constrained to those allocations. Deterministic scatter scratch
buffers remain alive through queue completion. Initialization, reset, host
flush/invalidate, barriers, and object destruction follow the shared Vulkan
runtime contracts.

| Gate | Result |
|---|---|
| Warning gate | library and production smoke build under MSVC C++20 `/W4 /WX` |
| CPU differential | gather, scatter, and histogram exact at 0, 1, 16, 257, and 65,535 elements |
| Bounds and tails | gather, scatter, and histogram guard elements remain unchanged |
| Duplicate policy | default rejection and deterministic last-write-wins opt-in both pass |
| Rejection paths | out-of-range gather/scatter indices, histogram values, and zero bins fail before submission |
| Repeatability | full Intel Arc smoke passes twice consecutively |
| Local paths | SDK discovery uses `VULKAN_SDK`; tracked inputs contain no resolved local SDK or reference-tree path |

## S2B-primitive-5: recursive reduction and exclusive scan

- Status: accepted locally
- Date: 2026-08-01
- Scope: reusable FP32 sum/min/max reduction and signed-bit-pattern exclusive
  scan without the former 256-block ceiling; this slice alone does not set
  `vulkanPrimitiveSuitePass`

Reduction emits contiguous sum/min/max records per 256-thread block, then
recursively reduces those records until one exact triple remains. Exclusive
scan recursively scans block sums and applies their offsets, so inputs larger
than 65,536 elements no longer require a CPU fallback. Scan addition is
specified as modulo `2^32`; this gives deterministic wrap behavior for signed
32-bit bit patterns instead of relying on language-specific signed overflow.

All intermediate buffers are scoped through queue completion. Host-visible
non-coherent buffers use explicit flush/invalidate transitions, descriptor
ranges remain inside live allocations, and buffer barriers use ignored queue-
family indices on the selected compute queue.

| Gate | Result |
|---|---|
| Warning gate | library and production smoke build under MSVC C++20 `/W4 /WX` |
| CPU differential | reduction and scan exact at 0, 1, 16, 257, 65,535, and 1,000,003 elements |
| Recursive depth | 1,000,003 elements exercise 3,907 first-level blocks and a second block-scan level |
| Reduction order | CPU oracle reproduces the shader's tree order; sum/min/max are bit-exact for finite inputs |
| Scan semantics | negative values and an explicit modulo-`2^32` wrap case pass exactly |
| Bounds and aliases | input/output guards remain unchanged; in-place scan rejects by default and passes with explicit opt-in |
| Validation layers | complete smoke passes twice with `VK_LAYER_KHRONOS_validation` enabled and no diagnostics |
| Local paths | build and shader discovery use `VULKAN_SDK`; no resolved machine path is tracked |

## S2B-primitive-6: stable stream compaction

- Status: accepted locally
- Date: 2026-08-02
- Scope: stable FP32 and uint32 compaction by uint32 flags on the same Vulkan
  device as the recursive scan; GPU-resident indirect dispatch remains a later
  shared-session exit

Compaction now extends `ReductionScanPrimitives` instead of creating another
device-owning wrapper. A GPU normalization pass maps every nonzero flag to one,
the existing recursive exclusive scan produces stable destinations, a one-
workgroup GPU pass writes the exact selected count, and a final scatter copies
FP32 values or preserves all uint32 bits. The count is read before scatter so an
undersized output is rejected without an out-of-bounds shader write.

`HostVisibleBuffer` exposes its owning `VkDevice` for validation. Reduction,
scan, and compaction therefore reject buffers created by another primitive
instance before descriptor update. Input/output aliasing and flag aliasing are
also rejected because a parallel stable scatter cannot safely overwrite unread
input elements.

| Gate | Result |
|---|---|
| Warning gate | production library and compaction smoke build under MSVC C++20 `/W4 /WX` |
| FP32 CPU differential | exact stable order at 0, 1, 16, 257, 65,535, and 1,000,003 elements |
| uint32 CPU differential | bit-exact at 0, 1, 257, and 1,000,003 elements |
| Flag semantics | zero rejects an element; every nonzero uint32 value selects exactly once |
| Count and guards | GPU count is exact; input, flags, inactive output tail, and zero-selection output remain unchanged |
| Rejection paths | mismatched lengths, insufficient capacity, input/output alias, flag alias, and cross-device buffers fail before scatter |
| Validation layers | complete compaction smoke passes twice with `VK_LAYER_KHRONOS_validation` enabled and no diagnostics |
| Existing regressions | low-level primitive, recursive reduction/scan, RNG, and runtime smokes pass after dependent objects are rebuilt |
| No-SDK regression | primitive targets skip cleanly when Vulkan package discovery and `VULKAN_SDK` are unavailable |
| Local paths | tracked text contains no resolved SDK, VTK, or comparison-tree path |

The localized MSVC `/showIncludes` output can prevent Ninja from noticing a
changed public header. This slice exposed that an old smoke object may allocate
the former class size while linking the new library. Acceptance therefore
requires a real dependent-object rebuild after public primitive layout changes;
relink-only output is not sufficient evidence.

## S2B-primitive-7: stable uint32 radix sort

- Status: accepted locally
- Date: 2026-08-02
- Scope: stable uint32 key/value sorting with eight 4-bit LSD passes; this is a
  correctness-first primitive and does not yet provide a fully GPU-resident
  multi-workgroup prefix

Each pass builds a 16-bin histogram per 256-element workgroup. The host reads
only the workgroup histograms, materializes deterministic bucket and workgroup
offsets, and writes those offsets back. Scatter remains parallel across
workgroups; invocation zero in each workgroup consumes its shared 256-element
tile in input order. This avoids the nondeterministic duplicate-key order of a
global atomic scatter while keeping all key/value movement in Vulkan shaders.

Out-of-place sorting alternates between the requested output and an internal
scratch buffer, so the input is never used as a destination. Exact paired
key/value in-place sorting requires explicit opt-in. Partial aliases,
cross-key/value aliases, undersized outputs, foreign-device buffers, and
storage-buffer limit violations fail before submission.

| Gate | Result |
|---|---|
| Warning gate | production library and smoke build under MSVC C++20 `/W4 /WX` |
| CPU differential | stable key/index pairs match `std::stable_sort` at 0, 1, 16, 257, 65,535, and 1,000,003 elements |
| Key coverage | duplicate-heavy fixed input, all-equal keys, reverse order, zero, and `UINT32_MAX` pass |
| Stability | original uint32 indices remain ordered inside every equal-key run |
| Bounds and aliases | input and output-tail guards pass; exact in-place opt-in passes; partial alias and insufficient capacity reject |
| Device ownership | output buffers owned by another `VkDevice` reject before descriptor update |
| Validation layers | complete radix smoke passes twice with `VK_LAYER_KHRONOS_validation` enabled and no diagnostics |
| Existing regressions | stable compaction and recursive reduction/scan production smokes pass |
| Local paths | shader and SDK discovery use configured targets and `VULKAN_SDK`; no resolved machine path is tracked |

The remaining performance exit is to replace the small host histogram prefix
with a shared-device recursive GPU scan. The current implementation is suitable
as a deterministic fallback and as the CPU-differential reference for that
later optimization.

## P3B-runtime-session: reusable compute session and smoke validation

- Status: accepted locally
- Date: 2026-08-02
- Scope: move Vulkan lifetime ownership from one-time wrappers to a reusable
  session object suitable for higher-level primitive orchestration

- `ComputeSession` now owns `VulkanInstance`, `VulkanDevice`, and
  `CommandContext` under one move-only RAII object.
- The wrapper exposes explicit manual selection by physical device index, device
  name, and UUID; when no manual preference is set it keeps the existing
  dedicated-queue-first auto selector.
- `initialize()` is idempotent for the same device options. Conflicting manual
  selectors fail closed, and changing an initialized session's selection requires
  an explicit `reset()` before reconfiguration. `reset()` tears down session
  state in dependency order.
- The runtime library now includes `compute_session.cpp`, and an additional
  `viennaps-runtime-compute-session-smoke` target validates that the wrapper can
  execute the same 16-element `y = 2x + 1` smoke successfully.

| Gate | Result |
|---|---|
| Session ownership | instance, device, queue/command pool, reset, move construction, and move assignment pass |
| Manual selection | current device reselected by physical index, exact name, and UUID; conflicts and invalid selectors rejected |
| Session smoke | 16 values exact, 0 ULP, exit 0 in two validation-layer runs |
| Runtime smoke | existing reuse smoke remains unchanged, exit 0 |
| CTest registration | both runtime compute smokes are registered when `BUILD_TESTING` is enabled |
| Validation build | runtime library and both compute-smoke executables built under MSVC C++20 with explicit `/W4 /WX` validation flags |

## P3-oracle-1: frozen ViennaLS single-step CPU oracle

- Status: accepted locally as a CPU reference, not a Vulkan implementation
- Date: 2026-08-01
- Scope: the smallest deterministic structure update used to validate the
  first Vulkan Level Set kernels

The existing CPU baseline advances a circle through its complete requested
duration. This additional oracle enables ViennaLS single-step mode, performs
exactly one forward-Euler/Engquist-Osher step at a 0.49 time-step ratio, and
fingerprints both the initial-to-updated topology transition and the exact
advected time. It deliberately depends only on ViennaLS/ViennaCore; the
higher-level ViennaPS `AdvectionHandler` oracle remains gated by the currently
unavailable ViennaCS dependency.

| Precision | Active / nodes / lines | Advected time | Transition fingerprint |
|---|---:|---:|---:|
| FP32 | 92 / 68 / 68 | `0.40459004530160253` | `0x51052040fecec990` |
| FP64 | 92 / 68 / 68 | `0.40459001562216945` | `0x6af00ac25d8971d0` |

The standalone MSVC C++20 `/W4` build and two consecutive executions pass.
Known narrowing/uninitialized-use warnings instantiated inside the current
ViennaLS headers were suppressed only in the local direct-compile command; the
oracle source itself compiles without a warning-specific source workaround.

## P3C-levelset-update: Forward Euler value update

- Status: accepted locally as an algorithm-layer component; ViennaLS state
  extraction and sparse-domain rebuild are not integrated yet
- Date: 2026-08-02
- Scope: the `Advect::updateLevelSet()` value-update phase for FP32 CSR-flattened
  rate data, executed through an externally owned `ComputeSession`

The kernel consumes per-point values and CSR ranges of gradient, dissipation,
and material stop rates. It preserves the current ViennaLS dissipation-check
operator precedence, skips values outside `integrationCutoff`, deducts time at
material boundaries, and keeps the final sentinel contract explicit. Host-side
validation rejects non-monotonic or empty ranges, unequal rate arrays, missing
sentinels, non-finite data, and dispatch overflow. A device status word remains
as a second fail-closed guard, and caller output is replaced only after a clean
dispatch and readback.

| Gate | Result |
|---|---|
| External runtime ownership | uses the caller's `ComputeSession`; creates no instance or logical device |
| FP32 CPU differential | bit-exact at 0, 1, 257, and 65,535 points |
| ViennaLS branches | cutoff, both dissipation reversal clauses, and multi-material transition pass exactly |
| Malformed input | short/empty CSR and missing sentinel rejected; prior output preserved |
| Validation layers | two consecutive runs pass on the selected Vulkan compute device |
| CTest | `viennaps-vulkan-levelset-update-smoke` passes |
| Strict build | MSVC C++20 `/W4 /WX` passes |
| No-SDK | option-on configure skips the Level Set target cleanly and the build exits 0 |

The CPU differential intentionally uses FP32 time arithmetic to define this
widely supported shader contract. ViennaLS currently keeps the remaining time
as `double` even for an FP32 domain; end-to-end integration must therefore
measure the single-step topology oracle above and either accept a documented
ULP/interface tolerance or gate the exact mixed-arithmetic path on
`shaderFloat64`. FP64 domain support remains CPU-only until its dedicated suite
passes.

## P3D-deployment-context: cached automatic runtime selection

- Status: accepted locally as the policy-to-session connection layer
- Date: 2026-08-02
- Scope: load and validate a recorded hardware profile once, resolve every
  requested simulation stage, and create a shared Vulkan session only when the
  resulting plan actually selects Vulkan

`DeploymentComputeContext` preserves the existing fail-closed policy rather
than duplicating thresholds. A valid profile may unlock Vulkan after primitive,
precision, memory, callback, and ray-tier gates. Missing, invalid, or stale
profiles set `requiresProbe` and resolve automatic workloads to CPU. A manual
backend request has policy precedence but still cannot bypass unavailable
hardware or a failed validation suite. Manual device index/name/UUID selection
is applied when the session is created, then the actual device and driver UUID,
vendor/device IDs, and name are checked against the active profile.

The prepared decision is cached for the process lifetime so simulation tasks do
not reload the profile or ask the user. Reconfiguration is explicit: call
`reset()`, apply new manual settings, and call `prepare()` again before worker
threads start. The context is intentionally not thread-safe during preparation.

| Gate | Result |
|---|---|
| Automatic valid profile | Level Set FP32 selects Vulkan and creates one shared session |
| Manual CPU | overrides automatic ranking and creates no Vulkan session |
| Cached execution | later prepare calls do not mutate a running simulation decision |
| Profile staleness | driver fingerprint mismatch requests a probe and falls back to CPU |
| Unsafe manual Vulkan | unavailable/stale-profile request fails and retains rejection provenance |
| Manual device | exact current device name succeeds and is rechecked against the profile identity |
| Strict/runtime validation | MSVC C++20 `/W4 /WX`, CTest, and two validation-layer runs pass |
| Local path policy | profile location remains caller/environment supplied; no machine path is compiled in |

## P3E-viennals-stage-executor: transactional integration seam

- Status: accepted locally as the upstream-compatible ViennaLS value-update
  seam
- Date: 2026-08-02
- Scope: insert a caller-supplied executor after ViennaLS reduces the top level
  set to one layer and before the existing sparse rebuild

The pinned ViennaLS v5.8.5 dependency receives a deterministic tracked patch
when the normal remote dependency path is used. A versioned CPM cache key keeps
that patched checkout separate from unmodified ViennaLS caches. The patch adds
an optional
`LevelSetUpdateExecutor` with `HANDLED`, `FALLBACK`, and `ERROR` results. Its
context exposes the reduced sparse domain and stored rates as const references,
plus time step, cutoff, dissipation, and velocity-output flags. The executor
returns separately owned value/velocity/dissipation arrays; ViennaLS validates
all segment sizes and finite values before committing any domain value.

No executor preserves the original CPU implementation. `FALLBACK`, thrown
exceptions, explicit `ERROR`, malformed output, and non-finite output also run
the unchanged CPU phase. A valid `HANDLED` result skips only the value-update
loop; ViennaLS still clears rates, rebuilds the sparse level set, combines
Runge-Kutta stages, and adjusts lower layers in its original order.

Local ViennaLS overrides deliberately skip automatic patching, because they are
development sources and may already contain the upstream API. Such a source
must implement this executor contract; its location remains supplied through
`VIENNAPS_VIENNALS_SOURCE_DIR` and is never tracked.

| Gate | Result |
|---|---|
| Patch drift | `patch --dry-run -p1` succeeds exactly against ViennaLS v5.8.5 |
| Public API compile | MSVC C++20 `/W4 /WX` compile smoke passes |
| Default CPU | frozen FP32/FP64 92-point fingerprints and times remain exact |
| Fail-closed behavior | `FALLBACK`, injected `ERROR`, and invalid `HANDLED` output match CPU exactly |
| Successful handling | valid echo output is accepted and bypasses the CPU value update |
| Temporal ordering | executor calls are FE=1, RK2=2, RK3=3; RK fallback matches CPU |
| Path policy | tracked patch, CMake, and tests contain no resolved local dependency path |

## P3F-viennals-vulkan-adapter: FP32 value-update execution

- Status: accepted locally as a direct ViennaLS-to-Vulkan executor adapter
- Date: 2026-08-02
- Scope: flatten the P3E const sparse-domain/rate view into the existing Vulkan
  value-update kernel contract and reconstruct owned per-segment output

`ViennaLsUpdateExecutorFp32` preserves the ViennaLS segment order and converts
the sentinel-terminated active-point rate stream to CSR arrays. ViennaLS does
not store rates for defined points outside the integration cutoff; the adapter
therefore inserts a zero-effect sentinel entry for those points so every Vulkan
work item retains a valid CSR range. It rejects mismatched segment counts,
missing or trailing sentinels, scalar/range overflow, and invalid session or
shader state before returning `HANDLED`.

The shader payload has shared ownership while the compute session remains an
explicit borrowed lifetime. Velocity-output requests return `FALLBACK` because
the current kernel produces values only. FP64 remains on CPU. A generated SPIR-V
target property supplies the test executable's runtime shader path without
putting any resolved SDK, source, or build path in tracked files.

| Gate | Result |
|---|---|
| CPU oracle | one Forward Euler step over the simple FP32 circle matches the CPU surface at 1e-6 quantization |
| Frozen topology | CPU and Vulkan both produce 92 active points, 68 surface nodes, and 68 lines |
| Executor contract | called once, returns `HANDLED`, leaves no executor error, and preserves the exact advected time |
| Vulkan correctness | two consecutive CTest runs pass with the Khronos validation layer enabled |
| No-SDK behavior | configuration with Vulkan discovery disabled builds and passes the CPU baseline; no Vulkan executor executable is generated |
| Path policy | all changed tracked/candidate files contain zero fixed local SDK/library paths and zero Windows absolute paths |

## P3G-process-executor-routing: generic production injection

- Status: accepted locally as the strategy-independent ViennaPS injection seam
- Date: 2026-08-02
- Scope: carry an optional ViennaLS Level Set update executor through
  `Process`, `ProcessContext`, and the common `AdvectionHandler`

The executor is empty by default, so existing CPU behavior is unchanged. Each
advection-handler initialization installs the current context value, including
an empty value, which prevents an executor from leaking across reused process
state. Analytic, flux-driven, and atomic-layer strategies all pass through this
common handler boundary and therefore require no Vulkan-specific strategy
forks. `Process::setLevelSetUpdateExecutor` and
`Process::clearLevelSetUpdateExecutor` provide the public injection surface
without adding Vulkan headers to the always-available CPU API.

The focused routing test performs one simple two-dimensional FP32 plane step.
It compares every flattened ViennaLS defined value from the default CPU path
against an injected executor that deliberately returns `FALLBACK`, and also
compiles the public set/clear API. This test passes in both a Vulkan-configured
build and a configuration where Vulkan package discovery is disabled.

| Gate | Result |
|---|---|
| Default behavior | empty executor leaves the original ViennaLS CPU path active |
| Fallback identity | injected `FALLBACK` is called and all defined values match CPU exactly |
| Strategy boundary | the common `AdvectionHandler` carries the executor for analytic, flux, and ALP strategies |
| Public API | `Process` set/clear calls compile without exposing Vulkan types |
| No-SDK behavior | focused test builds and passes with Vulkan discovery disabled |
| Path policy | no fixed local SDK/library path or Windows absolute path is tracked |

## P3H-levelset-process-controller: deployment-driven installation

- Status: accepted locally as the deployment/configuration controller; the
  final full-`Process::apply` execution gate remains open
- Date: 2026-08-02
- Scope: bind one caller-supplied hardware fingerprint and FP32 Level Set
  workload to `DeploymentComputeContext`, load the selected shader, and install
  P3F through P3G

`LevelSetProcessController<D>` does not duplicate capability ranking or device
selection. It passes the hardware fingerprint, actual workload, profile path,
manual backend policy, and optional device selector to the existing deployment
context. Profile and shader paths remain caller/environment supplied. A zero or
invented workload is not accepted implicitly: the caller must provide an FP32
`LEVEL_SET` workload so memory and feature thresholds retain their meaning.

CPU and automatic failure exits clear the Process executor. Missing/stale
profiles and configuration-time shader/session failures fail closed to CPU in
automatic mode with a structured degraded result. Manual CPU remains an
explicit, non-degraded CPU choice. During configuration, manual Vulkan cannot
silently downgrade: selection, device, session, and shader failures return an
error and leave no callback installed. On success the Process callback captures
shared state containing the deployment context and SPIR-V payload; it retrieves
the active session from that state on each call, avoiding a dangling borrowed
session after the controller's stack scope ends. The controller also rejects a
zero estimated working set and any selection precision that could override the
FP32 executor with FP64.

The controller smoke creates a temporary profile from the current Vulkan
device identity. It exercises valid automatic and manual Vulkan configuration,
exact manual device-name selection, missing/stale profile behavior, automatic
shader fallback, manual shader failure, and invalid workload rejection. The
temporary path is generated at runtime and removed by the test; no resolved
path is tracked. The smoke compiles with MSVC `/W4`; `/WX` is retained on the
Vulkan runtime and Level Set libraries, but cannot be enabled on this
Process-including translation unit until existing ViennaPS/ViennaLS/Embree
header warnings are isolated or fixed.

| Gate | Result |
|---|---|
| Automatic Vulkan | valid profile, workload, session, and shader select Vulkan |
| Manual overrides | manual CPU clears the executor; manual Vulkan and exact device-name selection succeed |
| Fail-closed auto | missing/stale profile and shader-load failure report degraded CPU |
| Strict manual configuration | unavailable/manual shader failure returns an error without silent CPU substitution |
| Validation layers | controller CTest passes twice with `VK_LAYER_KHRONOS_validation` |
| No-SDK behavior | Vulkan-disabled configuration generates no controller target/test and the CPU routing test passes |
| Path policy | profile/shader inputs are caller/environment supplied; changed files contain no resolved local paths |

## P3I-levelset-controller-execution: callback lifetime gate

- Status: accepted locally as the Process-owned callback lifetime and direct
  ViennaLS execution gate; full `Process::apply` progress remains a separate
  open issue
- Date: 2026-08-02
- Scope: execute the callback installed by P3H after the controller's local
  scope ends and compare its one-step result with the P3F CPU oracle

`Process::getLevelSetUpdateExecutor` returns a copy of the installed generic
executor without exposing Vulkan runtime types. The execution smoke configures
an automatic Vulkan backend, destroys the controller, copies the executor from
the still-live `Process`, and supplies it to a bounded ViennaLS Forward Euler
step. The copied callback retains the shared deployment context and SPIR-V
payload. The test asserts that it is non-empty, is called exactly once, returns
`HANDLED`, and reports no error; a CPU run without an executor is the oracle.
The advected time matches exactly and the extracted surface nodes match at the
existing 1e-6 geometry tolerance.

An initial version deliberately attempted the same gate through
`Process::apply`; the CPU path itself exceeded a 15-second test limit and was
stopped without retry. Read-only diagnosis found that `Advect` can return a
zero single-step time while `AnalyticProcessStrategy` has no explicit
no-progress exit, leaving its outer duration loop able to repeat indefinitely.
The accepted smoke therefore verifies the controller/Process/ViennaLS callback
lifetime directly and does not conceal the separate strategy-progress defect.

| Gate | Result |
|---|---|
| Callback ownership | executor copied from `Process` remains valid after controller destruction |
| Actual Vulkan execution | callback is called once, returns `HANDLED`, and has an empty error |
| CPU parity | exact advected time and surface nodes within 1e-6 |
| Bounded runtime | focused execution CTest completes in under one second locally |
| Validation layers | five-test Level Set chain passes twice with `VK_LAYER_KHRONOS_validation` |
| No-SDK behavior | Vulkan-disabled configuration generates no execution target/test and the CPU routing test passes |
| Path policy | test profile/temp paths are runtime generated; changed files contain no resolved local paths |

## P3J-levelset-runtime-failure-policy: mode-aware execution failure

- Status: accepted locally as the runtime failure contract for the installed
  Level Set executor
- Date: 2026-08-02
- Scope: preserve automatic-selection compatibility while making a successful
  manual Vulkan selection fail explicitly on executor runtime faults

The Process context now carries an explicit Level Set update failure policy.
Its default is `FALLBACK`, so existing callers and automatic deployment retain
the previous behavior: executor `ERROR`, exceptions, and invalid `HANDLED`
output shapes or values produce a warning and run the CPU value update. An
executor-declared `FALLBACK` always requests that CPU path, including after a
manual Vulkan selection.

The deployment controller changes the policy to `FAIL` only after manual
Vulkan configuration has fully succeeded. Configuration entry, every CPU or
failed configuration exit, and `clear()` reset it to `FALLBACK`. Under `FAIL`,
executor errors are propagated through ViennaLS and `AdvectionHandler` as
`ProcessResult::FAILURE`. The pre-update sparse values are restored before the
error leaves ViennaLS, later Runge-Kutta stages and rebuild work are skipped,
and ViennaPS does not increment its advection-step count or process time.
Breaking the ViennaLS outer loop on this state also prevents non-single-step
and ALP execution from repeating a known-failed update.

Seven independently time-bounded routing scenarios cover explicit fallback,
automatic/default fallback after executor error, exception, and invalid output,
and strict failure for the same three fault classes. The strict tests require
transactional value preservation, zero process-time advance, and zero accepted
step count. Each scenario completed well below its 15-second test timeout.

| Gate | Result |
|---|---|
| Default compatibility | explicit `FALLBACK`, `ERROR`, exception, and invalid output use the CPU oracle |
| Strict manual runtime | error, exception, and invalid output return `FAILURE` without hidden CPU update |
| Transaction boundary | sparse values are restored; time and accepted-step count remain zero |
| Multi-stage/loop exit | integration stages, rebuild, and the ViennaLS outer loop stop on strict failure |
| Controller lifecycle | only successful manual Vulkan selects `FAIL`; configure/CPU/error/clear exits select `FALLBACK` |
| Focused routing tests | seven of seven scenarios pass independently |
| Validation layers | five-test Level Set chain passes twice with `VK_LAYER_KHRONOS_validation` |
| No-SDK behavior | Vulkan-disabled configuration exposes no controller test; all seven CPU routing tests pass |
| Patch applicability | the ViennaLS patch dry-run applies to both modified upstream headers |
| Path policy | SDK, dependency, shader, and profile locations remain environment/caller supplied; no resolved local path is tracked |

## P3K-A-advection-progress-guard: ViennaPS single-step boundary

- Status: accepted locally for the single-step Analytic and Flux strategy
  boundary; P3K-B supplies the complementary ALP inner-loop guard
- Date: 2026-08-02
- Scope: classify the time returned by ViennaLS before ViennaPS accepts a step
  or advances process time

`AdvectionHandler` now accepts a step only after its returned time is finite,
non-negative, and strictly advances `processTime`. A finite zero step or a
positive step lost to floating-point rounding returns `EARLY_TERMINATION` with
no accepted-step or process-time advance. Defensive branches reject a negative
or non-finite step, or a finite step whose addition overflows process time,
through a non-throwing error log. These synthetic invalid-value branches are
compile- and review-validated here; direct negative/non-finite injection
coverage remains a future test extension. The all-zero-velocity sentinel is
checked first and retains its successful completion behavior for both the
template numeric type maximum and the double-precision compatibility value.

The focused CPU test uses a small two-dimensional plane. A non-zero velocity
with `timeStepRatio=0` was a 10-second RED timeout before the guard; it now
returns `EARLY_TERMINATION` in under one tenth of a second with zero time and
zero accepted steps. A separate zero-velocity case proves that the sentinel
still completes the requested duration successfully. The same no-SDK run also
executes all seven P3J routing scenarios.

| Gate | Result |
|---|---|
| RED evidence | zero-ratio analytic step exceeded the 10-second watchdog before the guard |
| Zero progress | returns `EARLY_TERMINATION` without time or accepted-step advance |
| Zero velocity | numeric-precision maximum sentinel remains `SUCCESS` and completes the duration |
| Positive progress | the seven existing routing scenarios retain their normal successful step behavior |
| Invalid-time defense | negative/non-finite step and non-finite accumulated-time branches compile and pass review; direct injection is still open |
| Regression | focused guard plus seven executor-routing scenarios pass eight of eight |
| No-SDK behavior | the CPU-only build compiles and runs the same focused guard |
| Path policy | changed source and tests contain no resolved SDK, dependency, or build path |

## P3K-B-advection-inner-loop-guard: ALP bounded progress

- Status: accepted locally for the ViennaLS non-single-step loop used by ALP
- Date: 2026-08-02
- Scope: stop a zero or invalid integration result before ViennaLS can repeat
  it indefinitely without returning to ViennaPS

`Advect::apply` now resets an independent advection-time error on every call.
The shared integration helper validates the rate-derived time immediately
after `computeRates` and before `updateLevelSet`, `Reduce`, or sparse rebuild.
The zero-velocity success sentinel skips the update without an error. Sentinel
recognition covers both `numeric_limits<T>::max()` promoted to double and the
double-precision compatibility value, which is required when ViennaLS runs
with `float`. A finite positive step proceeds. Zero, negative, and non-finite
values also skip the update: non-single-step mode records an error, while
single-step mode retains the value for the P3K-A boundary to classify. The
outer Advect loop separately validates accumulated time and strict forward
progress before committing its time or step count. Runge-Kutta stages now stop
after a strict executor or advection-time error instead of combining or
entering a later stage. Rejected or terminal stages clear cached rates before
returning, so a later `apply` on the same handler recomputes them. The current
time-step getter is left intact for compatibility. `AdvectionHandler` reads
the error before velocity output, process-time mutation, or accepted-step
accounting and returns `ProcessResult::FAILURE` through a non-throwing error
log.

The focused CPU fixture uses the same small two-dimensional plane with ALP
mode enabled, `disableSingleStep()`, and a bounded advection time. A non-zero
velocity with `timeStepRatio=0` returns `FAILURE` in under one tenth of a
second with zero ViennaPS time and accepted steps. Legal zero velocity
completes the remaining internal time in one step; a normal positive-velocity
case also remains successful. The zero-progress fixture snapshots sparse
`definedValues` before and after the call and verifies that pre-update rejection
leaves them unchanged. The exposed zero-velocity step is the remaining time
rather than the maximum sentinel because ViennaLS caps the internal rate step
to `advect(remaining)`; separate unbounded RK2 and RK3 fixtures exercise the
promoted `NumericType` maximum sentinel. Forward Euler, RK2, and RK3 each have
an isolated zero-progress CTest, and the recovery fixture changes the ratio
after an expected failure and succeeds on the same reinitialized handler.

| Gate | Result |
|---|---|
| Non-single zero progress | returns `FAILURE` without sparse-value, ViennaPS time, or accepted-step mutation |
| Legal zero velocity | completes the bounded ALP interval successfully in one internal step |
| Positive progress | normal positive velocity remains successful |
| Single-step compatibility | invalid/zero values remain available to the P3K-A outer classifier |
| Multi-stage exit | RK2/RK3 stop before combine or later stages after strict executor/advection-time errors |
| Sentinel precision | unbounded RK2/RK3 accept the promoted `NumericType` maximum without entering a large update |
| Rate-cache recovery | the same handler recomputes rates and succeeds after a prior zero-progress failure |
| Focused regression | six P3K-B, one P3K-A, and seven P3J routing scenarios pass 14 of 14 |
| Patch applicability | zero-context ViennaLS patch applies cleanly to both upstream headers |
| No-SDK behavior | the CPU-only build compiles and runs all focused progress/routing tests |
| Path policy | changed patch, source, and tests contain no resolved SDK, dependency, or build path |

## P3L-controller-process-transaction: strict fault and full HRLE rollback

- Status: accepted locally
- Date: 2026-08-02
- Scope: execute a controller-installed Vulkan callback through
  `Process::apply`, expose its terminal result without changing the existing
  void API, and make strict failure transactional across Level Set preparation

`Process` now records the last strategy result and exposes it through
`getLastProcessResult()`. Invalid input and missing-strategy exits also record
their explicit result. This keeps existing C++ and Python `apply()` behavior
compatible while allowing controller tests and future orchestration to inspect
the terminal state after the logger's established runtime-error boundary.

ViennaLS `FAIL` mode now snapshots every participating Level Set before
`prepareLS`. On executor failure it restores each existing object in place by
deep copy, preserving external smart-pointer identity while recovering the
grid, complete HRLE segmentation and values, Level Set width, and point data.
The previous partial restoration copied only `definedValues` after `Reduce`
had already changed the sparse structure; that path was removed. Successful,
fallback, time-error, and zero-velocity exits discard the snapshot.

The execution smoke configures manual Vulkan using a runtime-generated device
profile, obtains the controller-installed executor, runs the real Vulkan
update, and injects `ERROR` only after the Vulkan callback returned `HANDLED`.
The expected logging exception is caught, the process result remains
`FAILURE`, the executor is called once, and the pre-transaction Level Set width,
segment/point counts, HRLE values, and labeled point data are restored exactly.
The same contract is covered without a Vulkan SDK by eight CPU routing
scenarios, using the CPU update as the correctness oracle.

Windows test executables copy Embree and TBB runtime dependencies through
CMake target expressions. SDK, dependency source, cache, shader, and profile
locations remain environment/caller supplied; no resolved local path is part
of the target interface or tracked documentation. The complete caller-supplied
VTK release tree remains an optional `VIENNAPS_VTK_SOURCE_DIR` input. A
VTK-enabled root configuration currently exposes a ViennaLS/VTK install export
set conflict, so this Level Set execution gate is validated with VTK disabled;
that packaging conflict remains a separate follow-up.

| Gate | Result |
|---|---|
| Real Vulkan seam | controller executor returns `HANDLED` before injected strict failure |
| Process propagation | expected runtime error; recorded result is `FAILURE`; one executor call |
| Full transaction | pre-`prepareLS` snapshot restores width, sparse topology, values, and point data in place |
| CPU oracle | all eight executor-routing scenarios pass in the no-SDK build |
| Vulkan oracle | one-step surface and time match the CPU reference before the injected fault |
| Patch reproducibility | contextual v2 patch applies cleanly to an untouched ViennaLS source worktree |
| Runtime deployment | Embree/TBB DLLs are copied from imported targets, not resolved paths |
| Path policy | tracked inputs contain environment-variable names only and no machine-local path |

## P4A-HRLE-classification: CPU contract and Vulkan candidate kernel

- Status: accepted locally
- Date: 2026-08-02
- Scope: reproduce the ViennaLS HRLE rebuild candidate-classification branches
  as a standalone CPU oracle and an exact FP32 Vulkan compute kernel

The new CPU contract classifies the center and its four or six sparse-star
neighbors into positive undefined, negative undefined, or defined output. It
preserves the ViennaLS interface-crossing epsilon, positive and negative
half-cell clamps, neighbor-distance propagation, cutoff handling, source point
IDs, dimension-dependent neighbor order, and stable batch order. Invalid
dimensions, non-finite active inputs, invalid source IDs, and invalid cutoffs
fail without changing the caller's previous output.

The Vulkan adapter packs centers and six neighbor slots into 16-byte storage
records, validates dispatch, storage-buffer, descriptor, and push-constant
limits, then executes one classification invocation per candidate. Shader
status and returned actions are validated before the result vector is
committed. The integration smoke obtains its candidates through the real
two-dimensional ViennaHRLE sparse-star traversal of a small sphere. All 183
candidates, including 68 defined outputs, match the CPU oracle exactly in
action, source point ID, and FP32 bit pattern.

This slice does not change capability-profile schema v2. Automatic and manual
Vulkan selection remain behind the persisted primitive-suite result and safe
working-set budget. Unknown budgets and estimates above the known safe budget
exclude Vulkan; manual selection reports a hard failure instead of bypassing
these gates. The explicit above-budget policy test covers both Auto fallback
and manual rejection. Before this kernel is wired into the production Level
Set stage, its smoke result must also become part of the deployment-time
primitive suite so a stored profile can unlock it without a per-job prompt.

The production boundary remains Host Canonical Field: ViennaHRLE traversal and
final domain-segment insertion still execute on the CPU. Classification output
is not yet connected to the backend router or ViennaLS rebuild seam. The next
slice must add stable compaction and sparse reconstruction, then connect the
complete transactional path; a standalone low-level Vulkan call is not
evidence that automatic production routing is complete.

| Gate | Result |
|---|---|
| No-SDK CPU contract | active/inactive, clamp, cutoff, 2D/3D, ordering, empty, and transactional validation branches pass |
| Real Vulkan oracle | 183 of 183 sphere candidates match CPU action, point ID, and FP32 bits |
| Device limits | workgroup, buffer range, descriptor count, and push constants fail before dispatch |
| Output transaction | validation, session, shader, dispatch, and readback faults preserve the caller output |
| Deployment policy | primitive-suite and memory-budget gates cover Auto and manual Vulkan; no schema change |
| Integration boundary | traversal, compaction, sparse rebuild, and ViennaLS routing remain pending |
| Path policy | SDK and dependency discovery remain environment/cache supplied; tracked inputs contain no resolved local path |

## P4B-HRLE-compaction: stable Vulkan selection stream

- Status: accepted locally
- Date: 2026-08-02
- Scope: preserve the complete HRLE rebuild action stream while moving stable
  defined-point selection and exclusive offsets to existing Vulkan
  reduction/scan primitives

The CPU oracle records one action for every sparse-star candidate, a
`uint32_t` defined mask, an exclusive-offset stream with `N + 1` entries, and
the stable subset of defined point payloads. Keeping all positive and negative
undefined actions is required because ViennaLS inserts every candidate during
the final sparse-domain rebuild; the compact defined stream alone cannot
reconstruct the domain. Invalid actions, non-finite values, and defined entries
without a source point ID fail transactionally.

The Vulkan adapter reuses the existing normalized-mask, exclusive-scan, count,
and stable `uint32_t` compaction kernels. It compacts original candidate
indices, reads back the `N` offsets and selected count, validates offset/count
agreement, index range, strict stable order, and defined-only selection, then
gathers the defined payloads without changing their FP32 bits. Buffer,
dispatch, and readback failures leave the caller's previous result unchanged.
No new general-purpose shader was required.

The integration smoke uses the same real two-dimensional ViennaHRLE
sparse-star traversal of the small sphere as P4A. Its 183 candidates and 68
defined points match the CPU oracle exactly across the complete action stream,
mask, exclusive offsets, compacted candidate indices, source point IDs, and
FP32 bit patterns. Empty input and invalid action/source/value transaction
cases are also covered.

This is an independently accepted compute slice, not the production HRLE
rebuild route. `ReductionScanPrimitives` currently owns a separate Vulkan
instance/device, so classified decisions are host-visible inputs and payload
assembly remains a CPU gather. The next integration must compose
classification and compaction on one selected `ComputeSession`, keep the
intermediate arrays device-resident, and feed the complete action/defined
streams into transactional sparse HRLE insertion. Deployment capability and
safe-working-set gates remain unchanged until that end-to-end primitive suite
is wired and persisted.

| Gate | Result |
|---|---|
| No-SDK CPU contract | mixed, all-defined, all-undefined, empty, validation, and transactional cases pass |
| Real Vulkan oracle | 183 candidate actions and 68 stable defined payloads match CPU exactly |
| Stable compaction | existing exclusive scan and `uint32_t` compaction kernels are reused |
| Result validation | offsets, count, index range, stable order, and defined-only selection are checked |
| Output transaction | validation and Vulkan failures preserve the caller output |
| Production boundary | same-session device residency, sparse insertion, routing, and persisted deployment smoke remain pending |
| Path policy | SDK discovery is environment supplied; tracked inputs contain no resolved local SDK or dependency path |

## P4C1-HRLE-session-sharing: one selected Vulkan device

- Status: accepted locally
- Date: 2026-08-02
- Scope: allow reduction, scan, and compaction primitives to borrow the same
  deployment-selected `ComputeSession` used by HRLE classification

`ReductionScanPrimitives` now has an external-session initialization overload.
The original path remains compatible by creating and owning a private session,
while the new path borrows the caller's device, queue, command pool, and
capability selection. Reinitialization with the same session is idempotent;
attempting to switch an initialized primitive to another session fails without
invalidating its working state. Reset releases only the primitive-owned Vulkan
objects and command buffer and does not reset a borrowed session.

An explicit destructor and move implementation preserve the required teardown
order for the standalone compatibility path: primitive resources are released
before its privately owned device session. This also prevents a moved-from
object from freeing the moved command buffer. For a borrowed session, the
caller must keep the session object initialized and alive until the primitive
has been reset or destroyed. The shared command context remains externally
synchronized and the current production flow uses it serially.

The real sphere compaction smoke initializes classification and compaction from
one session, verifies their device handles are identical, rejects a different
session, resets compaction without invalidating the shared session, then
reinitializes and repeats the exact CPU/Vulkan comparison. The standalone
reduction/scan and compaction production smokes also pass, preserving existing
callers.

This slice removes duplicate device selection but does not yet keep the
classification-to-compaction arrays device-resident: the current adapter still
reads classifications to host-visible vectors and uploads masks for compaction.
No deployment profile schema or routing rule changes are needed for this
lifetime refactor.

| Gate | Result |
|---|---|
| Shared selection | classification and compaction use the same `VkDevice` from one session |
| Borrowed lifetime | reset preserves the caller session; cross-session reinitialization is rejected |
| Standalone compatibility | private-session initialization and destruction pass both production smokes |
| Exact oracle | the 183-candidate, 68-defined sphere comparison remains bit exact |
| Concurrency boundary | shared command pool is used serially and remains externally synchronized |
| Residency boundary | intermediate classification and mask streams still cross host memory |
| Path policy | SDK and dependency discovery remain environment/cache supplied; no resolved local path is tracked |

## P4C2-HRLE-sparse-reconstruction: transactional CPU insertion oracle

- Status: accepted locally
- Date: 2026-08-02
- Scope: validate the complete classified/compacted stream and reconstruct a
  canonical sparse HRLE domain before production Vulkan routing

The reconstruction contract accepts the stable candidate-index stream, all
defined and undefined actions, the compacted defined payloads, and the old
Level Set defined-point count. It validates exact vector sizes, binary masks,
exclusive offsets, stable compacted candidate IDs, finite defined values,
strict lexicographic candidate order, grid membership, and old point IDs before
modifying either output.

Defined `sourcePointId` values are ViennaHRLE defined-point IDs, not candidate
stream indices. The function therefore returns a stable new-defined-point to
old-defined-point mapping for ViennaLS point-data translation. This corrects an
initial rejected interface that incorrectly required one old value per sparse
candidate; a real rebuild commonly has many more sparse-star candidates than
defined points. Undefined entries are inserted with the exact positive or
negative HRLE sentinel, while defined FP32 values retain their bit pattern.

Validation and local HRLE construction happen before the caller's domain and
source map are committed. Mixed, all-defined, all-undefined, empty, and simple
three-dimensional inputs pass. Invalid sizes, actions, source IDs, offsets,
non-binary masks, non-finite defined values, unordered indices, and out-of-grid
indices fail without changing an already populated output.

This is the CPU canonical insertion oracle, not the final production rebuild.
It currently builds a single-segment HRLE domain and does not translate point
data itself; the returned source map supplies that next seam. Strict recovery
from allocation failure or an exception inside ViennaHRLE `finalize` or
`deepCopy` is not proven. Deployment memory gating and a production
segmentation/point-data comparison remain mandatory before automatic Vulkan
routing is enabled.

| Gate | Result |
|---|---|
| Complete action stream | mixed defined and both undefined signs reconstruct exactly |
| Point-data seam | stable new-point to old-point IDs are returned and range checked |
| Dimensional coverage | focused 2D branches and a simple 3D reconstruction pass |
| Malformed input | sizes, action, mask, offset, finite value, order, grid, and source ID are rejected |
| Validation transaction | domain and source map remain unchanged on every tested false return |
| No-SDK behavior | CPU target builds and all three HRLE contract tests pass without Vulkan |
| Residual boundary | allocation/HRLE exceptions, segmentation, and point-data translation remain pending |
| Path policy | no SDK, dependency, source checkout, or build-cache path is tracked |

## P4C3-runtime: device-local buffer and synchronous transfer foundation

- Status: accepted locally
- Date: 2026-08-02
- Scope: provide the device-local storage and transfer primitive required to
  keep HRLE classification and compaction intermediates on one Vulkan device

The runtime now provides a move-only `DeviceBuffer` backed by device-local
memory with storage, transfer-source, and transfer-destination usage. Upload,
download, and device-to-device copy operations are submitted through the
caller's shared `ComputeSession`. Each operation uses a temporary host-visible
staging buffer where needed, overflow-safe range validation, an explicit
transfer/compute memory dependency, and a synchronous fence wait. This avoids
releasing a command buffer whose submitted transfer might still be pending.

The contract rejects zero-sized allocation or transfer, null host data,
uninitialized buffers, out-of-range copies, aliases, different logical Vulkan
devices, and incompatible repeated creation. Repeating creation with the same
device, size, and session generation is idempotent. Existing host-visible APIs
remain intact. A process-unique non-zero `ComputeSession` generation now moves
with the session, remains stable across idempotent initialization, and changes
after reset/reinitialize. Generation-aware buffers reject foreign or stale
sessions before transfer. A stale wrapper skips destruction through its dead
Vulkan device; legacy handle-only buffers retain the compatibility requirement
to reset before device destruction. Session reset must not run concurrently
with buffer transfer or reset.

The same slice corrects Vulkan buffer teardown order for host-visible and
device-local buffers: the bound buffer is destroyed before its memory is
freed. The new smoke verifies an exact upload, device-to-device copy, and
download round trip, move state, and the principal validation failures.

| Gate | Result |
|---|---|
| Device-local allocation | storage and bidirectional transfer usage pass on the selected Vulkan device |
| Exact transfer | upload, device copy, and download preserve four uint32 values exactly |
| Session isolation | device and generation must match; foreign and reset/reinitialized sessions are rejected |
| Generation lifecycle | idempotent initialize and session moves retain the token; reset clears it; reinitialize changes it |
| Transfer safety | zero, null, uninitialized, out-of-range, alias, and size mismatch cases fail closed |
| Runtime compatibility | deployment context, runtime compute, compute session, and device-buffer smokes pass 4/4 |
| No-SDK behavior | the 17 relevant CPU, policy, HRLE, executor, and probe tests pass |
| Deployment schema | no schema change; use existing suite gate, estimated bytes, and safe working-set threshold |
| Conservative HRLE peak | 2D is at least `120N + 8`; 3D is at least `152N + 8`, plus scan scratch and alignment |
| Residual boundary | concurrent reset, timeout/deferred cleanup, level-set shaders, and materialization remain pending |
| Path policy | no SDK, dependency, source checkout, or build-cache path is tracked |

The deployment-time profile already contains `vulkanPrimitiveSuitePass` and
`safeVulkanWorkingSetBytes`, while each stage supplies `estimatedBytes`.
Therefore P4C3 does not require a profile-schema change. Automatic routing
must require the primitive suite, compute capability, and sufficient safe
working set. Manual selection continues to override policy, but not missing
hardware capabilities or an unsafe memory bound; those cases retain the
existing strict/fallback behavior.

## P4C3-device-scan: resident exclusive offsets and selected count

- Status: accepted locally
- Date: 2026-08-02
- Scope: execute the HRLE defined-mask exclusive scan and selected-count
  calculation without moving intermediate arrays through host-visible memory

`ReductionScanPrimitives` now overloads integer exclusive scan for
generation-aware `DeviceBuffer` inputs and outputs. Recursive block sums and
offsets are allocated on the same `ComputeSession`, and the existing scan-block
and add-offset shader operations are reused. A second device API writes the
compaction count to a one-element device buffer. It validates flags, offsets,
count, session generation, capacity, and aliasing before the N=0 short circuit,
so operation 4 never evaluates `elementCount - 1` for an empty stream.

The existing host-visible reduction, scan, and stable-compaction APIs remain
unchanged. Device dispatch uses explicit transfer/compute and compute/compute
dependencies and waits for fence completion before recycling the shared
command buffer. Final materialization will download the N offsets and count K,
append K as the N+1 tail offset, and compare them exactly with the CPU oracle.

The primitive suite also records the generation of its bound compute session.
Initialization, readiness checks, device access, moves, and reset now reject or
clear stale generations before any dispatch. A moved-from session is detected
fail-closed, and the primitives can be rebound after an explicit reset while
the moved-to session still owns the Vulkan device. The bound session object
must outlive the primitives: callers reset the primitives before resetting,
reinitializing, or destroying the session.

| Gate | Result |
|---|---|
| Scan boundaries | N = 0, 1, 255, 256, 257, and 513 pass |
| Exact oracle | every device exclusive offset equals the CPU prefix sum |
| Count oracle | mixed flags produce the exact CPU selected count on device |
| Session isolation | foreign-generation buffers and stale primitive generations fail before dispatch |
| Validation | length and alias errors fail closed; N=0 does not dispatch the count shader |
| Compatibility | legacy host reduction/scan production checks still pass |
| Combined Vulkan gate | runtime quartet plus reduction/scan production smoke pass 5/5 |
| No-SDK behavior | the 17 relevant CPU, policy, HRLE, executor, and probe tests pass |
| Residual boundary | action-flags shader, 16-byte stable scatter, device classification output, and final readback remain pending |
| Path policy | no SDK, dependency, source checkout, or build-cache path is tracked |

## P4C3-device-classification: resident HRLE decisions

- Status: accepted locally
- Date: 2026-08-02
- Scope: retain HRLE rebuild decisions and shader status in device-local
  buffers for the following compaction stages

The Vulkan classifier now has a move-only device-result API carrying the
decision and status buffers, candidate count, and owning compute-session
generation. Centers and neighbors are uploaded through generation-aware
`DeviceBuffer` staging, while classification output remains on the selected
device after a synchronously completed dispatch. The existing host API is a
compatibility wrapper that materializes the device result only when requested.

The device path validates the 32-bit candidate count, fixed shader workgroup
size, dispatch and storage-buffer limits, descriptor and push-constant
capacity, and all host-side input invariants before publishing a result. Empty
input returns an empty result without allocation or dispatch. Materialization
rejects foreign or stale sessions and undersized buffers, checks shader status,
action range, and finite values, and preserves the caller's output on failure.

| Gate | Result |
|---|---|
| Exact geometry oracle | the 2D sphere produces 183 candidates and 68 defined decisions, bit-exact with CPU |
| Device residency | decisions and status remain device-local until explicit materialization |
| Session isolation | a different compute-session generation is rejected and preserves the output sentinel |
| Empty stream | N=0 performs no allocation or dispatch and returns invalid empty buffers |
| Compatibility | the legacy host classifier is implemented through device classification plus materialization |
| No-SDK behavior | the three CPU HRLE classification, compaction, and reconstruction contracts pass |
| Residual boundary | resident compaction is covered below; sparse HRLE insertion and production routing remain pending |
| Path policy | no SDK, dependency, source checkout, or build-cache path is tracked |

## P4C3-device-compaction: resident stable HRLE selection

- Status: accepted locally
- Date: 2026-08-02
- Scope: compose device classification, action flags, exclusive scan, selected
  count, and stable 16-byte scatter before one explicit terminal materialization

The complete compaction chain now keeps the `N` classification decisions,
defined mask, exclusive offsets, selected count, and compact records on the
same generation-aware `ComputeSession`. The action-flags shader maps only
`DEFINED` decisions to one, the shared primitive suite computes offsets and
`K`, and the scatter shader writes stable records containing candidate index,
FP32 value bits, source point ID, and action. No intermediate host vector is
used between classification and scatter.

Terminal materialization downloads shader status, `K`, the `N` decisions and
offsets, and only the `K` compact records. It validates session generation,
buffer ownership and capacity, action/value/source invariants, every prefix
offset, count agreement, strictly increasing candidate indices, stable scatter
positions, and payload bit equality before transactionally publishing the CPU
result. N=0 returns `{0}` offsets without allocation or dispatch; K=0 remains a
valid all-undefined result. The conservative working-set gate remains at least
`120N + 8` bytes in 2D and `152N + 8` bytes in 3D, plus recursive scan scratch
and allocation alignment.

| Gate | Result |
|---|---|
| Exact geometry oracle | the 2D sphere produces N=183 and K=68; all actions, offsets, indices, source IDs, and FP32 bits match CPU |
| Empty selection | an all-positive one-candidate stream produces K=0 and exact CPU output |
| Metadata fault | a tampered candidate count is rejected while preserving the caller sentinel |
| Device residency | classification through stable scatter remains device-local; host transfer occurs only in the explicit materializer |
| Session and limits | session generation, primitive binding, workgroup, descriptor, push-constant, buffer-range, and capacity checks fail before dispatch |
| No-SDK behavior | focused CPU HRLE classification, compaction, and sparse reconstruction pass 3/3 |
| Working-set policy | existing primitive-suite and safe-working-set deployment gates remain sufficient; no schema or fixed local path is added |
| Residual boundary | CPU sparse reconstruction is composed below; segmented production integration and deployment probing remain pending |

## P4C4-HRLE-transaction: Vulkan selection to CPU sparse field

- Status: accepted locally
- Date: 2026-08-02
- Scope: expose one transactional API from resident Vulkan classification and
  compaction through canonical CPU ViennaHRLE reconstruction

`rebuildHrleRebuildFp32DeviceToCpu<D>` now composes device classification,
action flags, exclusive scan/count, stable scatter, terminal materialization,
and the existing CPU sparse reconstructor. Device intermediates are local to
the call and remain valid until materialization completes. The API rejects an
uninitialized session, mismatched runtime/template dimensions, mismatched
candidate/index counts, and any session-generation change between stages.
Caller domain and source-ID outputs are published only after the complete
transaction succeeds.

The exact oracle builds the same small 2D sphere through the CPU-only and
Vulkan-to-CPU transactions. It compares every candidate index through sparse
iterators, including defined state, stored value bits, defined-value bits, and
the complete source-point-ID vector. The existing N=183/K=68 classification
and compaction oracle remains exact. Candidate-count and dimension failures
preserve the preloaded domain and ID sentinels, and N=0 completes without a
device dispatch.

| Gate | Result |
|---|---|
| Complete structure oracle | CPU-only and Vulkan-to-CPU sparse domains match bit-for-bit on all 183 candidate indices; source-ID vectors are identical |
| Compaction oracle | K=68 stable defined records remain exact |
| Transaction faults | candidate/index and runtime/template dimension mismatches preserve domain and ID sentinels |
| Empty stream | N=0 succeeds through the complete API and publishes an empty source-ID vector |
| Session lifetime | generation is checked after each device stage and immediately before publish |
| No-SDK behavior | focused CPU HRLE classification, compaction, and sparse reconstruction pass 3/3 |
| Production boundary | the current reconstructor creates one segment and a flat source-ID map; segmented domains, point-data translation, and executor commit are not yet production-safe |
| Path policy | no SDK, dependency, source checkout, shader-file, or build-cache path is tracked |

## P4C5-A-HRLE-rebuild-seam: transactional ViennaLS executor contract

- Status: accepted locally
- Date: 2026-08-02
- Scope: add an optional rebuild executor at the real ViennaLS sparse-domain
  replacement boundary without routing production Vulkan work yet

The patched ViennaLS `Advect::rebuildLS()` now exposes an independent rebuild
executor after the scheme-specific cutoff and final width are known. Its input
describes the old HRLE domain and point-data policy; a handled output supplies
a complete replacement ViennaLS domain plus one source-point-ID vector per
HRLE segment. Validation rejects missing domains, grid/boundary mismatches,
invalid segment maps, and source IDs outside the old defined-point range before
the caller-visible domain can change.

ViennaLS retains transaction ownership. It builds and finalizes the replacement
locally, translates point data locally when requested, snapshots the current
domain before commit, and restores that snapshot if commit throws. A callback
fallback, error, exception, or malformed handled output follows the unchanged
CPU rebuild under `FALLBACK`; under `FAIL` it stops the integration stage and
leaves process time, step count, HRLE state, and point data uncommitted. Forward
Euler, RK2, and RK3 now gate subsequent stages on this rebuild error state.
ViennaPS carries the executor through `ProcessContext`, `Process`, and
`AdvectionHandler`, with explicit set/get/clear APIs.

| Gate | Result |
|---|---|
| Real call path | the executor is invoked from `Advect::rebuildLS()`, not the earlier value-update seam |
| CPU fallback | explicit fallback, callback error, exception, and malformed handled output reproduce the CPU result under `FALLBACK` |
| Strict rollback | the same failure classes return failure with exact pre-step level-set values, zero process time, and zero accepted steps |
| API propagation | `Process` set/get/clear and `ProcessContext`/`AdvectionHandler` forwarding are compiled and exercised |
| Integration gates | Forward Euler, RK2, and RK3 stop after rebuild failure |
| Focused validation | the no-SDK routing group passes 17/17; the ViennaLS patch applies cleanly to its pinned source tree |
| Valid handled output | a real 2D replacement commits exact HRLE values and exact scalar/vector PointData selected by the callback-time global source-ID map |
| PointData disabled | `updatePointData=false` commits the replacement and publishes no stale scalar/vector PointData |
| Residual boundary | multi-segment/3D handled input and RK2/RK3 rebuild failure are not yet runtime-tested |
| Production boundary | no segment-aware Vulkan adapter or deployment-suite connection is installed by this slice |

The PointData oracle must sample the source at rebuild-callback time. Earlier
advection preparation can already change the current PointData layout, so a
pre-`apply()` snapshot is not the source addressed by the executor contract.
`translateFromMultiData` consumes the callback's segment vectors in order, but
each value is a global index into that callback-time flat PointData array.

### P4C5-B: segmented ViennaLS Vulkan rebuild adapter

- Status: accepted locally; controller connection pending
- Date: 2026-08-02

`ViennaLsRebuildExecutorFp32<D>` now owns its compute session, reduction/scan
primitives, and three HRLE shader programs through shared state. The callback
collects lexicographically ordered star-neighborhood candidates independently
for every segment in the requested output segmentation, retains global
callback-time source point IDs, and runs the resident
classification/compaction transaction once per output segment.
Canonical HRLE construction remains on the CPU. All segments are assembled in
a private replacement, canonically re-segmented, and their flat source-ID order
is repartitioned against the final per-segment defined counts before one
`HANDLED` publication. Any invalid state, input, device-stage failure, session
generation change, or source-map mismatch returns `ERROR` without changing the
caller output.

| Gate | Result |
|---|---|
| Real seam differential | a small 2D sphere runs one CPU Forward Euler step and one Vulkan rebuild step through the real ViennaLS callback |
| Canonical structure | segment boundaries, defined/undefined values, run types, run breaks, and start indices match the CPU result exactly |
| PointData | scalar and vector arrays match the CPU result exactly after callback-time global source-ID translation |
| Multi-segment contract | the smoke runs with two OpenMP segments; a separate CPU seam matrix validates handled 3D and multi-segment PointData, including `updatePointData=false` |
| Transaction failure | an empty classification program returns `ERROR` and preserves sentinel domain and source IDs |
| Lifetime | the copyable executor captures shared ownership of session, primitives, and SPIR-V programs |
| Residual boundary | later P4C6 verifies D=3 Forward Euler and P4C7 guards RK2/RK3 on CPU; explicit empty-output-segment execution remains untested and session invalidation is assumed not to race the final publish |
| Production boundary | device classification and compaction are accelerated; candidate collection, canonical HRLE construction, re-segmentation, and PointData translation remain CPU work |

### P4C5-C: level-set controller installation and policy routing

- Status: accepted locally; deployment-time suite connection pending
- Date: 2026-08-02

`LevelSetProcessController` now installs the segmented ViennaLS rebuild
executor together with the existing level-set update executor. The controller
keeps rebuild runtime state (session, reduction/scan primitives, and the three
HRLE programs) under shared ownership, and accepts optional rebuild SPIR-V
paths while retaining generated target defaults. Auto mode clears both
executors and returns a degraded CPU result when any rebuild program, primitive,
or session initialization fails. Manual Vulkan returns a strict error, restores
the complete pre-call controller state, and does not install new executors. CPU
selection and `clear()` remove both executor callbacks and restore `FALLBACK`
policy.

| Gate | Result |
|---|---|
| Auto pair install | update and rebuild callbacks are both observable after a valid profile/configuration |
| Auto bad rebuild | missing classification program clears both callbacks and falls back to CPU |
| Manual bad rebuild | returns failure and restores the pre-call update/rebuild callbacks and policy (an initially empty process remains empty) |
| Clear | clears update and rebuild callbacks and restores fallback policy |
| Existing controller regression | auto/manual/stale/shader/gate smoke passes; execution smoke preserves strict rollback |
| Residual boundary | deployment probe does not yet execute the full primitive/rebuild suite; later P4C6 verifies D=3 Forward Euler and P4C7 retains RK2/RK3 on CPU |
| Path policy | all SDK and generated SPIR-V paths enter through environment/CMake configuration and are not persisted in tracked source |

### P4C6: real 3D ViennaLS Vulkan rebuild differential

- Status: accepted locally
- Date: 2026-08-02
- Scope: prove that the segmented rebuild executor dispatches the real D=3
  ViennaLS callback and remains bit-exact with the CPU Forward-Euler oracle

The rebuild executor smoke now uses dimension-templated HRLE fixtures and runs
both the existing 2D sphere and a small 3D sphere through the actual
`Advect<float, 3>` rebuild callback. The Vulkan callback is wrapped only to
count invocations and require an explicit `HANDLED` result; a CPU fallback can
therefore no longer make the differential test pass accidentally. The 3D
fixture uses a one-cell grid and a wider boundary margin so sparse-star
candidate indices remain inside the HRLE grid's half-open domain semantics.

For both dimensions the smoke compares segment count and boundaries, defined
and undefined FP32 bit patterns, every run type/break/start-index array, and
scalar/vector PointData after callback-time global source-ID translation. The
existing invalid-program test still checks that an `ERROR` preserves its
caller-owned domain and source-ID sentinel. OpenMP is fixed to two threads for
stable segmented execution.

| Gate | Result |
|---|---|
| D=3 callback | `Advect<float, 3>` invokes the segmented Vulkan executor and returns `HANDLED` |
| CPU differential | 3D sphere Forward-Euler CPU and Vulkan rebuild outputs match bit-for-bit |
| Canonical structure | segmentation, defined/undefined values, run types, run breaks, and start indices match |
| PointData | scalar/vector arrays and global source-ID translation match exactly |
| Transaction fault | invalid classification SPIR-V preserves the preloaded 2D sentinel output |
| Runtime validation | MSVC build plus direct Vulkan smoke with `OMP_NUM_THREADS=2` exits 0 |
| Path policy | no SDK, dependency, source, shader, or Windows absolute path is tracked |

The remaining Level Set validation gap is the deployment probe's broader 3D
matrix; this slice does not change controller selection policy or the CPU
fallback contract.

### P4C7: temporal-scheme safety gate

- Status: accepted locally
- Date: 2026-08-02

`Process` now exposes a read-only `AdvectionParameters` view so the Vulkan
controller can inspect the actual temporal scheme rather than inferring it from
the callback. The current device update executor is verified only for a single
Forward-Euler step. It therefore remains installed only for Forward Euler.
RK2/RK3 Auto selection clears both level-set callbacks and deliberately returns
a degraded CPU plan; a Manual Vulkan request fails strictly and restores the
complete pre-call callbacks and failure policy.

| Gate | Result |
|---|---|
| Forward Euler | Vulkan update and rebuild callbacks remain installed; the real sphere CPU/Vulkan execution regression passes |
| RK2 Auto | returns CPU/degraded and clears update/rebuild callbacks; a non-empty CPU RK2 oracle advances normally |
| RK3 Manual | returns an error, restores prior callbacks and `FAIL` policy, and the strict process path reports failure |
| API boundary | only a const `Process::getAdvectionParameters()` accessor was added; no mutable context is exposed |
| Residual boundary | a future multi-stage Vulkan callback state machine is required before RK2/RK3 can be accelerated |

This is a correctness gate, not a claim of RK acceleration: unsupported
temporal schemes cannot silently run the one-step shader for every stage.

## P4D-deployment-probe audit: persisted automatic selection gap

- Status: audited; implementation pending
- Date: 2026-08-02
- Scope: verify that deployment-time hardware evaluation can persist enough
  evidence for later Auto selection without recording local SDK or source paths

The current probe enumerates physical-device identity, queues, extensions,
features, limits, memory heaps, and optional memory-budget data. It does not
create a `ComputeSession` or dispatch any primitive or HRLE shader. Its raw
diagnostic therefore reports every validation suite as `not-run`, and the
profile adapter currently writes both `vulkanPrimitiveSuitePass` and
`vulkanFp64SuitePass` as false. A profile generated solely by this probe cannot
unlock Vulkan Auto selection, even when the device supports the required
compute operations.

Profile lookup and hard policy boundaries are already suitable foundations.
The runtime prefers an explicitly configured profile path, then
`VIENNAPS_DEVICE_PROFILE_DIR`, then the external default profile directory; it
rejects missing, invalid, or stale fingerprints and falls back to CPU in Auto.
Manual mode overrides ranking and stage choice but still passes the same
compute-suite, feature, precision, and safe-working-set checks. An unsafe
memory estimate or missing hardware capability therefore remains a hard
rejection rather than a silent fallback unless stage fallback was explicitly
authorized.

| Audit item | Current result | Required exit |
|---|---|---|
| Hardware facts | UUIDs, vendor/device/driver identity, queues, features, extensions, limits, heaps, and budget are queried | keep identity and capability query as the pre-dispatch filter |
| Primitive validation | no shader is dispatched; all suites are `not-run` | execute the production elementwise, reduction/scan, and resident HRLE chain on the selected session |
| Persisted eligibility | profile adapter forces Vulkan suite flags false | publish pass only after every required exact oracle succeeds; publish failure evidence otherwise |
| Multi-device persistence | only the first enumerated deployment profile is written | select by requested UUID or write one atomic record per device |
| Simulation startup | profile is loaded once and a matching session is created only for an eligible Vulkan plan | retain cached automatic selection and exact fingerprint match |
| Manual override | policy choice is overridden, hard capability and memory limits are not | preserve strict diagnostics and explicit `allowStageFallback` semantics |
| Path policy | profile path is caller-controlled and build definitions may contain generated absolute SPIR-V paths | default records outside the repository and never serialize SDK, library, source, build, or shader-file paths |

The implementation should first freeze a failing test proving that a
probe-generated profile is currently ineligible. It then adds one suite runner
that receives shader payloads or an installed shader pack, reuses a single
session, executes deterministic small oracles including the N=183/K=68 HRLE
transaction, and atomically persists versioned suite evidence. Missing SDK or
shader payloads must leave Vulkan disabled without writing a false pass.

### P4D1 validation-evidence adapter and level-set deployment suite

- Status: accepted locally
- Date: 2026-08-02

`VulkanProbeDeviceFacts` now carries explicit `NOT_RUN`, `PASS`, or `FAIL`
evidence for the primitive, FP32, and FP64 suites. The capability adapter
enables `vulkanPrimitiveSuitePass` only when both the primitive and FP32 suites
explicitly pass. It enables `vulkanFp64SuitePass` only when the device exposes
`shaderFloat64` and the FP64 suite explicitly passes. Defaults, failures,
unknown enum values, missing FP32 evidence, and unsupported FP64 remain
fail-closed. The budget and fingerprint contracts are unchanged.

The deployment probe now connects the existing level-set update and HRLE
rebuild transaction to this evidence gate when all generated SPIR-V artifacts
are available. It creates one device-pinned `ComputeSession`, dispatches the
FP32 level-set update, and runs the 2D sphere HRLE classification, action flags,
recursive scan, stable compaction, and CPU sparse reconstruction transaction.
The update values, defined/undefined HRLE values, defined-value bits, and
source-point-ID map are compared with the CPU oracle before a suite pass is
published. A missing artifact, dispatch error, CPU mismatch, or the
`VIENNAPS_VULKAN_PROBE_FORCE_SUITE_FAIL` deployment test hook records a `FAIL`
reason and leaves the persisted Vulkan suite gates false; no schema-3 field or
machine-local path is introduced. The raw schema-1 diagnostic adds the
`validation.levelSetSuiteReason` string while the schema-2 profile remains
backward compatible.

The focused no-SDK adapter test still covers default, pass, fail, unknown,
precision-feature, and safe-budget invariants. On the local Intel Arc device,
the deployment probe reports `primitiveSuite=pass`, `fp32Suite=pass`, and
`levelSetSuiteReason="level-set update and HRLE rebuild CPU differential
passed"`; the generated schema-2 profile round-trips and validates.

### P5-B1: exact FP32 NeutralTransport surface velocity kernel

- Status: accepted locally; process/controller integration pending
- Date: 2026-08-02

`NeutralTransportSurfaceModelFp32` establishes the first Vulkan surface-model
contract for the etch-front branch of
`impl::NeutralTransportSurfaceModel::calculateVelocities`. It accepts
coverage, material IDs, and velocity as three independent FP32 SoA buffers and
retains the CPU model's parameter order. The kernel uses an explicit uint32
mantissa long division with guard/sticky nearest-even rounding for finite normal
FP32 values. This avoids a reproducible one-ULP difference in the device's
hardware division without requiring `shaderInt64`.

The host validates every parameter, coverage value, and intermediate multiply
or divide stays zero or normal finite FP32. Subnormal, NaN, infinity, and
overflow/underflow paths reject before dispatch and preserve the caller's
velocity sentinel; they remain CPU work until a separately proven contract is
available.

| Gate | Result |
|---|---|
| CPU differential | 1,299/1,299 NeutralTransport velocity outputs are bit-exact on Intel Arc; max ULP is 0 |
| Floating-point boundaries | signs, positive/negative zero, minimum/maximum normal values, and varied parameter tuples are bit-exact |
| Fail-closed input | subnormal coverage, NaN parameters, invalid lengths, aliases, and out-of-range lengths return errors without overwriting output |
| CTest | a fresh `VIENNAPS_BUILD_TESTS=ON` configuration discovers and passes the surface smoke |
| Scope boundary | only the fixed etch-front velocity formula is accelerated; coverage evolution, graph diffusion, ray transport, and process/controller routing remain CPU |
| Path policy | Vulkan SDK and SPIR-V locations are generated or supplied through the local environment; no machine-specific path is tracked |

### P5-N1: CPU-neutral velocity executor seam

- Status: accepted locally as an explicit, CPU-canonical injection seam; the
  Vulkan bridge and Process-level policy route remain pending
- Date: 2026-08-03

`NeutralTransportVelocityWork<T>` exposes coverage, material-ID, and candidate
velocity spans plus the explicit etch-front parameter tuple without exposing a
Vulkan or ProcessContext type. `NeutralTransportSurfaceModel` uses it only
when an explicit executor is installed. A candidate is committed only when the
callback returns success and reports the complete output count; absence,
failure, exception, or incomplete output reruns the unchanged CPU formula.
`stickingCoefficient` bookkeeping remains on the existing CPU path.

| Gate | Result |
|---|---|
| CPU fallback | focused float and double test retains the historical velocity result for no executor, callback failure, exception, and incomplete output |
| Transaction boundary | the output candidate is zero-initialized, passed by span, and committed only after explicit complete/count acknowledgement |
| Explicit execution | a successful caller-provided executor replaces only the velocity candidate; no backend is selected automatically |
| Direct validation | an independent MSVC C++20 compile and focused executable completed successfully; root CMake/CTest remains externally gated by CPM release-asset access |
| Scope boundary | no Vulkan type enters the public model header; no GPU bridge, numerical promotion, Process/Context wiring, or B2A surface-diffusion change is included |

### P5-B2: exact FP32 CSR graph-diffusion primitive

- Status: accepted on local Vulkan hardware by CPU differential; surface-process
  integration remains pending
- Date: 2026-08-02

`SurfaceGraphDiffusionFp32` implements one explicit FP32 graph-diffusion
step for the CSR form of `SurfaceDiffusionSolver::stepExplicit`. One Vulkan
invocation owns one CSR row and performs its products and accumulation in the
input edge order, then applies `field + diffusionStep * laplacian`. The shader
uses `precise`, and its generated SPIR-V contains `NoContraction` decorations
for the multiply/add operation boundaries.

The primitive now also accepts a caller-owned `ComputeSession` and a
device-resident `DeviceBuffer` variant. Row offsets, columns, weights, field,
and output remain on that session's device for the dispatch; only the explicit
caller upload and terminal download cross the host. Host validation mirrors
the uploaded spans so malformed CSR, non-finite values, stale/foreign session
generations, aliases, and insufficient capacity fail before submission.

The host validates a deliberately strict finite-normal-or-zero FP32 domain
before dispatch: CSR offsets must be monotonic and complete, columns must be
in range, all intermediate products/sums/scaled values must stay in the
domain, buffers must be non-aliasing and device-owned, and invalid input is
rejected before it can write the output buffer. Empty `N=0` CSR input is a
successful no-op.

| Gate | Result |
|---|---|
| CPU differential | CPU row-order volatile FP32 oracle and Intel Arc Vulkan output are bit-exact (max ULP 0) for the five-node, eleven-edge mixed-sign CSR fixture |
| Surface invariants | constant field is preserved; a symmetric three-node peak is bit-exact, mass-conserving, and smoothing |
| Dispatch boundaries | unaligned output tail remains intact; empty CSR is a no-op |
| Fail-closed input | malformed row-offset length, out-of-range columns, NaN weight, undersized output, buffer aliasing/capacity/device mismatch, and strict-domain violations preserve caller output |
| Standalone build | a fresh `VIENNAPS_BUILD_VULKAN_SURFACE_SMOKE=ON`, probe-off build links the runtime, executes the Intel Arc smoke, and discovers the CTest |
| Device residency | borrowed-session DeviceBuffer path keeps all five CSR/field/output buffers device-local through dispatch; terminal output is bit-exact to the existing CPU oracle |
| Device empty graph | a valid zero-node DeviceBuffer graph preserves its tail sentinel; aliased empty buffers are rejected before the no-op |
| Scope boundary | this is a reusable CSR primitive only; `psSurfaceDiffusion` and coverage/process/controller routing remain CPU |

### P5-B3: independent FP32 coverage convergence delta metric

- Status: accepted locally as a bounded correctness primitive; no production
  route is enabled
- Date: 2026-08-03

`CoverageDeltaMetricFp32` is an independent surface primitive for the
channel-major coverage convergence delta. One invocation owns one channel and
walks the original point order; no cross-thread or atomic reduction is used.
The strict FP32 host oracle rejects non-normal values, empty point domains,
intermediate overflow/underflow, malformed dimensions, aliases, and capacity
errors before dispatch. GPU results are first written to a private scratch
buffer so rejected calls do not publish caller output.

This primitive deliberately does not change `psCoverageManager`,
`psFluxProcessStrategy`, `Process`, backend policy, or any production route.
The production tolerance remains a double-valued gate, and any future bridge
must first satisfy the P5-B2 surface-diffusion integration prerequisite before
this metric can be considered for controller use.

| Gate | Result |
|---|---|
| CPU differential | MSVC and local Intel Arc Vulkan smoke passed 1/1. It covers three channels at N=1, 16, and 257 with bitwise FP32 comparison, output-tail sentinels, and both host-visible and device-resident entry points |
| Fail-closed input | Invalid shape, N=0, NaN, FP32 overflow, aliases, and insufficient capacity are rejected before publication |
| Dispatch shape | One work item per channel; channel-major contiguous input and one FP32 output per channel |
| Production boundary | No `CoverageManager`/`Process`/backend routing change; production convergence tolerance remains double |
| Bridge prerequisite | P5-B2 surface-diffusion integration is required before any production bridge is considered |

### P5-C: deterministic FP32 ray-record reducer

- Status: accepted locally as a bounded correctness primitive; traversal and
  process integration pending
- Date: 2026-08-02

`DeterministicRayReducer` defines the Vulkan handoff between future ray
generation and surface-flux aggregation. A record contains stable `rayId`,
`surfaceId`, and FP32 weight; the canonical CPU oracle orders records by
`(surfaceId, rayId, inputIndex)` and uses that fixed order for one FP32 sum per
surface. The current Vulkan shader deliberately uses one invocation and the
same selection order, so it establishes reproducibility before any scalable
parallel sort/reduction is introduced.

GPU output is first materialized in private temporary buffers. The shader has
an explicit output-capacity guard, and the host validates returned count,
strictly increasing surface IDs, surface-domain membership, and finite
normal-or-zero FP32 weights before publishing caller buffers or `outputCount`.
CPU and Vulkan rejection paths preserve caller IDs, weights, and count.

| Gate | Result |
|---|---|
| CPU differential | Intel Arc output is bit-exact to the CPU ordered reducer, including a shuffled record order and repeated dispatch |
| Determinism | identical input is stable; duplicate `(surfaceId, rayId)` records retain input-index order |
| Transaction boundary | output tail sentinels and `outputCount` survive invalid surface IDs, NaN weights, and insufficient output capacity |
| Empty input | `N=0` is a valid no-op |
| Shader validation | `spirv-val` passes; generated SPIR-V retains `NoContraction` on its FP32 additions |
| Build and test | a local-cache, ray-only configuration builds the Intel Arc smoke and its focused CTest passes |
| Scope boundary | this is an O(N²), one-invocation correctness baseline; it does not trace rays, intersect geometry, sample particles, or route production fluxes |

Post-acceptance correction: the shader seeds each surface accumulator from its
first sorted input record instead of `+0.0`. This preserves a singleton
negative-zero weight exactly, matching the CPU oracle; the real-device smoke
now asserts the sign bit explicitly.

### P5-D: exact FP32 ray-triangle hit primitive

- Status: accepted locally as a bounded correctness primitive; ray-batch and
  process integration pending
- Date: 2026-08-02

`TriangleHitPrimitive` adds the corresponding deterministic geometry primitive:
the CPU oracle and compute shader both scan triangles in ascending index order
with the same Moller-Trumbore FP32 operation order. A strict `t < bestT`
replacement gives coincident hits the lowest triangle index. The shader uses
explicit `precise` dot and cross helpers, and generated SPIR-V marks their
floating-point arithmetic and the final hit calculations `NoContraction`.

The host has separate origin/near, direction/far, triangle, and hit buffers;
all are required to be non-aliasing and owned by the selected device. Strict
normal-or-zero finite validation, non-negative ordered near/far limits, and
capacity checks execute before dispatch. Results first enter a private hit
buffer; only a valid fixed miss sentinel or an in-range finite hit is copied
to the caller, so invalid input and malformed shader output cannot partially
publish a result.

| Gate | Result |
|---|---|
| CPU differential | Intel Arc Vulkan output is bit-exact to the CPU oracle for a hit, equal-distance tie, and misses; `t`, `u`, and `v` have max ULP 0 |
| Determinism | triangles are scanned by ascending index and coincident nearest hits select the lower index |
| Transaction boundary | output tail sentinels survive an undersized capacity, NaN input, negative `tMin`, and the `N=0` no-op |
| Input and output domain | CPU and GPU reject subnormal or non-finite data, invalid near/far limits, buffer aliases, wrong-device buffers, and insufficient capacity before caller output is published |
| Shader validation | `spirv-val` passes and the generated SPIR-V contains the required `NoContraction` decorations |
| Build and test | the local ray-only Intel Arc smoke prints `triangle hit Vulkan dispatch PASS`; the focused CTest passes 1/1 |
| Scope boundary | this is a bounded O(rays times triangles) primitive only; no BVH, ray generation/reflection, particle sampling, surface-flux routing, or process selection is accelerated |

Post-acceptance correction: the host-side result guard now compares a hit's
`t` against packed `tMin` (`origin.w`), rather than `origin.x`. The smoke adds
a translated triangle/ray where `origin.x > t`, proving the valid GPU result
is retained and remains bit-exact against the CPU oracle.

### P5-E feasibility gate: neutral-transport coverage reaction remains CPU

- Status: confirmed CPU-only on the local adapter; FP32 Vulkan implementation
  remains declined and a separate FP64 capability-gated proposal is required
  before reconsidering it
- Date: 2026-08-03

The actual `NeutralTransportSurfaceModel<float, D>::updateCoverages` loop is
not an FP32-only contract. `constants::N_A` is declared as `double`; its
product with `surfaceSiteDensity` promotes the adsorption division to double.
The optional desorbed-flux fallback uses `0.` and the explicit update uses
`1.`, which also promote their respective expressions. Both the steady-state
ratio and explicit time-step calculation therefore execute with double
intermediates and only round when the coverage vector is written.

No FP32 shader or build directory was created for this slice. Substituting an
FP32 equation would be a different numerical model and cannot satisfy the
required CPU bitwise oracle. This work remains CPU by default. A future Vulkan
route must first pass the selected device's `shaderFloat64` gate and a direct
CPU/GPU double-intermediate differential, or separately specify and validate a
software-double implementation. Manual configuration may select CPU or any
eligible backend, but cannot force an unavailable or numerically inexact
Vulkan route past either gate.

The local deployment check on Intel Arc (driver `101.8860`) reports
`shaderFloat64 = false`; its FP32 signed-zero/Inf/NaN preservation,
denormal preservation, and round-to-nearest-even controls are all true, but
they do not satisfy this double-intermediate requirement. The persisted
hardware profile must therefore keep neutral-transport coverage reaction on
CPU for Auto selection and reject a Manual Vulkan request with that capability
diagnostic on this adapter.

### P5-F: deterministic ray hit-to-record batch

- Status: accepted locally as a bounded composition primitive; reduction and
  process integration pending
- Date: 2026-08-02

`RayHitBatchPrimitive` is the explicit seam between P5-D geometry hits and
P5-C's reducer input: it scans hit records in source-ray order and emits only
non-miss `(rayId, surfaceId, weight)` records. It performs no floating-point
calculation; the ray weight, including a negative-zero bit pattern, is copied
exactly. The CPU oracle uses the same stable input order. Both primitives can
be initialized on one external `ComputeSession`, so buffers produced by the
triangle-hit dispatch are accepted directly by the batch primitive.

Before dispatch the host validates hit sentinels, hit/barycentric domains,
strict FP32 weights, capacity, aliasing, and device ownership. It derives the
entire expected output sequence from the validated host inputs. GPU records
first enter private buffers; the returned count and every ray, surface, and
weight bit pattern must equal that sequence before caller SoA buffers and count
are published.

| Gate | Result |
|---|---|
| CPU differential | mixed hit/miss records, same-surface records, and negative-zero weights are bit-exact; max ULP is 0 for every copied weight |
| P5-D composition | Intel Arc smoke uses one shared session: P5-D computes the hit buffer, P5-F compacts it, and the P5-D CPU oracle followed by the CPU batch oracle matches exactly |
| Transaction boundary | caller tails and count survive insufficient capacity, NaN weight, malformed hit, and `N=0` no-op cases on the actual device |
| Shader validation | `spirv-val` passes; the shader has no FP arithmetic, so no `NoContraction` claim is required |
| Build and test | a clean-first local ray-only build prints `ray hit batch Vulkan dispatch PASS`; the focused CTest passes 1/1 |
| Scope boundary | this is one-invocation stable compaction only; it does not run P5-C reduction, use device-resident chaining, sample particle physics, trace reflections, map materials, normalize flux, or route a Process |

### P5-G: staged ray-flux pipeline

- Status: accepted locally as an end-to-end staged composition baseline; no
  production ray/process routing
- Date: 2026-08-02

`RayFluxPipeline` composes P5-D triangle intersection, P5-F hit compaction,
and P5-C deterministic reduction through one shared `ComputeSession`. The CPU
oracle applies the same three accepted CPU primitives in sequence. On the GPU
path, each component retains its independently validated host-visible staging
and readback boundary; this is deliberately a staged pipeline, not a claim of
device-resident dispatch fusion.

| Gate | Result |
|---|---|
| CPU/GPU differential | Intel Arc executes a 5-ray/3-triangle fixture with hits, misses, same-surface accumulation, and a singleton negative-zero weight; output count, surface IDs, and all output FP32 words are exact (0 ULP) |
| Shared-session composition | P5-D, P5-F, and P5-C initialize against the same `ComputeSession`; the GPU result is produced by their actual dispatches, not by a CPU fallback |
| Transaction boundary | malformed weight and insufficient final capacity preserve all caller result slots and count; `N=0` is a success no-op for both CPU and GPU APIs |
| Lifetime boundary | the aggregate is intentionally non-movable because each primitive stores a pointer to the shared session; `device()` follows the primitive convention and returns an empty device when uninitialized |
| Build and test | the local ray-only target prints `ray flux pipeline Vulkan dispatch PASS`; its focused CTest passes 1/1 |
| Scope boundary | no BVH, device-resident intermediate buffers, ray generation/reflection, particle sampling, material/coverage coupling, flux normalization, Process routing, or production backend selection is implemented |

### P5-H: fused device-resident ray-flux correctness baseline

- Status: accepted locally as a one-dispatch device-resident correctness
  baseline; not a performance or Process-routing backend
- Date: 2026-08-02

`FusedRayFluxPrimitive` performs triangle intersection, hit selection, and
deterministic per-surface reduction in one `local_size_x=1` dispatch. Its
input, used-mask scratch, and output buffers are `DeviceBuffer` instances:
the host uploads inputs before dispatch and reads only the final count and
reduced columns after it completes. It neither reads back P5-D hits nor
materializes P5-F records on the host between those stages. A CPU evaluation
using the accepted P5-D/P5-F/P5-C helpers is deliberately retained as an
integrity guard; caller storage is committed only after GPU count, IDs, and
every FP32 word compare bit-for-bit.

| Gate | Result |
|---|---|
| GPU work and residency | Intel Arc executes actual Vulkan work over device-local inputs, `used` scratch, and result buffers; the fused shader, rather than a CPU fallback, computes the returned candidate result |
| Exactness contract | Moller-Trumbore tracing keeps the first equal-distance triangle; reduction selects ascending `(surfaceId, rayId)` order and seeds from the first selected weight, preserving singleton `-0`; generated SPIR-V validates and contains `NoContraction` decorations |
| CPU/GPU differential | the 5-ray/3-triangle fixture covers hit, miss, repeated surface accumulation, and singleton negative zero; count, IDs, and all weights are exact (0 ULP) before caller output changes |
| Transaction boundary | malformed weight, insufficient output capacity, and the existing nonzero-surface-domain rejection preserve result slots and count; `N=0` remains a success no-op |
| Build and test | MSVC builds `viennaps-vulkan-ray-flux-fused-smoke`; direct Intel Arc execution prints `fused ray flux Vulkan dispatch PASS` and focused CTest passes 1/1 |
| Scope boundary | the one-invocation `O(rays * triangles * rays)` shader and mandatory CPU integrity guard are intentionally too slow for production; no BVH, scalable sort/reduce, particle transport, reflection, material/coverage coupling, flux normalization, Process route, or automatic backend selection uses it |

### P5-I: parallel device-buffer triangle-hit producer

- Status: accepted locally as the composable D-stage for the next ray
  pipeline; no production Process route
- Date: 2026-08-02

`DeviceTriangleHitPrimitive` dispatches one Vulkan invocation per ray and
writes the existing 16-byte `TriangleHit` ABI directly to caller-owned
`DeviceBuffer` storage. It supports an owned or external `ComputeSession`, so
a subsequent device stage can consume origin, direction, triangle, and hit
buffers without a host round trip. Its upload/download helpers are explicit
boundary operations for setup and tests; `dispatch()` itself neither reads
back results nor runs a CPU fallback.

| Gate | Result |
|---|---|
| Exact GPU work | Intel Arc runs a 64-lane ray dispatch; triangle selection, translated near/far limits, miss, and equal-distance first-triangle handling match `intersectCpu` bit-for-bit for `t`, index, `u`, and `v` |
| Device contract | every buffer must be valid, non-aliased, large enough, and owned by the primitive session's Vulkan device and generation; the output remains a reusable device buffer after the dispatch |
| Numeric contract | the shader uses precise scalar subtract/cross/dot operations, finite checks, and strict nearest-hit comparison; generated SPIR-V validates and contains `NoContraction` decorations |
| Transaction boundary | invalid capacity, alias, foreign session/device, and `N=0` are rejected or accepted before dispatch without changing the caller's hit-buffer sentinel data |
| Build and test | MSVC builds `viennaps-vulkan-triangle-hit-device-smoke`; direct Intel Arc execution prints `triangle hit device Vulkan dispatch PASS` and focused CTest passes 1/1 |
| Scope boundary | this is intersection only: it does not compact hits, sort records, reduce flux, maintain a BVH, perform a per-call CPU integrity check, or route particle/process work |

### P5-JA: stable device-resident ray-record compaction

- Status: accepted locally as the first scalable ray-flux data stage; no
  production Process route
- Date: 2026-08-02

`DeviceRayRecordCompactor` receives the `DeviceTriangleHitPrimitive` output
and a device-resident weight-word buffer, then runs a GPU flags pass, the
existing `ReductionScanPrimitives` DeviceBuffer exclusive scan/count writer,
and a stable GPU scatter. It produces the 16-byte std430-compatible
`RayRecord { rayId, surfaceId, weightBits, reserved }` layout, retaining the
input order of accepted rays. Weight data is declared as `uint` in the shader
and copied directly into `weightBits`; this deliberately has no floating-point
arithmetic, so a singleton negative zero cannot be canonicalized.

| Gate | Result |
|---|---|
| Exact GPU work | a real P5-I hit DeviceBuffer feeds flags -> device scan/count -> scatter without intermediate host readback; after final download, records and `weightBits` (including `-0`) match the CPU `compactCpu` oracle exactly |
| Device contract | the primitive supports owned/external `ComputeSession`; nonempty input/output buffers must be valid, non-aliased, sized, and owned by the active device and session generation; a post-scatter compute/transfer visibility barrier makes records available to the next device stage |
| Capacity and transaction | because P5-JA does not read the device count back, it conservatively requires `outputCapacity >= rayCount` before the first dispatch; invalid capacity, input/output alias, foreign session, zero surface domain, and `N=0` leave caller record sentinels unchanged |
| Numeric and shader gate | generated SPIR-V passes `spirv-val`; it exposes a uint weight/record interface and contains no FP arithmetic or `NoContraction` requirement |
| Build and test | MSVC builds `viennaps-vulkan-ray-record-compaction-smoke`; direct Intel Arc execution prints `ray record compaction Vulkan dispatch PASS` and focused CTest passes 1/1 |
| Scope boundary | scan/count and scatter are currently ordered through multiple submissions from the reusable scan primitive, rather than P5-J's final one-command submission; no device radix sort, segment reduction, exact dynamic capacity admission, BVH, particle transport, or Process route is included |

### P5-JB: device-resident stable ray-record rank-sort baseline

- Status: accepted locally as a composable correctness baseline; it is not a
  production acceleration route
- Date: 2026-08-02

`DeviceRayRecordSort` consumes the P5-JA record and count buffers directly in
the same `ComputeSession` and writes one sorted `RayRecord` per accepted
input. Each invocation computes its output rank by comparing
`(surfaceId, rayId, originalIndex)`, establishing the required stable order
without downloading the device count or records during `sort()`. The shader
uses only integer words, so `weightBits`, `reserved`, and a negative-zero
weight are copied byte-exactly.

| Gate | Result |
|---|---|
| Exact GPU work | a real P5-I hit buffer feeds P5-JA flags/scan/scatter and then P5-JB; the terminal download matches `compactCpu` plus host `stable_sort` record-for-record, including `weightBits`, `reserved`, repeated surfaces, and `-0` |
| Device contract | records and count stay device-resident; nonempty buffers must be non-aliased, valid, sufficiently sized, and owned by the active device and session generation; a compute/transfer barrier makes the result reusable by a following stage |
| Capacity and transaction | because the dynamic device count is intentionally not read back, P5-JB conservatively requires `outputCapacity >= inputCapacity`; invalid capacity, record aliases, a foreign session, and `N=0` preserve caller sentinels |
| Numeric and shader gate | generated SPIR-V passes `spirv-val`; the rank shader has no floating-point arithmetic and needs no `NoContraction` decoration |
| Build and test | MSVC builds `viennaps-vulkan-ray-record-sort-smoke`; two direct Intel Arc runs print `ray record sort Vulkan dispatch PASS` and focused CTest passes 1/1 |
| Scope boundary | this is deliberately an O(N^2) rank-sort correctness baseline, not the planned scalable route; it does not implement the required device radix scratch/ping-pong pipeline, segment reduction, dynamic exact capacity admission, BVH, particle transport, or Process routing |

### P5-JB2A: capacity-gated device-resident radix-sort candidate

- Status: accepted locally as a bounded GPU candidate; it is not yet the
  unbounded production radix route and has no Process routing
- Date: 2026-08-02

`DeviceRayRecordRadixSort` replaces P5-JB's per-record rank calculation with
sixteen stable 4-bit LSD passes: eight `rayId` nibbles followed by eight
`surfaceId` nibbles. Every pass uses a 64-record workgroup histogram, a
device-only 256-workgroup tile scan, a device-only scan of the tile sums, and
a stable scatter into ping-pong `RayRecord` buffers. The shader moves four
uint words, so `weightBits` (including `-0`) and `reserved` are never touched
by floating-point arithmetic. The P5-JA count buffer remains the dynamic work
limit; `sort()` does not download either count or records.

| Gate | Result |
|---|---|
| Exact GPU work | Intel Arc runs a 16,448-ray P5-I -> P5-JA -> P5-JB2A chain; its 14,048 compacted records span two prefix tiles and match `compactCpu` plus stable host ordering exactly after the terminal download |
| Stable-order differential | a separate device-buffer permutation contains repeated `(surfaceId, rayId)` keys, arbitrary `reserved` words, and a negative-zero word; output matches host `stable_sort`, proving surface-primary/ray-secondary order and equal-key stability |
| Device and transaction contract | owned/external sessions, device and generation ownership, non-aliasing, buffer sizes, and conservative `outputCapacity >= inputCapacity` are enforced before dispatch; alias, insufficient capacity, foreign-session, `N=0`, and hierarchy-overflow calls leave the output and tail sentinels unchanged |
| Capacity gate | the two-level implementation supports at most 256 tiles of 256 workgroups of 64 records, further limited by `maxComputeWorkGroupCount[0]`; requests beyond the resulting device limit fail before buffer validation or allocation, rather than truncate or dispatch zero groups |
| Numeric and shader gate | all three generated SPIR-V modules pass `spirv-val` and contain no floating-point arithmetic or `NoContraction` requirement; the final barrier exposes the last scatter to a terminal transfer download and the next compute stage |
| Build and test | the standalone `gpu/vulkan` CMake entry builds `viennaps-vulkan-ray-record-radix-sort-smoke` under MSVC; two direct Intel Arc runs print `ray record radix sort Vulkan dispatch PASS` and focused CTest passes 1/1 |
| Scope boundary | per-workgroup rank remains a fixed 64-lane scan and prefixing is only two levels; no recursive device hierarchy, exact dynamic output admission, segment reduction, BVH, particle transport, or Process route is included |

### P5-JB2B: recursively prefixed device-resident radix sort

- Status: accepted locally as the scalable ordering foundation; it remains
  unconnected to Process routing
- Date: 2026-08-02

P5-JB2B removes the fixed 256-tile software gate. The existing 64-record
histogram and stable 16-pass ping-pong scatter are unchanged: eight `rayId`
nibbles run before eight `surfaceId` nibbles. Prefix `mode=0` produces local
workgroup offsets and level-0 tile sums; `mode=1` recursively scans each
256-way sum level in device-local storage; `mode=2` propagates parent prefixes
back down; the fixed 16-item `mode=3` scan converts digit totals to exclusive
digit bases; and `mode=4` combines those bases with the level-0 offsets before
scatter. The host constructs only capacity-derived buffer and dispatch geometry
and never reads a count, histogram, prefix, or record during `sort()`.

| Gate | Result |
|---|---|
| Exact GPU work | two direct Vulkan executions of the 16,448-ray P5-I -> P5-JA -> P5-JB2B chain pass; all 14,048 compacted records match `compactCpu` followed by stable host ordering byte-for-byte after its terminal download |
| Stable-order differential | a separate device-buffer permutation with repeated `(surfaceId, rayId)`, arbitrary `reserved` words, and a negative-zero `weightBits` word matches `stable_sort` exactly; output-tail sentinels are unchanged |
| Recursive hierarchy contract | CPU structural checks cover group counts `1`, `64`, `257`, `65,536`, `65,537`, `1,000,000`, and the largest count representable from the uint32 input capacity; they exercise the 256/257 threshold and multi-level plans without allocating a huge device buffer |
| Hardware admission | `sort()` requires a 256-invocation/256-x workgroup, 16 y-dimension workgroups, 1 KiB shared memory, and scratch buffers within `maxStorageBufferRange`; descriptor ranges are bounded to the actual records, histogram, and hierarchy bytes rather than `VK_WHOLE_SIZE` |
| Boundary behavior | the smoke invokes the device's `maxComputeWorkGroupCount[0] + 1` capacity when representable and verifies rejection before user-buffer validation/allocation; alias, insufficient capacity, foreign-session, and `N=0` cases retain their existing no-write contract |
| Numeric and shader gate | histogram, recursive-prefix, and scatter SPIR-V pass `spirv-val`; their disassembly contains no floating-point arithmetic or `NoContraction` requirement |
| Build and test | the standalone `gpu/vulkan` CMake entry builds under MSVC; two direct GPU executions print `ray record radix sort Vulkan dispatch PASS` and focused CTest passes 1/1 |
| Scope boundary | this proves recursive ordering and capability admission, not a physical >256-tile run on every adapter; exact dynamic output admission, segment reduction, BVH, particle transport, Process routing, and multi-stage composition remain out of scope |

### P5-JC: device-resident surface segments and ordered FP32 reduction

- Status: accepted locally as the post-sort aggregation seam; it remains
  unconnected to Process routing and final command fusion
- Date: 2026-08-02

`DeviceRaySurfaceReducer` consumes a record/count buffer that the caller has
already ordered stably by `(surfaceId, rayId)`, as established by P5-JB2B. It
marks surface changes in a device flag buffer, uses the existing device-buffer
exclusive scan and compaction-count primitive to construct dense segment
indices, then lets only segment-start invocations perform one sequential FP32
sum each. The first `weightBits` word seeds that sum, and `precise` produces a
`NoContraction` SPIR-V `OpFAdd`; no record or count is downloaded during
`reduce()`. The reduced surface-id, weight, segment flags, offsets, and count
remain `DeviceBuffer` objects for the next composition stage.

| Gate | Result |
|---|---|
| Exact CPU differential | an eight-slot device fixture with seven sorted records produces the same four dense surface outputs and count as `reduceCpu`, compared bit-for-bit after the terminal download; it includes a singleton negative-zero segment and the order-sensitive `1e20 + (-1e20) + 0.25` sequence |
| Segment contract | flags are `index == 0 || surfaceId[index] != surfaceId[index - 1]`; the device scan maps every active record to its dense segment, leaves the inactive capacity tail zeroed, and drives the device-only segment count |
| Device boundary | `reduce()` takes and returns only device buffers and has no production host download; the current flags, scan/count, and reduction passes use separate ordered submissions, so this is not yet the final one-command P5-J composition |
| Hardware admission | input and output capacity must fit `uint32_t`; a 64-invocation x-workgroup, dispatch count, byte products, storage-buffer ranges, session generation, non-aliasing buffers, and exact descriptor ranges are checked before scratch allocation or dispatch |
| Boundary behavior | alias, insufficient output capacity, foreign-session buffers, and zero-capacity requests are covered; rejected calls leave the existing output buffer unchanged |
| Numeric and shader gate | both generated shaders pass `spirv-val`; the reduction disassembly contains `NoContraction` and no fused multiply-add, preserving the tested ordered FP32 additions |
| Build and test | standalone `gpu/vulkan` build under MSVC succeeds; two direct Intel Arc executions print `ray surface reduction Vulkan dispatch PASS`, and focused CTest passes 1/1 |
| Scope boundary | P5-JC requires the P5-JB2B sorted-input contract and does not independently prove sorting; it deliberately has no device-visible non-normal/overflow status, so CPU-equivalent rejection of non-finite or out-of-domain intermediate sums is not claimed by P5-JD and remains a later P5-JE/deployment gate |

#### Strict-FP32 deployment-probe finding

The local Intel Arc driver reports `shaderDenormPreserveFloat32`,
`shaderSignedZeroInfNanPreserveFloat32`, and
`shaderRoundingModeRTEFloat32` as supported. A separately assembled,
`spirv-val`-valid reduction shader that requested the corresponding
`DenormPreserve`, `SignedZeroInfNanPreserve`, and `RoundingModeRTE` execution
modes nevertheless exceeded a 60-second isolated smoke timeout on this driver.
The experimental shader and build directory were removed; no such mode is
enabled by the accepted P5-JC implementation. Therefore, the deployment record
must treat the static float-control properties as a candidate only: a
process-isolated, watchdog-bounded numerical smoke must pass bitwise CPU
differential cases before the strict GPU profile is selected automatically.

### P5-RAY-PHYSICS: CPU-side ray physics contracts

- Status: `DONE-LOCAL` — CPU-side contracts accepted on main 2026-08-06.
- Predecessor: `P5-JD/JE/JF-REGRESSION-HARDENING` device data chain and
  `P5-RAY-ROUTE` Process route injection.
- Exclusive scope:
  - `include/viennaps/ray/ray_reflection.hpp`
  - `include/viennaps/ray/ray_roulette.hpp`
  - `include/viennaps/ray/ray_event_queue.hpp`
  - `include/viennaps/ray/ray_surface_response.hpp`
  - `tests/rayPhysics/rayPhysics.cpp` and `tests/rayPhysics/CMakeLists.txt`

This slice defines deterministic, CPU-testable contracts for reflection,
Russian roulette, event ordering, and material/surface response. It deliberately
reuses ViennaRay's authoritative CPU helpers and the existing `SurfaceModel` /
`MaterialMap` CPU contracts; no Vulkan types appear in the public headers and no
Process / `FluxProcessStrategy` behavior is changed.

| Contract | Reused authority | Verified property |
|---|---|---|
| Diffuse/specular/coned-cosine reflection | `viennaray::ReflectionDiffuse` / `ReflectionSpecular` / `ReflectionConedCosine` | Normalized reflected direction, mirror law, and cone degeneracies |
| Orthonormal basis | `rayInternal::getOrthonormalBasis` | Three orthonormal axes for an arbitrary input vector |
| Russian roulette | ViennaRay constants `0.1*initialWeight` and `0.3*initialWeight` | High weights continue unchanged; low weights survive with unbiased probability `w/renewWeight` |
| Event queue | `std::priority_queue` with `(particle, bounce, sequence)` ordering | Deterministic pop order and insertion-order tie-breaking |
| Surface response | `MaterialMap::isMaterial` and `SurfaceModel::getCoverages` | Masked materials zero weight; multi-mask lists supported; coverage scalar lookup handles missing/out-of-range labels |

Acceptance evidence: `rayPhysics` CTest passes in the reused Release MSVC C++20
build (a temporary `.tmp_*` evidence directory) under `-DVIENNAPS_BUILD_TESTS=ON`:

```bat
cmake --build <build-dir> --config Release --target rayPhysics
ctest --test-dir <build-dir> -C Release -R rayPhysics --output-on-failure
```

Result: `Test #81: rayPhysics ....................... Passed 0.54 sec`.

This card does **not** add device shaders, multi-bounce end-to-end validation,
or automatic Process routing; those remain gated by `P5-SURFACE-INTEGRATION` and
`P5-MODEL-MATRIX`.

### P5-K1: strict-FP32 numerical evidence profile and routing gate

- Status: accepted locally as the persistence and policy seam; P5-K2 below
  supplies its isolated numerical-smoke producer
- Date: 2026-08-02

The capability profile schema is now version 3. Its Vulkan numerical-smoke
evidence records `NOT_RUN`/`PASS`/`FAIL`, the fixed contract id, CPU case and
bitwise mismatch counts, max ULP, elapsed and watchdog milliseconds, and a
failure diagnostic. The parser is strict about types, required fields, unknown
or duplicate members, and integer overflow, including nested hardware and
evidence objects. Schema 1 and 2 records remain readable but default this
evidence to `NOT_RUN`, so older deployments fail closed for strict Auto
selection rather than accidentally receiving a new guarantee.

The probe-to-profile adapter transports the evidence but does not manufacture
it from advertised float-control features. Auto Vulkan selection now requires
an exact strict contract, nonzero case count, zero bitwise mismatches and ULP,
the configured watchdog, a non-timeout elapsed value, and no failure
diagnostic. A normal Manual Vulkan selection can still override Auto after the
existing availability, primitive-suite, budget, precision, and ray-mode gates;
its plan explicitly says that no strict FP32 guarantee is present. A caller
that explicitly requests strict Manual Vulkan is rejected with the same clear
evidence diagnostic when that evidence is absent or incompatible.

| Gate | Result |
|---|---|
| Schema compatibility | focused profile-I/O tests round-trip schema 3 and load schema 1/2 with `NOT_RUN` defaults |
| Parser boundary | unknown/duplicate fields and uint64 overflow are rejected, including nested hardware and strict-smoke objects |
| Policy boundary | Auto fails closed before a valid strict smoke; normal Manual Vulkan retains an explicit non-strict override; strict Manual Vulkan fails until the evidence passes |
| Focused validation | `backendPolicy`, `capabilityProfileIO`, and `probeProfileAdapter` compile and pass under the MSVC C++20 test harness |
| Scope boundary | P5-K1 itself owns no watchdog executable or device numerical suite; P5-K2 supplies that producer without static float-control promotion or an automatic production Process route |

### P5-K2: isolated strict-FP32 deployment smoke

- Status: accepted locally as a deployment-profile producer; it is not a
  production Process route
- Date: 2026-08-02

`viennaps-device-probe --strict-fp32-smoke` starts an isolated child of the
same executable and applies the fixed 60-second watchdog. The child executes a
dedicated, non-experimental Vulkan shader and returns raw FP32 words, rather
than relying on advertised float-control properties or requesting float-control
execution modes. Its 18 bitwise cases comprise the CPU oracle's signed-zero and
ordered-cancellation checks plus 16 GPU results, including signed zero and the
same ordered cancellation expression. The parent accepts only well-formed PASS
evidence with the exact contract id, zero mismatches and ULP distance, the fixed
watchdog, a non-timeout elapsed value, and an empty diagnostic.

The child result is written with OS-level exclusive creation (`CREATE_NEW` on
Windows and `O_EXCL | O_NOFOLLOW` on POSIX), so it cannot truncate an existing
path. Failed children remove their own result; successful results are consumed
and removed by the parent, which also best-effort removes its candidate path
on every abnormal child exit. Launch, wait, timeout, nonzero child exit,
malformed evidence, contract-invariant violation, and UUID ambiguity all fail
closed. The parent applies evidence only to exactly one deployment record whose
Vulkan device UUID equals the child UUID, preventing a smoke from one adapter
from authorizing another.

The CMake shader target discovers `glslc`/`glslangValidator` normally through
the environment or toolchain; no local SDK path is committed. It is independent
of the general smoke target, and the optional ViennaLS suite is feature-checked
so the strict probe itself remains buildable when the Vulkan SDK is present but
the complete level-set dependency set is absent.

| Gate | Result |
|---|---|
| Actual GPU differential | Intel Arc executes all 18 cases with bitwise match, zero ULP, and a PASS result persisted under the matching profile record |
| Isolation and cleanup | a deliberately forced child failure returns failure to the parent and leaves no child-result file behind |
| Artifact validity | the generated strict shader passes `spirv-val --target-env vulkan1.2` |
| No-SDK behavior | a fresh configure with `VULKAN_SDK` unset builds the diagnostic probe stub; the strict smoke returns FAIL and exit code 1 |
| Scope boundary | this creates validated deployment evidence only; Process routing, general automatic backend enablement, dynamic intermediate-status propagation, BVH, and particle transport remain separate work |

### P5-JD: device-resident ray-flux composition baseline

- Status: revalidated locally on the Release Vulkan configuration; it is not the
  final one-command path or a production Process route
- Date: 2026-08-04

`DeviceRayFluxPipeline` is the physical composition of
P5-I triangle hits, P5-JA record compaction, P5-JB2B stable radix ordering,
and P5-JC surface reduction. It allocates and uploads only the original
ray, triangle, and weight inputs, retains all hit/record/count/segment
intermediates in the shared `ComputeSession`, and downloads only the terminal
surface id, weight, and count. The first acceptance fixture is five rays and
three triangles: three hits on surface 0 reduce to `3.0`, one translated hit
is a singleton `-0`, and one ray misses. Every terminal word is compared with
the existing CPU pipeline oracle. The standalone Vulkan build compiles the new
smoke under MSVC; its Release execution prints
`device ray-flux pipeline Vulkan dispatch PASS`, and the focused CTest passes
1/1.

This deliberately changes the prior milestone wording: the reusable stage
APIs each own and submit their command buffer today, so P5-JD cannot honestly
claim a single submission. P5-JE is the subsequent command-recording refactor
that will share scratch buffers, insert explicit stage barriers, and execute
the whole chain in one terminal submission. The P5-JD acceptance boundary is
therefore device residency and CPU-bitwise terminal differential only.

| Gate | Result |
|---|---|
| Residency | no host readback of hits, compacted records, sorted records, flags, offsets, or the active count occurs before final reduction completion |
| CPU oracle | five-ray simple-geometry output count, surface ids, FP32 weight bits, and caller output tails match `RayFluxPipeline::runCpu` exactly |
| Transaction | malformed inputs and conservative output-capacity rejection leave caller output spans and count unchanged; zero rays preserve the caller output, while nonempty rays with no triangles produce a zero count without dispatch |
| Scope boundary | no claim of one-command fusion, strict intermediate FP32 rejection equivalence, BVH, particle transport, Process routing, or automatic backend eligibility |

### P5-JE: single-command device-resident ray-flux compute chain

- Status: revalidated locally on the Release Vulkan configuration; it is not a
  production Process route or strict-FP32 deployment promotion
- Date: 2026-08-04

P5-JE turns the P5-I -> P5-JA -> P5-JB2B -> P5-JC composition into one
primary compute command buffer and one queue submission guarded by one fence.
The primitive stages now expose record-only APIs: they validate ownership and
capacity, update their descriptors, record their barriers/dispatches, and
never reset, begin, end, submit, wait, or download. The aggregate owns the
command buffer, the shared recursive scan scratch, and every intermediate
buffer through the terminal fence. Input upload and terminal result download
remain explicit transfer boundaries and are intentionally excluded from the
compute-submission count.

| Gate | Result |
|---|---|
| One-compute-submit contract | `DeviceRayFluxPipeline::lastComputeSubmissionCount()` reports exactly `1` after the complete recorded chain; all record-only APIs were checked to contain no lifecycle, submit, wait, or download operation |
| CPU bitwise differential | the real Intel Arc device runs the five-ray/three-triangle fixture; count, dense surface ids, FP32 weight words (including singleton `-0`), and output-tail sentinels match `RayFluxPipeline::runCpu` exactly |
| Integration correction | the initial differential exposed a compact count of 5 instead of 4: `recordDispatch` had recorded into its private member command buffer rather than the supplied primary buffer. It now records every Vulkan command into the supplied buffer; the compact and reduced device counts are 4 and 2 respectively before terminal readback |
| Regression group | Release standalone `gpu/vulkan` target build succeeds and five focused CTests pass: triangle-hit device, ray-record compaction, recursive radix sort, surface reduction, and the full device ray-flux pipeline |
| Scope boundary | no device-visible non-finite/overflow intermediate status, strict-FP32 deployment probe, exact dynamic output admission, BVH, particle transport, Process routing, or automatic backend eligibility is claimed |

### P5-JF: strict-FP32 reduction status and fail-closed terminal commit

- Status: revalidated locally on the Release Vulkan configuration; it closes
  the device-visible intermediate-sum rejection gap in the P5-JE aggregate,
  but is not a production ray-tracing or Process route
- Date: 2026-08-04

P5-JF adds a one-word, device-local sticky status buffer to surface reduction.
The reduction shader validates the accumulator seed, every addend, and every
ordered `precise` FP32 sum using raw IEEE-754 bits: only a normal value or an
exact signed/unsigned zero is admissible. NaN, infinity, and subnormal values
set the status atomically and terminate that segment. The aggregate clears the
status in its sole recorded command buffer, reads it before any terminal result
buffer, and returns failure without changing caller output spans or count when
the flag is nonzero. The non-recording primitive follows the same contract.

| Gate | Result |
|---|---|
| CPU rejection oracle | Two `FLT_MAX` records for one surface overflow their ordered FP32 sum. `RaySurfaceReducer::reduceCpu` rejects the input and leaves prefilled output ids, weights, and count unchanged. |
| Device reduction | The same two-record fixture sets the device status. `DeviceRaySurfaceReducer::reduce()` returns failure and its caller-owned device output buffers retain their sentinels. |
| Aggregate transaction | `DeviceRayFluxPipeline::runCpu()` and `runGpu()` both reject the same-surface overflow fixture; the GPU path checks status before count/id/weight readback, so its output sentinels and count remain unchanged. |
| Existing normal path | The five-ray/three-triangle CPU differential and device ray-flux smoke pass in Release, preserving the accepted bitwise normal-path result. |
| Revalidation | `spirv-val --target-env vulkan1.2` accepts `ray_surface_reduce.comp.spv`; the five focused P5-JD/JE/JF CTests, including the strict aggregate overflow fixture, pass in the fresh Release Vulkan build. |
| Broader-suite revalidation | The current standalone `gpu/vulkan` Release CTest registration contains 12 ray tests and the complete suite passes 12/12. The prior 24/24 wording is not reproducible from the current standalone registration and is not used as an acceptance denominator. |
| Scope boundary | This adds numerical failure propagation only. It does not add a BVH, reflection/multi-bounce transport, CUDA callable-equivalent surface physics, Process routing, dynamic-output admission, or automatic backend promotion. |

### P5-R1: compute-only flattened BVH triangle traversal

- Status: accepted locally as the first non-brute-force ray-intersection
  primitive; build and physical-transport integration remain pending
- Date: 2026-08-03

`TriangleBvhHitPrimitive` builds a median-split flattened BVH on the CPU,
expands every node bound outward by one FP32 representable value, then uploads
nodes, packed triangles, and stable original-triangle indices to device-local
buffers. A six-buffer compute shader traverses that representation; it has no
Vulkan ray-tracing extension dependency. CPU construction/upload is the
explicit geometry-change boundary in this slice. Intersection dispatch uploads
rays and downloads only final hits.

P5-R2 retains that convenience path and adds `recordDispatch`: a caller-owned
command buffer can consume device-resident origin/direction buffers and produce
device-resident hits without beginning, ending, submitting, waiting, or
performing host transfers. It validates session generation, buffer ownership,
capacity, and aliases before recording the transfer-to-compute and
compute-to-compute barriers required by the next device ray-flux stage.

P5-R3 exposes that traversal as an optional `DeviceRayFluxSpirv` path. When
provided, each `runGpu` call builds/uploads the BVH at its explicit
geometry-change boundary, omits the brute-force triangle buffer/upload, and
records BVH hits followed by the existing device compaction, stable radix sort,
and strict surface reduction into one primary command buffer. An empty path
keeps the established brute-force implementation unchanged.

P5-R4 adds explicit `prepareGeometry`/`runGpuPrepared`/`resetPreparedGeometry`
for callers that can prove the surface is unchanged across multiple flux
evaluations. Preparation builds/uploads once; prepared runs upload only rays
and weights before the same one-submission chain. A failed replacement
preparation invalidates the old prepared state before attempting the new build,
so stale device geometry can never be reused. The existing `runGpu` remains
the conservative rebuild-on-every-call API.

| Gate | Result |
|---|---|
| ABI and traversal | A 32-byte node stores conservative bounds, `leftFirst`, and leaf count. Internal nodes reserve adjacent child roots before recursive construction, so `leftFirst` and `leftFirst + 1` remain valid at arbitrary tested depth. |
| CPU oracle | The smoke compares the raw `TriangleHit` fields from `intersectCpu` against device results. A normal-scale 20-triangle, six-ray tree puts equal-distance IDs 9 and 10 in opposite root branches; the device first visits ID 10 but must replace it with ID 9. A separate `1e-31` x-direction case verifies that a small nonzero slab direction is not culled. A successful empty-BVH rebuild returns all misses without a self-referential root traversal. A deterministic LCG differential over 32 triangles and 16 rays reports zero bit mismatches (seed `0x5EED`). |
| Record-only chain | The smoke records P5-R2 into a caller command buffer, submits that buffer exactly once, and raw-bit compares downloaded hits to the CPU oracle. A null-command-buffer rejection preserves a pre-uploaded device hit sentinel. |
| End-to-end composition | With the optional BVH SPIR-V path, a 20-triangle/six-ray cross-root equal-distance fixture flows through compaction, sort, and reduction in one compute submission. CPU/GPU count, surface IDs, and weight bits agree; the lower tied surface ID 9 is present. |
| Prepared geometry | After one prepare, two identical 20-triangle/six-ray weighted runs remain bit-identical and each submits once. Empty or failed replacement preparation invalidates the prepared route; its next call rejects before modifying output sentinels. |
| Transaction boundary | Invalid normal-FP32 inputs and undersized output return before dispatch and preserve caller sentinels. A zero-ray call preserves the output sentinel. |
| Build and test | A fresh standalone Ninja build using MSVC `19.44.35223` and the local Intel Arc Vulkan adapter builds `viennaps-vulkan-triangle-bvh-hit-smoke`; focused CTest passes 1/1. |
| Scope boundary | This is CPU-built BVH plus compute traversal and optional device-ray-flux selection. It does not perform GPU BVH construction/refit, reflection or multi-bounce transport, particle/material physics, Process integration, or use Vulkan RT extensions. |

### P5-R5: fixed-topology device BVH refit

- Status: accepted locally as a fixed-topology correctness primitive; no
  production route is enabled
- Date: 2026-08-03

`recordRefit` records a caller-owned dynamic-vertex copy followed by leaf and
bottom-up internal-node AABB refit. It neither begins/ends/submits/waits nor
reads back; explicit compute/transfer barriers make repeated refits and a
following traversal ordered. The topology and node-ID ranges stay private to
the primitive. Host validation is strict normal-or-zero FP32, exact triangle
count, capacity, device, and session generation.

| Gate | Result |
|---|---|
| Build and hardware smoke | MSVC 19.44 built the new shader, library, and focused smoke; local Intel Arc Vulkan CTest passed 1/1 and `spirv-val` accepted the refit shader. |
| Fail-closed smoke contract | The smoke covers null command, host-mirror count mismatch, NaN, subnormal, undersized, foreign-session, and stale-generation dynamic buffers; each rejection checks raw node preservation and an unchanged hit sentinel. |
| Refit oracle | Two consecutive refits (moved geometry then restored geometry) compare CPU/GPU raw hit fields and manually computed root raw bounds; the moved fixture changes the equal-distance hit owner from 9 to 10. A full CPU rebuild is not a refit oracle because repartitioning can change the fixed topology. |
| Alias boundary | The public API cannot alias the primitive's private BVH buffers; the implementation retains a defensive handle-alias check, but no test hook exposes those private buffers. |
| Scope boundary | This is fixed-topology refit only. It does not add GPU BVH construction, topology mutation, Process routing, surface diffusion, Vulkan RT extensions, or automatic backend promotion. |

### P5-R6: prepared-pipeline dynamic vertex refit

- Status: accepted locally as an explicit prepared-geometry refit route; no
  automatic production selection is enabled
- Date: 2026-08-03

`DeviceRayFluxPipeline::refitPreparedGeometry` keeps the existing default
`runGpu(rays, triangles, ...)` rebuild path unchanged while updating an
already prepared fixed-topology BVH through R5's caller-command refit. The
pipeline owns a persistent packed float4 vertex buffer, performs strict
triangle-count and normal-or-zero FP32 preflight, submits the refit once, and
only then replaces the prepared host mirror. Invalid user preflight preserves
the prior prepared route; an untrusted refit submission or wait clears its
prepared marker fail-closed. A successful default `runGpu` rebuild explicitly
invalidates any prepared route before replacing the shared BVH, so a later
`runGpuPrepared` cannot mix two geometry generations.

| Gate | Result |
|---|---|
| Prepared-state transaction | Count, NaN, and subnormal refit attempts are rejected while the old prepared geometry still produces the original raw CPU/GPU result. |
| Dynamic refit oracle | A moved tie triangle refits, then `runGpuPrepared` matches the moved CPU count, surface IDs, and weight bits; a second refit restores the original prepared result. |
| Submission semantics | The refit and each prepared traversal report one compute submission; uploads and terminal transfers remain outside that compute count. |
| Rebuild interlock | `prepare(A) -> runGpu(B) -> runGpuPrepared` rejects with zero compute submissions and unchanged output sentinels after the default rebuild invalidates A's marker. |
| Build and hardware smoke | A fresh standalone Vulkan build under MSVC `19.44.35223` succeeded; local Intel Arc focused CTest passed 1/1. |
| Scope boundary | No automatic backend promotion, policy selection, topology mutation, Process integration, or B2A coupling is added. |

## Next slice

The segmented rebuild adapter is installed by the level-set controller, the
deployment probe executes the small FP32 update plus HRLE transaction before
persisting the primitive gate, and D=3 Forward Euler has a real differential.
RK2/RK3 remain deliberately CPU-only until a multi-stage device state machine
is proven. The first surface velocity formula is now exact but intentionally
unwired to process selection. Direct negative/non-finite time injection and
the optional VTK-enabled install/export conflict remain validation gaps.

P5-JD, P5-JE, and P5-JF are now locally revalidated: the device-resident ray
composition, one-submit recording chain, and strict-FP32 fail-closed terminal
status are evidenced on Intel Arc. The next serial boundary is to release the
uncommitted regression-hardening claim, then connect the accepted ray data path
to a real Process route. P5-R1 through P5-R6 provide compute-BVH traversal and
prepared/refit primitives, but GPU construction, physical transport
reflection/roulette, surface-model coupling, and Process routing remain open.

P5-K2 populates P5-K1's evidence with an isolated watchdog probe; its
dedicated raw-word shader and process boundary cannot reuse the HostVisible
radix helpers. The P5-N2/B2B/COV bridges likewise remain explicit seams until
the shared Process/deployment installation and model support matrix are
accepted. CPU differential checking remains an explicit validation gate, not a
production per-call guard. Coverage reaction is a capability-gated FP64
candidate, without weakening the current fail-closed gate.

After the P5 route and model gates, the single-developer order advances to the
P6 linear-algebra/oxidation cards and then P7 resident execution, calibration,
soak, recovery, and release gates. The detailed dependency order is recorded
in `Total-plan continuation board: P5 to P7` below.

### P5-K3: synchronous deployment-profile provisioning seam

- Status: accepted locally as a deployment-thread-only provisioning seam; it
  does not yet expose a Process/controller configuration API
- Date: 2026-08-02

`provisionDeploymentProfile` resolves the existing per-device profile first and
invokes a caller-supplied synchronous probe at most once for missing, stale, or
invalid state. Probe output must be schema-valid and match all seven current
hardware-fingerprint fields before it is atomically persisted and re-read. An
incomplete current fingerprint is rejected before lookup or callback
invocation, including the `unknown-device.json` path. Any callback, validation,
directory, write, or re-read failure returns an explicit failure while retaining
the CPU fail-closed plan. Replacement uses a temporary file and platform-safe
atomic move, preserving a last-known-good target when a replacement fails; no
fixed local paths are introduced.

| Gate | Result |
|---|---|
| Reuse/probe cardinality | focused CPU-only test covers valid reuse with zero callback calls and one-call provisioning for missing/stale/invalid profiles |
| Fail closed | focused tests cover incomplete fingerprint, callback absence, throw, failure, mismatch, and a write-path failure; the implementation also fail-closes on re-read failure |
| Persistence hygiene | successful provisioning creates the parent directory only when needed, re-reads a VALID record, and leaves no temporary files |
| Replacement safety | implementation uses atomic replacement without pre-deleting a target; focused coverage verifies a failed directory destination is preserved and its temporary artifact is cleaned |
| Scope boundary | no Process routing, Vulkan probe implementation, flux-engine, or worker-thread changes are included |

### P5-K3B: Level Set deployment-profile preparation facade

- Status: implemented as a controller-only pre-apply seam; no generic Process,
  FluxEngine, ray, or shader/pipeline routing is changed
- Date: 2026-08-02

`LevelSetProcessController` retains its existing `configure` overload, while
an explicit `configureResolved` entry accepts a `DeploymentProfileDecision`
resolved by the deployment/configuration thread. On the Vulkan-capable AUTO or
Manual-Vulkan path, the resolved plan must match the explicit `StageWorkload`;
it is then consumed without profile lookup or probing. Manual overrides remain
authoritative: an explicit CPU request bypasses profile/decision validation and
is executable without a profile or complete hardware fingerprint, while Manual
Vulkan requires a valid matching persisted profile and returns an explicit
failure instead of silently degrading. AUTO uses the resolved plan and degrades
to CPU when the plan is a fail-closed missing/stale profile plan. Controller
configuration still completes before worker callbacks are installed.

| Gate | Result |
|---|---|
| Deterministic preparation | CPU-only controller smoke covers a valid resolved plan, plan-stage mismatch, missing-profile fail-closed plan, manual CPU bypass, and explicit Manual Vulkan rejection without requiring a GPU |
| Cache boundary | resolved-decision configure path does not call profile I/O or a deployment probe; legacy configure remains source-compatible and performs its established profile resolution |
| Hardware binding | a Vulkan-selected resolved plan is rejected unless the decision is VALID, has a profile, and its complete fingerprint matches current hardware; the selected session also matches every Vulkan-observable identity field (UUIDs, vendor/device IDs, name, driver version), with a focused driver-version mismatch rejection; driver date remains a deployment-time stale-gate field |
| Scope boundary | only the Level Set controller seam, its focused smoke, the runtime context preparation overload, and this status entry are changed; generic Process, CUDA, FluxEngine, ray, fixed SDK/VTK paths, and profile persistence remain untouched |

### P5-K3C: isolated strict-probe deployment adapter

- Status: implemented as a reusable synchronous `DeploymentProfileProbe`
  factory; it is intended for install/configuration hosts and is never called
  from Process/controller apply paths
- Date: 2026-08-02

`makeVulkanDeploymentProfileProbe` builds the exact argv for one
`viennaps-device-probe --strict-fp32-smoke --write-deployment-profile <path>
--validate-profile` invocation. The default launcher uses tokenized
`fork`/`exec` (or `CreateProcess` on Windows), with a parent watchdog; tests
can inject a launcher without requiring a Vulkan device. The adapter accepts
only a caller-selected, non-existing absolute output path inside a canonical
directory, rejects traversal, symlink, or non-regular output, preserves an
already-existing target, and removes a generated output on every post-launch
exit path.

Before returning a candidate record it requires schema version 3, all seven
hardware identity fields, an exact identity match with the current deployment
fingerprint, and PASS strict-FP32 evidence with the versioned contract,
nonzero case count, zero mismatches/ULP, the required watchdog, and bounded
elapsed time. Launch, timeout, malformed-output, schema, identity, and evidence
failures are diagnostic and fail closed; no candidate record is trusted on
failure.

| Gate | Result |
|---|---|
| CPU-only handoff | injectable fake launcher test covers one-call success and profile handoff to `provisionDeploymentProfile` without a Vulkan device |
| Fail closed | focused test covers nonzero/timeout, malformed output, mismatched identity, and invalid strict evidence; generated outputs are removed |
| Process isolation | default path passes a vector of argv tokens to shell-free process creation and retains the existing strict child watchdog boundary; no SDK path is introduced |
| Scope boundary | only the public adapter header, focused CPU test, and this design section are changed; Process/controller, CUDA/OptiX, shaders, and profile persistence are untouched |

### P5-K3D: deployment bootstrap composition core

- Status: implemented as a synchronous, CPU-testable composition helper; it
  remains outside Process/controller execution paths
- Date: 2026-08-03

`bootstrapVulkanDeploymentProfile` composes an injectable hardware collector,
the existing strict probe factory, and `provisionDeploymentProfile`. AUTO and
Manual-Vulkan configurations collect hardware and invoke the probe only when
the selected profile is missing, stale, or invalid. The runtime facade collects
the exact Vulkan physical-device identity and enumeration index, then forwards
that index to the strict child probe when known. Probe output is generated
under a unique absolute path in an existing caller-selected transient directory
(or the system temporary directory selected by the runtime facade) and is
removed by the existing probe adapter. A manual configuration bypasses both
collector and probe only when every requested stage resolves to CPU, and then
succeeds with an incomplete fingerprint. Collector, path, launcher,
validation, and persistence failures retain the existing CPU fail-closed plan.

| Gate | Result |
|---|---|
| Composition/cardinality | CPU-only fake collector/launcher test covers one collection and one probe for a missing profile |
| Manual CPU bypass | focused test succeeds without hardware, collector, probe executable, or launcher callbacks |
| Failure boundary | collector failure and unavailable transient output produce a CPU plan without invoking the probe |
| Temp hygiene | generated output is unique under the selected directory; probe adapter removes it on success/failure |
| Runtime identity | actual Vulkan smoke verifies default and manual-UUID sessions produce the same complete fingerprint and physical-device index; the index reaches the probe argv when known |
| Scope boundary | bootstrap core, runtime selected-device collector/facade, focused CPU test, runtime smoke/CMake wiring, probe index forwarding, and this status section are changed; Process/controller, CUDA/OptiX, shaders, and profile persistence remain untouched |

### P5-K3E: Level Set deployment-session integration seam

- Status: implemented and CPU-executed; full top-level CTest remains a
  long-running dependency gate
- Date: 2026-08-03

`LevelSetDeploymentSession<D>` is the application-facing deployment-thread
facade for Level Set execution. `provision()` composes the existing selected
device bootstrap exactly once and caches the request, selected hardware,
resolved decision, and effective probe executable, including a fail-closed
CPU decision. A subsequent `provision()` returns the cached result with
`reused=true`; callers must use `resetDeployment()` before changing workload,
selection, device, profile, or probe configuration. `configure()` is strictly
cache-only and delegates to `configureResolved()`, so it cannot repeat profile
I/O, hardware collection, or strict probing during a simulation task.

The explicit bootstrap probe path has precedence over
`VIENNAPS_DEVICE_PROBE_PATH`; no SDK, VTK, or fixed local path is recorded in
source. A Manual-CPU request retains the deployment core's executable bypass
and never initializes Vulkan or invokes a collector/probe. AUTO and
Manual-Vulkan retain the existing CPU fail-closed or explicit-error behavior.

The level-set CMake slice now enables the prerequisite primitives subproject
when the level-set smoke is selected, and publishes `Vulkan::Vulkan` as a
public runtime dependency because runtime public headers expose Vulkan types.
The new smoke is intentionally emitted only by a top-level build that provides
the `ViennaPS` target; the standalone GPU subproject reports the omission
instead of producing an under-specified executable.

| Gate | Result |
|---|---|
| CPU deployment execution | a locally compiled temporary executable passed Manual-CPU bypass, repeat-provision cache reuse, explicit-probe-over-environment precedence, environment fallback, fake collector/probe cardinality, and fail-closed cache retention; no CUDA device was used |
| Cache-only controller seam | an isolated MSVC C++20 syntax check, against the project's versioned ViennaLS patch, compiled the `configure()`/`configureResolved()` type path; it introduced no dependency-cache modification |
| CMake dependency contract | standalone level-set configuration now reaches generation with the primitives dependency present; the deployment-session smoke is deliberately skipped unless the top-level `ViennaPS` target supplies ViennaHRLE/ViennaLS include contracts |
| Release profile/deployment cluster | the top-level Release tree now passes the focused 9-test cluster (`deployment-session`, both Process-controller smokes, `backendPolicy`, `capabilityProfileIO`, `deploymentProfile`, `probeProfileAdapter`, `vulkanDeploymentBootstrap`, and `vulkanDeploymentProbe`) 9/9; this closes the local focused evidence but not the full long-running suite |
| Residual top-level gate | full top-level CTest remains a long-running dependency gate; the VTK-enabled install/export variant is separately blocked by the ViennaLS/VTK export-set conflict recorded below |
| Temp hygiene | all temporary compile overlays and `build-p5k3e-*` validation directories were removed after validation; the generic `build` directory was preserved |
| Scope boundary | deployment-session header/smoke, Vulkan CMake dependency propagation, and this status entry are changed; Process/controller behavior, CUDA/OptiX, shader algorithms, profile persistence, and fixed local paths remain untouched |

### P6-C: focused Vulkan smoke option propagation

- Status: implemented as a CMake-only validation seam; no computation or
  simulation behavior changed
- Date: 2026-08-03

The root project now declares the surface-model focused smoke option alongside
the existing runtime, primitive, level-set, and ray switches. The Vulkan probe
helper recognizes ray and surface switches when Vulkan is otherwise disabled,
while keeping every focused switch default-off. Runtime, primitive, and level-set
CTest registration accepts either CMake's `BUILD_TESTING` or ViennaPS's
`VIENNAPS_BUILD_TESTS`; each smoke family additionally requires its focused
switch. Runtime deployment checks use the documented runtime/level-set
deployment switches, while primitive smoke tests additionally require the
explicit primitive switch, so prerequisite targets pulled in by level-set or
ray builds do not silently become test selections. SDK and dependency paths
remain environment/cache supplied.

| Gate | Result |
|---|---|
| Option surface | root parses `VIENNAPS_BUILD_VULKAN_SURFACE_SMOKE`; all focused switches remain opt-in |
| Registration | runtime, primitives, and level-set smoke tests use the same test-enable condition as ray/surface |
| Compatibility | standalone `gpu/vulkan` defaults and existing target names are unchanged |
| Validation | temporary CMake configure and target-graph checks are run outside the repository; no full build or CUDA path is required |
| Residual gate | root CPU-only configure and the two new executor CTests now pass through a temporary patched ViennaLS override; full VTK/Vulkan smoke coverage remains a separate follow-up |
| Scope boundary | only CMake option/probe propagation, smoke test registration, and this status record changed |

### P6-D: verified offline CPM bootstrap

- Status: accepted locally as a build-bootstrap reliability repair; simulation
  behavior and dependency versions are unchanged
- Date: 2026-08-03

`cmake/cpm.cmake` now hashes an existing `CPM_DOWNLOAD_LOCATION` before doing
anything else. A cache file matching the pinned `CPM_HASH_SUM` is included
directly; a missing or mismatched file follows the original pinned release URL
and `EXPECTED_HASH` download path, and is re-hashed before inclusion. This
preserves `CPM_SOURCE_CACHE` precedence while allowing an already-verified
deployment cache to configure without a GitHub release-asset request.

| Gate | Result |
|---|---|
| Valid-cache offline fixture | A temporary CMake fixture configured successfully using the workspace `CPM_0.42.0.cmake` cache, without a download path. |
| Invalid-cache rejection | A zero-byte cache with its URL replaced by a nonexistent `file://` resource entered the download path and failed with `CPM download failed (37)`; it was never included. |
| Root configure | A CPU-only root configure reused cached PackageProject, ViennaCore, ViennaRay, ViennaHRLE, ViennaLS, and ViennaCS, generated the complete solution, and registered the two new focused tests. |
| Residual build gate | The subsequent focused build reaches `coverageDeltaExecutor.cpp` but fails before test code because the local ViennaLS cache lacks the pre-existing `Advect::LevelSetUpdateExecutor` and `LevelSetRebuildExecutor` aliases expected by `psProcessContext.hpp`. |
| Temp hygiene | All nine CPM/root fixture directories and their idle MSBuild node-reuse processes were removed; the repository `build` directory remains. |
| Scope boundary | Only CPM bootstrap control flow is changed; no pinned version, URL, hash, package declaration, SDK path, or simulation backend policy changes. |

### P6-E: patch-addressed ViennaLS CPM cache key

- Status: accepted as a stale-source-cache prevention repair; the patched
  dependency API and simulation algorithms are unchanged
- Date: 2026-08-03

The ViennaLS `CUSTOM_CACHE_KEY` now contains the first sixteen hexadecimal
characters of the SHA-256 of the checked-in `lsAdvect` executor patch. Changing
that patch therefore selects a new source-cache directory instead of silently
reusing a source tree generated before the patch existed. Existing caches are
not edited. Offline deployments can pre-populate the matching patched cache or
use the existing `VIENNAPS_VIENNALS_SOURCE_DIR` environment/cache override.

| Gate | Result |
|---|---|
| Root cause | The former `v5.8.5-levelset-update-v2` cache was a clean upstream `v5.8.5` tree; CPM cache hits bypass its `PATCH_COMMAND`, leaving the two executor aliases absent. |
| Isolated patched source | A temporary copy of that cache accepted the existing patch with `git apply --check`, then exposed both executor aliases and setters without modifying the original cache. |
| Root CPU acceptance | With the temporary source override, CPU-only root configure and build produced both new executor tests; `ctest -C Debug -R '^(coverageDeltaExecutor|surfaceDiffusionExecutor)$'` passed 2/2. |
| Residual fresh-cache gate | A network-backed first population of the new hash-addressed CPM entry was not run; the key derivation is deterministic and the source override provides the documented offline route. |
| Scope boundary | Only the CPM cache identity of the existing ViennaLS patch is changed; no ViennaLS cache contents, pinned tag, patch content, SDK path, or runtime backend policy changes. |

### P5-N2: explicit neutral-transport velocity bridge

- Status: accepted locally as an explicit FP32 bridge; no automatic
  Process/backend selection is enabled
- Date: 2026-08-03

`VulkanNeutralTransportVelocityExecutor` is a caller-owned, non-copyable FP32
bridge. `makeExecutor()` returns a callback that owns shared bridge state, so a
model may retain the callback without outliving the creator object. The state
owns the existing neutral-transport Vulkan primitive and reusable host-visible
coverage, material-id, and velocity buffers. Buffers are released before the
primitive's `ComputeSession` is reset; buffer growth explicitly recreates a
smaller allocation.

The bridge rejects non-FP32-compatible lengths, byte overflow, negative or
non-integral legacy material IDs, non-finite/subnormal inputs, invalid strict
FP32 parameters, and dispatch/resource failures before mutating the generic
work output. It computes the existing CPU FP32 formula, dispatches the Vulkan
primitive, reads back a private candidate, and compares every result by raw
IEEE-754 bits. Only an all-element match commits `output`, `writtenCount`, and
`complete`; all other paths return false for the N1 CPU fallback.

| Gate | Result |
|---|---|
| CPU seam | N1's existing CPU-only executor contract remains the only automatic model seam; no Process/Context/backend selection was changed |
| Vulkan smoke | new standalone explicit-install smoke covers legacy CPU raw-bit equality, zero-density, strict-input rejection, reusable buffer growth, reset, lifetime, and output sentinels |
| CMake | bridge is part of `viennaps_vulkan_surface`; its smoke uses only the generic executor seam and is registered by the focused surface switch without a top-level `ViennaPS`/CPM dependency |
| Model integration | the root-level actual N1 model plus bridge composition remains a CPM-dependent residual integration gate; it is not required to accept the standalone primitive bridge |
| Hardware acceptance | fresh standalone MSVC 19.44.35223 build succeeded; local Intel Arc focused CTest passed 1/1 |
| Scope boundary | only bridge files, surface CMake/smoke wiring, and this status entry changed; existing primitives/shader, Process/Flux/Context, B2A, CPM, and backend policy remain untouched |

### P5-COV1: CoverageManager delta executor seam

- Status: accepted locally as an ABI-neutral, transaction-based CoverageManager
  seam; no Vulkan or automatic backend selection is enabled
- Date: 2026-08-03

`CoverageDeltaWork<NumericType>` exposes channel-major scalar spans and channel
offsets without leaking PointData or a backend type. `CoverageManager` keeps
the canonical CPU metric as the fallback, offers an explicit executor setter,
and only publishes an executor candidate when it reports success, completion,
and the exact channel count. Exceptions, false returns, mismatched channel
lengths detected while preparing backend work, and incomplete/partial writes
retain the CPU result. The contract is
available for both float and double; P5-COV2 will add the Vulkan bridge and
raw-bit oracle against this canonical result.

| Gate | Result |
|---|---|
| Contract surface | New header-only `CoverageDeltaWork`/`CoverageDeltaExecutor` is independent of Vulkan, CUDA, SDK, and fixed local paths. |
| Transaction boundary | Focused test covers CPU default, successful replacement, false/throw, invalid written count, and incomplete output for float and double. |
| Actual manager integration | Root CMake configures and registers the test through the verified CPM cache. With the documented temporary patched ViennaLS override, the actual `CoverageManager::saveCoverages` / `checkCoveragesConvergence` CTest passes in Debug. |
| Scope boundary | Only CoverageManager, the new executor contract/test, and this status record are changed; Process/ProcessContext, B2A, CMake dependency logic, and Vulkan primitives remain untouched. |

### P5-COV2: Vulkan coverage-delta executor bridge

- Status: accepted locally as an explicit FP32 Vulkan bridge; no automatic
  CoverageManager/Process installation or backend selection is enabled
- Date: 2026-08-03

`VulkanCoverageDeltaExecutor` owns a reusable `CoverageDeltaMetricFp32` and
three host-visible buffers behind a mutex-protected shared state. Its callback
accepts only channel-major, equal-width nonzero point ranges with checked
offsets, sizes, byte counts, and Vulkan `uint32` dimensions. Inputs and all
intermediate CPU operations are restricted to finite normal-or-zero FP32. The
bridge computes the legacy `sum += (updated - previous)^2; sum /= N` result,
dispatches the Vulkan metric into a private candidate, and compares every
channel by raw IEEE-754 bits. Only an all-channel match commits caller output,
`writtenCount`, and `complete`; validation, device, reset, or comparison
failures leave all caller-owned output and metadata unchanged. The returned
executor retains state after bridge destruction, while `reset()` makes that
callback fail closed.

| Gate | Result |
|---|---|
| Standalone contract smoke | Covers N=1/16/257, three channels, malformed offsets/length, NaN/subnormal inputs, reset, callback lifetime, output sentinels, and CPU raw-bit oracle. |
| Hardware acceptance | Fresh MSVC 19.44.35223 + `VULKAN_SDK` build on the local Intel Arc; focused CTest `viennaps-vulkan-coverage-delta-executor-smoke` passed 1/1 in 0.26 s. |
| Root manager integration | Not run for COV2: the actual bridge installation into CoverageManager remains a residual gate for the next deployment-profile phase. The CPU-only CoverageManager seam has already passed its separate root Debug acceptance. |
| Scope boundary | Only the COV2 bridge files, surface CMake wiring, and this status entry changed; no public Vulkan header, shader/primitive, Process/ProcessContext, policy, or CPM logic changed. |

### P5-COV3: Process-to-CoverageManager executor binding

- Status: implemented as an explicit Process callback binding; no automatic
  deployment profile or backend selection is enabled
- Date: 2026-08-03

`ProcessContext` now carries the existing type-erased
`CoverageDeltaExecutor<NumericType>`, and `Process` provides set/get/clear
methods for both float and double. Every `FluxProcessStrategy::setupProcess`
call rebinds the current context callback into its `CoverageManager`, including
an empty callback, so reusing a strategy cannot retain an executor from a
previous run. The manager's existing CPU metric, partial-write rejection, and
exception fallback remain authoritative.

| Gate | Result |
|---|---|
| API contract | Process and Context expose only the generic coverage executor; no Vulkan/CUDA or deployment types were added. |
| Normal setup reachability | The focused test uses a CPU `NeutralTransport` model and the public `FluxProcessStrategy::calculateFlux` setup path with the Process callback copied into Context, then clears and reruns the same strategy to verify no stale callback remains. |
| Root CPU acceptance | Independent Ninja root build with a temporary patched ViennaLS source/cache override: focused `coverageDeltaExecutor` CTest passed 1/1 in 0.11 s. The Visual Studio generator remains host-blocked by duplicate `PATH`/`Path` environment keys (`MSB6001`), but this did not affect the Ninja acceptance. |
| Scope boundary | Only ProcessContext, Process, Flux strategy, the existing coverage executor test, and this status entry changed; Vulkan/CUDA, CPM, deployment policy, surface diffusion, and Level Set code remain untouched. |

### P5-B2A: Surface-diffusion executor seam

- Status: accepted locally as an explicit Process executor seam; no Vulkan
  bridge or automatic backend selection is enabled
- Date: 2026-08-03

`SurfaceDiffusionWork<NumericType>` carries the existing explicit-step CSR
matrix, current field, private output candidate, and a completion/count
acknowledgement. `Process` exposes explicit set/get/clear methods and the Flux
strategy builds CSR only when an executor is installed. The historical CPU path
is unchanged otherwise. An executor candidate is published only after
`SUCCESS`, `complete`, and `writtenCount == output.size()`; failures,
incomplete output, and exceptions return the existing Process failure result
without moving the prior field or target. P5-B2B will provide the explicit FP32
Vulkan bridge and CPU differential gate.

| Gate | Result |
|---|---|
| ABI and transaction | Work spans do not leak Vulkan/CUDA types; float and double retain their native precision and incomplete successful callbacks cannot publish a partial field. |
| Focused contract | The focused test covers default CPU mode, full completion, partial/incomplete rejection, exceptions, and set/get/clear behavior for float and double. |
| Direct compilation | An MSVC C++20 header-only contract compile passed; `git diff --check` passed. |
| Root CPU acceptance | Root CMake registers the focused test. With the documented temporary patched ViennaLS override, the Debug CTest passes after the actual Flux/Process headers compile. |
| Scope boundary | This adds only the Process-level ABI seam and CSR conversion; no Vulkan dispatch, shader, controller, profile selection, B2A-to-COV coupling, or CPU algorithm replacement is enabled. |

### P5-B2B: Vulkan surface-diffusion status bridge

- Status: accepted locally as an explicit FP32 Vulkan bridge; no automatic
  Process installation or backend selection is enabled
- Date: 2026-08-03

P5-B2B adds a backend-independent `SUCCESS`/`FAILURE` status callback while
retaining the legacy `ProcessResult` executor alias. `Process` adapts the new
status callback through the existing `ProcessContext` callback, preserving the
Flux strategy's transaction and error semantics. The standalone Vulkan bridge
is FP32-only, caller-owned, mutex-protected, and lifetime-safe. It validates
zero or nonzero fields, strict monotonic CSR with bounded columns, finite
normal-or-zero values, non-aliasing spans, and checked Vulkan sizes. It runs
`SurfaceGraphDiffusionFp32` into private scratch buffers, compares every result
to the existing volatile CPU row-order solver by raw IEEE-754 bits, and only
then commits output and completion metadata. Failures, exceptions, reset, and
uninitialized callbacks leave caller output and metadata unchanged.

| Gate | Result |
|---|---|
| Contract coverage | Smoke includes N=0/1/257, mixed CSR rows, output sentinels, malformed CSR, aliasing, NaN/subnormal inputs, reset, and callback lifetime. |
| Hardware acceptance | Fresh MSVC 19.44.35223 + `VULKAN_SDK` build on the local Intel Arc; focused CTest `viennaps-vulkan-surface-diffusion-executor-smoke` passed 1/1 in 0.21 s. |
| Root Process mapping | Patched-ViennaLS root Debug CTest `surfaceDiffusionExecutor` passed 1/1 in 0.46 s, including SUCCESS/FAILURE status adaptation through the legacy ProcessContext callback. |
| Scope boundary | Only the status alias/adapter, bridge, surface CMake wiring, smoke, and this record are in scope; no Flux strategy, shader, primitive, dependency, or backend-policy changes. |

### PD0: coverage stage policy and per-stage manual CPU bypass

- Status: accepted locally as a policy/runtime gate; shared Process deployment
  session binding remains the next stage
- Date: 2026-08-03

`Stage::COVERAGE` is now an independent backend-policy stage and serializes as
`coverage`; it is appended after the existing stage values so prior enum-backed
indices remain stable. `DeploymentComputeContext` resolves the effective manual
backend for every supplied workload (`perStageBackend` first, then
`globalBackend`). The fail-closed CPU bypass is taken only when every effective
manual request is CPU. A single per-stage Vulkan/CUDA/AUTO request therefore
keeps the profile/session gate active, while automatic selection remains
fail-closed to CPU when no valid profile exists.

| Gate | Result |
|---|---|
| Independent policy | Backend-policy focused test selects `Stage::COVERAGE` independently and verifies `toString` is `coverage`. |
| Manual CPU bypass | Vulkan runtime smoke covers multi-stage global CPU with all per-stage effective CPU and a missing profile; it prepares CPU without a Vulkan session. |
| Manual override precedence | The same smoke covers global CPU plus coverage Vulkan and a mixed Level Set/Vulkan plan; both reject without a valid profile instead of bypassing. |
| Existing route | Existing single-stage auto/manual/profile/session checks remain in the focused runtime smoke; the fixture now carries the strict-FP32 evidence required by the current automatic Vulkan gate. |
| Validation | RED MSVC build failed on the missing `Stage::COVERAGE`; GREEN backend-policy executable passed, and `viennaps-deployment-compute-context-smoke` CTest passed 1/1 on the local Vulkan SDK/device. |
| Scope boundary | Only backend policy, deployment context runtime/smoke, backend-policy test, and this status entry changed; no bridge, Process/Flux, profile schema, CUDA, algorithm, or CMake dependency logic changed. |

### PD1-A: borrowed deployment-session bridge initialization

- Status: implemented at the coverage-delta and surface-diffusion bridge layer
  and consumed by the PD1-B Process deployment binding
- Date: 2026-08-03

Both FP32 surface bridges now accept a validated caller-owned
`runtime::ComputeSession&` in addition to their existing self-owned
`initialize(shaderPath, error)` path. Borrowed initialization rejects an
invalid/reset session, never resets or owns the caller session, and documents
that the session must outlive bridge state and every executor callback. Smoke
coverage exercises exact CPU raw-bit output for borrowed and self-owned paths,
rejection without output/metadata mutation, and session survival across bridge
reset/destruction.

| Gate | Result |
|---|---|
| Hardware acceptance | Root Visual Studio 2022 configuration using the local CPM cache built both bridge smoke targets; Intel Arc focused CTest passed 2/2 (`viennaps-vulkan-surface-diffusion-executor-smoke` in 0.29 s and `viennaps-vulkan-coverage-delta-executor-smoke` in 0.32 s). |
| Standalone CMake note | The direct `gpu/vulkan` configuration hit an Embree FetchContent directory-removal failure before compilation; its exact temporary directory was removed. The accepted root configuration uses the project CPM cache and does not depend on that FetchContent path. |
| Scope boundary | Only the two surface bridge headers/implementations, their smokes, and this record changed; no Process binding, shaders, numerical primitives, backend policy, CUDA, or dependency logic changed. |

### PD1-B: cache-only Process surface binding facade

- Status: implemented as a Vulkan-only deployment binding for FP32 coverage
  convergence and surface diffusion; Level Set lifecycle remains separate
- Date: 2026-08-03

`ProcessDeploymentBinding<D>` consumes a resolved deployment decision,
hardware fingerprint, stage workloads, manual selection, device options, and
the two SPIR-V paths without reading profiles or probing hardware. It clears
both existing Process callbacks before every reconfiguration, prepares one
`DeploymentComputeContext`, and borrows exactly that context session for both
bridges when coverage and surface diffusion select Vulkan. CPU-only selection
uses no session. Automatic bridge/path failures degrade these two seams to CPU
with an explicit diagnostic; manual and selection-plan failures remain
fail-closed errors. A shared callback holder keeps the context and bridge state
alive when a copied Process callback outlives the binding, with bridge teardown
ordered before session teardown.

| Gate | Result |
|---|---|
| Scope boundary | Only the surface binding facade, its smoke target, CMake registration, and this status entry are in scope; core Process APIs, Level Set, shaders, policy/profile schema, CUDA, and dependencies remain untouched. |
| Hardware acceptance | A fresh root Visual Studio 2022 configuration with the local CPM cache and a temporary patched ViennaLS source built the binding smoke; Intel Arc focused CTest `viennaps-vulkan-process-deployment-binding-smoke` passed 1/1 in 0.34 s. Both exact temporary directories were removed after the run. |
| Validation coverage | The binding smoke covers unsupported-stage rejection, manual mixed-profile fail-closed behavior, automatic empty-shader degradation, real profile/session binding, raw-bit coverage/surface executor checks, and callback retention after binding destruction. |

### PD2-A: neutral-transport velocity stage policy

- Status: accepted locally as a policy-visible stage; neutral velocity binding
  implementation remains the next card
- Date: 2026-08-03

`Stage::NEUTRAL_TRANSPORT_VELOCITY` is appended after `COVERAGE` and before
`COUNT`, preserving all existing stage-backed indices. It serializes as
`neutralTransportVelocity` and uses the existing generic backend policy for
automatic and independent manual selection. No neutral transport binding,
Process integration, bridge, shader, dependency, or deployment-context change
is included.

| Gate | Result |
|---|---|
| RED | MSVC compile of the focused backend-policy test failed on the absent `Stage::NEUTRAL_TRANSPORT_VELOCITY` member before the header change. |
| GREEN | Focused backend-policy executable passed all existing tests plus automatic strict-FP32 Vulkan selection, global CPU/per-stage neutral Vulkan override, direct neutral CPU override, and stable `toString`. |
| Fresh root CTest | Fresh Visual Studio 2022 CMake tree under `C:\tmp` (VTK/Vulkan disabled, existing CPM cache) built `backendPolicy`; focused CTest passed 1/1. |
| Scope boundary | Only `backendPolicy.hpp`, the focused backend-policy test, and this status entry changed; no neutral binding implementation or unrelated policy behavior changed. |

### PD2-B: borrowed neutral-transport velocity session bridge

- Status: implemented at the neutral primitive and executor bridge layer;
  Process binding remains the next milestone
- Date: 2026-08-03

`NeutralTransportSurfaceModelFp32` now supports both its existing self-owned
session lifecycle and a caller-owned `runtime::ComputeSession&` path. Borrowed
mode retains only a non-owning pointer, uses the caller's device/command
context, and releases bridge-local Vulkan objects without resetting the caller
session. `VulkanNeutralTransportVelocityExecutor` exposes the analogous
borrowed overload while preserving the self-owned overload and callback
lifetime behavior.

| Gate | Result |
|---|---|
| RED | Fresh local Ninja build failed at the frozen borrowed smoke call because the executor had no three-argument `initialize` overload (MSVC C2660). |
| GREEN | The borrowed executor smoke built and passed with exact CPU raw-bit equality, self-owned compatibility, invalid-session rejection, reset/destruction session survival, and callback use while the caller session remained valid. |
| Fresh root hardware acceptance | Fresh temporary configuration supplied `VULKAN_SDK` only through the command environment and used the local `.cpm-cache`; focused CTest passed 2/2 on the Intel Arc (`viennaps-vulkan-neutral-transport-surface-smoke`, `viennaps-vulkan-neutral-transport-velocity-executor-smoke`). |
| Scope exception | The necessary low-level `neutral_transport_surface.hpp/.cpp` session-lifetime change was authorized after inspection showed no existing borrowed capability; no CMake, shader, policy, Process, CUDA, or dependency files changed. |
| Cleanup | Temporary validation trees `C:\tmp\viennaps-pd2-build` and workspace `.tmp_pd2_red_ninja` were removed after validation. |

### PD2-C: single-session Process neutral surface binding

- Status: implemented and accepted by fresh Intel Arc hardware CTest
- Date: 2026-08-03

`ProcessDeploymentBinding<D>` now has an explicit caller-retained CPU process
model overload. When a neutral-transport workload selects Vulkan, the binding
discovers `impl::NeutralTransportSurfaceModel<float,D>` through that handle,
retains the concrete surface model for callback cleanup, and initializes
coverage, surface diffusion, and neutral velocity bridges against the one
`DeploymentComputeContext` session. The neutral callback captures the shared
holder, so it remains usable after the binding object is destroyed while the
caller retains the model/session; `clear(process)` removes Process callbacks
and the model callback before holder/session release. Existing two-stage
overloads still reject neutral workloads. Automatic bridge, shader, or model
failure degrades the complete selected surface set atomically; manual Vulkan
failure is fail-closed.

| Gate | Result |
|---|---|
| Interface gap | The pre-change facade has no retained-neutral-model overload; the smoke call preserves that missing-interface case. A separate pre-change compile was not claimed because the first agent shell had no C++ compiler. |
| Fresh root hardware CTest | A fresh Visual Studio 2022 root configuration with temporary `VULKAN_SDK`, local CPM cache, and a temporary patched ViennaLS source built the smoke; Intel Arc CTest `viennaps-vulkan-process-deployment-binding-smoke` passed 1/1 in 0.48 s. |
| Validation coverage | The single smoke checks CPU raw-bit neutral velocity alongside coverage/diffusion, one shared session identity, caller-retained callback lifetime, clear/reconfigure, invalid or non-neutral models, atomic automatic degradation, and manual fail-closed behavior. |
| Cleanup | The temporary build, copied ViennaLS source, and build logs were removed after the CTest; the repository root `build` directory was retained. |
| Scope boundary | Only the Process binding facade, its smoke, surface CMake target wiring, and this append-only status entry changed. |

### Parallel execution board (PD2-C onward)

- Snapshot: 2026-08-04; this is the scheduling source of truth for concurrent
  Vulkan work.
- Status legend: `DONE` = evidence accepted; `RUN` = code exists with a named
  gate pending; `READY-S` = serial predecessor is accepted; `READY-P` = safe
  to implement in parallel; `BLOCKED` = an external capability or dependency
  gate is missing.
- Claim rule: a `RUN` row names its card owner and exact exclusive files before
  implementation begins. Other roles may inspect the boundary but may not edit
  it; a required expansion is re-carded and recorded before the edit. Release
  the claim only after the acceptance evidence and cleanup are recorded.
- Update rule: every card update records an owner/card ID, changed-file
  boundary, exact command/result, CPU oracle outcome, and (when Vulkan is
  selected) hardware evidence. Do not record an unmeasured speedup as a
  result. Deployment composition has exactly one session owner; copied
  callbacks keep their holder alive until explicit clear/reconfiguration.

| Card / phase | Current state and predecessor | Exclusive ownership boundary | Parallel scheduling | Required acceptance evidence |
|---|---|---|---|---|
| `BASE` through `PD1-B` | `DONE`: profile cache/probe, primitives/BVH, Level Set seam, CPU executor seams, coverage/surface bridges, and two-stage Process binding are accepted. | Existing runtime, primitives, profile I/O, coverage, and surface-diffusion code; no compatibility rewrite. | Baseline only; do not reopen without a defect card. | Existing focused CPU differentials and Intel Arc smoke records in this document. |
| `PD2-A` / `PD2-B` / `PD2-C` | `DONE`: neutral stage policy, borrowed neutral bridge, and three-stage Process binding accepted. | `backendPolicy.hpp`; neutral surface/executor; Process binding and its smoke/CMake target. | Baseline only; later cards consume these interfaces. | Policy CTest; neutral bridge CTests 2/2; Process-binding CTest 1/1 with CPU raw-bit oracle. |
| `PD3-LS-SHARED-SESSION` | `DONE` — accepted 2026-08-03; the implementation claim is released. | `levelset_process_controller.hpp`, its focused smoke/CMake wiring, and this status record. Runtime interface expansion requires a recorded re-card first. | Serial prerequisite is accepted; `PD3-SESSION-COMPOSE` may now be claimed. | Intel Arc controller CTests 2/2 confirm equal nonzero update/rebuild generation and identical device name; no second session initialization; CPU-oracle, Manual-CPU bypass, teardown/reconfigure gates remain covered. |
| `PD3-SESSION-COMPOSE` | `DONE` — accepted on main 2026-08-03 after root rebuild and focused execution evidence. | Exclusive: a composition orchestrator, its focused execution smoke, narrow Process-binding adapters, and this status evidence only. | Follow-up work may continue on `PD4-PERF-BASELINE`; do not treat this row as a release gate. | One session across coverage, diffusion, neutral velocity, Level Set update/rebuild; copied-callback lifetime; atomic mixed AUTO/MANUAL fallback matrix. |
| `PD3-ROOT-INTEGRATION` | `DONE` — accepted on main 2026-08-03; full targeted integration evidence is recorded below. | Root integration tests and process/trench fixtures only; no algorithm rewrite. | Serial integration gate is released. | Root configure/build, focused executor and deployment tests, 96/96 non-benchmark CTest excluding the pre-existing long `vulkanCpuBaseline`, and trench CPU oracle. |
| `PD4-HW-MATRIX` | `DONE` — accepted on main 2026-08-03 after a fresh no-SDK/CPU/Intel Arc rerun. | Exclusive: `docs/design/pd4-hw-matrix-card.md`, `gpu/vulkan/VulkanProbe.cpp`, `tests/vulkanDeploymentProbe/`, `tests/probeProfileAdapter/`, `tests/vulkanDeploymentBootstrap/`, `tests/capabilityProfileIO/`. No production routing changes; no Level Set code. | Hardware-matrix evidence is now a PD5 input; do not reopen it without a new hardware/probe defect card. | No-SDK probe exit 0; four focused CPU CTests 4/4; Intel Arc strict PASS with matching identity/queue; forced strict failure exits nonzero, cleans child evidence, and remains fail-closed. |
| `PD4-PERF-BASELINE` | `DONE` — accepted on main 2026-08-03 as a reproducible baseline evidence card; no optimization claim is implied. | `cmake/run-pd4-perf-baseline.ps1` and its dated JSON evidence only. | Baseline is available to PD5 and later optimization cards. | Five selected CPU/Vulkan smoke suites, three repetitions each, all oracle gates PASS; dispatch/submit/buffer contract metrics and wall-time samples recorded. |
| `PD5-CI-DOCS-INTEGRATION` | `DONE` — CI/docs integration accepted locally on main 2026-08-03; no remote GitHub run is claimed. | `.github/workflows/build.yml`, the CI section of `vulkan-compute-acceleration-development-report.md`, and this status record. No C++/CMake production changes. | `PD5-INSTALL-EXPORT` remains independently gated; a future hosted/self-hosted run verifies runner provisioning, not this local integration claim. | Default CPU/no-SDK lane is explicit; optional dispatch-only labeled self-hosted Vulkan lane is isolated; path-hygiene job and linked PD4 evidence are present; CI never installs or requires an SDK. |
| `PD5-INSTALL-EXPORT` | `DONE` — local CPU/no-SDK acceptance completed 2026-08-04; the opt-in Vulkan payload extension and installed profile persistence consumer were accepted locally on 2026-08-18. | CMake install/export, consumer smoke, and deployment documentation. | Serial release-facing gate; remote CPU CI remains the next evidence step. | CPU configure/build/install plus independent CTest 1/1; optional Release Vulkan runtime/shader/SPIR-V payload and independent consumer CTest 1/1 pass. The installed consumer now validates profile serialize/provision/reload/stale handling; VTK, remote CI, and Process preview remain separate gates. |
| `PD5-VTK-INSTALL-EXPORT` | `BLOCKED-LOCAL-ENV` — the optional VTK variant is isolated from the accepted CPU/no-SDK and Vulkan-payload paths. | Own only the VTK module-selection and dependency-export contract needed by an installed ViennaPS consumer; do not patch cached VTK/ViennaLS sources or weaken CPU/Vulkan defaults. | A cached VTK v9.3.1 configure with `VTK_MODULE_ENABLE_VTK_IOHDF=NO` passes the HDF5 probe but still fails generation because ViennaLS's `ViennaLSTargets` references VTK targets outside that export set. Unlock requires an upstream/package export repair, then a VTK producer install and independent consumer CTest. |
| `PD5-CI-REMOTE` | `BLOCKED-EXTERNAL` — live read-only GitHub verification on 2026-08-18 confirms default branch `master`, zero workflow runs, and zero registered self-hosted runners. The remote `master` `build.yml` is the older test-only workflow and does not contain the current PD5 `path-hygiene`, `install-export`, or Vulkan hardware jobs. | Remote publication/branch setup and the existing `.github/workflows/build.yml`; no new product code. | `READY-S` after the current mainline is published to a remote branch that contains the current workflow. | GitHub-hosted `path-hygiene`, `test`, and `install-export` checks complete on the published commit; record commit SHA, run IDs and URLs. The optional Vulkan lane additionally needs a registered `self-hosted,vulkan` runner and is a separate gate. |
| `P5-JD` | `DONE-LOCAL` — implementation is on local `main` and revalidated on the Release Vulkan configuration; not a production Process route. | `gpu/vulkan/ray/` device ray-flux composition and its focused smoke; no Process/model routing. | `P5-JE` and `P5-JF` consume this device-resident terminal-differential contract; do not reopen without a regression card. | Five-ray/three-triangle CPU bitwise oracle, device residency, transaction guards, and focused Intel Arc execution pass. |
| `P5-JE` | `DONE-LOCAL` — one-compute-submit composition is on local `main` and revalidated; no strict deployment promotion. | Record-only ray stages, shared scratch/command buffer, and aggregate smoke. | `P5-JF` consumes the single-submit chain; production routing remains gated by the P5 physics/model cards. | Exactly one compute submission, explicit barriers, terminal differential, and the five focused ray CTests pass. |
| `P5-JF` | `DONE-LOCAL` — strict-FP32 status propagation is on local `main` and revalidated; no automatic backend eligibility. | Surface-reduction status shader/API and aggregate fail-closed commit. | Unlocks numerical-integrity evidence for production-route design, not production promotion itself. | Overflow fixture sets device status; CPU/GPU outputs remain unchanged; strict shader validation and focused suite pass. |
| `P5-JD/JE/JF-REGRESSION-HARDENING` | `DONE-LOCAL` — committed 2026-08-05; smoke diagnostics, descriptor-lease reuse coverage, and CTest pass/fail regex hardening are on local `main`. | Exclusive: `gpu/vulkan/ray/CMakeLists.txt`, `gpu/vulkan/ray/device_ray_flux_pipeline_smoke.cpp`; do not mix with Process/model integration. | Unlocks `P5-RAY-ROUTE` for Process/model integration. | Fresh standalone `gpu/vulkan` Release build on Intel Arc; complete configured CTest suite 12/12 passed; five JD/JE/JF-focused tests 5/5 passed. |

Dependency guard: `PD3-LS-SHARED-SESSION` must extend the existing Level Set
owner rather than creating another `ProcessDeploymentBinding` session;
`PD3-SESSION-COMPOSE` is the only card allowed to own cross-surface/Level Set
lifecycle composition. `PD4` cards are observational until their acceptance
evidence exists. Every implementation card must use CPU results as its
correctness oracle on non-CUDA hosts.

### Total-plan continuation board: P5 to P7

- Snapshot: 2026-08-04, with the governing P5 Formal Exit overlay approved on
  2026-08-19. This board expands the original P0--P7 plan into executable
  follow-up cards; it does not turn an implementation note into a completion
  declaration. All current P5 closeout work follows
  [the approved execution plan](p5-formal-exit-execution-plan.md).
- `DONE-LOCAL` means the card's local acceptance boundary is evidenced. It does
  not mean production Process routing, remote CI, cross-vendor coverage, or
  release readiness is complete. `READY-S` is serially executable after its
  predecessor; `READY-P` may proceed in parallel; `BLOCKED` needs an external
  capability or publication step.

| Card | State and predecessor | Exclusive scope | Acceptance / next unlock |
|---|---|---|---|
| `P5-FORMAL-EXIT-EXECUTION-PLAN` | `RUN` — approved by the user on 2026-08-19 with all three recommended decisions confirmed. It is the governing dependency and dispatch contract for the complete P5 closeout. The execution amendments serialize all local validation and establish a local-only Wave interval plus one reviewed remote push boundary, reducing repeated trusted-execution reviews without bypassing security controls. | Own [the P5 Formal Exit plan](p5-formal-exit-execution-plan.md), its glossary terms, validation mutex, trusted-execution review minimization protocol, and record synchronization only. Implementation ownership remains with the individual cards. | Current P5 closeout work must follow `P5-X0 -> P5-N1/N2 -> P5-S0/S1 -> P5-M0 -> P5-E0`. Read-only preparation may be concurrent, but at most one CMake/build/CTest/reference/Vulkan workload may run locally. Sub-agents do not own validation or network access; the main line performs one verified push per accepted Wave. `P5-E0` alone may mark the formal exit complete. |
| `P5-X0-CLOSEOUT-BASELINE` | `DONE-REMOTE-SNAPSHOT` — Wave 0 was accepted by the main line on 2026-08-19. | Production, tests, CMake and governing records in the explicit 178-file snapshot; generated `.claude/` and multibounce `.tmp_mod/` state are excluded and not deleted. | Commit `39e644082f5b007d05856dce1c6f96cb850e6209` is pushed to `origin/codex/p5-closeout-base`; a detached clean worktree configures with Vulkan ON and VTK/tests/examples OFF using the verified CPM cache. GNU patch dry-run passes. The VTK-enabled export-set failure transfers to `P5-D0`; `P5-N1`, `P5-K0` and `P5-D0` are unlocked. |
| `P5-N1-NEUTRAL-ROOTCAUSE-DISCRIMINATION` | `PATCH-ADOPTED-LOCAL / N2-GATE-READY` — N1E/N1F identified the root cause on 2026-08-19/20, the N1G `IterativeTraverse` repair candidate passed Lane C and Lane E on 2026-08-20, and the user-approved adoption on 2026-08-20 landed the fix as the CPM patch `cmake/patches/viennacore-v2.2.1-kdtree-traversedown-nullcheck.patch` (applied at fetch time via `PATCHES` + `CUSTOM_CACHE_KEY`, local source override exempt). A fresh configure fetched ViennaCore v2.2.1 into the new cache key with a patched header byte-identical to the validated overlay; the paired fixture built against the patched cache headers (no overlay) passes Release OMP 1/2/4/8 raw-equal (`max_ulp=0`), and the default-flags (no explicit optimization level) differential re-check passes OMP 1/2/4/8 raw-equal, matching the 2026-08-09 baseline. Upstream MSVC/ViennaCore reports are deferred by user decision. `P5-N2` gate review is the next main-line step. | Own only the phased paired runner, test-only phase logging, Neutral oracle record, isolated compiler/dependency evidence, and the disabled explicit `KDTree<float, array<float,3>>` specialization candidate. CPU/KDTree formulas, Process/model semantics, reference sources, required Release/OpenMP flags, Vulkan eligibility, and support rows remain prohibited. | Hostx86/x64 and Hostx64/x64 builds retain `/O2 /Ob2 /DNDEBUG /openmp:llvm /MD /Zi`. After the runner canonicalized the duplicated `Path`/`PATH` child environment, the corrected Mod OMP=1 diagnostic flushed `Particle 0` after the real CPU ray trace and then exited `0xC0000005`; the phase log is 1836 bytes and the oracle is zero bytes. This aligns the current first bad boundary with the historical `ElementToPointData::prepare -> KDTree::findNearestWithinRadius -> KDTree::traverseDown` stack. No reference or OMP 2/4/8 run was allowed. The N1E probe (`neutral_cpu_oracle_kdtree_probe.cpp`, caller-owned and test-only) replicates the exact D==2 `updateSurface` element-tree construction and the full `ElementToPointData::apply()` post-processing frame on the real fixture geometry (9 disk nodes, 16 elements, radius 1.0, 60 radius-query results) with Embree/TBB linked but no ray trace executed: the Mod build exits 0 at OMP 1/2/4/8 and the reference build exits 0 at OMP 1/8 under `/O2 /Ob2 /DNDEBUG /openmp:llvm /MD /Zi`, with byte-identical normalized outputs (raw Mod OMP=1 SHA256 `ab20a29a1c62a3e0ef9a86d6fa1f675839927d00bbba14c2a90274c6b31cf052`, reference OMP=1 `e189eb6df0b398c60e7c849ec4d1739764b6bc7fcb0eee045eb7b211c649ac4f`; the 6-byte delta is exactly `source=mod` versus `source=reference`). The built tree and the exact historical crash frame are therefore exonerated on real data. N1F subsequently reproduced the identical fault without any ray-tracer execution, superseding the interim trace-phase hypothesis; see the N1F checkpoint in [p5-neutral-cpu-oracle.md](p5-neutral-cpu-oracle.md). N1E excludes the built real-data tree and the exact post-processing frame on real data. N1F then reproduced the identical `traverseDown+0x52` fault in the caller-owned probe WITHOUT executing the ray tracer, excluded the second-KDTree build and TraceTriangle construction as triggers, proved via the audit overlay that the tree is structurally valid at build return in the crashing binary itself, and disassembled the fault: MSVC 14.44.35207 `/O2 /Ob2` eliminates the second `traverseDown` recursion into a loop whose back-edge (`mov rdi,[rdi+rax*8+20h]; jmp +0x52`) skips the entry null check, so a null leaf child is dereferenced at the `axis` load (`rdi=0` in the captured registers). Root-cause category: external toolchain codegen defect triggered by the ViennaCore recursive traversal shape; the source is correct and the reference tree needs no semantic change. Repair boundary: an iterative (loop-form) `traverseDown` ViennaCore overlay candidate, pending user approval, then Lane C (probe) and Lane E (paired fixture) under the ordered Mod-first gates; until they pass, `P5-N2`, complete surface integration, aggregate matrix, and formal exit remain locked. |
| `P5-N2-NEUTRAL-ORACLE-GREEN` | `DONE-LOCAL` — main-line gate rerun on 2026-08-20 with an independent rebuild of both sides against the patched ViennaCore cache headers (`v2.2.1-kdtree-nullcheck-31f6423c177a320d`, no overlay, no source-tree edit to ViennaCore). | Minimal candidate adoption (the CPM patch from `P5-N1`) and the independent paired runner. Production Process routing, automatic backend selection, and remote CI remain out of scope. | Full acceptance matrix passes: Mod and unmodified reference, OMP 1/2/4/8, exact Release flags `/O2 /Ob2 /DNDEBUG /openmp:llvm /MD /Zi`, identical dependency/runtime closure, all exit 0 (no SEH exception), raw serialized equality `empty=exact active=exact flux=exact geometry=exact process=exact max_ulp=0` at every OMP count; the default-flags differential was re-confirmed unchanged in the same adoption wave. Reproducible commands are recorded in [the Neutral oracle record](p5-neutral-cpu-oracle.md). Closes `P5-NEUTRAL-CPU-ORACLE`; unlocks `P5-S0` and `P5-NEUTRAL-VELOCITY-SUBSTAGE` evidence. |
| `P5-S0-SURFACE-ACCEPTANCE-RED` | `DONE-LOCAL` — serialized main-line validation on 2026-08-20; both legs share the one fixture in `tests/surfaceProcessAcceptance/`. | Own only the shared acceptance fixture, the CPU acceptance executable, the Vulkan composition smoke `gpu/vulkan/surface/surface_process_acceptance_smoke.cpp`, and the narrow CMake wiring (surface target registration plus WIN32 runtime-DLL copies, the ray-subdirectory SPIR-V wiring block, the root ray guard). CPU physics, model semantics, ray eligibility, and production routing remain unchanged. | CPU leg GREEN: `surface_process_acceptance_cpu` exits 0 with a complete bit-pattern record (`processResult=0`, 9-cell flux, coverage convergence in 2 iterations). Vulkan leg RED on Intel Arc as designed: all five stages (COVERAGE, SURFACE_DIFFUSION, NEUTRAL_TRANSPORT_VELOCITY, RAY_TRACING/COMPUTE_BVH, composition-owned LEVEL_SET) resolve Vulkan on one shared session, negative battery 4/4 fail-closed; the positive route then fails deterministically and identically across two runs — the ray-flux engine declares the NeutralTransport model outside the evidenced FP32 2D single-particle slice (no `neutralFlux` cell data), and the device HRLE rebuild reports candidate indices outside the grid, fail-closed via `Process failed.` into `RED_BOUNDARY=vulkanRoute.exception`, exit 1. Unlocks `P5-S1`; no production-route, promotion, or release claim. |
| `P5-S1-SURFACE-INTEGRATION-GREEN` | `DONE-LOCAL` — serialized main-line validation on 2026-08-20; adapters only, no CPU formula/order change. | Own the NeutralTransport<float,2> frontier admission row in the Vulkan ray-flux engine (single particle, no custom source, `maxReflections <= 2`, coverage global-data and element material ids mirrored from the CPU oracle), the HRLE rebuild boundary predicate fix (`grid.isOutsideOfDomain`, hrle-authoritative), and the fixture `preApplyHook` that reinstalls the ray-flux override between `calculateFlux()` and `apply()`. Production routing, automatic backend promotion, and model-matrix expansion remain prohibited. | Vulkan leg GREEN on Release/Intel Arc: `viennaps-vulkan-surface-process-acceptance-smoke` exits 0 deterministically across two runs — all five stages resolve Vulkan on one shared session, and the full record (flux, coverages, surface points, material ids, process time, callback sequence) is bit-exact equal to the paired CPU record; the negative battery stays 4/4 fail-closed. CPU leg unchanged: the rerun record is bit-identical to the accepted S0 record. The S0 RED rebuild failure root cause was evidenced as an over-strict `isInDomain` (maxIndex-exclusive) check rejecting legal reflective-boundary-plane points and fixed to hrle `isOutsideOfDomain`; a latent `readPoints` tokenization bug and CRLF gap in the fixture deserializer were fixed in the same wave. Focused regressions pass: 46/46 Vulkan smokes, `hrleSparseReconstruction` (new boundary-semantics case), `hrleRebuildCpuFixture`, `levelSetRebuildHandledMatrix` (3 variants); the N2 neutral-oracle inputs are untouched by diff scope. Unlocks `P5-M0`; no production-route, promotion, or release claim. |
| `P5-K0-TOP-LEVEL-PREFLIGHT` | `DONE-DIAGNOSTIC / INCOMPLETE-BUILD` after serialized main-line acceptance on 2026-08-19; `P5-K1` is not unlocked. | Own only the CPU-only top-level inventory, one focused test boundary, timeout/flaky classification, process audit, and [K0 preflight record](p5-k0-long-suite-preflight.md). It does not own product fixes, Vulkan acceptance, or the integrated long-suite gate. | The pre-mutex `--parallel 2` focused build is historical preparation evidence only. Under the mutex, 94 tests were discovered and `backendPolicy` passed 1/1 in 0.14 s. `CSVFileProcess` was then `Not Run` because its executable was absent, so the remaining 92 tests and all Vulkan coverage were not run. K1 must first build its selected targets under the current serial policy, then execute the integrated suite one test at a time. |
| `P5-D0-DEPLOYMENT-PREPARATION` | `DONE-LOCAL-STATIC / VALIDATION-NOT-RUN` after main-line acceptance on 2026-08-19; `P5-D1` remains gated. | Own only [the D0 deployment preparation record](p5-d0-deployment-preparation.md), local packaging evidence classification, VTK ownership, serial validation order, and hosted-evidence gaps. It does not own Process/model/runtime changes or remote mutation. | CPU/no-SDK and local opt-in Vulkan install/export evidence remain locally accepted. The VTK-enabled ViennaLS export-set failure remains external; hosted CI still lacks accepted run IDs/URLs and a registered Vulkan runner. No D0 configure/build/install/CTest command ran in this checkpoint. |
| `PD5-CI-REMOTE` | `BLOCKED-EXTERNAL` — live GitHub read-only query on 2026-08-18 confirms `master` as the default branch, the remote `master` workflow is the older test-only file, workflow run list is empty, and registered self-hosted runner count is zero. | Remote branch publication, workflow trigger, and run evidence only. | Publish a branch containing the current workflow; capture commit SHA and hosted CPU/no-SDK run IDs/URLs, then unlock release-facing install/export evidence. A separately provisioned `self-hosted,vulkan` runner is required for the optional hardware lane. |
| `P5-RAY-ROUTE` | `DONE-LOCAL` — single-bounce Process route validated against CPU_TRIANGLE oracle on Intel Arc. | `Process`/`FluxEngine` injection, backend policy, CPU/manual fallback, and ray result transaction. | `ray_flux_process_route_smoke` passes: totalRelDiff 0.29%, maxRelDiff 2.07%, 20000/20000 rays hit; fail-closed unprepared route deposits no flux. | `P5-RAY-PHYSICS` |
| `P5-RAY-PHYSICS` | `DONE-LOCAL` — CPU-side reflection/roulette/event-queue/surface-response contracts accepted on main 2026-08-06. | Boundary/reflection, roulette/event queue, material/surface response, and multi-bounce contracts. | `rayPhysics` CTest passes in reused Release MSVC C++20 build; no Vulkan types in public headers; no Process/FluxProcessStrategy change. | `P5-SURFACE-INTEGRATION` |
| `P5-SURFACE-INTEGRATION` | `S1-GREEN DONE-LOCAL` — the complete surface fixture entered deterministic RED under `P5-S0` on 2026-08-20 (the shared-session five-stage Vulkan composition holds; the device route fails closed at the NeutralTransport slice boundary and the HRLE rebuild); the paired Release NeutralTransport CPU oracle is green since 2026-08-20 (`P5-N2`, CPM-patched ViennaCore headers, OMP 1/2/4/8 raw-equal). — strict callback/configuration and ray rows are locally narrow-accepted, and the former oracle blocker is resolved: the paired Release NeutralTransport CPU oracle is green since 2026-08-20 (`P5-N2`, CPM-patched ViennaCore headers, OMP 1/2/4/8 raw-equal). | Coverage, surface diffusion, neutral velocity, and complete `Process::calculateFlux/apply` callback installation. | The complete surface fixture is RED under `P5-S0` (deterministic, Intel Arc, `RED_BOUNDARY=vulkanRoute.exception`); `P5-S1` completed the green transition on 2026-08-20 using only the adapters that RED boundary required (see the `P5-S1-SURFACE-INTEGRATION-GREEN` row). No setup or executor smoke substitutes for the paired oracle, which passes at `/O2 /Ob2 /DNDEBUG /openmp:llvm /MD /Zi`. |
| `P5-MODEL-MATRIX` | `BLOCKED-PREDECESSOR` — Row-01, the MultiParticle, IonBeam, CF4O2, SF6O2/SF6C4F8, concrete Fluorocarbon, SingleParticleALD, TEOSPECVD and OxideRegrowth CPU/fallback rows, WetEtch, SelectiveEpitaxy and single-precursor TEOS are narrow rows; aggregate support remains behind surface/Neutral gates. | Multi-particle/species Vulkan transport, ion/neutral material semantics, fluorocarbon/plasma/TEOS, wet-etch/selective-epitaxy/oxide-regrowth coverage. | Each additional row needs its own transport/surface seam, paired CPU/reference oracle, conservation/geometry evidence, and explicit fallback; aggregate promotion remains locked. |
| `P5-DEPLOYMENT-EXIT` | `LOCKED` behind `PD5-CI-REMOTE`, `P5-MODEL-MATRIX`, and the long top-level `P5-K3E` gate. | Install/export variants, deployment profile, Process preview documentation, and support matrix. The local opt-in Vulkan payload is now present, but no automatic Process promotion is implied. | Hosted install/export plus optional VTK-enabled case, deployment-profile persistence, release-scoped Vulkan Process preview, and final support matrix; unlocks P6/P7 integration. |
| `P6-LA-BASELINE` | `READY-P` after the existing P2/P3 field contracts; single-agent order places it after the P5 route. | FP64 matrix assembly, SpMV, AXPY/dot/norm, deterministic reduction, BiCGSTAB/Jacobi, OOM admission. | 2D/3D field and matrix CPU/Vulkan differential with convergence/residual history; unlocks oxidation stages. |
| `P6-OXIDATION-COUPLING` | `READY-S` after `P6-LA-BASELINE`. | Oxidant diffusion, pressure/Stokes, harmonic extension, deformation, and batched SIMPLE/coupling. | Trench/fin/LOCOS field and physical acceptance; unsupported FP64 devices fail closed per stage; unlocks full-physics integration. |
| `P6-PHYSICS-EXIT` | `READY-S` after `P6-OXIDATION-COUPLING`. | Oxidation outer loop, rollback, residual diagnostics, and CPU/CUDA/Vulkan parity. | No silent geometry on non-convergence, residual history retained, and model support matrix updated; unlocks P7. |
| `P7-RESIDENT-EXECUTION` | `READY-S` after P5 and P6 exits. | Cross-step device working set, callback invalidation/detach telemetry, command segmentation, and cache lifetime. | Repeated multi-step process run has bounded transfers and correct invalidation/recovery; unlocks calibration. |
| `P7-CALIBRATION` | `READY-S` after `P7-RESIDENT-EXECUTION`. | Cost model, pipeline/AS cache, workload thresholds, and Selection Record replay. | Hardware-matrix calibration chooses only qualified plans and records replayable evidence; unlocks release gates. |
| `P7-CI-SOAK-RELEASE` | `READY-S` after `P7-CALIBRATION`. | CPU-only regression, optional CUDA compatibility, Lavapipe/vendor matrix, long soak, device-lost recovery, install/diagnostics docs. | Cross-platform release gates pass; old `VIENNAPS_USE_GPU` behavior has no unexpected regression; this is the total-plan completion gate. |

The approved P5 closeout critical path is now:
`P5-X0-CLOSEOUT-BASELINE` -> `P5-N1-NEUTRAL-ROOTCAUSE-DISCRIMINATION` ->
`P5-N2-NEUTRAL-ORACLE-GREEN` -> `P5-S0-SURFACE-ACCEPTANCE-RED` ->
`P5-S1-SURFACE-INTEGRATION-GREEN` -> `P5-M0-MODEL-MATRIX-AGGREGATE` ->
`P5-E0-FINAL-AUDIT`. `P5-K0` and local deployment preparation may run in
parallel after `P5-X0`, but their final gates wait for the integrated
candidate. P6/P7 work remains locked until P5 Formal Exit.

### P5 gap-closure execution board (2026-08-06)

Update 2026-08-09: the binding/codegen repair is locally narrow-accepted; the
new rows below keep complete Vulkan physics, model support, promotion, and
release readiness explicitly locked.

This board turns the current implementation audit into bounded work.  It is
additive to the total-plan board above: a `RUN` row is not an acceptance claim.
Every card keeps the ViennaPS CPU result as its non-CUDA correctness oracle;
an unsupported Vulkan route must degrade to CPU in Auto mode or fail closed in
Manual mode.  The coordinator owns integration, card acceptance, and any
second repair attempt.  A card receives at most one owner correction after
review; two unsuccessful attempts are reclaimed to the main line.

For every CPU helper, fallback, and non-compute semantic under review, the
unmodified `D:\Codex_lib\code_reference\ViennaPS` tree is the implementation
authority.  A local CPU mirror is admissible only with an explicit reference
diff, a card ID, and a CPU differential; Vulkan convenience code must never
silently become the new CPU definition.

Each `READY`/`RUN` row below is a strong card: its 100--200-word global-position
brief is the state/predecessor plus owned-boundary columns; its ordered exits,
acceptance, validation, handoff, and next milestone are recorded in the last
two columns. The owning fast agent may make one evidence-targeted correction;
the coordinator then either accepts it or reclaims it to the main line.

| Card | State / predecessor | Owned boundary and invariant | Ordered exit / acceptance | Unlock |
|---|---|---|---|---|
| `P5-RAY-ELIGIBILITY-GATE` | `DONE-LOCAL` after local `P5-RAY-ROUTE`; it consumes only the accepted single-bounce `SingleParticleProcess<float,2>` evidence. | Own `gpu/vulkan/ray/vulkan_ray_flux_engine.{hpp,cpp}` and its focused route smoke. Define an explicit support predicate before device work: only evidenced precision/dimension/model/particle-label/source/reflection semantics may enter the Vulkan engine. Do not change `Process`, `FluxProcessStrategy`, CPU engines, or model physics. | Accepted: explicit `SingleParticleProcess<float,2>` / one-particle / one-label / zero-reflection gate; negative cases reject multi-particle, reflection, double, and 3-D; Auto executes CPU triangle fallback. The fixed-seed real-device differential now uses `gridDelta=0.5` and reports `totalRelDiff=0`, `maxRelDiff=0` on the local Intel Arc path. | `P5-RAY-APPLY-TRANSACTION`, model-matrix implementation. |
| `P5-NEUTRAL-CPU-ORACLE` | `EVIDENCE-GAP-MAIN` after the paired fixture; the default MSVC pair is exact, but the required Release optimization gate still crashes. | Global position: P5 may replace a compute operation, never the CPU definition of NeutralTransport. The oracle is reference-only; no `include/viennaps/**`, Process strategy, or reference-tree edits are allowed. The public `NeutralTransportVelocityExecutor` alias remains backend-neutral. | `tests/neutralCpuReferenceDifferential/` provides an independent bit-pattern checker: default MSVC reference/Mod empty-executor and adapter-active cases report `empty=exact active=exact flux=exact geometry=exact process=exact max_ulp=0`. The latest 2026-08-18 paired rerun under identical `/O2 /Ob2 /DNDEBUG /openmp:llvm /MD /Zi` flags and OMP=1 again gives both Mod and unmodified reference `0xC0000005`: `ElementToPointData::prepare$omp$1` -> shared ViennaCore `KDTree::findNearestWithinRadius` -> `traverseDown+0x52`, in `CPUTriangleEngine::calculateSourceFluxes:180`. Line 180 is `postProcessing_.apply()` after `runRayTracer()` has returned, and before coverage update or any surface-model mutation. Earlier OMP=1/2/4/8 evidence remains identical. Both use the same cached ViennaCore/ViennaRay headers and Embree 4.3.3, TBB 2023.0.0, LLVM OpenMP 5.0 binaries. A test-only Process-TU split was also tried once: Mod OMP=1 produced output, but no paired reference or OMP matrix was completed, so it is non-accepted and cannot unlock the gate. No CPU/reference/cache workaround was used; see [p5-neutral-cpu-oracle.md](p5-neutral-cpu-oracle.md). Full NeutralTransport coverage/transport/desorption/diffusion and Vulkan integration remain out of scope. The one correction budget is consumed; a second failure is reclaimed. | `P5-SURFACE-VULKAN-PHYSICS`; no surface/model/promotion claim until the Release paired CPU oracle is available. |
| `P5-NEUTRAL-CPU-ORACLE-EXTERNAL-UPSTREAM-REPRO` | `DONE-LOCAL-DIAGNOSTIC` after the one-correction isolated upstream run; it resolves neither CPU parity nor the parent crash. | Own [p5-neutral-cpu-oracle-external-repro.md](p5-neutral-cpu-oracle-external-repro.md) and its caller-owned temporary evidence only. It records an independently sourced ViennaCore v2.2.0, exact compiler/ABI/runtime identity, and the fixed pure KDTree `build()`/radius-query matrix. It does not edit a dependency, suppress `/O2`/`/Ob2`/OpenMP, weaken the fixture, or alter CPU semantics. | The pure upstream float KDTree lane passes byte-identically for `OMP_NUM_THREADS=1/2/4/8`, while the unchanged Mod/reference composed wrappers still terminate with the same `ElementToPointData` -> `KDTree::traverseDown` access violation. This excludes a minimal standalone KDTree reproduction but does not authorize an upstream patch, runtime lock, dependency update, or parent-oracle state change. | `P5-NEUTRAL-CPU-ORACLE-COMPOSED-BOUNDARY-ISOLATION`; NeutralTransport and all dependent surface/model gates remain blocked. |
| `P5-NEUTRAL-CPU-ORACLE-COMPOSED-BOUNDARY-ISOLATION` | `DONE-LOCAL-DIAGNOSTIC / RECLAIMED-MAIN` after the agent used two compile corrections; the coordinator independently reran the final matrix and accepted only its diagnostic evidence. | The disposable fixture includes `ElementToPointData` plus a fixed six-disk-node/three-element KDTree mapping and conversion, with no Process, ray tracing, surface model, Level Set advection, or Vulkan execution. It holds Release flags, shared dependency identities, and Mod/reference source roots fixed. Production, reference, cache, and model semantics remain prohibited. | Main-line verification reran Mod/reference across `OMP_NUM_THREADS=1/2/4/8`: all eight lanes exited `0`, produced the same raw stdout hash, and completed `prepare()`/`convert()` with the expected values. This excludes the synthetic mapping boundary only. Because the subagent exceeded its one-correction budget, no further subagent work is permitted on this card. | `P5-NEUTRAL-CPU-ORACLE-REAL-MESH-ISOLATION`; the parent `P5-NEUTRAL-CPU-ORACLE` remains `EVIDENCE-GAP-MAIN`. |
| `P5-NEUTRAL-CPU-ORACLE-REAL-MESH-ISOLATION` | `DONE-LOCAL-DIAGNOSTIC` after its agent hit the two-correction build stop and the coordinator recovered the existing temporary fixture using the already-configured Embree-4/ViennaCS/ViennaHRLE closure. | The fixture constructs the plane-derived `surfaceMesh`, `elementKdTree`, `diskMesh`, normals and `ElementToPointData` mapping used by the CPU triangle setup, then stops before ray tracing, model update, Level Set advection and Vulkan. The reference-only VTK forward declaration is parse-only and never instantiated. | Main-line Release/OpenMP verification ran Mod/reference at `OMP_NUM_THREADS=1/2/4/8`; all eight lanes exited `0` with one identical raw stdout hash and matching real-mesh counts, hashes and mapped data. This excludes real mesh construction plus the point-map conversion, but not engine update/tracing. No CPU/reference/cache semantic workaround was applied. | `P5-NEUTRAL-CPU-ORACLE-CPUTRIANGLE-PREFLIGHT-ISOLATION`; no parent-oracle or model gate unlock. |
| `P5-NEUTRAL-CPU-ORACLE-CPUTRIANGLE-PREFLIGHT-ISOLATION` | `DONE-LOCAL-DIAGNOSTIC / RECLAIMED-MAIN` after the agent created an invalid Embree-3 shim; the coordinator discarded that shim as evidence and rebuilt with the existing Embree-4 CMake definitions. | The disposable fixture enters `CPUTriangleEngine::checkInput`/`initialize`/`updateSurface` and obtains the point KDTree, then stops before `calculateSourceFluxes`, `TraceTriangle::runRayTracer`, surface-model update, Level Set advection and Vulkan. Its reference VTK forward declaration is parse-only and never instantiated. | Main-line Release/OpenMP verification ran Mod/reference at `OMP_NUM_THREADS=1/2/4/8`; all eight lanes exited `0` with one identical raw stdout hash and matching engine/mesh/point-KDTree observables. This excludes CPU triangle preflight only. No shim, fallback, optimization change, reference/cache edit, or test-only result is used as product evidence. | `P5-NEUTRAL-CPU-ORACLE-SOURCE-FLUX-ISOLATION`; parent `P5-NEUTRAL-CPU-ORACLE` remains `EVIDENCE-GAP-MAIN`. |
| `P5-NEUTRAL-CPU-ORACLE-SOURCE-FLUX-ISOLATION` | `NON-ACCEPTED / RECLAIMED-MAIN`; its historical direct-call AV is not current independent evidence. | The caller-owned fixture builds a plane/disk mesh, initializes fixed `NeutralTransport` coverages, then calls `CPUTriangleEngine::checkInput`/`initialize`/`updateSurface`/`calculateSourceFluxes` directly. It retains the Release closure and Mod/reference split, but it bypasses `FluxProcessStrategy::setupProcess` (mesh generation, coverage manager and Process setup). It does not modify production/reference/cache semantics. | On 2026-08-17, the historical fixture and the attempted `useCoverages`/ray-count split did not return in ten seconds under the exact runtime closure; all child processes were interrupted through tracked sessions and verified absent. The missing full Process setup makes that non-termination non-diagnostic. Do not use the historical direct-call AV to narrow the parent failure. The only accepted current phase evidence is the paired full Process capture at `calculateSourceFluxes:180`. | `P5-NEUTRAL-CPU-ORACLE-PROCESS-POSTPROCESS-OBSERVATION`; no NeutralTransport, surface, matrix or deployment unlock. |
| `P5-NEUTRAL-CPU-ORACLE-TRACE-POSTPROCESS-SPLIT` | `NON-ACCEPTED / RECLAIMED-MAIN`; the card consumed its single diagnostic attempt without a valid phase distinction. | It owned only a caller fixture that varied `useCoverages` and `raysPerPoint`; it made no source/header/reference/cache/product edits. | `raysPerPoint=0` was not a legal empty-trace probe and did not return; `raysPerPoint=1` without the full strategy setup likewise did not establish an accepted phase result. All started temporary descendants were explicitly reaped. The experiment neither weakens nor contradicts the full paired SEH capture, but supplies no gate evidence. | `P5-NEUTRAL-CPU-ORACLE-PROCESS-POSTPROCESS-OBSERVATION`; parent Release oracle remains blocked. |
| `P5-NEUTRAL-CPU-ORACLE-PROCESS-POSTPROCESS-OBSERVATION` | `NON-ACCEPTED / RECLAIMED-MAIN` after its one paired-path correction; no independent oracle evidence was produced. | The caller-only wrapper retained `Process::checkInputUpdateContext()` and drove a `FluxProcessStrategy` with a delegating `CPUTriangleEngine`; it changed no production/reference/cache/compiler/model semantics. | Both wrappers compile, but the Mod OMP=1 run logs `checkInput`/`initialize`/`updateSurface` success and `source_flux.enter`, then does not return within ten seconds rather than reproducing the full fixture AV. Because the wrapper changes the call/code-generation shape and no paired phase result exists, reference was deliberately not run. The tracked session was interrupted and the process audit is clean. It only corroborates that the full strategy reaches source flux; it cannot replace the paired SEH stack at `calculateSourceFluxes:180`. | `P5-NEUTRAL-CPU-ORACLE-EXTERNAL-CODEGEN-ROOTCAUSE`; parent Release oracle, all Neutral surface work and model promotion stay blocked. |
| `P5-NEUTRAL-CPU-ORACLE-EXTERNAL-CODEGEN-ROOTCAUSE` | `RECLAIMED-MAIN-PARTIAL` — diagnostic boundary exhausted without a safe repair. | Freeze the existing paired full-fixture executable, compiler `/O2 /Ob2 /DNDEBUG /openmp:llvm /MD /Zi` closure, dependency/runtime hashes and the 2026-08-18 SEH stack. Own only external debugger/compiler/runtime evidence; CPU algorithms, reference semantics, flags, OpenMP, allocator and Vulkan promotion remain prohibited. | The pure upstream ViennaCore float KDTree six-point matrix passes byte-identically at OMP 1/2/4/8, while the unchanged composed Mod/reference wrappers still exit `128` through `ElementToPointData` -> `KDTree::traverseDown`; the CRT-only rerun also leaves the stack unchanged. This excludes the tested standalone KDTree and CRT identities but does not distinguish the remaining composed boundary, so no repair or parent-oracle unlock is claimed. One correction was consumed; reclaim is terminal until an approved dependency/toolchain change or upstream reproducer is available. | An approved dependency/toolchain repair or upstream reproducer; parent Release oracle remains an evidence gap until a paired raw-bit run passes. |
| `P5-SURFACE-INTEGRATION-BASELINE` | `DONE-LOCAL-NARROW` after `P5-SPECIALIZATION-CODEGEN-ROOTCAUSE`; the earlier `RECLAIMED-MAIN` KDTree crash remains historical evidence. | Own only the five-stage composition smoke, its narrow CMake wiring, `LevelSetSurfaceDeploymentComposition`, and `ProcessDeploymentBinding`. Preserve current user changes. CPU execution remains callback-free; Vulkan may replace only callback/executor compute operations and the eligible ray engine. Invariant: a passing setup smoke never promotes full surface physics. | Ordered validation: build the Release surface binding and composed deployment targets; run the binding smoke and the composed smoke to exit 0; assert Manual CPU/no-callback, Manual Vulkan setup, Auto missing-ray degradation, callback cleanup, and the CPU `Process::calculateFlux()` oracle. The existing `levelset-surface-composition-execution-smoke` additionally executes coverage, diffusion, neutral velocity, and LevelSet callbacks with raw-bit/geometry CPU oracles. The new five-stage smoke still does not execute the ray Process path or complete surface/ray physics, so this is a narrow callback/configuration baseline only. One owner correction is allowed; a second failure is reclaimed. | `P5-SURFACE-VULKAN-PHYSICS` is required before full surface integration or model-matrix support. |
| `P5-MODEL-MATRIX-INVENTORY` | `DONE-LOCAL` after `P5-RAY-ROUTE`; it ran in parallel because it changes no production route. | Own `docs/design/p5-model-matrix-inventory.md` only. Inventory every CPU/GPU model and classify it as eligible now, CPU fallback, or unsupported, with the exact missing transport/surface semantics and required CPU oracle. Do not label an implementation supported or modify production code. | Accepted inventory covers 15 named model families, their CPU authority, current seam, predicate, fallback, missing semantics, and CPU-led oracle. It explicitly rejects CUDA `getGPUModel()` as Vulkan evidence. | `P5-MODEL-MATRIX` implementation card. |
| `P5-RAY-APPLY-TRANSACTION` | `DONE-LOCAL-NARROW` after `P5-RAY-ELIGIBILITY-GATE`; full surface transaction remains gated. | Own `gpu/vulkan/ray/ray_flux_process_route_smoke.cpp` only. Preserve strategy ordering, CPU rollback semantics, and the strict eligibility predicate; no `Process`/CPU engine edits. | Focused Release build and Intel Arc run pass. The smoke now covers MultiParticle `Auto` CPU fallback through repeated `Process::apply()`, Manual unprepared fail-closed with geometry/metadata snapshots, and the already accepted Manual eligible Vulkan route. A probed Manual multi-step path hit an existing `0xC0000409` and was removed from scope; no broad transaction claim is made. | `P5-SURFACE-INTEGRATION` and model-matrix rows. |
| `P5-SURFACE-INTEGRATION-NONACCEPTANCE` | `DONE-LOCAL-NARROW` after `P5-SPECIALIZATION-CODEGEN-ROOTCAUSE`; prior crash and superseded CTest result remain historical. | Own `gpu/vulkan/levelset/levelset_surface_ray_deployment_smoke.cpp` and its CMake test wiring. CPU/Vulkan separation is invariant: preserve the CPU `calculateFlux()` oracle and do not label setup/fallback coverage as surface physics. | Release composed smoke exits 0 and covers Manual missing-profile fail-closed, CPU fallback without callbacks, shared-session configuration, cleanup, and Auto missing-ray degradation, followed by the CPU `Process::calculateFlux()` oracle. It does not execute the ray Process path. The separate callback execution smoke passes the coverage/diffusion/neutral/LevelSet Vulkan callbacks, but that is not a complete ray/surface physics acceptance. One correction is allowed; a second failure is reclaimed. | `P5-SURFACE-VULKAN-PHYSICS`, then full surface integration. |
| `P5-CPU-KDTREE-ROOTCAUSE` | `RECLAIMED-MAIN-PARTIAL` — historical diagnostic, superseded by the narrow specialization repair; no broad root-cause claim. | Read-only/diagnostic first. Compare Process mesh construction, `getPointKdTree()` rebuild/lifetime, and `ElementToPointData` against `D:\Codex_lib\code_reference\ViennaPS`; no reference edits, no CPU semantic changes, no fixture weakening. | Native evidence mapped the prior AV to `KDTree<float,array<float,3>>::traverseDown`, dereferencing an invalid node pointer at the `axis` load. CPU triangle and `ElementToPointData` were reference-identical. The explicit-template/public-alias repair removes the reproduction in the narrow Release smokes, but does not prove a general KDTree cause. No second correction is authorized; reclaim remains the terminal state. | `P5-SPECIALIZATION-CODEGEN-ROOTCAUSE` and `P5-SURFACE-CPU-REPRO-ISOLATION`; never unlocks Vulkan by itself. |
| `P5-SURFACE-CPU-REPRO-ISOLATION` | `DONE-LOCAL-NARROW` after the 2026-08-07 Release `/O2 /Ob2 /openmp:llvm` matrix and the specialization repair. | Own the minimal target/link matrix for the exact `SingleParticleProcess<float,2>` plane CPU oracle. Hold model/grid/ray parameters fixed; vary only the deployment translation unit, composition header instantiation, and static-link closure. CPU source remains byte/semantic identical to `D:\Codex_lib\code_reference\ViennaPS`. Invariant: no callback bypass or optimization suppression. | Bare Mod/reference and header-only Process probes pass; the former `sizeof(ProcessDeploymentBinding<2>)` KDTree AV is no longer reproduced after moving heavy binding templates to `process_deployment_binding.cpp` and using the public `NeutralTransportVelocityExecutor` alias. Release binding and composed smoke exits are 0, with CPU `calculateFlux()` as the only composed runtime oracle. One correction is allowed; a second failed reproduction is reclaimed. | `P5-SPECIALIZATION-CODEGEN-ROOTCAUSE` narrow closure, then `P5-SURFACE-VULKAN-PHYSICS`; full surface/model acceptance remains locked. |
| `P5-SPECIALIZATION-CODEGEN-ROOTCAUSE` | `DONE-LOCAL-NARROW` after `P5-SURFACE-CPU-REPRO-ISOLATION`; this closes the reproducible orchestration/codegen failure only. | Own `gpu/vulkan/surface/process_deployment_binding.hpp/.cpp`, the public `include/viennaps/models/psNeutralTransportVelocityExecutor.hpp` alias boundary, and their Release smoke wiring. Invariants: explicit template instantiation keeps heavy Process/ViennaLS bodies out of the deployment header; the executor type is backend-neutral and contains no Vulkan or Process context; CPU formulas and model semantics remain reference-led. Prohibited: CPU algorithm changes, `/Od`/`/Ob0` workarounds, object-heap tricks, or support/promotion claims. | Ordered validation: (1) compare the prior failing specialization and repaired target under identical Release flags; (2) build the Release surface binding smoke; (3) run it to exit 0; (4) run the composed deployment smoke to exit 0; (5) verify Manual/Auto configuration, callback cleanup, and CPU `Process::calculateFlux()` oracle. The composed smoke does not dispatch complete Vulkan surface/ray physics, so retain this as narrow closure. One owner correction is allowed; any second unsuccessful correction is reclaimed. | `P5-SURFACE-VULKAN-PHYSICS`; it does not unlock Process route expansion, surface physics, model matrix, backend promotion, or release readiness. |
| `P5-SURFACE-VULKAN-PHYSICS` | `DONE-LOCAL-NARROW` after mainline correction of the mismatched CPU/Vulkan geometry fixture. | Own `gpu/vulkan/levelset/levelset_surface_ray_deployment_smoke.cpp`, the composition execution smoke, and the smallest required binding/composition adapters. Invariants: CPU baseline remains the unmodified reference-led `Process` path; each Vulkan callback/FluxEngine publishes output transactionally; Auto falls back to CPU and Manual fails closed on unsupported or incomplete setup. Prohibited: changing CPU physics, broadening the ray eligibility predicate, treating P5-JD/JE/JF device reduction as Process/surface acceptance, or editing model-matrix support rows. | Release Intel Arc smoke now uses the same bounded-circle fixture on both sides and executes real Vulkan `Process::calculateFlux()` plus `Process::apply()`: `maxFluxUlp=0`, `totalFluxRelDiff=0`, `maxGeometryDelta=0`. The companion shared-session callback execution smoke passes coverage, surface diffusion, neutral-velocity and LevelSet callback CPU/geometry oracles. Manual missing/incomplete setup fails closed; Auto missing-ray setup degrades and clears callbacks; copied callback lifetime and cleanup pass. This is a narrow strict-route closure, not all surface models. | `P5-SURFACE-INTEGRATION`, then `P5-MODEL-MATRIX-ROW-01`; no promotion or release claim. |
| `P5-SURFACE-INTEGRATION-IMPLEMENTATION` | `BLOCKED-ORACLE` after the source-anchored acceptance audit; existing configuration, callback, and strict-ray evidence is not a full surface Process acceptance. | A future card owns one complete focused `Process::calculateFlux/apply` surface fixture and its narrow Level Set CMake wiring; adapters may change only where required to preserve existing callback/session ownership. The CPU `FluxProcessStrategy` sequence, surface model formulas, neutral transport/diffusion, Level Set ownership, ray predicate, and model matrix remain outside the change boundary. | Before a RED/implementation attempt, the Release paired NeutralTransport CPU oracle must pass under `/O2 /Ob2 /DNDEBUG /openmp:llvm /MD /Zi`; the common ViennaCore crash is a hard stop. Afterward the fixture must cover CPU-callback-free versus shared-session Vulkan callbacks for coverage/diffusion/neutral velocity/Level Set, flux/coverage/conservation/geometry/time/metadata/callback-order results, copied callback lifetime, and Auto/Manual/status/session transactional sentinels. Direct kernel CPU-oracles, setup success, and a strict SingleParticle ray pass are insufficient. | `P5-SURFACE-INTEGRATION` narrowly only after the external CPU-oracle gate and real-device Process differential; neutral/model support and deployment exit stay independently locked. |
| `P5-RAY-DEVICE-PHYSICS` | `DONE-LOCAL-GAP` after the focused negative contract and rollback hardening; `DeviceRayFluxPipeline` multi-bounce remains intentionally unsupported. | Own `gpu/vulkan/ray/` physics adapters and a focused Process-facing smoke. Invariants: ViennaRay/CPU host sampling, reflection, roulette, event ordering, material response, normalization, and transaction rules remain the semantic authority; device status must fail closed. Prohibited: treating triangle-hit/compaction/reduction throughput as multi-bounce physics, changing CPU ray helpers without a paired differential, or widening the eligibility predicate/model matrix. | `DeviceRayFluxPipeline` is still hit -> compact -> sort -> reduce with host reconstruction after one submit. The separate Slice-01 now proves a device-only event-wire transition for host-produced decisions, but does not supply CPU decisions or reach the pipeline. The route smoke machine-checks `devicePhysicsGap()`, runs fixed-seed `maxReflections=1` CPU oracle, verifies Auto CPU fallback and Manual rejection, and the device pipeline smoke verifies reset/session-loss sentinel rollback. Release Intel Arc route and device pipeline both exit 0, but no end-to-end multi-bounce claim is made. | `P5-SURFACE-VULKAN-PHYSICS` narrow route only; multi-bounce physics, model expansion, promotion and release readiness remain locked. |
| `P5-RAY-MULTIBOUNCE-STATE-MAP` | `DONE-LOCAL` after `P5-RAY-PHYSICS` and `P5-RAY-DEVICE-PHYSICS` gap evidence; it adds no executable backend route. | Own [p5-device-multibounce-state-map.md](p5-device-multibounce-state-map.md) only. Freeze ViennaRay's ordered trace state, branch-local RNG use, and host/device ownership before device code is written. Prohibited: accepting a host event loop as device physics, moving callbacks/model semantics to shaders, or broadening `Process` eligibility. | The map identifies the CPU seed/particle/source -> scatter -> boundary -> backface -> collision -> reflection -> weight -> max-reflection -> roulette -> successor order, std430 wire/descriptor/barrier requirements, and a fixed decision/oracle fixture. It explicitly preserves `runGpuPhysics()` fail-closed behavior until Slice-01 is accepted. | `P5-RAY-MULTIBOUNCE-SLICE-01-IMPLEMENTATION`. |
| `P5-RAY-MULTIBOUNCE-SLICE-01-IMPLEMENTATION` | `DONE-LOCAL-NARROW` after the state-map acceptance; no model row or Process route is unlocked. | Own only the focused `gpu/vulkan/ray/` event-wire, SPIR-V, pipeline and smoke additions for host-produced collision/reflection/roulette decisions. CPU callbacks, random-decision generation, materials and Process transaction remain authoritative. | Fixed std430 `Event`/`Decision`/`Accumulation` ABI is 64/64/16 bytes; the decision carries host-produced successor origin/direction as well as action/weight. One command buffer records two real compute dispatches with alternating device event buffers, a device memory barrier and status-before-publish transaction. The Intel Arc smoke and CTest pass the two-bounce high/low raw-FP32 oracle and malformed-relation, capacity-overflow and reset/session-loss sentinel checks; generated SPIR-V also validates. It deliberately does not run triangle hit, create decisions, sample RNG or route `Process`. | `P5-RAY-MULTIBOUNCE-SLICE-02-CPU-DECISION-PRODUCER`; it does not unlock multi-bounce `Process`, model-matrix, promotion or release claims. |
| `P5-RAY-MULTIBOUNCE-SLICE-02-CPU-DECISION-PRODUCER` | `DONE-LOCAL-NARROW` after the one mainline-directed RNG correction; no model row or Process route is unlocked. | Host CPU remains the ViennaRay authority. The focused helper consumes only an already-known front-face hit and caller-owned particle/RNG/local/global data, then creates Slice-01 decisions. It preserves `surfaceCollision` -> `surfaceReflection` -> weight update -> reflection-limit -> roulette; GPU applies but never interprets these records. Device traversal, Process/FluxStrategy/model/header/reference changes, shader callback translation, model eligibility expansion and host-loop masquerading remain prohibited. | The fixed particle fixture proves callback ordering and successor state. A seed-42 pre-limit termination preserves the RNG after exactly its two callback draws; low-weight seed-1 survival and seed-42 rejection each match an independently seeded RNG after exactly collision + reflection + one roulette draw. Producer records feed Slice-01; the Release Intel Arc smoke and both Slice CTests pass raw-FP32 event/action/accumulation checks. Invalid hit state keeps the published decision sentinel. | `P5-RAY-MULTIBOUNCE-SLICE-03-HIT-DECISION-CHAIN`; no `Process` multi-bounce, model-matrix, promotion or release claim. |
| `P5-RAY-MULTIBOUNCE-SLICE-03-HIT-DECISION-CHAIN` | `DONE-LOCAL-NARROW` after mainline Intel Arc/CTest acceptance; one device hit crosses to CPU authority once and returns as a completed record. | Reuses `DeviceTriangleHitPrimitive` only for existing FP32 intersection. The focused chain runs one front-face triangle hit, downloads `TriangleHit`, reconstructs point/normal/material on host and calls Slice-02; Slice-01 alone applies the resulting record. Host loops, Process/model/FluxStrategy/public-header/reference edits, generic routes and eligibility promotion remain prohibited. | Device hit and CPU Moller-Trumbore match raw `t`/index/barycentrics. The host decision reaches device accumulation/action/successor state, all raw-FP32 checked on Intel Arc. Miss never enters the producer; backface and session loss fail closed with sentinels retained. The three Slice CTests pass. The second decision slot is an explicit fixed terminal record, not a bounce loop. | A bounded two-hit sequencing design card; no multi-bounce `Process`, model-matrix, promotion or release claim. |
| `P5-RAY-MULTIBOUNCE-SLICE-04-TWO-HIT-CHAIN` | `DONE-LOCAL-NARROW` after mainline Release/Intel Arc acceptance; it proves a second actual device hit from the first CPU callback successor, not a fabricated terminal record. | Global position (about 180 tokens): own one focused ray smoke and its minimal CMake registration. Fix exactly two triangles, one source ray, `maxReflections=1`, no scatter/boundary/custom source and one known particle law. Dispatch hit-1 on device; CPU uses Slice-02 and transfers only its authoritative successor origin/direction into hit-2 device input; dispatch hit-2, then CPU produces the limit termination record. Slice-01 applies both records once. This is an explicitly unrolled two-hit fixture, not a general host bounce loop. Prohibited: Process/model/public header/reference/shader/triangle algorithm edits, host queue/loop abstraction, generic material or route expansion. | Both device hits match their CPU `TriangleHit` records exactly. The second ray is reconstructed from the first decision's successor bytes; callback/RNG ordering proves `numReflections > maxReflections` terminates the second hit without roulette. Raw event/accumulation outputs and second-hit miss/backface/session-loss sentinels pass. Mainline Release build, real Intel Arc executable and the four Slice CTests pass. | `P5-RAY-MULTIBOUNCE-GENERAL-QUEUE-DESIGN`; no multi-bounce `Process`, model-matrix, promotion or release claim. |
| `P5-RAY-MULTIBOUNCE-GENERAL-QUEUE-DESIGN` | `DONE-LOCAL` after its one mainline topology correction; it is a design gate, not a device-physics completion claim. | [p5-device-multibounce-general-queue.md](p5-device-multibounce-general-queue.md) freezes a bounded, deterministic multi-event queue while CPU remains responsible for ViennaRay callbacks, material semantics, source/boundary handling, roulette and transaction decision publication. It defines identity/order, buffer ownership, host/device synchronization, capacity/status failure semantics, the CPU raw-bit RED fixture, and exact `Process` prerequisites. The accepted topology is an explicit stable frontier: GPU computes a hit/classification frontier, CPU produces decisions in trace order, then GPU advances the next frontier. | The reviewed contract traces fields/branches to `D:\\Codex_lib\\code_reference\\ViennaPS` and current ViennaRay; it fixes `maxTotalEvents`, `maxFrontierEvents`, and `maxRounds` fail-closed limits and distinguishes CPU orchestration from device physics. Links and scoped diff check pass. Slice-01..04 remain narrow evidence only. The one correction was consumed; any implementation mismatch follows its own retry budget. | `P5-RAY-MULTIBOUNCE-GENERAL-QUEUE-IMPLEMENTATION`, subject to the still-blocked Release CPU/model oracle; no route or matrix unlock. |
| `P5-RAY-MULTIBOUNCE-GENERAL-QUEUE-IMPLEMENTATION` | `DONE-LOCAL-GAP` after one owner correction and mainline recovery of its malformed active-lane fixture; it remains below the Process/model gates. | Own only the bounded `gpu/vulkan/ray/` frontier queue, its SPIR-V/ABI and focused fixture. The GPU applies complete host decisions and stages status/accumulation; the smoke composes a real `DeviceTriangleHitPrimitive` at every active frontier and passes only raw-matched hit records to CPU decision production. CPU retains callback/reflection/weight/limit/roulette order. Identity `(particleId,bounce,sequence)`, explicit capacity limits, and transactional sentinels are preserved. Prohibited: Process/FluxStrategy/model/public-header/reference edits, C++ callbacks/RNG in shaders, eligibility expansion, throughput claims, or calling CPU frontier orchestration device-resident physics. | Release build, SPIR-V validation, Intel Arc executable, and the 10-test focused P5 suite pass. The three-frontier fixture covers high continuation, seed-1 roulette survival, reflection-limit termination without a roulette draw, raw device hit fields, capacity, malformed-decision and lost-session bytewise sentinels. The first malformed case exposed an inactive lane and was fixed on mainline. The separately compiled reference-vs-Mod `TraceKernel` oracle now passes, but proves only CPU transition order; it does not turn the queue into a Process/model route. | `P5-RAY-MULTIBOUNCE-FRONTIER-REFERENCE-ORACLE`; no Process route or matrix unlock. |
| `P5-RAY-MULTIBOUNCE-QUEUE-GENERATION-RECOVERY` | `DONE-LOCAL-NARROW` after direct Release MSVC rebuild/link and real-adapter acceptance on 2026-08-17; it is a correctness repair below Process routing. | Own only the queue's captured `ComputeSession` generation, stale-resource teardown, and its focused smoke. A queue bound to an earlier session must reject before any old descriptor/pipeline/fence use or output publication. `DeviceBuffer` remains generation-aware; non-buffer Vulkan wrappers are explicitly abandoned only after their owning device has already been destroyed. No Process/model/FluxStrategy/reference/shader/eligibility change is permitted. | Code records the bound generation, checks it in `isInitialized()` and before publish, returns a distinct stale-generation error, and the smoke adds reset -> reinitialize -> stale-queue sentinel coverage after releasing the unrelated hit primitive. Scoped `git diff --check` and MSVC `/Zs` syntax checks pass. After canonical `Path`/`PATH` cleanup, the Release CMake target and the focused Intel Arc CTest both pass (`1/1`); the executable prints `multibounce frontier queue Vulkan dispatch PASS` with exit 0. | A verified queue recovery primitive for `P5-RAY-MULTIBOUNCE-PROCESS-INTEGRATION-IMPLEMENTATION`; it unlocks neither a route nor a model row. |
| `P5-RAY-MULTIBOUNCE-PROCESS-INTEGRATION-DESIGN` | `DONE-LOCAL` after source-anchored coordinator review; it changes no route. | Own [p5-multibounce-process-integration-contract.md](p5-multibounce-process-integration-contract.md) only. It binds any later frontier engine to the existing `FluxEngine` and `Process` transaction while retaining CPU trace/callback/RNG/model/surface/normalisation authority. It also records the exact strict predicate and current runtime Auto gap. | Links and scoped diff check pass. The design prohibits predicate/model expansion and requires independent reference trace, Intel Arc differential, staged output, conservation/geometry and an explicit runtime CPU retry before calling Auto fallback. It records that the current frontier queue merely applies completed decisions. | `P5-RAY-MULTIBOUNCE-PROCESS-INTEGRATION-IMPLEMENTATION` only after a passing reference oracle and row-specific gates. |
| `P5-RAY-MULTIBOUNCE-PROCESS-INTEGRATION-IMPLEMENTATION` | `DONE-LOCAL-NARROW` after the authorized reference seam, strict Row-01 evidence, and mainline Release/Intel Arc acceptance on 2026-08-17. | Global position (about 180 tokens): own one separately admitted bounded `FluxEngine` route for `float/2D/SingleParticleProcess`, one label, default source, no coverage/desorption, and `maxReflections=1`. Preserve the unmodified CPU `TraceKernel` producer and `FluxProcessStrategy` order; Vulkan owns only device hit/frontier application/reduction. The existing engine override is the only injection seam. Prohibited: public Process API changes, model-matrix expansion, callback/RNG shaders, source/boundary relaxation, or device-resident semantic claims. | The paired TraceKernel observer and strict CPU row are accepted. The focused Process smoke builds and runs on Intel Arc with `totalRelDiff=0,maxRelDiff=0`, Manual unprepared output sentinel, and CTest 2/2 alongside the existing route smoke. A first Vulkan step followed by callback-triggered session reset exercises stale frontier detection and Auto CPU retry; generation-safe triangle resources keep the process successful and publish no stale device output. Hit parity is the existing bounded ULP-4 oracle; generic multi-bounce, surface physics, aggregate model matrix and deployment exit remain locked. | `P5-SURFACE-INTEGRATION` narrow closure only; aggregate matrix/deployment exit remain independently locked. |
| `P5-RAY-MULTIBOUNCE-FRONTIER-REFERENCE-ORACLE` | `DONE-LOCAL-NARROW` after explicit test-only seam authorization and coordinator acceptance on 2026-08-13. | Own [the paired observer fixture](../../tests/multibounceFrontierReferenceDifferential/README.md) only. Reference resolves `D:\\Codex_lib\\code_reference\\ViennaPS` plus an independent authorized ViennaRay source tree; Mod resolves the workspace plus its cached ViennaRay. The real `TraceKernel` remains the sole source of Embree loop, callbacks, reflection/roulette and Philox state. Prohibited: CPU reimplementation, Process/model route use, Vulkan output as oracle, and any non-test observer path. | `run_paired_oracle.ps1` and registered CTest compile two test translation units, run the true trace, and compare 3072 raw bytes. The checker proves four hits/collision/reflection/weight transitions, two high-weight continuations, one roulette state advance, and one third-hit reflection-limit termination with unchanged Philox state. This is a narrow ViennaRay CPU-state oracle, not full CPU Process or model conformance. | `P5-RAY-MULTIBOUNCE-PROCESS-INTEGRATION-IMPLEMENTATION` remains behind its strict Process, Intel Arc and model-row gates. |
| `P5-RAY-MULTIBOUNCE-REFERENCE-SEAM-AUTHORITY` | `DONE-LOCAL-NARROW` after the explicit user authorization on 2026-08-13. | Own the default-off `VIENNAPS_P5_TRACE_OBSERVER` seam in the two authorized ViennaRay test copies, plus the independent emitter fixture. The observer is append-only, records only after normal CPU transitions, and has no production API/link dependency or mutation authority. | A fixed 124-byte raw record plus magic stores hit/callback/action/weight/successor/reflection and real Philox counter/key/output-index state. The CTest pair passes raw-byte equality and branch-order checks. The seam remains test-only; generic multi-bounce stays CPU fallback/Manual fail-closed until separately accepted Process integration. | `P5-RAY-MULTIBOUNCE-FRONTIER-REFERENCE-ORACLE` accepted narrowly; model, deployment and release gates remain independently locked. |
| `P5-CPU-REFERENCE-CONFORMANCE` | `DONE-LOCAL` — inventory accepted after coordinator review; the former Mod-only `/O2` crash is now a narrow orchestration repair, not a parity result. | [p5-cpu-reference-conformance.md](p5-cpu-reference-conformance.md) compares every P5-reachable CPU implementation with `D:\Codex_lib\code_reference\ViennaPS`; CPU triangle and `ElementToPointData` are semantic-identical, while Process/Context/FluxStrategy changes are bounded optional executor/injection seams. | Empty-executor CPU formulas and order remain reference-led. Binding/composed smoke exit 0 plus CPU `calculateFlux()` only establish narrow setup/oracle coverage; every changed support row still requires a paired reference/Mod differential and a real Vulkan physics differential. One correction is allowed; a second is reclaimed. | Protects `P5-MODEL-MATRIX-ROW-01` and `P5-DEPLOYMENT-EXIT` from claiming CPU parity or Vulkan physics without evidence. |
| `P5-MODEL-MATRIX-ROWS` | `BLOCKED-PREDECESSOR` behind the Release Neutral CPU oracle and full surface integration; Row-01 plus CPU/fallback evidence for MultiParticle, IonBeam, CF4O2, SF6O2, SF6C4F8, concrete Fluorocarbon, SingleParticleALD, TEOSPECVD and OxideRegrowth still do not open the aggregate lane. | Implement one model row at a time from `docs/design/p5-model-matrix-inventory.md`; Vulkan eligibility is opt-in per evidenced predicate, all other rows remain CPU fallback/unsupported. Do not add a global Vulkan switch or infer CUDA support. Rank-14 Oxidation is explicitly deferred to the P6 managed-solver phase. | Each row must provide a reference-CPU differential, conservation/geometry checks appropriate to its semantics, explicit fallback or fail-closed behavior, and a named missing-semantics reason. Stop and reclaim after one correction failure. | `P5-DEPLOYMENT-EXIT` only after every claimed P5 row and the CPU surface blocker are resolved. |
| `P5-MODEL-MATRIX-COMPUTE-MAP` | `DONE-LOCAL` after the 15-row inventory; it is design evidence, not a support update. | Own [p5-model-matrix-compute-map.md](p5-model-matrix-compute-map.md) only. Map the CPU R/C/D/L/M computation surface, missing device kernels, retained CPU semantics, and row-specific oracle for strict plus all 14 non-eligible rows. | The map rejects CUDA classes, generic callbacks, and fallback configuration as Vulkan support. Neutral velocity remains an evidence-gapped compute candidate; WetEtching, SelectiveEpitaxy, and single-precursor TEOS now each have narrow numeric/production Process evidence, but every other complete non-strict model row remains CPU fallback/Manual fail-closed. | `P5-NEUTRAL-VELOCITY-SUBSTAGE`, the separately accepted WetEtching, SelectiveEpitaxy, and TEOS adapters, without unlocking aggregate matrix. |
| `P5-WETETCH-VELOCITY-SUBSTAGE` | `DONE-LOCAL-NARROW-COMPUTE` after the rank-11 compute-map review and the separately admitted seam implementation on 2026-08-18; it remains a compute-only predecessor. | Own [p5-wetetch-velocity-substage.md](p5-wetetch-velocity-substage.md), `include/viennaps/models/psWetEtchingVelocityExecutor.hpp`, the opt-in Vulkan executor/shader/smoke, and the paired CPU fixture. The caller-owned work callback freezes the CPU-oracle/session/transaction boundary; `TranslationField`, `Process`, model construction and Level Set publication remain CPU authoritative. No stage enum, profile key, callback binding or aggregate model-row change is authorized. | Paired Mod/reference CPU runner is raw-exact at OMP 1/2/4/8 under `/O2 /Ob2 /DNDEBUG /openmp:llvm /MD`. Release Intel Arc Vulkan smoke reports `maxUlp=1`; malformed material, NaN, reset and stale-session sentinels pass. SPIR-V validation and focused CTest `2/2` pass. The numeric seam is consumed only by the separately accepted narrow production adapter; it does not itself imply generic WetEtching Process support. | `P5-WETETCH-PROCESS-ADAPTER` is accepted only for its strict row; aggregate model matrix, Neutral/surface integration and deployment exit remain locked. |
| `P5-WETETCH-PROCESS-ADAPTER-PROBE` | `DONE-LOCAL-NARROW-PROBE` historical diagnostic, superseded by the production adapter row below; it does not alter generic deployment policy. | Own `gpu/vulkan/levelset/wetetch_velocity_process_adapter_smoke.cpp` and its minimal CMake/CTest wiring. The fixture remains a bounded regression probe around unchanged `AnalyticProcessStrategy`, preserving CPU fallback for callback failure. It is retained as supporting evidence and must not be read as the production row by itself. | Release Intel Arc CTest passes the complete CPU/Vulkan LevelSet comparison with `maxGeometryUlp=0`, `gpuSamples=5`, and `fallbackSamples=0`. The probe alone is not a paired reference Process oracle, full-sample GPU execution, or generic WetEtching support. | `P5-WETETCH-PROCESS-ADAPTER` provides the production setter and paired CPU Process gate; model matrix and deployment exit stay locked. |
| `P5-WETETCH-PROCESS-ADAPTER` | `DONE-LOCAL-NARROW` after the paired CPU Process oracle and Release Intel Arc acceptance on 2026-08-18; this is one explicit model row, not deployment promotion. | Own the opt-in `WetEtching::setVelocityExecutor` bridge in `include/viennaps/models/psWetEtching.hpp`, the focused production adapter smoke and CMake target, and `tests/wetetchProcessReferenceDifferential/`. The model computes the unchanged CPU scalar first, stages a one-point Vulkan candidate only when a caller installs the executor, and returns CPU output on callback/status/finite/count failure. No `Process`, `TranslationField`, `FluxProcessStrategy`, stage/profile binding, or generic eligibility change is authorized. | The independent Mod/reference CPU Process fixture is raw-exact at OMP 1/2/4/8 under `/O2 /Ob2 /DNDEBUG /openmp:llvm /MD`. The Intel Arc production adapter reports `maxGeometryUlp=0,calls=5,accepted=5,fallbackExactUlp=0`; its deliberate failing callback preserves exact CPU geometry. Clean Release build and focused CTest pass. This closes only the strict `WetEtching<float,2>` analytic one-Si-rate row. | D=3, multiple materials/species, non-analytic callbacks, automatic deployment/profile promotion, Neutral/surface integration, aggregate model matrix and deployment exit remain locked. |
| `P5-SELECTIVE-EPITAXY-VELOCITY-SUBSTAGE` | `DONE-LOCAL-NARROW` after the rank-12 seam implementation and paired CPU evidence on 2026-08-18; it is a numeric substage, not a generic model promotion. | Own [p5-selective-epitaxy-velocity-substage.md](p5-selective-epitaxy-velocity-substage.md), `include/viennaps/models/psSelectiveEpitaxyVelocityExecutor.hpp`, the opt-in `SelectiveEpitaxy::setVelocityExecutor` adapter, Vulkan executor/shader/smokes and `tests/selectiveEpitaxyProcessReferenceDifferential/`. CPU material gating, mask BooleanOperations, stencil setup/finalization, Process order and Level Set publication remain authoritative. No stage/profile binding or device domain ownership is authorized. | SPIR-V validation and clean Release build pass. Intel Arc executor smoke reports `maxUlp=4` with malformed/NaN/reset/stale-session sentinels; production Process adapter reports `maxGeometryUlp=0,calls=5,accepted=5,fallbackExactUlp=0`; deliberate callback failure is exact CPU fallback. Independent Mod/reference CPU Process runner is raw-exact at OMP 1/2/4/8 under `/O2 /Ob2 /DNDEBUG /openmp:llvm /MD`. | D=3, custom materials, Boolean/stencil/device domain migration, generic deployment/profile promotion, Neutral/surface integration, aggregate model matrix and deployment exit remain locked. |
| `P5-SELECTIVE-EPITAXY-PROCESS-ADAPTER` | `DONE-LOCAL-NARROW` for one explicit analytic row; it does not close the rank-12 family. | Own only `gpu/vulkan/levelset/selective_epitaxy_velocity_process_adapter_smoke.cpp` and its CMake/CTest wiring. The model computes the unchanged CPU scalar first and accepts a one-point Vulkan candidate only after transaction/oracle checks; callback failure returns the exact CPU scalar. Process/strategy/domain transforms remain unchanged. | Release Intel Arc Process smoke passes with `maxGeometryUlp=0`, five accepted callback samples and exact CPU fallback; paired Mod/reference Process raw-bit fixture passes OMP 1/2/4/8. | No D=3/custom-material/full Boolean-stencil route or automatic promotion; aggregate matrix and P5 deployment exit remain locked. |
| `P5-TEOS-VELOCITY-SUBSTAGE` | `DONE-LOCAL-NARROW` after the rank-9 compute-map review and single-precursor seam implementation on 2026-08-18; it remains a numeric substage, not complete TEOS support. | Own `docs/design/p5-teos-velocity-substage.md`, `include/viennaps/models/psTEOSVelocityExecutor.hpp`, the opt-in single-precursor `TEOSDeposition` bridge, Vulkan executor/shader, focused smokes, and [the paired CPU fixture](../../tests/teosProcessReferenceDifferential/README.md). CPU particle transport, sticking, coverage, precursor ordering, advection and Process strategy remain authoritative. No multi-precursor, TEOSPECVD, stage/profile binding or ray eligibility expansion is authorized. | SPIR-V validation, clean Release build and Intel Arc executor smoke pass (`maxUlp=1`; malformed/NaN/reset/stale-session sentinels). The production Process adapter passes with `maxGeometryUlp=1`, one accepted batch and exact CPU fallback; independent Mod/reference CPU Process output is raw-exact at OMP 1/2/4/8 under `/O2 /Ob2 /DNDEBUG /openmp:llvm /MD`. | Multi-precursor labels/sticking, TEOSPECVD/radical-ion transport, automatic promotion, Neutral/surface integration, aggregate matrix and deployment exit remain locked. |
| `P5-TEOS-PROCESS-ADAPTER` | `DONE-LOCAL-NARROW` for one explicit single-precursor analytic row; it does not close rank 9. | Own only `gpu/vulkan/levelset/teos_velocity_process_adapter_smoke.cpp` and its CMake/CTest wiring. CPU computes the original `rate * pow(flux, order)` vector first; Vulkan is a candidate batch with status/oracle/finite checks and CPU fallback on failure. `FluxProcessStrategy`, particle callbacks, coverage updates and Level Set publication remain unchanged. | Release Intel Arc Process smoke passes with `maxGeometryUlp=1`, one accepted callback batch and exact CPU fallback; paired Mod/reference Process raw-bit fixture passes OMP 1/2/4/8. | Multi-precursor/D=3/custom reaction domains, automatic deployment/profile promotion, Neutral/surface integration, aggregate matrix and P5 deployment exit remain locked. |
| `P5-NEUTRAL-VELOCITY-SUBSTAGE` | `BLOCKED-PREDECESSOR` behind the Release paired CPU oracle and full surface-integration gate; it is not a `NeutralTransport` support row. | Own the existing backend-neutral `NeutralTransportVelocityExecutor` compute seam and its focused CPU/Vulkan differential. Preserve the CPU particle transport, coverage, desorption, surface diffusion, material behavior and Process order. | Accept only raw-bit velocity candidate equality on the bounded N2 input domain, transactional callback publication, CPU fallback/Manual fail-closed, and a paired full CPU process/conservation/geometry oracle. The row remains unsupported until all neutral transport stages have independent device contracts. | Reusable numeric-operation evidence only; aggregate model matrix remains locked. |
| `P5-MODEL-MATRIX-ROW-01-REFERENCE-CPU-ORACLE` | `DONE-LOCAL-NARROW` after the paired strict CPU script on 2026-08-17; it is CPU-authority evidence for Row-01 only. | Own `tests/strictRow01ReferenceDifferential/`: two independently compiled caller fixtures, raw-byte checker, CTest registration and runner. It freezes only `SingleParticleProcess<float,2>`, one default label/source, seed 42, `raysPerPoint=1`, SOURCE normalisation and zero reflections. It excludes Vulkan, coverage/NeutralTransport, non-strict models, header edits and CPU algorithm changes. | The runner builds Mod and unmodified `D:\Codex_lib\code_reference\ViennaPS` TU pairs under `/O2 /Ob2 /DNDEBUG /openmp:llvm /MD /Zi` with the CMake-locked dependency closure. OMP 1/2/4/8 all produce byte-identical 1152-byte flux/geometry/Process serializations; the checker passes four times. The standalone CTest configure is not accepted on this host because its Visual Studio generator cannot discover CXX despite `cl.exe` in the development environment; the registered test has not been substituted for the actual successful runner. | `P5-MODEL-MATRIX-ROW-01`; no Neutral, non-strict model, surface, promotion or release claim. |
 | `P5-MODEL-MATRIX-ROW-01` | `DONE-LOCAL-NARROW` after its independent strict CPU-reference oracle and existing Intel Arc route/conservation acceptance. | Own only the first eligible `SingleParticleProcess<float,2>`/one-label/default-source/zero-reflection ray slice plus explicit CPU-fallback assertions for every other inventory family. Preserve CPU setup, surface semantics, ordering, rollback, and fail-closed publication boundaries. Prohibited: production headers, reference-tree edits, CUDA promotion, and treating this default-only exactness as aggregate model support. | The strict CPU pair is now raw-exact at OMP 1/2/4/8 through `P5-MODEL-MATRIX-ROW-01-REFERENCE-CPU-ORACLE`. `model_matrix_fallback_smoke` passes with 15 inventory rows (1 strict, 14 CPU fallback/unsupported). The Intel Arc Row-01 smoke passes SOURCE-normalized flux, zero-flux/invalid-status rollback, `Process::apply()` geometry and conservation with `maxFluxUlp=0`, `totalFluxRelDiff=0`, `maxGeometryDelta=0`. The Neutral Release oracle remains independently blocked and does not invalidate this no-coverage strict row; it still blocks Neutral/surface rows. | `P5-MODEL-MATRIX-ROW-02-MULTIPARTICLE-CPU-FALLBACK`; aggregate `P5-MODEL-MATRIX`, Neutral/surface support and `P5-DEPLOYMENT-EXIT` remain locked until all required rows and gates resolve. |
 | `P5-MODEL-MATRIX-ROW-02-MULTIPARTICLE-CPU-FALLBACK` | `DONE-LOCAL-NARROW-CPU-FALLBACK` after the paired Release CPU differential on 2026-08-18; this row records CPU parity and fallback behavior only. | Own [the paired MultiParticle fixture](../../tests/multiParticleProcessReferenceDifferential/README.md) and its CTest wiring. Freeze `MultiParticleProcess<float,2>` with one fully-sticking neutral label and one fully-sticking ion label, source normalization, `maxReflections=0`, and the CPU triangle engine. Do not add a Vulkan species/energy/label seam, widen the ray predicate, or infer support from the CUDA conversion class. | Mod and unmodified reference outputs are raw-exact (`2207` bytes) at OMP 1/2/4/8 under `/O2 /Ob2 /DNDEBUG /openmp:llvm /MD`; both flux labels are finite/nonnegative and triangle geometry is byte-identical. The existing 15-row fallback smoke remains the routing gate: Auto stays CPU and Manual Vulkan fails closed before publication. This does not close MultiParticle Vulkan transport, surface coupling, aggregate matrix, or deployment exit. | `P5-MODEL-MATRIX-ROW-03-IONBEAM-CPU-FALLBACK`; aggregate review remains behind the Neutral Release oracle and full surface integration. |
 | `P5-MODEL-MATRIX-ROW-03-IONBEAM-CPU-FALLBACK` | `DONE-LOCAL-NARROW-CPU-FALLBACK` after the paired Release CPU differential on 2026-08-18; this row records IonBeam CPU parity and fallback behavior only. | Own [the paired IonBeam fixture](../../tests/ionBeamProcessReferenceDifferential/README.md) and its CTest wiring. Freeze `IonBeamEtching<float,2>` with fixed energy (`meanEnergy=100`, `sigmaEnergy=0`), threshold 20, one CPU triangle, source normalization, `raysPerPoint=1`, and `maxReflections=0`. Do not add an ion-energy, reflection, redeposition, or material Vulkan seam, widen the ray predicate, or infer support from the CUDA conversion class. | Mod and unmodified reference outputs are raw-exact (`2249` bytes) at OMP 1/2/4/8 under `/O2 /Ob2 /DNDEBUG /openmp:llvm /MD`; ion/redeposition labels are finite and nonnegative and triangle geometry is byte-identical. The existing fallback smoke remains the routing gate: Auto stays CPU and Manual Vulkan fails closed before publication. This does not close IonBeam transport, reflection, redeposition, surface coupling, aggregate matrix, or deployment exit. | `P5-MODEL-MATRIX-ROW-04-CF4O2-CPU-FALLBACK`; aggregate review remains behind the Neutral Release oracle and full surface integration. |
 | `P5-MODEL-MATRIX-ROW-04-CF4O2-CPU-FALLBACK` | `DONE-LOCAL-NARROW-CPU-FALLBACK` after the paired Release CPU differential on 2026-08-18; this row records CF4O2 CPU parity and fallback behavior only. | Own [the paired CF4O2 fixture](../../tests/cf4o2ProcessReferenceDifferential/README.md) and its CTest wiring. Freeze `CF4O2Etching<float,2>` with fixed four-label CPU parameters, one CPU triangle, source normalization, `raysPerPoint=1`, and `maxReflections=0`. Do not add a four-species Vulkan transport/surface seam, widen the ray predicate, or infer support from CUDA conversion classes. | Mod and unmodified reference outputs are raw-exact (`2921` bytes) at OMP 1/2/4/8 under `/O2 /Ob2 /DNDEBUG /openmp:llvm /MD`; all four ion/etchant/oxygen/polymer labels are finite/nonnegative and triangle geometry is byte-identical. The existing fallback smoke remains the routing gate: Auto stays CPU and Manual Vulkan fails closed before publication. This does not close CF4O2 transport, coverage/surface chemistry, aggregate matrix, or deployment exit. | `P5-MODEL-MATRIX-ROW-05-SF6O2-CPU-FALLBACK`; aggregate review remains behind the Neutral Release oracle and full surface integration. |
 | `P5-MODEL-MATRIX-ROW-05-SF6O2-CPU-FALLBACK` | `DONE-LOCAL-NARROW-CPU-FALLBACK` after the paired Release CPU differential on 2026-08-18; this row records SF6O2 CPU parity and fallback behavior only. | Own [the paired SF6O2 fixture](../../tests/sf6o2ProcessReferenceDifferential/README.md) and its CTest wiring. Freeze `SF6O2Etching<float,2>` with fixed ion/etchant/passivation parameters, one CPU triangle, source normalization, `raysPerPoint=1`, and `maxReflections=0`. Do not add plasma transport/reflection/coverage Vulkan semantics, widen the ray predicate, or infer support from the CUDA conversion class. | Mod and unmodified reference outputs are raw-exact (`3131` bytes) at OMP 1/2/4/8 under `/O2 /Ob2 /DNDEBUG /openmp:llvm /MD`; all five ion/etchant/passivation labels are finite/nonnegative and triangle geometry is byte-identical. The existing fallback smoke remains the routing gate: Auto stays CPU and Manual Vulkan fails closed before publication. This does not close SF6O2 transport, plasma coverage/surface chemistry, aggregate matrix, or deployment exit. | `P5-MODEL-MATRIX-ROW-06-SF6C4F8-CPU-FALLBACK`; aggregate review remains behind the Neutral Release oracle and full surface integration. |
 | `P5-MODEL-MATRIX-ROW-06-SF6C4F8-CPU-FALLBACK` | `DONE-LOCAL-NARROW-CPU-FALLBACK` after the paired Release CPU differential on 2026-08-18; this row records SF6C4F8 CPU parity and fallback behavior only. | Own [the paired SF6C4F8 fixture](../../tests/sf6c4f8ProcessReferenceDifferential/README.md) and its CTest wiring. Freeze `SF6C4F8Etching<float,2>` with ion+etchant and explicit `passivationFlux=0`, one CPU triangle, source normalization, `raysPerPoint=1`, and `maxReflections=0`. Do not add plasma/polymer transport/reflection/coverage Vulkan semantics, widen the ray predicate, or infer support from CUDA conversion classes. | Mod and unmodified reference outputs are raw-exact (`2903` bytes) at OMP 1/2/4/8 under `/O2 /Ob2 /DNDEBUG /openmp:llvm /MD`; all four active labels are finite/nonnegative and triangle geometry is byte-identical. The existing fallback smoke remains the routing gate: Auto stays CPU and Manual Vulkan fails closed before publication. This does not close SF6C4F8 transport, polymer/surface chemistry, aggregate matrix, or deployment exit. | `P5-MODEL-MATRIX-ROW-07-FLUOROCARBON-PLASMA-CPU-FALLBACK`; aggregate review remains behind the Neutral Release oracle and full surface integration. |
 | `P5-MODEL-MATRIX-ROW-07-FLUOROCARBON-CPU-FALLBACK` | `DONE-LOCAL-NARROW-CPU-FALLBACK` after the paired Release CPU differential on 2026-08-18; this row covers only the concrete production `FluorocarbonEtching<float,2>` family member and not the absent `PlasmaEtching` type. | Own [the paired Fluorocarbon fixture](../../tests/fluorocarbonProcessReferenceDifferential/README.md) and its CTest wiring. Freeze ion/etchant/polymer fluxes `56/500/100`, `delta_p=1`, temperature 300, one CPU triangle, source normalization, `raysPerPoint=1`, and `maxReflections=0`. Do not invent a `PlasmaEtching` class, add a fluorocarbon/plasma Vulkan transport or surface seam, widen the ray predicate, or infer support from CUDA conversion classes. | Mod and unmodified reference outputs are raw-exact (`3338` bytes) at OMP 1/2/4/8 under `/O2 /Ob2 /DNDEBUG /openmp:llvm /MD`; all five active labels are finite/nonnegative and triangle geometry is byte-identical. The existing fallback smoke remains the routing gate: Auto stays CPU and Manual Vulkan fails closed before publication. The inventory's absent `PlasmaEtching` name remains uncovered; this does not close fluorocarbon/plasma transport, surface chemistry, aggregate matrix, or deployment exit. | `P5-MODEL-MATRIX-ROW-08-SINGLEPARTICLEALD-CPU-FALLBACK`; aggregate review remains behind the Neutral Release oracle and full surface integration. |
 | `P5-MODEL-MATRIX-ROW-08-SINGLEPARTICLEALD-CPU-FALLBACK` | `DONE-LOCAL-NARROW-CPU-FALLBACK` after the paired Release CPU differential on 2026-08-18; this row records SingleParticleALD CPU parity and fallback behavior only. | Own [the paired SingleParticleALD fixture](../../tests/singleParticleAldProcessReferenceDifferential/README.md) and its CTest wiring. Freeze ballistic `SingleParticleALD<float,2>` with `gasMeanFreePath=-1`, `stickingProbability=1`, no evaporation, no growth per cycle, no coverage diffusion, one CPU triangle, source normalization, `raysPerPoint=1`, and `maxReflections=0`. Do not add ALD transport/coverage Vulkan semantics, widen the ray predicate, or infer support from the CUDA ballistic class. | Mod and unmodified reference outputs are raw-exact (`2492` bytes) at OMP 1/2/4/8 under `/O2 /Ob2 /DNDEBUG /openmp:llvm /MD`; `ParticleFlux` and `Coverage` are finite, Coverage is within `[0,1]`, and triangle geometry is finite/byte-identical. The existing fallback smoke remains the routing gate: Auto stays CPU and Manual Vulkan fails closed before publication. This does not close ALD transport, adsorption/desorption cycles, coverage diffusion, aggregate matrix, or deployment exit. | `P5-MODEL-MATRIX-ROW-09-TEOSPECVD-CPU-FALLBACK`; aggregate review remains behind the Neutral Release oracle and full surface integration. |
 | `P5-MODEL-MATRIX-ROW-09-TEOSPECVD-CPU-FALLBACK` | `DONE-LOCAL-NARROW-CPU-FALLBACK` after the paired Release CPU differential on 2026-08-18; this board row covers inventory rank-10 `TEOSPECVD` only, while rank-9 single-precursor `TEOSDeposition` remains a separate accepted narrow numeric row. | Own [the paired TEOSPECVD fixture](../../tests/teospecvdProcessReferenceDifferential/README.md) and its CTest wiring. Freeze `TEOSPECVD<float,2>` with `radicalRate=1`, `radicalOrder=1`, `ionRate=0`, `ionOrder=1`, one CPU triangle, source normalization, `raysPerPoint=1`, and `maxReflections=0`. Do not add PECVD radical/ion transport/reflection/surface Vulkan semantics, widen the ray predicate, or infer support from the CUDA callable model. | Mod and unmodified reference outputs are raw-exact (`2494` bytes) at OMP 1/2/4/8 under `/O2 /Ob2 /DNDEBUG /openmp:llvm /MD`; `radicalFlux` and `ionFlux` are finite/nonnegative and triangle geometry is finite/byte-identical. The existing fallback smoke remains the routing gate: Auto stays CPU and Manual Vulkan fails closed before publication. This does not close TEOSPECVD transport, reaction-order surface coupling, aggregate matrix, or deployment exit. | `P5-MODEL-MATRIX-RANK-13-OXIDEREGROWTH-CPU-FALLBACK`; ranks 11/12 remain covered by their separately accepted WetEtch/SelectiveEpitaxy narrow rows, and aggregate review remains behind the Neutral Release oracle and full surface integration. |
 | `P5-MODEL-MATRIX-ROW-10-OXIDEREGROWTH-CPU-FALLBACK` | `DONE-LOCAL-NARROW-CPU-FALLBACK` after the paired Release CPU differential on 2026-08-18; this row records the real `Process.apply()`/`ByproductDynamics` CPU path only. | Own [the paired OxideRegrowth fixture](../../tests/oxideRegrowthProcessReferenceDifferential/README.md) and its CTest wiring. Freeze the production `OxideRegrowth<float,2>` stack/dense-cell fixture with `processTime=0.1`, generated cell-set geometry, callback pre/post-advect, and serialized `byproductSum`, `Material`, filling fractions and nodes/elements. Do not add a Vulkan callback/dense-cell/redeposition seam, promote P6 solvers, or infer support from a velocity field. | Mod and unmodified reference outputs are raw-exact (`19774` bytes) at OMP 1/2/4/8 under `/O2 /Ob2 /DNDEBUG /openmp:llvm /MD`; `byproductSum`, `Material`, filling fractions and dense-cell geometry are finite/nonnegative and byte-identical. The bounded mass check is not a complete physical conservation oracle; a non-fatal `byproductSum` initialization warning remains recorded. Auto stays CPU and Manual Vulkan fails closed before publication. This does not close OxideRegrowth callback/device transport, aggregate matrix, or deployment exit. | `P6-LA-BASELINE` after P5 Neutral/surface/remote/deployment gates; rank-14 Oxidation remains a P6 managed-solver candidate, not a P5 row. |
 | `P5-CPU-REUSE-RAY-ROUND2` | `DONE-LOCAL` for the existing eligibility slice; broader routes remain closed. | Audit and remediate ray host helpers only. The CPU authority is the unmodified `D:\Codex_lib\code_reference\ViennaPS`; do not change CPU engines or model physics. | Resolved for `SingleParticleProcess<float,2>`/default source/zero reflection: CPU `runNumber=1` seed sequence, terminated-boundary ray removal (with original denominator), D2 physical line area, and MAX zero-flux division behavior. The non-unit-grid real-device route smoke passes at `totalRelDiff=0`, `maxRelDiff=0`. 3-D, custom source, coverage/logging, multi-particle, surface physics and full boundary matrices remain CPU fallback/unsupported and require their own oracle cards. | P5 exit review; no eligibility expansion. |
| `P5-MODEL-MATRIX` | `BLOCKED-PREDECESSOR` after narrow eligibility/inventory acceptance; full surface/Neutral oracle and aggregate row evidence remain open. | One support row at a time; no broad "Vulkan enabled" switch. | Each row has an eligibility rule, CPU differential, conservation/geometry criterion, fallback behavior, and unsupported reason. | `P5-DEPLOYMENT-EXIT`. |

Current audit evidence: `P5-RAY-ROUTE` has hardware evidence only for a
single-bounce `SingleParticleProcess<float,2>` `calculateFlux()` fixture;
`P5-RAY-PHYSICS` supplies CPU-side contracts, while `P5-RAY-DEVICE-PHYSICS`
now records a machine-checkable multi-bounce gap and CPU fallback contract.
The Release surface binding and deployment smokes exit 0 after the
specialization repair. The shared-session callback execution smoke runs
coverage, diffusion, neutral velocity, and LevelSet update/rebuild against CPU
oracles; the companion surface deployment smoke now executes the eligible Vulkan
`Process::calculateFlux()` and `Process::apply()` route with
`maxFluxUlp=0`, `totalFluxRelDiff=0`, and `maxGeometryDelta=0` on Intel Arc.
This closes only the strict narrow row: full surface-model composition,
multi-bounce physics, aggregate model matrix, promotion, and release readiness
remain locked.

The 2026-08-17 local Release rerun additionally rebuilt the previously missing
model-matrix executables. The first combined 13-test invocation was contaminated
by orphaned build/CTest descendants and was terminated; after reaping those
processes, the binding smoke was rerun in isolation and passed CTest 1/1, with
the route/model/surface/multi-bounce focused targets also passing in their
isolated CTest runs. No residual compiler, CTest, or Vulkan smoke process
remains.

The 2026-08-18 WetEtch rerun now closes one deliberately narrow production
model row while preserving the CPU-first design. The paired Mod/reference CPU
Process fixture is raw-exact at OMP 1/2/4/8 under the required Release flags.
The opt-in `WetEtching::setVelocityExecutor` adapter computes the unchanged CPU
scalar first, stages only a one-point Vulkan candidate, and falls back exactly
on a deliberately failing callback. Its Release Intel Arc smoke reports
`maxGeometryUlp=0,calls=5,accepted=5,fallbackExactUlp=0`; the compute-only
executor still reports `maxUlp=1` with malformed/NaN/reset/stale-generation
sentinels. This closes only `WetEtching<float,2>` with one Si material-rate
entry and the existing analytic Process path. No stage/profile binding,
automatic promotion, D=3/broader model support, Neutral/surface integration,
aggregate matrix, or deployment-exit claim is added.

The 2026-08-18 SelectiveEpitaxy rerun closes a second deliberately narrow
production numeric row. Its CPU formula, material gating, mask Boolean/stencil
setup, and Level Set restoration remain unchanged; the opt-in executor replaces
only the scalar crystal-velocity candidate. Intel Arc reports `maxUlp=4` for
the executor and `maxGeometryUlp=0,calls=5,accepted=5,fallbackExactUlp=0` for
the Process adapter, with malformed/NaN/reset/stale-session and deliberate
CPU-fallback checks. The independent Mod/reference Process fixture is raw
exact at OMP 1/2/4/8. D=3, custom materials, device domain ownership and
automatic promotion remain locked.

The same rerun closes rank 9 only for single-precursor TEOS. The CPU model
first evaluates the reference `rate * pow(flux, order)` vector; Vulkan then
serves as a checked batch candidate. The Intel Arc executor reports
`maxUlp=1` with malformed/NaN/reset/stale-session sentinels, and the Process
adapter reports `maxGeometryUlp=1,calls=1,accepted=1,fallbackExactUlp=0`. The
paired Mod/reference CPU Process fixture is raw-exact at OMP 1/2/4/8.
Multi-precursor TEOS,
TEOSPECVD transport, aggregate matrix, surface integration and deployment exit
remain independently locked.

The current mainline revalidation also passes the bounded frontier queue: its
Release clean-environment target, Intel Arc executable, and CTest are green,
and the focused 29-test Release regression cluster is `29/29` (including the
frontier reference differential, accepted narrow model rows, profile/deployment
tests, and Process route smokes). The previously unbuilt ray pipeline and
fused smoke targets now also build and pass CTest `2/2`. This closes only
the bounded host-decision/device-apply primitive; CPU callbacks, RNG, material
semantics, Process integration and general model admission remain outside its
scope. The TEOS executor's caller-owned-session reset test now reports the
stale-generation sentinel explicitly. The P5 completion audit therefore still
has four open gates: the paired Neutral Release oracle, full surface Process
integration, aggregate model rows, and remote/deployment exit evidence.

The 2026-08-18 model-matrix follow-up completed the executable CPU/fallback
rows in dependency order: MultiParticle (2207 bytes), IonBeam (2249), CF4O2
(2921), SF6O2 (3131), SF6C4F8 (2903), concrete Fluorocarbon (3338),
SingleParticleALD (2492), TEOSPECVD (2494), and OxideRegrowth (19774). Each
paired Mod/reference Release runner passed OMP 1/2/4/8 raw-byte comparison;
the aggregate routing smoke plus these nine new differential tests passed
10/10, and the focused Intel Arc route/surface/multi-bounce/Row-01 cluster
passed 8/8. These are CPU-authority/fallback records only: no row widens the
Vulkan predicate or supplies a model-specific device transport/surface seam.
Rank-14 Oxidation remains a P6 managed-solver candidate and was deliberately
not started before P5 exit.

An independent post-cleanup CMake scan resolves all 39 literal Vulkan source
and shader paths, with no missing file; the tree currently contains 35 `.comp`
sources. This is a repository-completeness check only and does not turn the
bounded queue or callback executors into full Process/model support.

The 2026-08-18 deployment follow-up also rechecked the packaging boundary. The
default CPU/no-SDK configure keeps `VIENNAPS_ENABLE_VULKAN=OFF` and
`VIENNAPS_INSTALL_VULKAN_PAYLOAD=OFF`; the standalone install/export consumer
fixtures are excluded from the in-tree graph and are only configured from an
installed prefix. The opt-in Vulkan payload validator and installed consumer
remain green. A cached VTK-enabled configure with
`VTK_MODULE_ENABLE_VTK_IOHDF=NO` gets past the bundled HDF5 POSIX probe, but
then stops at ViennaLS's `ViennaLSTargets` export because its VTK link targets
are not exported in that set; VTK export remains an explicit deployment-exit
blocker. The same Release tree now has a focused 9/9 profile/deployment CTest
cluster pass.

### PD4-HW-MATRIX

- Status: `DONE` — accepted on main after a fresh no-SDK, CPU control-plane,
  and Intel Arc strict-FP32 rerun
- Date: 2026-08-03

The current main worktree rerun used three isolated, temporary build directories:
a no-SDK diagnostic probe, four CPU/fixture tests, and an SDK-enabled strict
probe. The no-SDK probe built and returned the disabled build-time fallback
contract with exit code 0. The focused configuration ran
`capabilityProfileIO`, `probeProfileAdapter`, `vulkanDeploymentBootstrap`, and
`vulkanDeploymentProbe` successfully 4/4 in 1.29 s. Those fixtures cover
missing, stale, malformed, duplicate/type-invalid, and unknown-schema profiles;
CPU fail-closed behavior; Manual CPU bypass; and strict child nonzero, timeout,
malformed-output, mismatched-hardware, invalid-evidence, and cleanup paths.

On the Intel Arc device, strict FP32 succeeded under
`fp32-bitwise-watchdog-v1` with 18 cases, zero mismatches, zero ULP, 166 ms
within the 60 s watchdog, and a unique matching device UUID. Profile validation
passed; the selected compute queue was family 1 with a dedicated queue, and
compute families were `[0,1]`. The forced strict failure exited nonzero with
an explicit diagnostic, did not promote Vulkan, and left no new strict-child
evidence file. The complete raw hardware profile and JSON evidence were kept
only for the run and then removed with the temporary builds; no local SDK, VTK,
cache, driver, or profile path is tracked.

| Gate | Result |
|---|---|
| No-SDK contract | Standalone diagnostic probe: PASS; exit 0; `status=disabled`, empty devices, and `source=build-time-fallback`. |
| CPU control plane | Focused CTest: PASS 4/4 in 1.29 s. |
| Strict success | Intel Arc strict child: PASS; exact FP32 contract and matching deployment profile identity. |
| Strict negative path | Forced strict failure: PASS; parent exited nonzero, explicit diagnostic retained, no new child-evidence file remained, and fixture cleanup/adoption coverage passed. |
| Scope boundary | PD4 hardware acceptance is complete; production routing, Level Set, global CMake, and local environment paths remain untouched. |

### PD5-CI-DOCS-INTEGRATION

- Status: `DONE` — local implementation and static validation accepted on main
  2026-08-03; no GitHub-hosted or self-hosted workflow result is asserted.
- Scope: `.github/workflows/build.yml`, the CI integration section of the
  development report, and this status record. No production C++, CMake, SDK,
  profile, or hardware output is tracked.

The standard cross-platform `test` job now explicitly configures
`VIENNAPS_ENABLE_VULKAN=OFF`, `VIENNAPS_BUILD_VULKAN_PROBE=OFF`, and
`VIENNAPS_BUILD_VULKAN_SMOKE=OFF`; it remains the ordinary CPU/no-SDK build and
non-benchmark CTest lane. `vulkan-hardware` is a separate job that runs only
when a manually dispatched workflow sets `run_vulkan_hardware=true`, and only
on a runner labeled both `self-hosted` and `vulkan`. It uses `RUNNER_TEMP` for
its build, profile, and probe log, leaving no hardware identity or local SDK
path in the repository or CI summary.

`path-hygiene` runs on a hosted Ubuntu runner and scans the controlled workflow
and CI documentation with boundary-aware patterns for Windows and Unix absolute
paths plus SDK environment assignments. The pattern construction avoids
self-matching its own rule and URLs. The development report contains the same
commands, lane boundary, and the link to the PD4 hardware matrix evidence.

| Gate | Local evidence |
|---|---|
| Workflow syntax | Python YAML parse: PASS; required `path-hygiene` and `vulkan-hardware` jobs present. |
| Default CPU/no-SDK boundary | Fresh no-SDK CMake evidence used by PD4 configured `VIENNAPS_ENABLE_VULKAN=OFF`, probe/smoke OFF and ran the four control-plane CTests 4/4. The CI job now passes the same flags explicitly on every hosted platform. |
| Path hygiene | The workflow's six boundary-aware `git grep -E` patterns produced zero matches against the two controlled files: PASS. |
| Optional hardware lane | Static workflow review: dispatch-only boolean input, `self-hosted` + `vulkan` labels, strict probe and focused CTest commands, all generated paths under `RUNNER_TEMP`: PASS. Remote runner provisioning/execution is not claimed. |
| Scope boundary | No SDK installation was added to hosted CI; no performance result, hardware promotion, or remote workflow success is inferred. |

### PD5-INSTALL-EXPORT

- Status: `DONE` — local CPU/no-SDK install/export acceptance completed on
  2026-08-04. GitHub-hosted CPU evidence is a separate, subsequent step.
- Scope: the ViennaLS patch transport, root CMake export compatibility bridge,
  `cmake/run-pd5-install-export.cmake`, the standalone consumer fixture, and
  the isolated CPU/no-SDK CI job. No production Vulkan routing, VTK support,
  CUDA behavior, profile, or hardware evidence is changed.

The acceptance script owns a caller-selected temporary work directory. It
configures the producer with VTK and every Vulkan target disabled, builds the
complete producer/dependency closure, installs it, and then configures the
consumer only through the resulting install prefix. It disables
`CMAKE_FIND_USE_INSTALL_PREFIX` for the producer configuration so a failed or
partial prior install cannot satisfy an upstream dependency search. The
consumer imports `ViennaTools::ViennaPS`, creates a plane domain, and exits
successfully only when the public headers and transitive targets compile, link,
and run.

The initial consumer configuration exposed a real ViennaLS v5.8.5 export
defect: its generated config required VTK even when configured without VTK.
The versioned ViennaLS integration patch was normalized against a clean source
checkout, and the root CMake compatibility bridge rewrites only that generated
binary-tree dependency list in the no-VTK configuration. VTK-enabled exports
remain outside this card and are not claimed as passing.

| Gate | Local evidence |
|---|---|
| Patch transport | Clean-source dry-run application: PASS for the tracked ViennaLS integration patch. |
| Producer configuration | Fresh CPU/no-SDK configure: PASS with VTK, Vulkan probe, and Vulkan smoke disabled. The resulting ViennaLS config requires `ViennaHRLE` and `ViennaCore`, not VTK. |
| Producer build and install | PASS after complete Embree build; package configs, headers, runtime libraries, and dependency exports installed to the isolated prefix. |
| Independent consumer | `find_package(ViennaPS 4.6 CONFIG REQUIRED)`, `ViennaTools::ViennaPS` link, compile, and run: PASS; CTest 1/1 in 0.46 s. |
| Optional VTK export | `BLOCKED-LOCAL-ENV` on 2026-08-18: a project-side probe with `VTK_MODULE_ENABLE_VTK_IOHDF=NO` gets past the bundled HDF5 `popen/pclose` probes, but generation then fails because ViennaLS's install export references VTK targets that are not in `ViennaLSTargets`; no VTK-enabled install/export or VTK payload claim is made. |
| Optional Vulkan cases | Accepted separately below for the opt-in payload; this does not imply VTK, Process preview, hardware promotion, or remote CI. |

#### Opt-in Vulkan payload extension — local acceptance 2026-08-18

`VIENNAPS_INSTALL_VULKAN_PAYLOAD=ON` is an explicit release-packaging mode and
requires `VIENNAPS_ENABLE_VULKAN=ON` plus a discovered Vulkan SDK. It enables
the complete configured Vulkan stage set, builds the reusable
`viennaps_vulkan_compute_runtime`, and exposes
`ViennaTools::viennaps_vulkan_compute_runtime` through the installed export
with a `Vulkan` package dependency. The install prefix contains the runtime
headers/library, the original GLSL sources, generated SPIR-V files, and a
versioned payload manifest. The default CPU/no-SDK install path is unchanged.

| Gate | Local evidence |
|---|---|
| Release configure/build | Fresh Ninja configure with `VIENNAPS_ENABLE_VULKAN=ON`, `VIENNAPS_INSTALL_VULKAN_PAYLOAD=ON`, VTK off, and the repository CPM cache: PASS. `viennaps-vulkan-package-payload` generated 32 SPIR-V files; the Release runtime and Embree dependency were built. |
| Script replay | `cmake -P cmake/run-pd5-vulkan-install-export.cmake` replayed in the caller-owned Release tree and exited 0, including reconfigure, install, validator, consumer build, and CTest. |
| Install payload validator | `cmake -DVIENNAPS_VULKAN_INSTALL_PREFIX=... -P cmake/validate-pd5-vulkan-install-payload.cmake`: PASS (`spv=32,runtime=1`); manifest, representative GLSL, headers, export target, and Vulkan dependency were checked. |
| Independent consumer | `tests/vulkanInstallExportConsumer` configured only with the install prefix, linked `ViennaTools::viennaps_vulkan_compute_runtime`, and passed CTest 1/1 in 0.65 s. The same installed consumer exercised profile serialization, provisioning/reload, and stale-fingerprint fail-closed selection, printing `PD5 Vulkan install consumer profile persistence PASS`. |
| Boundary | This is package/payload evidence only. It does not install a Process deployment profile, promote AUTO routing, provide a release Process preview, or close hosted CI/VTK gates. |

### PD3-LS-SHARED-SESSION


- Status: `DONE` — accepted on main 2026-08-03; the implementation claim is released
- Date: 2026-08-03

`LevelSetProcessController<D>` now provisions the update and rebuild paths from
the one `DeploymentComputeContext` session. The rebuild executor stores an
aliasing `shared_ptr` whose control block retains `RuntimeState` while its
pointer targets the context-owned `ComputeSession`; this preserves callback
lifetime without a second initialization or an ownership cycle. Result
observability reports update/rebuild generation and device name, and both
controller smokes assert identity. Manual CPU still clears both callbacks and
uses no session; existing automatic/manual rollback paths are unchanged.

| Gate | Result |
|---|---|
| RED | No pre-change session identity hook exists; a compile-failure oracle was not invented. The focused assertions were added with the narrow observability needed to exercise the former duplicate-session behavior. |
| GREEN | A fresh root build with the cached, patched ViennaLS source compiled both controller targets. `ctest -C Debug -R '^viennaps-vulkan-levelset-process-controller(-execution)?-smoke$'` passed 2/2 on Intel Arc in 2.56 s. Both paths asserted a nonzero equal generation and matching nonempty device name. The controller fixture now carries the existing strict-FP32 smoke evidence, so its automatic Vulkan selection exercises the policy instead of weakening it. |
| Scope boundary | Only `levelset_process_controller.hpp`, its two focused smoke sources, their CTest registration, and this status entry changed; runtime/session types, Process APIs, rebuild executor interfaces, shaders, CUDA, dependencies, and fixed local SDK paths remain untouched. |

### PD3-SESSION-COMPOSE

- Status: `DONE` — accepted on main after root rebuild and focused execution evidence
- Date: 2026-08-03

`LevelSetSurfaceDeploymentComposition<D>` is now the sole deployment-time
owner coordinating the Process surface binding and Level Set controller. It
prepares the exact four-stage workload plan once, then passes the Process
binding's `DeploymentComputeContext` to Level Set as a borrowed dependency.
Coverage, surface diffusion, neutral velocity, Level Set update, and Level Set
rebuild therefore execute through one `ComputeSession`. The composition clears
all installed callbacks before releasing state; copied callbacks retain their
shared holders and remain usable until their copies are released. A partial
AUTO configuration degrades atomically to CPU, while Manual Vulkan failure
remains explicit and Manual CPU provisions no Vulkan state.

| Gate | Result |
|---|---|
| Build | Fresh root Vulkan/Ninja configuration with the repository CPM cache succeeded; `cmake --build .tmp_pd3_vulkan_final_20260803 --parallel 4` compiled the root targets and the five-stage execution fixture. |
| Focused CTest | `ctest --test-dir .tmp_pd3_vulkan_final_20260803 --output-on-failure -R 'viennaps-vulkan-(levelset-(deployment-session|surface-composition|process-controller)|process-deployment-binding)'` passed 6/6 in 7.30 s. |
| Five-stage execution | On Intel Arc, the execution fixture reported `five callbacks share one generation/device with CPU oracle PASS`: coverage and diffusion raw-bit oracles, neutral velocity raw-bit CPU/GPU oracle, and CPU/Vulkan Level Set advection comparison all passed. It asserted a nonzero common generation and identical device identity for surface, update, and rebuild. |
| Lifetime and policy matrix | The fixture invokes copied coverage, diffusion, neutral, update, and rebuild callbacks after `clear(process)`. It also verifies valid mixed Manual Vulkan-surface/Manual-CPU-Level-Set selection, atomic AUTO degradation with a missing surface shader, explicit Manual Vulkan failure with no silent CPU callback set, and Manual CPU success without a context or GPU callbacks. |
| Companion regressions | The API smoke and both Level Set controller smokes passed. The controller execution fixture also exercised its existing strict callback failure paths and completed successfully. |
| Scope boundary | The composition owner/execution fixture, Process binding/CMake artifact wiring required for the exact full plan and neutral callback retention are complete. Probe/profile policy, CUDA, and production routing remain outside this card. |

### PD3-ROOT-INTEGRATION

- Status: `DONE` — accepted on main 2026-08-03; the targeted integration claim is released
- Date: 2026-08-03

The root checkout now has a reproducible Vulkan-enabled configuration using the
repository-local CPM cache and the Visual Studio developer environment. The
root build compiles the PD3 deployment session, Process binding, Level Set
controller, surface composition, surface executors, and the existing CPU test
suite without changing production routing or requiring VTK.

| Gate | Result |
|---|---|
| Root configure | `cmake -S D:\\Codex_lib\\ViennaPSMod -B .tmp_pd3_vulkan_final_20260803 -G Ninja -DVIENNAPS_USE_VTK=OFF -DVIENNAPS_VTK_RENDERING=OFF -DVIENNAPS_ENABLE_VULKAN=ON -DVIENNAPS_BUILD_TESTS=ON` passed with the local CPM cache. |
| Root build | `cmake --build .tmp_pd3_vulkan_final_20260803 --parallel 4` passed; all configured targets were compiled. |
| Focused deployment/composition gate | Six tests passed in 7.30 s: deployment session, surface composition, composition execution, Process controller, controller execution, and Process deployment binding. |
| CPU oracle | `ctest --test-dir .tmp_pd3_vulkan_final_20260803 --output-on-failure -R '^trench$'` passed 1/1 in 8.67 s. |
| Non-benchmark regression | `ctest --test-dir .tmp_pd3_vulkan_final_20260803 --output-on-failure -E "Benchmark|Performance|vulkanCpuBaseline"` passed 96/96 in 423.90 s. |
| Host-path note | The broader 97-test command reached 89/97 before the existing `vulkanCpuBaseline` exceeded the 600 s command limit; this unrelated long-running test is not silently counted as a pass. |
| Scope boundary | Root integration and CPU fixtures are accepted; no algorithm rewrite, production backend promotion, CUDA change, or VTK dependency change is included. |

### PD4-PERF-BASELINE

- Status: `DONE` — reproducible baseline evidence accepted on main 2026-08-03; no optimization or speedup claim is made
- Date: 2026-08-03

`cmake/run-pd4-perf-baseline.ps1` is the checked-in harness. It repeats the
already-gated CPU/Vulkan smoke executable for coverage, graph diffusion,
neutral transport, Level Set update, and the five-stage Level Set composition
path three times each. Every run must exit 0 and emit its CPU/Vulkan oracle
marker before the result is written. The captured evidence is
`docs/design/pd4-perf-baseline-20260803.json`.

| Suite | Runs | Elapsed ms (min / median / max) | Dispatch / submit / buffer contract sites |
|---|---:|---:|---:|
| coverage | 3 | 333.37 / 344.85 / 366.30 | 2 / 2 / 3 |
| diffusion | 3 | 410.63 / 451.36 / 468.75 | 2 / 2 / 4 |
| neutral | 3 | 441.60 / 452.06 / 479.17 | 1 / 1 / 2 |
| levelset-update | 3 | 246.07 / 250.96 / 260.26 | 1 / 1 / 1 |
| levelset-composition | 3 | 3442.13 / 4332.26 / 4458.09 | 8 / 8 / 16 |

The exact command was:
`pwsh -NoProfile -File .\\cmake\\run-pd4-perf-baseline.ps1 -BuildDir .tmp_pd3_vulkan_final_20260803 -Iterations 3 -OutputPath docs/design/pd4-perf-baseline-20260803.json`.
All 15 runs passed their CPU/Vulkan correctness gate and the JSON records
`deterministic_correctness: true`. Dispatch, submit, and buffer values are
explicitly source-contract site inventories, while `elapsed_ms` is measured
per process run; the harness does not infer a runtime counter or claim a
speedup. A future optimization card may replace these contract inventories
with runtime instrumentation without reopening the correctness baseline.

### Mainline continuation snapshot: 2026-08-03

- Status: `RUN` — progress was merged for continued development; this entry is
  not a completion or release declaration.
- Mainline commits: `4d96298` (`test: compose Vulkan deployment session`) and
  `b4f39b7` (`docs: record PD4 hardware matrix acceptance`).
- PD3 progress now present on `main`: the cross-surface/Level Set composition
  owner, borrowed deployment-context seam, five-stage execution fixture, and
  focused CMake registration.
- PD4 progress retained on `main`: the hardware-matrix card plus focused test
  hardening for profile fail-closed behavior, numerical-evidence fallback,
  selected-device propagation, probe-failure cleanup, and strict probe-device
  argument propagation.
- At this snapshot: `PD3-SESSION-COMPOSE`, `PD3-ROOT-INTEGRATION`, and
  `PD4-PERF-BASELINE` were accepted while `PD4-HW-MATRIX` had not yet been
  accepted. The later mainline rerun recorded in the current PD4 entry above
  supersedes that PD4 state; the remaining PD5 cards still require their own
  evidence.
- The snapshot remains historical and is intentionally not rewritten as a
  completion declaration. No production-routing promotion, optimization
  claim, or release gate is implied by that historical entry.
