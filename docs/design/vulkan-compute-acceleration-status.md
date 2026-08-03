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

- Status: accepted locally as an ordered multi-submit composition baseline;
  it is not the final one-command path or a production Process route
- Date: 2026-08-02

`DeviceRayFluxPipeline` is the physical composition of
P5-I triangle hits, P5-JA record compaction, P5-JB2B stable radix ordering,
and P5-JC surface reduction. It will allocate and upload only the original
ray, triangle, and weight inputs, retain all hit/record/count/segment
intermediates in the shared `ComputeSession`, and download only the terminal
surface id, weight, and count. The first acceptance fixture is five rays and
three triangles: three hits on surface 0 reduce to `3.0`, one translated hit
is a singleton `-0`, and one ray misses. Every terminal word is compared with
the existing CPU pipeline oracle. The standalone Vulkan build compiles the
new smoke under MSVC; two direct Intel Arc executions print
`device ray-flux pipeline Vulkan dispatch PASS`, and its focused CTest passes
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

- Status: accepted locally as a compute-submission fusion slice; it is not a
  production Process route or strict-FP32 deployment promotion
- Date: 2026-08-02

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
| Regression group | standalone `gpu/vulkan` build under MSVC succeeds and five focused CTests pass: triangle-hit device, ray-record compaction, recursive radix sort, surface reduction, and the full device ray-flux pipeline |
| Scope boundary | no device-visible non-finite/overflow intermediate status, strict-FP32 deployment probe, exact dynamic output admission, BVH, particle transport, Process routing, or automatic backend eligibility is claimed |

### P5-JF: strict-FP32 reduction status and fail-closed terminal commit

- Status: accepted locally as a numerical-integrity slice; it closes the
  device-visible intermediate-sum rejection gap in the P5-JE aggregate, but is
  not a production ray-tracing or Process route
- Date: 2026-08-03

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
| Existing normal path | The existing five-ray/three-triangle CPU differential and device ray-flux smoke still pass, preserving the accepted bitwise normal-path result. |
| Validation boundary | The two affected standalone CTests pass on the local Vulkan device. A complete standalone build remains blocked by the pre-existing `/W4 /WX` C4530 exception-handling warning in `gather_histogram_primitives.cpp`; this slice does not claim that unrelated full-suite gate. |
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
P5-JD first proves physical device-resident composition of P5-I, P5-JA,
P5-JB2B, and P5-JC while retaining the sixteen stable P5-JB2B LSD passes,
their full `RayRecord` bit contract, and device-local count storage. P5-JE
then records those stages with explicit barriers into one compute submission.
P5-JF completes the next numerical-integrity condition with a device-visible
status and a fail-closed terminal commit for non-finite or out-of-domain
intermediate FP32 sums, matching the relevant `reduceCpu` rejection boundary.
P5-R1 next supplies compute-only traversal over a CPU-built, device-resident
flat BVH. The remaining ray work is GPU BVH construction/refit,
boundary/reflection/multi-bounce behavior, surface-model coupling, and Process
routing. P5-K2 now populates P5-K1's evidence with an isolated watchdog probe;
its dedicated raw-word shader and process boundary cannot reuse the HostVisible
radix helpers.
CPU differential checking remains an explicit validation gate, not a
production per-call guard. Coverage reaction is a capability-gated FP64
candidate, without weakening the current fail-closed gate.

After those production-seam gates, the Level Set work advances to HRLE
sparse rebuild integration, followed by particle/ray and surface/oxidation
stages.

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
| Residual top-level gate | local VTK-source configuration reached VTK feature detection but exceeds the bounded interactive configuration window; a future long-running top-level build must execute the registered deployment-session CTest with the same environment-provided SDK/dependency paths |
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

- Snapshot: 2026-08-03; this is the scheduling source of truth for concurrent
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
| `PD3-SESSION-COMPOSE` | `DONE` — accepted 2026-08-03; five-stage composition uses one shared session. | Exclusive: a composition orchestrator, its focused execution smoke, narrow Process-binding adapters, and this status evidence only. | Accepted serial prerequisite; `PD3-ROOT-INTEGRATION` and `PD4-PERF-BASELINE` may proceed. | One session across coverage, diffusion, neutral velocity, Level Set update/rebuild; copied-callback lifetime; atomic mixed AUTO/MANUAL fallback matrix. |
| `PD3-ROOT-INTEGRATION` | `READY-S`: requires `PD3-SESSION-COMPOSE` and available patched ViennaLS/CS/VTK prerequisites. | Root integration tests and process/trench fixtures only; no algorithm rewrite. | Serial integration gate. | Root configure/build, focused executor and deployment tests, then non-benchmark CTest/trench CPU oracle; record any host-path blocker exactly. |
| `PD4-HW-MATRIX` | `DONE` — accepted 2026-08-03: valid isolated worktree; no-SDK disabled contract, profile/CPU fail-closed fixtures, strict-FP32 Intel Arc success and forced-failure behavior, and fingerprint/queue evidence recorded. | Exclusive: `docs/design/pd4-hw-matrix-card.md`, `gpu/vulkan/VulkanProbe.cpp`, `tests/vulkanDeploymentProbe/`, `tests/probeProfileAdapter/`, `tests/vulkanDeploymentBootstrap/`, `tests/capabilityProfileIO/`. No production routing changes; no Level Set code. | Evidence accepted; future changes require a new defect/extension card. | No-SDK probe exit 0; four focused CPU CTests 4/4; Intel Arc strict PASS with matching identity/queue; forced strict failure exits nonzero and remains fail-closed. |
| `PD4-PERF-BASELINE` | `READY-S`: requires `PD3-SESSION-COMPOSE` so session-overhead measurements are meaningful. | Benchmark harness/scripts and status evidence only. | May run beside the hardware matrix after its predecessor. | Repeated deterministic CPU/Vulkan runs; submit/dispatch/buffer metrics for coverage, diffusion, neutral, Level Set; CPU correctness check before any performance claim. |
| `PD5-CI-DOCS-INTEGRATION` | Docs drafting is `READY-P`; CI merge gate is `READY-S` after PD3 root integration and PD4 evidence. | CI workflow/CMake focused options and this status record. | Documentation can proceed in parallel; CI changes wait for evidence. | CPU/no-SDK CI lane, optional self-hosted Vulkan lane, path-hygiene check, linked matrix evidence; CI never requires an SDK. |
| `PD5-INSTALL-EXPORT` | `READY-S`: requires root integration; optional release gate. | CMake install/export, consumer smoke, and deployment documentation. | Serial release-facing gate. | Install/export consumer compile; CPU configure required; optional Vulkan/VTK cases recorded without absolute local paths. |

