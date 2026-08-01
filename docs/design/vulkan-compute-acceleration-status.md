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

## Next slice

S2 delivers the reusable Vulkan runtime, versioned profile ingestion and cache
invalidation, calibrated memory budget, and the primitive differential suite.
It unlocks the first Vulkan implementation of the frozen two-dimensional Level
Set scenario.
