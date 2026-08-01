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
| CPU Level Set FP32 | 96 active points, 68 surface nodes/lines, fingerprint `0xd3a34f98ceafe71b` |
| CPU Level Set FP64 | 96 active points, 68 surface nodes/lines, fingerprint `0x39e664622bea5583` |
| Local VTK source override | root configure recognizes the local source and ViennaLS reuses its targets |

The machine-local build uses environment variables, for example:

```bat
set VULKAN_SDK=<local Vulkan SDK>
set VIENNAPS_VTK_SOURCE_DIR=<local VTK source tree>
cmake -S . -B build -G Ninja -DVIENNAPS_ENABLE_VULKAN=ON
```

These values live only in the process environment and ignored CMake cache.
The same rule applies to external comparison trees used during development:
automation may read them through environment variables such as
`VIENNAPS_MPROCESS_SOURCE_DIR`, but neither their resolved value nor a
machine-local fallback path may enter tracked CMake, presets, tests, or docs.

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

## Next slice

The next S2B slice removes the current 256-block reduction/scan bound. P3 can
then implement the first Vulkan Level Set step against the frozen oracle above.
Higher-level process integration remains behind the ViennaCS dependency gate.
