# Vulkan compute acceleration implementation status

This file records verified implementation slices. It complements, and does not
replace, the accepted development report and ADR.

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
device and size is idempotent. Existing host-visible APIs remain intact. The
buffer must be reset before its owning session is reset or destroyed; a
session-generation token for detecting stale handles after reset/reinitialize
is not implemented yet and remains a gate before the production resident
pipeline owns buffers across session transitions.

The same slice corrects Vulkan buffer teardown order for host-visible and
device-local buffers: the bound buffer is destroyed before its memory is
freed. The new smoke verifies an exact upload, device-to-device copy, and
download round trip, move state, and the principal validation failures.

| Gate | Result |
|---|---|
| Device-local allocation | storage and bidirectional transfer usage pass on the selected Vulkan device |
| Exact transfer | upload, device copy, and download preserve four uint32 values exactly |
| Session isolation | a buffer created by one logical device is rejected by a different session |
| Transfer safety | zero, null, uninitialized, out-of-range, alias, and size mismatch cases fail closed |
| Runtime compatibility | deployment context, runtime compute, compute session, and device-buffer smokes pass 4/4 |
| No-SDK behavior | the 17 relevant CPU, policy, HRLE, executor, and probe tests pass |
| Deployment schema | no schema change; use existing suite gate, estimated bytes, and safe working-set threshold |
| Conservative HRLE peak | 2D is at least `120N + 8`; 3D is at least `152N + 8`, plus scan scratch and alignment |
| Residual boundary | session generation, timeout/deferred cleanup, device scan overloads, shaders, and materialization remain pending |
| Path policy | no SDK, dependency, source checkout, or build-cache path is tracked |

The deployment-time profile already contains `vulkanPrimitiveSuitePass` and
`safeVulkanWorkingSetBytes`, while each stage supplies `estimatedBytes`.
Therefore P4C3 does not require a profile-schema change. Automatic routing
must require the primitive suite, compute capability, and sufficient safe
working set. Manual selection continues to override policy, but not missing
hardware capabilities or an unsafe memory bound; those cases retain the
existing strict/fallback behavior.

## Next slice

Remove the classification-to-compaction host round trip with device-resident
intermediate buffers, then feed the validated final readback into the P4C2 CPU
insertion oracle. Wire the complete path into the ViennaLS executor and
deployment-time primitive suite only after those gates pass. Direct
negative/non-finite time injection also remains a defensive-branch coverage
gap. The optional VTK-enabled install/export conflict should be isolated from
the compute backend before packaging validation.

After those production-seam gates, the Level Set work advances to HRLE
sparse rebuild integration, followed by particle/ray and surface/oxidation
stages.
