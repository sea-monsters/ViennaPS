# P5 WetEtch Process Adapter

Status: `DONE-LOCAL-NARROW` (`P5-WETETCH-PROCESS-ADAPTER`, 2026-08-18).
This card closes one explicit WetEtching `float, D=2` analytic Process row. It
does not promote the aggregate model matrix or install a global Vulkan stage.

## Card contract

**Milestone.** Consume the accepted numeric seam from
[`p5-wetetch-velocity-substage.md`](p5-wetetch-velocity-substage.md) through a
caller-owned executor on the production `WetEtching` model while preserving
the existing CPU `Process`/`AnalyticProcessStrategy`/`TranslationField` and
ViennaLS advection path.

**Predecessors.** The paired scalar Mod/reference oracle and Intel Arc
executor smoke in the compute substage, plus the existing LevelSet CPU/Vulkan
transaction tests.

**Global position (about 170 tokens).** This card admits only the numeric
crystal-velocity operation for `WetEtching<float,2>` with one Si material-rate
entry, finite constructor parameters, and the existing Forward-Euler analytic
Process. The unmodified CPU field evaluates every request first and remains
the semantic and rollback authority. An explicitly installed callback may
evaluate that same one-point candidate on Vulkan; its result is staged behind
the CPU value and is returned only when the bridge reports completion, finite
output, count agreement, session validity, and the independent CPU-oracle
check. Any callback failure, malformed input, stale/reset session, or numeric
mismatch returns the already computed CPU value without changing Process,
TranslationField, metadata, time-step, mesh, or LevelSet ownership. No stage
enum, deployment profile key, automatic promotion, 3-D admission, callback
reordering, or other model eligibility is implied.

## Implementation boundary

`include/viennaps/models/psWetEtching.hpp` now retains an optional
`WetEtchVelocityExecutor<NumericType>` and exposes:

- `setVelocityExecutor(...)` for an explicitly owned callback;
- `clearVelocityExecutor()` and `hasVelocityExecutor()` for lifecycle checks.

`impl::WetEtchingVelocityField::getScalarVelocity` still executes the original
CPU formula and material/normal branches. Only after that value exists does it
construct a one-point `WetEtchVelocityWork`; a successful complete callback
may replace the scalar. A failed callback returns the CPU scalar. The model
constructor remains CPU-only by default, and no `Process` or `TranslationField`
source was changed.

The focused production smoke is
[`wetetch_velocity_process_adapter_smoke.cpp`](../../gpu/vulkan/levelset/wetetch_velocity_process_adapter_smoke.cpp).
It runs three paths on the same plane fixture: CPU-only, a deliberately
failing callback (exact CPU fallback), and the real Vulkan executor. It checks
the complete flattened LevelSet, executor call/accept/reject counts, and raw
FP32 ULP distance.

## Acceptance evidence

1. The independent CPU Process fixture in
   [`tests/wetetchProcessReferenceDifferential/`](../../tests/wetetchProcessReferenceDifferential/)
   compiles Mod and unmodified `D:\Codex_lib\code_reference\ViennaPS`
   separately with `/O2 /Ob2 /DNDEBUG /openmp:llvm /MD`; OMP 1/2/4/8 all pass
   byte-identical LevelSet serialization (`raw_exact`).
2. Clean Release target build succeeds with
   `cmake/invoke-cmake-clean-env.ps1 --build build --config Release --target
   viennaps-vulkan-wetetch-velocity-process-adapter --parallel 2`.
3. The Intel Arc executable reports
   `wet-etch Process adapter PASS (maxGeometryUlp=0, calls=5, accepted=5,
   fallbackExactUlp=0)`. The failing callback path is exact CPU geometry and
   the real callback accepts all five scalar requests.
4. Focused CTest passes both
   `viennaps-vulkan-wetetch-velocity-process-adapter` and
   `wetetch-process-cpu-reference-differential`; the existing compute-only
   executor/probe and scalar CPU differential remain passing.

## Explicit residuals

This is a narrow opt-in model row, not deployment support. It does not add a
profile/stage selector, bind the callback through `ProcessDeploymentBinding`,
or change Auto/Manual backend policy. D=3, multiple materials/species,
non-analytic callbacks, other WetEtching rates, and any ray/surface physics
remain CPU fallback or unsupported until separately carded. NeutralTransport's
Release CPU oracle is still an independent evidence gap, so aggregate surface
integration, the remaining model rows, promotion, and deployment exit remain
locked.
