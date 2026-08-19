# P5 Wet-Etch Velocity Compute Substage

Status: `DONE-LOCAL-NARROW-COMPUTE` (`P5-WETETCH-VELOCITY-SUBSTAGE`,
2026-08-18). The isolated backend-neutral executor, Vulkan kernel and paired
CPU oracle are accepted below; this remains a compute-only artifact. The
follow-on narrow production model adapter is documented in
[`p5-wetetch-process-adapter.md`](p5-wetetch-process-adapter.md).

## Card brief

**Milestone.** Define the smallest Vulkan compute substitution for the
`WetEtchingVelocityField::getScalarVelocity` arithmetic while ViennaPS process
ordering, material mapping, level-set ownership, and advection remain CPU
authoritative.

**Predecessor.** [`P5-MODEL-MATRIX-COMPUTE-MAP`](p5-model-matrix-compute-map.md)
rank 11, [the Vulkan program intent framework](vulkan-program-intent-framework.md)
§2, and the existing Level Set/session evidence recorded in the
[P5 status board](vulkan-compute-acceleration-status.md). The predecessor
identified wet-etch velocity as an isolated candidate; this card now freezes
and exercises the smallest legal seam for that candidate.

**Next unlock.** The separately carded
[`P5-WETETCH-PROCESS-ADAPTER`](p5-wetetch-process-adapter.md) consumes this
seam for one explicit `float,D=2` analytic row. That adapter does not widen the
predicate or unlock the aggregate matrix or deployment exit.

**Global position (about 160 words).** P5 may replace only a proved numeric
operation. The unmodified `D:\Codex_lib\code_reference\ViennaPS` wet-etch
velocity field remains the semantic authority for crystal-axis construction,
normal handling, material masks, rates, units, and zero-velocity branches.
Vulkan may evaluate a bounded batch of already selected surface samples, then
return a fully validated candidate vector to the host. `Process`,
`FluxProcessStrategy`, `TranslationField`, `viennals::Advect`, callbacks,
time-step selection, mesh/material ownership, and publication remain on the
CPU. A device result is staged behind a sentinel and committed only after
session-generation, finite-value, count, raw-bit/tolerance, and geometry
checks. Auto uses the CPU pointwise field for the same request after any
selection, dispatch, status, or validation failure. Manual Vulkan fails closed
without mutating velocity, level-set, metadata, or process state. CUDA model
classes, CPU fallback configuration, and a successful neutral executor are not
Vulkan wet-etch evidence.

## 1. CPU authority and exact call anchors

The reference implementation is
`D:\Codex_lib\code_reference\ViennaPS\include\viennaps\models\psWetEtching.hpp`:

- `impl::WetEtchingVelocityField` and its constructor are lines `14-45`.
- `getScalarVelocity` is lines `47-88`: material match, normal-norm guard,
  D-dimensional normal projection, normalization, absolute crystal-direction
  dot products, descending sort, the two piecewise rate formulas, and material
  multiplier.
- `WetEtching::initialize` constructs the default `SurfaceModel` and velocity
  field at lines `120-131`; parameters and material-rate metadata are recorded
  at lines `133-145`.

The Mod copy is semantically the same at
`include/viennaps/models/psWetEtching.hpp:14-88,120-145` and is not to be
changed by this design card.

The generic Process path does not expose a velocity executor:

- `VelocityField::getScalarVelocity` and `prepare` are
  `include/viennaps/process/psVelocityField.hpp:9-38`.
- `TranslationField` calls the model field pointwise at
  `include/viennaps/process/psTranslationField.hpp:32-60`.
- `FluxProcessStrategy::calculateVelocities` delegates only to
  `SurfaceModel::calculateVelocities` at
  `include/viennaps/process/psFluxProcessStrategy.hpp:406-414`; wet etch uses
  the model velocity field, not the neutral surface executor.
- The authoritative advection handoff is
  `include/viennaps/process/psFluxProcessStrategy.hpp:340-343`, followed by
  callbacks/advection at `345-377`.

The existing Vulkan Level Set controller only installs update/rebuild
executors (`gpu/vulkan/levelset/levelset_process_controller.hpp:60-72` and
the executor wiring at `:260-330`); it cannot substitute a model velocity
field. The surface binding accepts only existing stage families and a retained
neutral model (`gpu/vulkan/surface/process_deployment_binding.cpp:27-38,133-148`),
and its callback installation is neutral-specific (`:217-225`).

**Resolution for this narrow card.** The backend-neutral seam is now a
caller-owned batch callback in
`include/viennaps/models/psWetEtchingVelocityExecutor.hpp`. It is deliberately
not installed by `TranslationField` or `Process`; those CPU paths remain the
only publication authority. Direct Vulkan calls from `TranslationField`, a
second Process loop, and reuse of the neutral executor remain prohibited.

## 2. Implemented narrow seam (compute-only)

