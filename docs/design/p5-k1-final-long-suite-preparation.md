# P5-K1 final long-suite execution preparation

## Card identity and current position

- **Card ID:** `P5-K1-TOP-LEVEL`.
- **Naming rule:** this is the final P5 long-suite gate named `P5-K1` in the
  formal-exit plan. It is distinct from the earlier, accepted `P5-K1`
  strict-FP32 evidence-profile/policy seam in the Vulkan status report. All
  new closeout records use the fully qualified `P5-K1-TOP-LEVEL` ID.
- **State:** `READY-S / PREPARED`; no K1 configure, build, executable, or
  CTest command has run, and this document does not claim K1 acceptance.
- **Predecessors:** `P5-S1-SURFACE-INTEGRATION-GREEN` (`de7c568`) and
  `P5-M0-MODEL-MATRIX-AGGREGATE` (`5768825`) are locally accepted on the
  candidate branch. `P5-K0` remains diagnostic-only.
- **Candidate boundary:** begin from the committed `5768825` candidate (or a
  later explicitly recorded replacement) in one verified worktree. The root
  worktree's generated/untracked evidence remains user-owned and must neither
  enter the candidate nor be removed by this card.
- **Next unlock:** local completion supplies the top-level test receipt for
  `P5-E0`; it does not substitute for PD5 remote CI, install/export, or a
  release claim.

### Global position (approximately 170 tokens)

P5's narrowed surface and model-matrix paths are implemented and locally
accepted: the current committed candidate contains the S1
`NeutralTransport<float, 2>` frontier/rebuild fixes and M0's 15-row alignment.
The last top-level attempt, K0, discovered 94 tests and passed only
`backendPolicy`; it then stopped because `CSVFileProcess` had not been built.
That is incomplete build coverage, not a product-test failure. The final K1
gate must create a fresh candidate configuration, build the aggregate test
target without parallelism, capture the resulting inventory, run the focused
CPU/Vulkan regression cluster, then execute the final non-benchmark CTest
inventory serially. No source or model expansion belongs to K1.

## Source-first implementation and dependency check

The preparation was derived from the current source graph rather than probe
results.

| Surface | Current source fact | K1 consequence |
|---|---|---|
| Test graph | Root `CMakeLists.txt` adds `tests/` when `VIENNAPS_BUILD_TESTS=ON`; `tests/CMakeLists.txt` creates `ViennaPS_Tests` and adds every test subdirectory except the two standalone install/export consumers. | Build `ViennaPS_Tests` in the fresh CPU candidate before interpreting any CTest result. The K0 count of 94 is historical, not an expected K1 count. |
| P5 model boundary | `VulkanRayFluxEngine::checkInput` admits only the documented FP32 2D slices: zero-reflection `SingleParticleProcess`, its bounded one-reflection frontier extension, and bounded `NeutralTransport`; unsupported rows remain CPU/fail-closed. | K1 verifies the already accepted boundary. It must not add a broad Vulkan switch, generic multi-bounce physics, or automatic Process routing. |
| ViennaHRLE semantics | ViennaHRLE's source defines `isInDomain` with a max-index-exclusive bound, while `isOutsideOfDomain` permits the reflective max-index plane and rejects the periodic one. The S1 reconstruction code calls the latter. | Keep `hrleSparseReconstruction` in the focused CPU cluster; do not revert the S1 predicate by reasoning from a probe-only symptom. |
| ViennaTools provenance | CMake resolves ViennaCore 2.2.1 through CPM and applies the P5-N1 KD-tree patch plus a content-addressed cache key unless `CPM_ViennaCore_SOURCE` overrides it. ViennaLS 5.8.5, ViennaHRLE, ViennaRay 4.3.1, and ViennaCS are likewise source dependencies. | Record only version/key/commit evidence, not machine paths. A local source override is not an accepted K1 dependency substitute unless its content and patch equivalence are separately proven. |

The currently inspected cache provenance is ViennaCore's patched
`v2.2.1-kdtree-nullcheck-*` source, ViennaLS's patched v5.8.5 source,
ViennaHRLE 1.1.2, ViennaRay 4.3.1, and ViennaCS 2.0.1. These are preparation
inputs only; K1 must re-record the actual sources selected by its fresh CMake
configuration.

## Ownership and prohibited scope

| Item | Definition |
|---|---|
| Owns | One final-candidate worktree, serial validation-lane receipt, fresh source/configuration provenance, test inventory, focused-cluster results, complete non-benchmark suite result, first-failure/timeout/flaky classification, descendant-process audit, and K1 record updates. |
| Does not own | P5 surface/model semantics, CPU formulas/order, ViennaTools source edits, dependency upgrades, new Vulkan routes, support-row expansion, CI publication, install/export work, cleanup of existing `.tmp_*` or `.claude/` content, or release completion. |
| Validation mutex | The main line acquires the one global local-validation lane. No CMake, compiler, CTest, reference, Vulkan executable, or unrelated CPU/GPU workload may overlap K1. |
| Handoff boundary | A clean local K1 receipt unlocks only its formal-exit input. Any product assertion failure moves to `CHECKPOINT`; remediation is a newly scoped card, not an implicit K1 edit. |

