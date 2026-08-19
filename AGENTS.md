# Repository Guidelines

## Project Structure & Module Organization

ViennaPS is a header-only C++20 process and topography simulation library.
Public headers live in `include/viennaps`; `lib/` contains optional precompiled
specializations, and `gpu/` contains GPU support. CMake integration is under
`cmake/`, runnable C++/Python examples are under `examples/`, and automated
tests are under `tests/`. Python bindings, stubs, scripts, and Python tests
are under `python/`. Assets used by documentation and examples are in
`assets/`; CI workflows are in `.github/workflows/`.

## Current Program State (2026-08-19)

The active program is the Vulkan compute acceleration plan. The authoritative
intent and evidence sources are:

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
`PD5-INSTALL-EXPORT` are locally accepted for the CPU/no-SDK boundary. Remote
CI is not accepted: the configured GitHub repository still has `master` as its
default branch; that branch has an older test-only `build.yml`, not the current
PD5 workflow, and no registered Vulkan runner or hosted run evidence exists.

The P5 device ray chain cards `P5-JD`, `P5-JE`, and `P5-JF` are locally
revalidated on the Release Vulkan/Intel Arc path. They provide device-resident
composition, one compute submission, and strict-FP32 fail-closed status
propagation. They do not enable a production Process route, complete surface
physics, automatic backend promotion, cross-vendor CI, or release readiness.
The approved closeout order is `P5-X0 -> P5-N1/N2 -> P5-S0/S1 -> P5-M0 ->
P5-E0`; only read-only K3E and deployment preparation may run independently.
The root worktree contains accumulated accepted and pending P5 changes. Freeze
them through the explicit `codex/p5-closeout-base` checkpoint before new
production work, exclude generated environments, and never convert local
implementation evidence into a project-wide completion claim.

During P5 closeout, local validation is globally serialized. Agents may
analyze or edit independently, but only one worktree may run CMake, builds,
CTest, reference emitters, or Vulkan executables at any moment. Other cards
remain at `CHECKPOINT` until the main line releases the validation lane.

## Build, Test, and Development Commands

Use an out-of-tree `build/` directory. Dependencies such as ViennaTools
components are fetched by CMake when needed.

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