The seam is a backend-neutral, caller-owned batch executor, analogous in
transaction shape to `NeutralTransportVelocityWork` but not interchangeable
with it. The frozen header defines `WetEtchVelocityWork<float>` with spans for
coordinates, normals, `int32` material IDs, a caller-provided CPU oracle and
output, plus the crystal-axis/rate parameters and completion counters. The
`WetEtchVelocityExecutor` is a `std::function` callback, so the model/process
layer does not depend on Vulkan. The Vulkan bridge is an opt-in implementation
of that callback; it keeps its own session generation and stages output until
validation succeeds.

The current implementation fixes an FP32, D=2 arithmetic fixture and a
bounded material-rate table. Coordinates are retained in the wire buffers for
stable batch shape even though the scalar formula uses the normal and material
ID. The caller owns point IDs, mesh/normal creation, and the CPU oracle.

Required ABI rules:

1. Fixed-width FP32 records for the admitted row; explicit point count and
   byte-size overflow checks; non-aliasing input/output buffers.
2. Directions are the constructor inputs before orthogonalization. The device
   operation must reproduce `Normalize`, Gram-Schmidt subtraction, cross
   product, absolute dot products, descending sort, branch predicate, and
   material multiplier. It may not read arbitrary callbacks or global model
   state.
3. Material IDs are host-resolved IDs with a bounded table. Unknown or
   non-etching materials produce the CPU zero result; malformed IDs fail closed
   before dispatch.
4. The host retains point IDs, surface/mesh ownership, normal-generation
   policy, and the CPU fallback vector. The executor returns only a complete
   candidate vector and typed status.

No `Stage::WET_ETCH_VELOCITY` enum, deployment profile key, or Process binding
is authorized by this document. The executor is an opt-in numeric primitive
only; stage policy and deployment binding require a new card after complete
WetEtching Process evidence exists.

## 3. Retained CPU semantics and transaction boundary

CPU must continue to own:

- `WetEtching` parameter/material construction and metadata;
- material-map translation in `TranslationField`;
- point/normal generation, point IDs, and level-set topology;
- process-time and time-step selection;
- `SurfaceModel`/Process callbacks, advection ordering, and all publication;
- NaN/normal-length policy (`abs(Norm(nv)-1) > 1e-4` returns zero before
  normalization), D=2 projection (`normal.z = 0`), and exact zero branches.

The host caller first computes the CPU vector (or retains a reproducible staged
CPU oracle), then invokes Vulkan only for a bounded candidate batch. The
current bridge checks its captured session generation before dispatch and
publication, validates finite/count/completion and material-table invariants,
and accepts only candidates within 32 ULP of the caller oracle. The production
WetEtching adapter now invokes that callback from the model field after the
CPU scalar has been computed; the existing `VelocityField::prepare` and
`TranslationField` interfaces remain unchanged. Its full contract and CPU
Process oracle are in the follow-on card linked above.

On any malformed input, unknown material, NaN/Inf, subnormal, overflow,
timeout, device loss/reset, stale generation, non-zero status, count mismatch,
or numerical mismatch: discard device buffers and preserve the CPU vector and
all caller sentinels. No partial velocity, mesh, metadata, process-time, or
Level Set publication is permitted.

## 4. Auto, Manual, and support boundary

The future row predicate must be explicit and narrow: FP32, one concrete
`WetEtching<float, D>` model, bounded D=2 or D=3 fixture selected by the card,
finite direction/rate/material tables, Forward Euler unless separately
proved, and a prepared Vulkan session/profile. This is a compute substage
predicate, not complete WetEtching support.

- **Auto:** any predicate, profile, session, executor, dispatch, status, or
  differential failure executes the unchanged CPU velocity path for the same
  request and continues the existing Process transaction.
- **Manual Vulkan:** unsupported input or any failure returns a non-throwing
  explicit failure and publishes no candidate or geometry state.
- **Manual CPU:** bypasses the executor and remains the direct CPU path.

No CUDA `getGPUModel()`, CUDA callable table, or neutral-transport callback may
make this row eligible. The compute-only bridge and explicit model setter do
not change generic deployment policy: callers that do not install the executor
remain on CPU, and unsupported/manual deployment paths remain fail-closed. The
narrow adapter row is accepted separately and does not imply automatic stage
promotion.

## 5. Oracle and acceptance contract

The implementation card must provide an independently frozen CPU oracle from
the reference tree, not a copied formula used as both sides. It must include:

1. 2-D and 3-D crystal-axis fixtures, including canonical 100/110/111/311
   normals, non-unit normals, D=2 z-components, and both piecewise branches.
2. Etching and non-etching material IDs, duplicate/material-map boundaries,
   zero and negative-rate validation, and finite-rate extremes.
3. Per-point scalar velocity raw-bit comparison for the strict domain; a named
   ULP/absolute tolerance manifest only where the admitted domain requires it.
4. Conservation/physics checks appropriate to this substage: no invented
   material flux, exact zero for masked materials, finite bounded velocity, and
   consistency of total CPU/device velocity sums under the named tolerance.
5. Full CPU `Process::apply()` geometry differential after the candidate is
   staged: level-set width/topology/material IDs, process time, callback order,
   and advection result. A velocity-only PASS is not WetEtching support.

