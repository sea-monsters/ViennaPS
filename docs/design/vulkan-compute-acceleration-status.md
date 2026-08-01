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

- Adapt the probe JSON directly into `CapabilityProfileRecord`, add safe
  working-set calibration, and atomically persist the resulting profile.
- Replace the reduction and scan reference kernels described below with
  multi-workgroup implementations, then add compaction, sort, gather/scatter,
  histogram, and deterministic RNG.

## S2B-reference: primitive differential scaffold

- Status: accepted as a correctness scaffold, not production acceleration
- Date: 2026-08-01
- Scope: optional developer smoke target for the first five primitive
  contracts; this does not complete the S2B primitive suite

### Delivered contracts

- The parent and standalone Vulkan builds expose
  `VIENNAPS_BUILD_VULKAN_PRIMITIVES_SMOKE`. When the SDK is unavailable, CMake
  skips the target cleanly and keeps the diagnostic probe usable.
- Fill, copy, affine transform, sum/min/max reduction, and exclusive signed
  32-bit scan are compared against CPU results at lengths 0, 1, 16, and 257.
- The checks include output mismatch counts, floating-point absolute/relative/
  ULP error, and guard-region validation around every logical buffer.
- Fill, copy, and transform dispatch one invocation per element. Reduction and
  scan currently execute their CPU-equivalent loops in a single GPU invocation.
  They validate resource binding, dispatch, synchronization, mapping, edge
  cases, and comparison machinery, but they are deliberately not classified as
  GPU acceleration.

### Acceptance evidence

| Gate | Result |
|---|---|
| Parent Vulkan configure/build | primitive shader and smoke executable build with the SDK supplied through `VULKAN_SDK` |
| Differential cases | five operations at four lengths pass; zero mismatches and exact floating-point results |
| Guard regions | unchanged for all operations and lengths |
| No-SDK regression | primitive target is skipped; diagnostic probe remains buildable and exits 0 |
| Warning gate | primitive host target builds under MSVC C++20 `/W4` without code warnings |
| Fixed-path audit | source and documentation contain environment-variable names only, not machine-local SDK or library paths |

### Required promotion work

- Use a shared-memory first stage plus recursive partial reduction without
  requiring optional floating-point atomics.
- Use block-local scan, recursively scan block sums, then add block offsets;
  preserve the exact exclusive-scan and zero-length contracts.
- Run the same CPU differential matrix across multiple workgroup boundaries and
  randomized deterministic inputs before setting the corresponding persisted
  profile suite gates.
- Complete the remaining primitive list and safe memory-budget calibration.

## Next slice

S2B promotes reduction and scan to genuinely parallel implementations, adds
the remaining primitive contracts, and delivers calibrated probe-to-profile
persistence. It unlocks the first Vulkan implementation of the frozen
two-dimensional Level Set scenario.
