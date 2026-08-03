# Build and parallel-development guide

This guide is the shared entry point for developers working on Vulkan or other
multi-role changes. It records project-local rules only; SDK, compiler, VTK,
and cache locations remain environment-owned.

## Before taking a card

1. Read the parallel execution board in
   `docs/design/vulkan-compute-acceleration-status.md` and the nearest task
   card.
2. Claim a `RUN` row before editing. State the owner, exclusive files,
   predecessor, acceptance command, and CPU-oracle requirement.
3. Use one Git worktree per active implementation card. A worktree must be a
   real directory reported by `git worktree list`; a `prunable` entry is not a
   valid isolation boundary.
4. Keep build output inside that worktree (for example, `<worktree>/.build`)
   or in a uniquely named temporary directory. Do not create `build-*`
   directories in the repository root. Do not edit another card's files from
   the shared main worktree.

When moving a dirty card, first create and verify the target worktree, copy
the exact diff, verify it there, and only then remove the source copy. Never
delete a directory that appears in `git worktree list` merely because its name
looks like a build directory.

## Windows CMake entry point

Some hosted shells expose both `PATH` and `Path` with different values.
MSBuild can reject that environment before compiling any CPM dependency. Use
the checked-in wrapper for configure, build, and test-driving CMake on Windows:

```powershell
pwsh -File cmake/invoke-cmake-clean-env.ps1 -- `
  -S . -B .build -DVIENNAPS_BUILD_TESTS=ON

pwsh -File cmake/invoke-cmake-clean-env.ps1 -- `
  --build .build --config Debug --target <focused-target>
```

The wrapper creates a child process with one canonical `Path` entry. It does
not set or persist an SDK path; `VULKAN_SDK`, `CPM_SOURCE_CACHE`, compiler
setup, VTK overrides, and other site settings still come from the caller's
environment. On systems without the duplicate-variable problem, direct CMake
invocation remains valid.

Use a Visual Studio generator in an MSVC environment. A plain Ninja invocation
without an initialized compiler environment is expected to report no C or C++
compiler; that is an environment setup failure, not a ViennaPS source failure.

## CPM, ViennaLS, and Embree

- `cmake/cpm.cmake` validates the pinned CPM file by SHA-256. A valid entry in
  `CPM_SOURCE_CACHE` is intentionally reused without a network download.
- ViennaLS is normally fetched at the pinned revision and receives the tracked
  Level Set patch. Its cache key includes the patch hash. A cache checkout with
  exactly the patched headers modified is expected, not evidence of corruption.
- A local `VIENNAPS_VIENNALS_SOURCE_DIR` overrides CPM. It must be a complete,
  compatible source tree with the required patch already applied. Use a fresh
  build directory when changing the override so stale `CPM_ViennaLS_SOURCE`
  cache state cannot win.
- ViennaCS cache hygiene warnings and Embree deprecation/codepage warnings are
  not build failures by themselves. Record the warning, then look for the
  first concrete configure, compile, or link error.
- Embree is an in-tree CPM target. `embree_DIR=NOTFOUND` is normal in that
  mode, and the first selected build can take substantial time to compile ISA
  variants. Build the requested smoke target in the same configuration
  (`Debug`, `Release`, or `RelWithDebInfo`) before diagnosing a missing
  `embree` or `viennaps_vulkan_surface` library.

## Validation and records

Run the smallest focused CTest first. A fixture result proves control-plane
behavior only; real Vulkan promotion additionally requires current strict-FP32
hardware evidence, matching device/driver identity, queue evidence, and the
specified CPU oracle. A missing hardware run is `NOT RUN`, never `PASS`.

After a successful accepted stage, remove its temporary build trees and logs,
retain only the agreed root build/worktree outputs, and update the task row
with the exact command and result. Do not record absolute SDK or library paths
in versioned docs.
