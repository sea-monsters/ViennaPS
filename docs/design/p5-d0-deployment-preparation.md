# P5-D0 deployment preparation

Status: `CHECKPOINT / STATIC PREPARATION COMPLETE` (2026-08-19)

## Card boundary

- **Milestone:** prepare the release-facing local deployment evidence and
  isolate optional VTK packaging ownership.
- **Predecessor:** `P5-X0-CLOSEOUT-BASELINE`, accepted at
  `39e644082f5b007d05856dce1c6f96cb850e6209` on
  `origin/codex/p5-closeout-base`.
- **Next unlock:** `P5-D1` local export/remote-CI exit preparation; the final
  deployment exit remains behind `P5-M0`, `P5-K1`, and `P5-E0`.
- **Validation mutex:** this pass used no validation slot. It performed only
  static inspection and this document edit; no CMake configure, build,
  install, CTest, consumer, Vulkan, reference, or cleanup command was run.
  Any later D0 validation must acquire the single global slot and execute one
  named command at a time.

### Global position (about 150 tokens)

This card owns the deployment evidence boundary, not runtime behavior. The
CPU/no-SDK package path is already accepted and remains the release baseline.
The opt-in Vulkan payload and installed profile persistence have local evidence,
but are packaging/control-plane evidence only: they do not promote automatic
Vulkan routing or provide a Process deployment route. The Wave 0 VTK probe has
an external export-set boundary in ViennaLS, where VTK link targets referenced
by the dependency are absent from its exported target set. That failure is not
owned by ViennaPS CMake or by a dependency pin in this card. Hosted CI has no
accepted run IDs or URLs, so it remains an explicit open gate. Subsequent
validation must use one named command at a time after the mutex is released.

## Wave 0 and Wave 1 position

Wave 0 froze the reviewed P5 baseline and explicitly recorded that a clean
Vulkan-ON/VTK-OFF configuration succeeds, while a VTK-enabled configuration
fails during ViennaLS/VTK export-set generation. Wave 1 therefore permits D0
deployment preparation in parallel with `P5-N1` and `P5-K0`, but does not permit
surface/model implementation or completion-state changes.

The current untracked `.tmp_p5_d0_*` directories are generated validation
state. They are not source evidence, are not edited by this card, and must not
be deleted while ownership and active-process status are unresolved.

Governing sources: [formal exit plan](p5-formal-exit-execution-plan.md),
[status board](vulkan-compute-acceleration-status.md),
[development report](vulkan-compute-acceleration-development-report.md), and
[build/worktree guide](../development-build-and-worktree-guide.md).

## Static ownership audit

### VTK first boundary

The project-side VTK options are forwarded to ViennaLS through
`CPMAddPackage(... OPTIONS "VIENNALS_USE_VTK ...")`. The failure occurs when
the dependency generates/exports `ViennaLSTargets`: its exported link interface
refers to VTK targets that are not present in that export set. The accepted
probe with `VTK_MODULE_ENABLE_VTK_IOHDF=NO` gets past the bundled HDF5 POSIX
probe and still stops at this ViennaLS export boundary.

Ownership is `BLOCKED-EXTERNAL` (also recorded as `BLOCKED-LOCAL-ENV` in the
PD5 status evidence), not a ViennaPS-owned CMake defect. The existing
no-VTK compatibility bridge intentionally rewrites only ViennaLS's generated
CPU/no-VTK dependency list. Extending it to VTK would mask or rewrite an
external export-set contract and is outside this card. No dependency source,
cache, pin, or global CMake behavior is changed.

### CPU/no-SDK baseline

`PD5-INSTALL-EXPORT` is locally accepted on 2026-08-04: the tracked ViennaLS
patch transport passed a clean-source dry run; a fresh VTK-OFF/Vulkan-OFF
producer configured, built, and installed; and an independent prefix-only
consumer imported `ViennaTools::ViennaPS`, compiled, linked, and ran (CTest
1/1). The generated package requires `ViennaHRLE` and `ViennaCore`, not VTK.