## Ordered execution contract

No command in this section has been executed by preparation.

1. **Freeze the candidate.** Verify the worktree using `git worktree list`,
   record `git rev-parse HEAD`, verify no tracked diff belongs to K1, and keep
   pre-existing untracked evidence untouched. Check that the selected CMake
   source provenance does not silently bypass the patched ViennaCore path.
2. **Configure and build the CPU top-level candidate.** In an MSVC developer
   environment, use the Windows clean-environment wrapper and a uniquely
   named card-local build directory. The caller provides SDK/cache variables;
   they are never written into versioned evidence.

   ```powershell
   pwsh -File cmake/invoke-cmake-clean-env.ps1 -- `
     -S . -B .tmp_p5_k1_cpu_<date> `
     -DVIENNAPS_BUILD_TESTS=ON `
     -DVIENNAPS_USE_VTK=OFF -DVIENNAPS_VTK_RENDERING=OFF `
     -DVIENNAPS_ENABLE_VULKAN=OFF `
     -DVIENNAPS_BUILD_VULKAN_PROBE=OFF `
     -DVIENNAPS_BUILD_VULKAN_SMOKE=OFF

   pwsh -File cmake/invoke-cmake-clean-env.ps1 -- `
     --build .tmp_p5_k1_cpu_<date> --config Release --target ViennaPS_Tests
   ```

   Confirm process quiescence after each command. The aggregate target is the
   missing K0 coverage boundary; building only `backendPolicy` is insufficient.
3. **Capture the fresh test contract.** Run `ctest -N` with `-C Release` and
   retain its inventory outside versioned records. Compare it with K0's 94
   tests only to explain additions/removals; do not fail merely because the
   count changed after S1/M0.
4. **Run the focused cluster serially.** Each command uses an anchored regular
   expression and is followed by a process audit: `backendPolicy`,
   `CSVFileProcess`, `hrleSparseReconstruction`, and
   `surface_process_acceptance_cpu` in the CPU tree. In a separate fresh
   Vulkan Release tree with the required focused smoke options enabled, run
   `viennaps-vulkan-surface-process-acceptance-smoke` and
   `viennaps-vulkan-model-matrix-fallback-smoke`. The Vulkan pair confirms the
   accepted S1/M0 wiring; it is not a replacement for the CPU suite or a new
   hardware-promotion claim.
5. **Run the final CPU suite serially.** Only after the cluster is green,
   execute the fresh non-benchmark inventory with one CTest worker:

   ```powershell
   ctest --test-dir .tmp_p5_k1_cpu_<date> -C Release `
     --output-on-failure --timeout 300 --parallel 1 `
     -E "Benchmark|Performance"
   ```

   The generated inventory defines the exact scope. If a test is intentionally
   excluded by the expression or project registration, record that reason.
6. **Classify and hand off.** On first failure, timeout, missing executable,
   source-provenance mismatch, or remaining child process, stop at
   `CHECKPOINT`, retain the first-failure receipt, and make no speculative
   product edit. One retry is allowed only with a named changed condition and
   a new evidence target. A clean run requires an explicit descendant audit
   after CTest and before the validation lane is released.

## Acceptance evidence to record

| Exit | Required receipt |
|---|---|
| Candidate identity | commit, worktree verification, clean K1-owned tracked diff, CMake generator/configuration, and selected ViennaTools versions/cache keys; no absolute paths in versioned docs |
| Build coverage | `ViennaPS_Tests` Release build result and the fresh CTest inventory |
| Focused regressions | four CPU entries plus the two named S1/M0 Vulkan smoke entries, each with command, result, duration, and device/driver identity for the Vulkan entries |
| Long suite | serial non-benchmark CTest command, pass count, excluded-test rationale, and first-failure/timeout/flaky classification if not clean |
| Process hygiene | post-command audit showing no compiler, CTest, Vulkan, or child process remains |
| Scope audit | no production source/model/route/dependency changes and no deletion of user-owned generated evidence |

`DONE-LOCAL` is permitted only when every required receipt is present. It still
does not claim remote CI, cross-vendor coverage, deployment exit, or P5
completion.


## Execution receipt (2026-08-21, main line)

**Candidate identity.** Root worktree of `codex/p5-closeout-base` at commit
`5768825` (`P5-M0-MODEL-MATRIX-AGGREGATE`). Tracked diff outside records:
only the scoped remediations listed below. Pre-existing untracked evidence
untouched. Configure selected the patched CPM sources directly from the
content-addressed cache: ViennaCore `v2.2.1-kdtree-nullcheck-31f6423c177a320d`,
ViennaLS `v5.8.5-levelset-update-81bfc8a5bec3ee70`, plus the pinned
ViennaHRLE/ViennaRay/ViennaCS cache entries; no source override variables were
set.

**Configuration and build.** Visual Studio 17 2022 generator, Release,
card-local `.tmp_p5_k1_cpu_20260821` directory, tests on, VTK off, Vulkan off.
Under named retry condition R1 the configuration added
`VIENNAPS_STATIC_BUILD=ON`: the differential runner recipe links
`viennaps.lib`/`viennals.lib`, which the initial header-only configuration did
not produce. Serial `ViennaPS_Tests` build exited 0 (60 executables). Fresh
CTest inventory: **95 tests** (K0 saw 94; S1/M0 registrations explain the
difference).

**Focused regressions.**

| Entry | Result |
|---|---|
| `backendPolicy` | PASS (0.03 s) |
| `CSVFileProcess` | PASS (0.73 s) — closes the K0 missing-target boundary |
| `hrleSparseReconstruction` | PASS |
| `surface_process_acceptance_cpu` | PASS |
| Vulkan surface-process-acceptance smoke | PASS x3 |
| Vulkan model-matrix-fallback smoke | PASS x2 after scoped fix |

Vulkan device identity: Intel Arc integrated graphics, driver 101.8974, API
1.4.356 — same hardware class as the accepted S1/M0 evidence.

**Scoped remediations (test infrastructure only, no product semantics).**

1. *Model-matrix runtime DLLs.* The smoke links ViennaPS but lacked the
   POST_BUILD runtime-DLL/TBB copy its sibling targets use, so a fresh tree
   launched without `embree4.dll`/`tbb12.dll` and timed out behind the loader
   dialog. Wired identically to the sibling pattern
   (`gpu/vulkan/ray/CMakeLists.txt`). Residual gap: five further ray smokes
   linking ViennaPS share the latent gap; not exercised by K1's focused pair.
2. *Runner consolidation.* The fifteen reference-differential runners
   hard-coded a historical root build tree and could not run in any compliant
   card-local directory. Replaced by one shared engine
   (`tests/common/run_reference_oracle.ps1`) plus thin per-test wrappers that
   preserve file identity, parameter surface, and recorded manual invocations;
   `-BuildDirectory` is supplied by all fifteen CTest registrations. The
   engine normalizes to the N2-recorded Release flag set including `/Zi`.
   The oxide-regrowth wrapper additionally points the REFERENCE-side include
   path at the reference snapshot's own ViennaCS headers (v2.1.2): its fixture
   uses the embedded-boundary DenseCellSet API that the Mod tree's pinned
   v2.0.1 does not provide — a pre-existing mismatch, now explicit. The
   multibounce-frontier card regenerated its documented generated asset: an
   instrumented reference ViennaRay tree restored from the Mod cache copy at
   the script's default lookup location (gitignored).
3. Result: the whole 15-row differential family went from 0/15 to **15/15
   PASS** inside the K1 tree.

**Final long suite.** `ctest --parallel 1 --timeout 900 -E "Benchmark|Performance"`
on the fresh inventory: **93/94 PASS in 464.51 s**. Excluded by expression:
`translationFieldBenchmark` (Benchmark naming rule).

**First-failure classification — `vulkanCpuBaseline` (RED, retained).**
Exit code 0xC0000409 within seconds. Diagnosis with a caller-built
instrumented copy of the unchanged test source: `VC_TEST_ASSERT(lineCount > 0)`
throws `std::runtime_error`, `main` has no handler, and the CRT converts the
uncaught exception into a fail-fast. It is therefore not memory corruption and
not in the MSVC codegen-defect family. Determinism holds — runs A/B publish
equal fingerprints (`0x9202b6f6e4e481aa`) — but the CPU_DISK trench scenario's
final disk mesh carries 17 nodes and zero lines, violating the structural
expectation authored on 2026-08-01. PD3 already recorded this test as a known
outlier and excluded it explicitly rather than counting it as pass. A separate
remediation card must decide between stale-test expectations and an intended
mesh-publication change before `DONE-LOCAL`.

**Process hygiene and scope.** Quiescence audited after every command; the
final audit found no compiler, CTest, Vulkan, or child processes. No product
source, model, route, or dependency change was made; no user-owned generated
evidence was deleted. Temporary evidence directories remain gitignored pending
the P5-E0 cleanup wave.

## R2 adjudication and clean closeout (2026-08-21)

`P5-K1-R2-VULKAN-CPU-BASELINE` resolved the classified RED at the source:

- Root cause: the expectation was authored against a mesh contract the pinned
  dependency never implemented. ViennaLS 5.8.5 `ToDiskMesh` (byte-identical
  between the reference tree and the patched Mod cache) publishes a point
  cloud — nodes, one vertex per interface point, and cell data — and contains
  no line or triangle element writers.
- Fix (test-only): `vulkanCpuBaseline` now asserts the actual disk-mesh
  contract (`vertexCount == nodeCount`, zero line/triangle elements) while
  keeping the full determinism battery, and `main` catches assertion throws
  and prints the failing condition instead of terminating through an opaque
  fail-fast code.
- Result: `vulkanCpuBaseline` PASS in 0.49 s.

**Final clean suite.** Full serial non-benchmark rerun on the same candidate:
**94/94 PASS in 700.93 s**, no failures, no timeouts. The focused Vulkan pair
re-passed 2/2 on the identical wiring. Descendant audit clean.

`P5-K1-TOP-LEVEL` is `DONE-LOCAL`. This supplies the top-level test receipt
for `P5-E0`; it does not substitute for remote CI, install/export, or release
claims.
