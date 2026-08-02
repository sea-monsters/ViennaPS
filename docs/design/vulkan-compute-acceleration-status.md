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

### P5-B2: exact FP32 CSR graph-diffusion primitive

- Status: accepted locally; surface-process integration pending
- Date: 2026-08-02

`SurfaceGraphDiffusionFp32` implements one explicit FP32 graph-diffusion
step for the CSR form of `SurfaceDiffusionSolver::stepExplicit`. One Vulkan
invocation owns one CSR row and performs its products and accumulation in the
input edge order, then applies `field + diffusionStep * laplacian`. The shader
uses `precise`, and its generated SPIR-V contains `NoContraction` decorations
for the multiply/add operation boundaries.

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
| Scope boundary | this is a reusable CSR primitive only; `psSurfaceDiffusion` and coverage/process/controller routing remain CPU |

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

- Status: FP32 Vulkan implementation declined; a separate FP64 capability-gated
  proposal is required before reconsidering it
- Date: 2026-08-02

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
software-double implementation; manual selection cannot bypass either gate.

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

## Next slice

The segmented rebuild adapter is installed by the level-set controller, the
deployment probe executes the small FP32 update plus HRLE transaction before
persisting the primitive gate, and D=3 Forward Euler has a real differential.
RK2/RK3 remain deliberately CPU-only until a multi-stage device state machine
is proven. The first surface velocity formula is now exact but intentionally
unwired to process selection. Direct negative/non-finite time injection and
the optional VTK-enabled install/export conflict remain validation gaps. The
next ray slice replaces P5-H's single-invocation baseline with scalable
device-resident hit compaction and deterministic sort/reduce while preserving
the accepted CPU/FP32 order; only then can the mandatory per-call CPU
integrity guard become an opt-in validation path. Coverage reaction is a
capability-gated FP64 candidate, without weakening the current fail-closed
gate.

After those production-seam gates, the Level Set work advances to HRLE
sparse rebuild integration, followed by particle/ray and surface/oxidation
stages.