### Vulkan payload and profile persistence

The 2026-08-18 local opt-in evidence records a Release Vulkan payload producer
and install/export replay passing with VTK OFF. The payload validator found 32
SPIR-V files, the runtime library, representative GLSL sources, headers,
manifest, exported runtime target, and Vulkan dependency. The independent
installed consumer passed CTest 1/1 and exercised profile serialization,
provisioning/reload, and stale-fingerprint fail-closed selection.

This does not install a Process deployment profile, widen an eligibility
predicate, promote AUTO routing, or close VTK/hosted-CI gates. Profile logic is
control-plane evidence only: complete fingerprints are required; valid profiles
reuse without probing; missing, invalid, or stale profiles require a probe; and
probe/write/reread failures fail closed to CPU.

### Hosted CI gap

`.github/workflows/build.yml` statically contains the path-hygiene, CPU/no-SDK
test, install-export, and dispatch-only self-hosted Vulkan jobs. Their paths are
runner-temporary in the workflow design. No hosted run ID/URL, default-branch
candidate publication, or registered Vulkan runner evidence is accepted, so
remote CI remains `NOT RUN` rather than `PASS`.

## Validation plan after mutex release

Run each line as a separate command, in order, stopping at the first nonzero
exit. Use caller-owned temporary directories and the repository's clean-env
wrapper/VS developer environment as required by the build guide. Do not combine
the lines into a batch or reuse a partial prefix.

1. Set the caller-owned `CPM_SOURCE_CACHE` environment value.
2. `cmake -DVIENNAPS_INSTALL_EXPORT_WORK_DIR=<cpu-work> -P cmake/run-pd5-install-export.cmake`
3. `cmake -DVIENNAPS_VULKAN_INSTALL_EXPORT_WORK_DIR=<vulkan-work> -P cmake/run-pd5-vulkan-install-export.cmake`
4. `cmake -DVIENNAPS_VULKAN_INSTALL_PREFIX=<vulkan-prefix> -P cmake/validate-pd5-vulkan-install-payload.cmake`
5. `cmake -S tests/installExportConsumer -B <cpu-consumer> -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=<cpu-prefix> -DBUILD_TESTING=ON`
6. `cmake --build <cpu-consumer> --config Release`
7. `ctest --test-dir <cpu-consumer> --output-on-failure -C Release`
8. `cmake -S tests/vulkanInstallExportConsumer -B <vulkan-consumer> -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=<vulkan-prefix> -DBUILD_TESTING=ON`
9. `cmake --build <vulkan-consumer> --config Release`
10. `ctest --test-dir <vulkan-consumer> --output-on-failure -C Release`

The exact local acceptance is: CPU/no-SDK producer, install, and independent
consumer pass; opt-in Vulkan producer, payload validator, and independent
consumer pass; profile serialize/provision/reload/stale cases pass; installed
manifests and CMake exports contain no absolute SDK or cache paths; and the VTK
lane is either independently repaired and accepted or retained as an explicit
external blocker. Remote acceptance additionally requires the exact published
candidate, path-hygiene/test/install-export hosted results, and recorded run
IDs/URLs.

## Prohibited scope and handoff boundary

Prohibited: ray, surface, Level Set, model, Process, physics, reference-tree,
profile-policy, automatic-promotion, shader, dependency/cache, workflow,
status-board, remote-write, architecture, and global CMake changes; deleting
temporary trees; and converting local evidence into a P5 completion claim.

No correction was attempted in this static pass, so the retry state is
`NOT STARTED`; if a later validation attempt fails, it must name one new
evidence target and use at most one `RETRY_ONCE`. A second failure at the same
boundary returns the card to the main line with its logs and unchecked items.
The handoff must include changed/untracked files, exact command results, VTK
ownership, profile conclusion, D1 unlock decision, diff-check result, and an
orphan/process audit performed under the appropriate validation authority.
