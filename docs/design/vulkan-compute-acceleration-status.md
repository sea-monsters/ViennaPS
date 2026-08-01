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

## Next slice

The next exit is a dedicated ViennaLS stage-executor seam between
`computeRates()` and `updateLevelSet()/rebuildLS()`. The existing velocity
callback fires after a Forward Euler update and cannot replace this phase; the
new hook must expose a bounded rate view, return handled/fallback/error, and
leave the unchanged CPU path as the fail-closed default. The accepted deployment
context supplies its selected backend and shared session to that hook. HRLE
active-run classification and sparse rebuild follow after the integration seam
is proven against the frozen 92-point oracle.
