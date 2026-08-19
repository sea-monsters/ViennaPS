# ViennaPS Code-Area Execution Guide

This file is the concise handoff guide for work in
`D:\Codex_lib\ViennaPSMod`. Repository-level rules remain in `AGENTS.md`; the
current program state and acceptance evidence remain in the two Vulkan design
documents below.

## Source of truth

Read these before changing Vulkan acceleration code or its records:

0. `docs/design/vulkan-program-intent-framework.md` — full-program functional
   intent whitepaper and overall Vulkan migration design philosophy: original
   ViennaPS (`code_reference/ViennaPS`) semantic baseline, mprocess
   (`D:\mprocess`) GPU-embedding reference boundaries, **CPU-path reuse outside
   compute** (hard requirement), Vulkan overlay mapping, drift-prevention
   contracts, P0–P4 compliance notes, current position, card boundaries, and
   engineering invariants (read first).
1. `docs/design/vulkan-compute-acceleration-development-report.md`
2. `docs/design/vulkan-compute-acceleration-status.md`
3. `docs/design/p0-p4-cpu-reuse-audit-round1.md` — P0–P4 Round 1 audit ledger
   (CPU-path reuse outside compute; recorded findings, open follow-ups)
4. `docs/development-build-and-worktree-guide.md` for parallel work
5. `docs/design/p5-formal-exit-execution-plan.md` — approved P5 closeout
   dependency order, strong-card dispatch contract, snapshot cadence and final
   exit definition

The status document's `Total-plan continuation board: P5 to P7` is the active
task order. A card is not complete merely because code exists or a local smoke
passes; record the exact acceptance command, CPU oracle, hardware evidence when
applicable, and remaining scope.

## Program position

- `PD0` through `PD4`, including `PD4-HW-MATRIX` and `PD4-PERF-BASELINE`, are
  accepted local control-plane milestones.
- `PD5-CI-DOCS-INTEGRATION` and `PD5-INSTALL-EXPORT` are accepted locally for
  their CPU/no-SDK boundaries. Remote CI remains blocked until a published
  branch contains `.github/workflows/build.yml` and produces traceable GitHub
  run evidence. Do not claim remote CI from local YAML review.
- `P5-JD`, `P5-JE`, and `P5-JF` are locally revalidated on Release Vulkan with
  Intel Arc: device-resident composition, one compute submission, and strict
  FP32 fail-closed terminal status. These cards do not provide Process routing,
  full particle/surface physics, automatic Vulkan selection, or release status.
- The root worktree contains accumulated accepted and pending P5 changes.
  Freeze them through the explicit `codex/p5-closeout-base` checkpoint before
  starting new production work; generated build/test environments are not
  checkpoint inputs.
- P6 oxidation/coupled solving and P7 resident execution, calibration, soak,
  recovery, and release gates remain future work.

## Next execution order

For a single developer, follow:

`P5-X0` → `P5-N1/N2` → `P5-S0/S1` → `P5-M0` → `P5-E0` →
`P6-LA-BASELINE` → `P6-OXIDATION-COUPLING` → `P6-PHYSICS-EXIT` →
`P7-RESIDENT-EXECUTION` → `P7-CALIBRATION` → `P7-CI-SOAK-RELEASE`.

P6 linear algebra may run in parallel with P5 in a multi-owner setup, but one
card still owns one verified worktree and an exclusive file boundary. Code
delegation uses the repository's configured fast Luna xhigh route, no more
than three concurrent workers. Reuse a worker for dependent cards; the main
line accepts and fixes each handoff. One named correction is allowed per card,
then reclaim further work to the main line.

Concurrent workers may analyze or edit, but validation is serialized across
the repository: only one CMake/build/CTest/reference/Vulkan workload may run
locally at a time. Other workers stop at `CHECKPOINT` until main-line release.

## Validation and claims

- Use a CPU result as the correctness oracle for non-CUDA hosts.
- For standalone Vulkan ray validation, use a temporary Ninja build from a
  Visual Studio developer environment with `VULKAN_SDK` supplied by the caller.
- The current ray evidence is five focused P5-JD/JE/JF tests plus the complete
  standalone 12-test ray suite. A CPU/no-SDK build validates fallback/build
  hygiene only; it is not hardware evidence.
- Keep input uploads and terminal downloads distinct from the one compute
  submission claim. Preserve fail-closed output sentinels and strict FP32
  status behavior.
- Remote CI requires a published commit, run ID, URL, conclusion, and relevant
  runner context. If the remote branch/workflow/runner is absent, mark the card
  `BLOCKED`; do not simulate success.
- After validation, remove only temporary directories created by the current
  task, after checking for live processes. Preserve `.claude/` and unrelated
  user-owned changes.

## Handoff format

Every completed or paused card reports:

- card ID, predecessor, and next unlocked card;
- changed files and exclusive ownership boundary;
- exact commands and results;
- CPU oracle outcome and Vulkan device evidence, if selected;
- local-only, remote-verified, or production-route status;
- residual risk and the smallest next action.
