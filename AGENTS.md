# Repository Guidelines

## Project Structure & Module Organization

ViennaPS is a header-only C++20 process and topography simulation library.
Public headers live in `include/viennaps`; `lib/` contains optional precompiled
specializations, and `gpu/` contains GPU support. CMake integration is under
`cmake/`, runnable C++/Python examples are under `examples/`, and automated
tests are under `tests/`. Python bindings, stubs, scripts, and Python tests
are under `python/`. Assets used by documentation and examples are in
`assets/`; CI workflows are in `.github/workflows/`.

## Current Program State (2026-08-21)

**P5 is closed by user declaration.** All locally executable acceptance rows
were green beforehand (K1 serial suite 94/94; E0 adversarial audit with zero
VUID diagnostics under forced Khronos validation; VTK-enabled install/export
chain closed via standalone find_package consumption; X1 hygiene Tier A
executed). The hosted-CI evidence gate was explicitly waived in the same
decision — recorded on the board as an authority decision, never a silent
conversion. The reviewed snapshot is published at remote
`codex/p5-closeout-base`.

**Active phase: P7 local scope complete (2026-08-22).** R0-R3 are
DONE-LOCAL on `codex/p7-base`: resident working set with measured 4x
host-transfer reduction, deterministic selection records, calibration
records; release support matrix published in p6-p7-execution-plan.md §4.
The hosted soak portion of P7-R4 remains BLOCKED-EXTERNAL under the standing
deferral decision - runner provisioning is the sole remaining external
dependency of the overall program. Execution regime unchanged:
serialized validation lane, one verified worktree per card, card-local
`.tmp_*` directories, CPU-oracle-first evidence, main-line-only record
acceptance and pushes, and intent-framework invariants 1–14.

The authoritative intent and evidence sources are:

- `docs/design/vulkan-program-intent-framework.md` — full-program functional
  intent whitepaper and overall Vulkan migration design philosophy (original
  ViennaPS semantic baseline; mprocess GPU-embedding reference boundaries;
  drift-prevention contracts; read first);
- `docs/design/vulkan-compute-acceleration-development-report.md` — total
  P0--P7 plan and current execution snapshot;
- `docs/design/vulkan-compute-acceleration-status.md` — acceptance evidence,
  exclusive ownership boundaries, and the `P5 to P7` continuation board;
- `docs/design/p5-formal-exit-execution-plan.md` — the approved dependency,
  dispatch, snapshot, and formal-exit contract for the current P5 closeout.

The local control-plane milestones through `PD4-HW-MATRIX` and
`PD4-PERF-BASELINE` are accepted. `PD5-CI-DOCS-INTEGRATION` and
`PD5-INSTALL-EXPORT` are locally accepted for the CPU/no-SDK boundary, and the
VTK-enabled install/export variant closed locally on 2026-08-21. Hosted CI was
explicitly waived by the P5 completion declaration; the deferred hosted-lane
work resurfaces as a hard dependency of `P7-R4-CI-SOAK-RELEASE`.

Historical P5 narrative (retained for context): the device ray chain cards
`P5-JD/JE/JF` provided device-resident composition with strict-FP32
fail-closed propagation; `P5-N1/N2`, `P5-S0/S1`, and `P5-M0` were accepted;
`P5-K1-TOP-LEVEL` closed DONE-LOCAL (serial non-benchmark suite 94/94 after
runner consolidation, model-matrix runtime-DLL wiring, and the `P5-K1-R2`
adjudication of `vulkanCpuBaseline` — stale disk-mesh expectation, ViennaLS
5.8.5 `ToDiskMesh` is point-cloud-only); `P5-E0` completed its local
adversarial audit (CPU 94/94, Vulkan tree 132/132, zero VUID under forced
Khronos validation) after two root-cause fixes: ray-reducer push-constant
layout mirroring, and the runtime resource ledger enforcing
VUID-vkDestroyDevice-device-05137 without breaking stale-handle rejection.
Never convert local implementation evidence into a project-wide completion
claim beyond what the board records.

## Build, Test, and Development Commands

Use an out-of-tree `build/` directory. Dependencies such as ViennaTools
components are fetched by CMake when needed. ViennaCore v2.2.1 is patched
at CPM fetch time with
`cmake/patches/viennacore-v2.2.1-kdtree-traversedown-nullcheck.patch`
(MSVC 14.44 `traverseDown` null-check miscompilation workaround, adopted in
P5-N1/N2);
setting a local `CPM_ViennaCore_SOURCE` override bypasses the patch.

```bash
cmake -S . -B build -DVIENNAPS_BUILD_TESTS=ON
cmake --build build --parallel
ctest --test-dir build --output-on-failure -E "Benchmark|Performance"
```

On Windows hosted shells, run the same CMake arguments through
`pwsh -File cmake/invoke-cmake-clean-env.ps1 -- <cmake args>` to remove a
duplicate `PATH`/`Path` child environment that MSBuild rejects. Keep SDK and
cache locations in caller environment variables. Before parallel work, read
`docs/development-build-and-worktree-guide.md`; one claimed card must use one
verified Git worktree and must not create a root `build-*` directory. Use a
caller-selected `.tmp_<card>_<date>` directory for temporary validation and
remove it after confirming no compiler, test, or launcher process still uses
it. Do not delete `.claude/` or other pre-existing build/worktree artifacts
unless their exact ownership and scope have been verified.

For standalone Vulkan ray validation, configure and build inside a Visual
Studio developer environment so MSVC's standard-library include variables are
available:

```powershell
cmd /d /s /c 'call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\Tools\VsDevCmd.bat" -arch=x64 >nul && cmake -S gpu/vulkan -B .tmp_p5_ray_YYYYMMDD -G Ninja -DVIENNAPS_ENABLE_VULKAN=ON -DVIENNAPS_BUILD_VULKAN_RAY_SMOKE=ON -DBUILD_TESTING=ON -DCMAKE_BUILD_TYPE=Release'
```

The current standalone ray acceptance is five P5-JD/JE/JF-focused tests and a
12-test ray suite, both requiring a real Vulkan adapter; a CPU/no-SDK result
must not be described as hardware acceptance.

To build examples, add `-DVIENNAPS_BUILD_EXAMPLES=ON`. To build Python
bindings locally, use `python -m pip install .`; GPU builds require a matching
CUDA toolkit and driver. The CI formatting check is run with:
`cmake --build build --target format-check`.

## Coding Style & Naming Conventions

Follow the LLVM C++ coding guidelines and the repository’s `.clang-format`
configuration: two-space indentation, an 80-column preference, and no tabs.
Keep C++ source and example names in the existing lowerCamelCase style (for
example, `trenchOxidation.cpp`). Use descriptive `snake_case` names for Python
tests and scripts, such as `test_basic_functionality.py`. Preserve nearby
formatting and CMake conventions when editing build files.

## Testing Guidelines

CTest is the primary C++ test runner. Add C++ tests as focused subdirectories
under `tests/`, with a local `CMakeLists.txt`; Python tests belong under
`python/tests` or beside the relevant example/test fixture. Run the focused
test first, then the full non-benchmark CTest command above. No repository-wide
coverage threshold is currently documented.

## Commit & Pull Request Guidelines

Use short, imperative subjects (for example, `Fix trench oxidation boundary
handling`) and keep each commit focused. The current checkout has an active
mainline history; do not infer conventions from an empty repository. Pull
requests should explain the motivation, affected modules, configuration or
dependency changes, and exact validation commands; link an issue when
applicable and add before/after images or generated-output comparisons for
visual simulation changes. A remote CI claim requires a published commit plus
the run ID and URL; a local build alone is insufficient.