Dependency guard: `PD3-LS-SHARED-SESSION` must extend the existing Level Set
owner rather than creating another `ProcessDeploymentBinding` session;
`PD3-SESSION-COMPOSE` is the only card allowed to own cross-surface/Level Set
lifecycle composition. `PD4` cards are observational until their acceptance
evidence exists. Every implementation card must use CPU results as its
correctness oracle on non-CUDA hosts.

### PD4-HW-MATRIX

- Status: `DONE` — accepted on Intel Arc; claim released
- Date: 2026-08-03

The card was recovered into the registered `claude/pd4-f84f7c` worktree; no
prunable legacy worktree or root-level build directory was used. The
no-SDK standalone probe built and returned the disabled build-time fallback
contract with exit code 0. In the focused Debug configuration,
`capabilityProfileIO`, `probeProfileAdapter`, `vulkanDeploymentBootstrap`, and
`vulkanDeploymentProbe` passed 4/4. Those fixtures cover missing, stale,
malformed, duplicate/type-invalid, and unknown-schema profiles; CPU
fail-closed behavior; Manual CPU bypass; and strict child nonzero, timeout,
malformed-output, mismatched-hardware, invalid-evidence, and cleanup paths.

On the Intel Arc device, strict FP32 succeeded under
`fp32-bitwise-watchdog-v1` with 18 cases, zero mismatches, zero ULP, 130 ms
within the 60 s watchdog, and a unique matching device UUID. Profile validation
passed; the selected compute queue was family 1 with a dedicated queue, and
compute families were `[0,1]`. The forced strict failure exited nonzero with
an explicit diagnostic and did not promote Vulkan. The complete raw hardware
profile and JSON evidence remain local to the validation environment; no local
SDK, VTK, cache, or profile path is tracked.

| Gate | Result |
|---|---|
| No-SDK contract | Standalone diagnostic probe: PASS; exit 0; `status=disabled`, empty devices, and `source=build-time-fallback`. |
| CPU control plane | Focused CTest: PASS 4/4 in 1.35 s. |
| Strict success | Intel Arc strict child: PASS; exact FP32 contract and matching deployment profile identity. |
| Strict negative path | Forced strict failure: PASS; parent exited nonzero, diagnostic retained, and fixture cleanup/adoption coverage passed. |
| Scope boundary | Only PD4 acceptance documentation changed; production routing, Level Set, global CMake, and local environment paths remain untouched. |


- Status: `DONE` — accepted on Intel Arc; claim released
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

- Status: `DONE` — accepted on Intel Arc; claim released
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
| Build | Fresh task-local Visual Studio Debug build compiled the new five-stage execution fixture and its three companion PD3 smoke targets with `cmake --build .build-pd3-session-compose --config Debug --parallel 8`. The build reused validated local dependency inputs only through ignored CMake cache settings. |
| Focused CTest | `ctest --test-dir .build-pd3-session-compose -C Debug --output-on-failure -R 'viennaps-vulkan-levelset-(surface-composition|surface-composition-execution|process-controller|process-controller-execution)-smoke'` passed 4/4 in 2.21 s. |
| Five-stage execution | On Intel Arc, the execution fixture reported `five callbacks share one generation/device with CPU oracle PASS`: coverage and diffusion raw-bit oracles, neutral velocity raw-bit CPU/GPU oracle, and CPU/Vulkan Level Set advection comparison all passed. It asserted a nonzero common generation and identical device identity for surface, update, and rebuild. |
| Lifetime and policy matrix | The fixture invokes copied coverage, diffusion, neutral, update, and rebuild callbacks after `clear(process)`. It also verifies valid mixed Manual Vulkan-surface/Manual-CPU-Level-Set selection, atomic AUTO degradation with a missing surface shader, explicit Manual Vulkan failure with no silent CPU callback set, and Manual CPU success without a context or GPU callbacks. |
| Companion regressions | The API smoke and both Level Set controller smokes passed. The controller execution fixture also exercised its existing strict callback failure paths and completed successfully. |
| Scope boundary | Only the narrow composition owner/execution fixture, Process binding/CMake artifact wiring required for the exact full plan and neutral callback retention, and this status record changed. Probe/profile policy, CUDA, shaders, global CMake, fixed local paths, and PD4-owned files remain untouched. |