The oracle must also exercise transaction negatives: malformed count/stride,
unknown material, NaN/Inf/subnormal input, stale generation/session loss,
non-zero device status, timeout, and candidate mismatch. Every case compares
bytewise velocity/mesh/metadata/Level Set sentinels before and after failure.

## 6. Strong implementation card and ordered exits

**Milestone.** Add one backend-neutral WetEtch velocity executor seam and one
Vulkan implementation for a single explicitly admitted FP32 fixture. This
milestone is now accepted as a compute-only substage; the Process adapter is a
separate future card.

**Predecessor evidence.** This design, the reference CPU anchors above,
[`P5-MODEL-MATRIX-COMPUTE-MAP`](p5-model-matrix-compute-map.md), and the
existing Level Set/session smokes.

**Owned files/interfaces.** This substage owns the new backend-neutral
executor header, its Vulkan bridge/shader, focused smoke and paired CPU fixture
under `tests/wetetchVelocityReferenceDifferential/`. It does not own a
Process/velocity-field adapter and does not edit the reference tree.

**Prohibited.** No current production edits, CMake, stage/profile expansion,
WetEtching model-row promotion, CUDA reuse, CPU formula copy as production
oracle, callback/RNG migration, second Process loop, or Level Set ownership
change.

**Ordered exits:**

1. Freeze the admitted dimension/model/predicate, work shape, caps,
   generation rule, and independent reference CPU oracle. **Passed** for the
   bounded FP32/D=2 fixture; no Process eligibility was widened.
2. Add the backend-neutral executor seam and deterministic transaction
   negatives. **Passed**: malformed material, NaN normal, reset and stale
   session preserve sentinels; the CPU formula remains in the model/reference
   path.
3. Implement the Vulkan arithmetic kernel and bridge. **Passed**: SPIR-V
   validation, Release build and Intel Arc execution report `maxUlp=1` with
   malformed/reset/stale-session sentinels.
4. Integrate through the existing velocity preparation boundary and run full
   CPU Process/geometry/conservation/callback differential. **Moved to and
   passed by** the follow-on Process adapter card for the strict D=2 analytic
   fixture.
5. Verify Auto CPU fallback and Manual no-publication for a Process route.
   **The adapter's callback failure path is exact CPU fallback; generic
   deployment Auto/Manual policy remains unchanged.**
6. Stop. A later model-row card may consume this substage evidence only after
   complete WetEtching Process acceptance; aggregate matrix/deployment exit
   remain locked.

## 7. Residual claim

The repository has a legal, narrow WetEtch numeric seam and validated Vulkan
arithmetic implementation. This compute-only card does **not** claim a global
stage, CUDA parity, aggregate model promotion, or deployment promotion. The
separately carded production adapter proves one explicit D=2 analytic model
row while `TranslationField`, `FluxProcessStrategy`, model construction, and
Level Set advection remain CPU-owned. The Neutral Release oracle remains a
separate blocker for complete surface integration.

## 8. Narrow implementation evidence

- Backend-neutral work and callback:
  `include/viennaps/models/psWetEtchingVelocityExecutor.hpp`.
- Vulkan bridge and smoke:
  `gpu/vulkan/levelset/wetetch_velocity_executor.{hpp,cpp}` and
  `gpu/vulkan/levelset/wetetch_velocity_executor_smoke.cpp`.
- Arithmetic shader: `gpu/vulkan/levelset/shaders/wetetch_velocity.comp`.
- Independent CPU pair: `tests/wetetchVelocityReferenceDifferential/`, compiled
  from the Mod and unmodified reference trees separately.
- `glslc` followed by `spirv-val --target-env vulkan1.2` passed. The clean
  Release target build passed, and the Intel Arc executable reported
  `wet-etch velocity Vulkan dispatch PASS (maxUlp=1, malformed/reset/stale-session sentinels PASS)`.
- Focused CTest passed `2/2`: the Vulkan executor smoke and the CPU reference
  differential. The paired CPU runner passed raw-exact at OMP 1/2/4/8 under
  `/O2 /Ob2 /DNDEBUG /openmp:llvm /MD`.
- The former test-only probe remains as a bounded seam diagnostic. The
  production adapter smoke
  `gpu/vulkan/levelset/wetetch_velocity_process_adapter_smoke.cpp` now invokes
  `WetEtching::setVelocityExecutor`, compares the complete CPU/Vulkan LevelSet
  result, and checks an exact failing-callback fallback. Release Intel Arc
  CTest passes with `maxGeometryUlp=0`, `calls=5`, `accepted=5`, and
  `fallbackExactUlp=0`.
- The paired Process oracle
  `tests/wetetchProcessReferenceDifferential/` passes raw-exact at OMP 1/2/4/8
  under the required Release flags. Residual: no deployment/profile binding,
  D=3 or broader WetEtching admission, aggregate matrix unlock, or complete
  surface integration while the Neutral Release oracle remains blocked.

Validation for this document includes the focused implementation evidence
above plus Markdown link resolution and scoped `git diff --check`.
