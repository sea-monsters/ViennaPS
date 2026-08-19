# P5 TEOS single-precursor velocity substage

Status: `DONE-LOCAL-NARROW` (2026-08-18).

## Global position (about 170 words)

P5 already accepts only compute substitutions whose CPU meaning is unchanged;
the strict ray row, WetEtching adapter, and SelectiveEpitaxy adapter establish
the transaction pattern used here. This card closes one low-coupling slice of
rank-9 `TEOSDeposition`: `float`, `D=2`, one `SingleTEOSParticle`, one
particle-flux label, nonnegative flux, finite deposition rate, and a
nonnegative reaction order. The production model evaluates the original
`depositionRate * std::pow(particleFlux, reactionOrder)` formula first, then
offers a caller-owned batch to an optional Vulkan executor. A failed callback,
malformed status, nonfinite value, reset, or stale session leaves the CPU
vector and Level Set publication untouched. Particle sampling, sticking,
coverage updates, precursor ordering, `FluxProcessStrategy`, advection, and
all multi-precursor semantics remain CPU authority. Owned files are the
backend-neutral work header, the single-precursor model bridge, one Vulkan
SPIR-V executor/shader, focused executor/Process smokes, and the paired CPU
fixture. No ray eligibility, deployment profile, stage binding, or aggregate
matrix promotion is implied. The next unlock is only a row-specific matrix
review; multi-precursor TEOS and TEOSPECVD remain fallback.

## CPU authority and seam

The reference implementation is
`D:\Codex_lib\code_reference\ViennaPS\include\viennaps\models\psTEOSDeposition.hpp`
(`SingleTEOSSurfaceModel::calculateVelocities`, lines 22--37). The Mod model
keeps that formula and calls the optional
[`TEOSVelocityExecutor`](../../include/viennaps/models/psTEOSVelocityExecutor.hpp)
only after the CPU oracle has been materialized. The executor receives flux,
parameters, CPU oracle, and caller-owned output spans; it has no Process or
surface-model ownership.

## Acceptance and ordered exits

1. Compile the shader with `glslc` and validate with `spirv-val --target-env
   vulkan1.2`.
2. Build the Release executor and Process adapter through the clean-environment
   wrapper.
3. On the Intel Arc adapter, require raw FP32 candidate comparison (at most 32
   ULP), malformed/NaN/reset/stale-session sentinels, and CPU fallback on a
   failed callback.
4. Run the independent Mod/reference CPU Process fixture at OMP 1/2/4/8 with
   identical Release flags and byte-exact serialized surface values.

The accepted result is a narrow numeric substage. It does not claim full TEOS
transport, multi-precursor coupling, TEOSPECVD, automatic promotion, or P5
completion.
