# P5 SelectiveEpitaxy Velocity Compute Substage

Status: `DONE-LOCAL-NARROW` (`P5-SELECTIVE-EPITAXY-VELOCITY-SUBSTAGE`,
2026-08-18). This closes one opt-in numeric velocity operation and its narrow
analytic Process adapter; it does not promote the SelectiveEpitaxy family or
the aggregate model matrix.

## Card brief

**Milestone.** Replace only the already selected `EpitaxyVelocityField` scalar
arithmetic with a validated Vulkan candidate while retaining CPU ownership of
mask construction, stencil preparation/finalization, Process ordering,
advection and Level Set publication.

**Predecessor.** [`P5-MODEL-MATRIX-COMPUTE-MAP`](p5-model-matrix-compute-map.md)
rank 12, the intent framework, and the existing Level Set/session contracts.
The predecessor identified this as a velocity-only candidate with no legal
Boolean/stencil seam.

**Next unlock.** The evidence may be consumed by the aggregate matrix review
as one explicit `SelectiveEpitaxy<float,2>` analytic row. It does not unlock
Neutral/surface integration, generic deployment binding, D=3, or release exit.

**Global position (about 160 words).** The reference CPU
`D:\Codex_lib\code_reference\ViennaPS` implementation remains authoritative
for material gating, orientation factors, the 2-D/3-D low bound, rate
interpolation, zero branches, and all domain transforms. The new callback is
optional and is reached only after the CPU field has computed the scalar
oracle for a selected point. Vulkan receives caller-owned coordinates,
normals, resolved built-in material IDs, the material-rate table and the CPU
oracle. It returns a candidate only after finite/count/status/session checks
and a bounded FP32 ULP comparison. Device buffers are staged behind output
sentinels; failures never publish a partial velocity. `SelectiveEpitaxy`
continues to build the mask, prepare/finalize ViennaLS stencils, choose time
steps, run `Process`/`AnalyticProcessStrategy`, and restore the original domain
on CPU. A failing callback therefore returns the exact CPU scalar. Manual or
future deployment code must not infer support from this callback; no stage or
profile key is added, and all unconfigured callers remain CPU-only.

## CPU authority and seam

The reference formula is
`D:\Codex_lib\code_reference\ViennaPS\include\viennaps\models\psSelectiveEpitaxy.hpp`:

- `impl::EpitaxyVelocityField` and its constructor: lines `12-31`;
- `getScalarVelocity`: lines `33-49` (material map lookup, weighted normal,
  `low/high` interpolation, negative velocity clamp and zero for other
  materials);
- `SelectiveEpitaxy::initialize/finalize`: lines `84-141` in the reference
  tree (mask BooleanOperations, stencil preparation and domain restoration).

The Mod CPU formula remains in
`include/viennaps/models/psSelectiveEpitaxy.hpp:39-98`; only an optional
candidate callback was added. The backend-neutral contract is
`include/viennaps/models/psSelectiveEpitaxyVelocityExecutor.hpp`. Its work
object is caller-owned and synchronous; it carries fixed-width spans,
parameters, the CPU oracle and transactional completion counters.

The Vulkan implementation is
`gpu/vulkan/levelset/selective_epitaxy_velocity_executor.{hpp,cpp}` with
`shaders/selective_epitaxy_velocity.comp`. It is deliberately not installed
into `TranslationField`, `FluxProcessStrategy`, the surface binding or a
deployment profile. The production opt-in setter is
`SelectiveEpitaxy::setVelocityExecutor`; no setter call means the unchanged
CPU path.

## Invariants and acceptance

- admitted numeric fixture: `float`, `D=2`, finite normals/coordinates, built-in
  material IDs, at most eight material-rate entries, and the model's default
  factors `{0.5,1,0}`;
- CPU computes first; Vulkan may only return a candidate within 32 ULP of that
  CPU value, otherwise the CPU scalar is retained;
- malformed ID, NaN/subnormal, span mismatch, overflow, reset or stale session
  fails closed and preserves the caller sentinel;
- `SelectiveEpitaxy` mask/stencil/domain semantics are not moved to the device;
- Auto-style callback failure is exact CPU fallback; no Manual Vulkan promotion
  or generic eligibility is implied.

The independent CPU fixture
`tests/selectiveEpitaxyProcessReferenceDifferential/` compiles the same
process source against Mod and unmodified reference headers under
`/O2 /Ob2 /DNDEBUG /openmp:llvm /MD`; OMP 1/2/4/8 all compare raw-exact.
The production adapter smoke compares CPU-only, deliberate callback-failure,
and Vulkan-backed `Process::apply()` Level Set snapshots. The executor smoke
also covers non-epitaxy zero output, malformed/NaN input, reset and external
session generation loss.

## Ordered exits and residual boundary

1. Freeze the CPU formula, admitted FP32/D=2 shape, table cap, status and
   sentinel rules — passed.
2. Add the backend-neutral seam and transactional Vulkan executor — passed.
3. Validate SPIR-V, Release build and Intel Arc execution — passed
   (`maxUlp=4`; all negative sentinels pass).
4. Exercise the unchanged Process/LevelSet route, exact CPU fallback and
   paired reference CPU serialization — passed (`maxGeometryUlp=0`, five
   accepted callback samples; OMP 1/2/4/8 raw-exact).
5. Stop at the narrow row. D=3, arbitrary/custom materials, broader
   deployment/profile policy, full model matrix, Neutral Release oracle and
   P5 deployment exit remain open.

No CUDA model, generic surface callback, second Process loop, Boolean/stencil
kernel, or altered reference CPU implementation is evidence for this row.
