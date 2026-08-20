# P5 Neutral CPU Reference Oracle

- Date: 2026-08-06
- Card: `P5-NEUTRAL-CPU-ORACLE`
- CPU implementation authority: `D:\Codex_lib\code_reference\ViennaPS`
- Result: `SUPERSEDED` — the original harness result is not current reference
  evidence.

## Current Paired Evidence (2026-08-09)

The superseded diagnostic harness is replaced by the controlled paired fixture
under `tests/neutralCpuReferenceDifferential/`. The same source is compiled
twice: once against the unmodified reference headers at
`D:\Codex_lib\code_reference\ViennaPS`, and once against the Mod headers. Both
use `float`, 2-D `MakePlane` (`gridDelta=0.5`, `xExtent=4`, `yExtent=4`),
`CPU_TRIANGLE`, seed `42`, one ray per point, and zero reflections. The fixture
serializes input identity, empty-executor and active velocity, flux, geometry,
and process-state records as FP32 bit patterns. The independent checker has no
ViennaPS dependency and compares every record plus first-divergence reporting.

The default MSVC compile/link closure (without an explicit optimization level)
ran both fixtures and the checker reported:

```text
paired neutral CPU differential PASS empty=exact active=exact flux=exact geometry=exact process=exact max_ulp=0
```

The active case is intentionally limited: the reference executes its canonical
CPU velocity loop, while Mod installs the backend-neutral
`NeutralTransportVelocityExecutor` adapter with the frozen parameters. This
proves the adapter output against the CPU oracle; it does **not** prove a
complete active NeutralTransport route (coverage evolution, ballistic
transport, desorption, surface diffusion, and Process/Vulkan integration stay
outside this card).

The required Release `/O2 /Ob2 /openmp:llvm` gate remains an evidence gap. Both
paired executables compile and link, but both terminate during the first
`calculateFlux()` setup with Windows exit `-1073741819` (`0xC0000005`) after
the first CPU ray-trace diagnostic. No optimization suppression or fixture
change is authorized to turn this into a pass.

## Release crash forensics (2026-08-12)

The failure was reproduced with a native SEH/DbgHelp wrapper at
`tests/neutralCpuReferenceDifferential/neutral_cpu_oracle_crash_capture.cpp`.
The wrapper includes the unchanged fixture source, renames its `main`, and
captures the exception code, instruction address, and symbolized stack. Both
the Mod and unmodified-reference wrappers were compiled manually from a
Visual Studio 2022 BuildTools x64 developer shell with the same flags:

```text
/O2 /Ob2 /DNDEBUG /openmp:llvm /MD /Zi /std:c++20
```

The only include-root difference was `D:\Codex_lib\ViennaPSMod\include\viennaps`
versus `D:\Codex_lib\code_reference\ViennaPS\include\viennaps`; both builds
used the same cached ViennaCore/ViennaRay/ViennaLS/ViennaHRLE/ViennaCS and
Embree headers from `D:\Codex_lib\ViennaPSMod\.cpm-cache`, and linked the
same `build\_deps\embree-build\Release\embree4.lib` plus `dbghelp.lib`.
The compiler banner was MSVC 19.44.35223.0 (`cl.exe` from
`VC\Tools\MSVC\14.44.35207\bin\Hostx64\x64`); the shell was entered with:

```powershell
cmd /d /s /c 'call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\Tools\VsDevCmd.bat" -arch=x64 >nul && cl ... /O2 /Ob2 /DNDEBUG /openmp:llvm /MD /Zi /std:c++20 ...'
```

The post-build dependency check was:

```powershell
dumpbin /DEPENDENTS .tmp_neutral_mod_crash_capture.exe
dumpbin /DEPENDENTS .tmp_neutral_reference_crash_capture.exe
dumpbin /DEPENDENTS build/_deps/embree-build/Release/embree4.dll
```

Both executable dependency lists are identical; the Embree list adds its
`tbb12.dll` dependency as expected.
The runtime PATH was ordered as:

```text
D:\Codex_lib\ViennaPSMod\build\_deps\embree-build\Release;
D:\Codex_lib\ViennaPSMod\build\gpu\vulkan\ray\Release;
C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Redist\MSVC\14.44.35112\debug_nonredist\x64\Microsoft.VC143.OpenMP.LLVM
```

Run command (for each wrapper):

```powershell
& .\.tmp_neutral_mod_crash_capture.exe .\.tmp_neutral_mod_capture.out
& .\.tmp_neutral_reference_crash_capture.exe .\.tmp_neutral_reference_capture.out
```

Both wrappers catch `0xc0000005` and return `128`; neither writes the fixture
output. The Mod fault address is `0x00007FF7EEE603F2`; the reference address
is `0x00007FF7C90A0022` (different image bases, same generated function
offsets). Both stacks are identical through:

```text
viennacore::KDTree<float,std::array<float,3>>::
  traverseDown<ClampedPQueue<...>> + 0x52
viennacore::KDTree<...>::findNearestWithinRadius + 0x8c
viennaps::ElementToPointData<float,float,float,1,0>::prepare$omp$1 + 0xee
_kmp_invoke_microtask -> _kmp_fork_call -> _kmpc_fork_call
viennaps::ElementToPointData<...>::prepare + 0x5c5
viennaps::CPUTriangleEngine<float,2>::calculateSourceFluxes + 0x401
viennaps::FluxProcessStrategy<float,2>::coverageInitIterations + 0x5b3
```

Therefore the first failing boundary is the shared ViennaCore KDTree lookup
used by `postProcessing_.apply()` after `TraceTriangle::runRayTracer()` has
returned, before the surface-model coverage update. `OMP_NUM_THREADS=1,2,4,8`
reproduces the same fault address and stack, so changing worker count is not a
fix. `dumpbin
/DEPENDENTS` reports the same `embree4.dll`, `libomp140.x86_64.dll`, and MSVC
runtime dependencies for both wrappers; Embree depends on `tbb12.dll`. The
resolved binaries are Embree 4.3.3 (SHA256
`4BAA0F01FC9881E0EEC2F87BEA3A40781D107B1DF7BBB36B8E49A8B930F4E67`), TBB
2023.0.0 (SHA256
`49E43F8B3B0528CFC58ED081DA71DB28FE93D281D674653ADDEE20E562A2314D`), and
LLVM OpenMP `libomp140.x86_64.dll` 5.0 (SHA256
`9AB1CB787E52B2A36133899C01C1DB1F876067D1471CD224EAD85F40A8152B99`).

This is a common external/toolchain-bound crash, not a Mod/reference semantic
divergence. No production code, CPU formula, reference tree, optimization
flag, or fixture oracle was changed to mask it; the Release paired oracle
remains unaccepted.

## Status Update (2026-08-07)

This document preserves the original diagnostic harness only. Fresh standalone
Mod and unmodified-reference `SingleParticleProcess<float,2>` CPU probes pass.
The Release `/O2` failure is instead isolated to a composed deployment target,
before any Vulkan configuration, in `viennacore::KDTree::traverseDown`.
Consequently this file must not be cited as proof of a reference-identical
NeutralTransport or CPU fixture failure. The current control record is
[p5-cpu-reference-conformance.md](p5-cpu-reference-conformance.md) and the
`P5-SURFACE-CPU-REPRO-ISOLATION` card in
[vulkan-compute-acceleration-status.md](vulkan-compute-acceleration-status.md).

## Fixture

The no-Vulkan CPU-only fixture uses a 2-D `MakePlane`,
`NeutralTransport<float, 2>`, `FluxEngineType::CPU_TRIANGLE`, fixed seed 42,
`maxReflections=1`, `smoothingNeighbors=1`, one ray per point, and initialized
coverage. It is the CPU portion of the rejected surface composition smoke.

The oracle source is compiled from the unmodified reference ViennaPS headers.
The temporary harness supplies only an uninstantiated VTK writer declaration so
the reference's optional visualization declaration does not require a VTK build;
it does not replace a CPU computation, Process strategy, model, ray tracer, or
Level Set routine.

## Historical Evidence (Superseded)

The reference executable emits:

```text
Coverages reinitialized.
preflight
checked
```

It then terminates with Windows exit `-1073741819` (`0xC0000005`) inside
`Process::calculateFlux()`. That result was later invalidated as a reference
classification because the fresh standalone reference probe passes. It remains
useful only as a record of the old harness, not a CPU or Vulkan conclusion.

## Consequence

`NeutralTransport` with reflections remains CPU-only and unsupported by the P5
Vulkan ray eligibility gate. A current paired oracle is blocked on the composed
target isolation; no fallback or model-route expansion is justified.

## P5-N1 serialized-validation checkpoint (2026-08-19)

This checkpoint preserves the historical evidence above and does not unlock
`P5-N2`. All commands below held the repository-wide local-validation mutex;
`P5-K0` and `P5-D0` remained stopped.

The existing `.tmp_p5_n1_hostx86_20260819` tree was resumed under the Visual
Studio developer environment and built the fixture and checker successfully
with the Hostx86/x64 MSVC 19.44.35207 compiler, Release
`/O2 /Ob2 /DNDEBUG /openmp:llvm /MD /Zi`, ViennaCore 2.2.1, and the existing
ViennaRay/Embree/TBB closure. The initial cache had
`VIENNAPS_NEUTRAL_ORACLE_MOD=OFF`; its runs are retained only as invalid-mode
diagnostics and are not Mod evidence. After the main line corrected the cache
to `ON`, the real Mod executable entered coverage initialization at OMP=1 but
made no CPU progress and published a zero-byte output before the bounded run
was terminated.

The main line then added the phased test-only `run_paired_oracle.ps1`. Each
invocation performs exactly one build, one fixture run, or one comparison and
enforces an exact-process timeout. A fresh Hostx64/x64 Mod build completed, but
its OMP=1 run timed out after 180 seconds with no oracle output. Therefore the
unmodified reference side was not built or run: the ordered Mod-success gate
had already failed.

One ViennaCore code-generation candidate was tested and exhausted. The test
build declared one external specialization and emitted exactly one object for
`KDTree<float, std::array<float, 3>>`; it changed no KDTree statement, CPU
formula, fixture input, OpenMP/optimization flag, or reference source. The
candidate built successfully but its Mod OMP=1 run also timed out after 180
seconds with no output. It is rejected and must not be adopted or described as
a repair.

- Root-cause category: **not resolved**. Host compiler frontend selection and
  duplicate KDTree template code generation are both excluded as sufficient
  repairs for the current fixture.
- `P5-N1`: **RECLAIMED-MAIN / BLOCKED-EXTERNAL** after its one toolchain lane
  and one ViennaCore candidate.
- `P5-N2` unlock: **NO**; no reference/Mod OMP=1 pair or raw comparison exists.
- Retry state: exhausted. Do not add another local candidate without a new
  upstream reproducer, supported compiler/runtime closure, or separately
  approved repair boundary.
- Cleanup: every fixture/compiler descendant, including the MSVC program
  database service, was reaped before releasing the validation mutex.

## Commands

```powershell
cmd /d /s /c 'call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\Tools\VsDevCmd.bat" -arch=x64 >nul && cmake -S D:\Codex_lib\ViennaPSMod\.tmp_p5_reference_cpu_20260806 -B D:\Codex_lib\ViennaPSMod\.tmp_p5_reference_cpu_20260806\manual-build -G Ninja -DCMAKE_BUILD_TYPE=Release && cmake --build D:\Codex_lib\ViennaPSMod\.tmp_p5_reference_cpu_20260806\manual-build --parallel 2'
$env:Path = "D:\Codex_lib\ViennaPSMod\.tmp_p5_route_20260805\gpu\vulkan\ray;D:\Codex_lib\ViennaPSMod\.tmp_p5_route_20260805\_deps\embree-build;$env:Path"
& D:\Codex_lib\ViennaPSMod\.tmp_p5_reference_cpu_20260806\manual-build\p5_reference_cpu_oracle.exe
```

## P5-N1D phase-boundary checkpoint (2026-08-19)

The main line accepted a test-only phase logger around the existing fixture.
It writes to `<oracle-output>.phase.log`, checks that the log file opens, and
uses the existing CPU-triangle `Particle 0` debug line as the completed
ray-tracer marker. The fixture data, random seed, CPU formulas, Process order,
reference tree, optimization/OpenMP flags, and production code are unchanged.

The first instrumented launch failed before program startup with
`0xC0000135`. Read-only dependency inspection confirmed that Embree, TBB, the
MSVC CRT, and LLVM OpenMP DLLs all existed. The actual cause was the hosted
process environment containing distinct `Path` and `PATH` keys. The phased
runner now creates a case-insensitive child environment with exactly one
canonical `Path`; this is the same boundary already used by the repository's
clean-environment CMake wrapper.

After that single named correction, the existing Mod executable was rerun at
OMP=1 with the same 180-second bound. It reached `phase=fixture_start`,
`phase=before_calculateFlux`, completed the real CPU ray trace and flushed
`DEBUG: Particle 0` (`19` rays, `16` geometry hits), then exited immediately
with `-1073741819` (`0xC0000005`). The phase log is 1836 bytes; the oracle file
is zero bytes; neither `phase=after_calculateFlux` nor an output-byte marker
exists. No build, test, or fixture process remained afterward.

This rules out process startup and the ray tracer as the first bad boundary.
It aligns the current Release failure with the historical
`ElementToPointData::prepare -> KDTree::findNearestWithinRadius ->
KDTree::traverseDown` stack. It does not identify or approve a repair. N2,
Surface RED/GREEN, and support-row expansion remain locked.

## P5-N1E real-tree query probe (2026-08-19)

The user approved a new source-grounded discrimination boundary after the
main-line source audit: replicate the exact D==2
`CPUTriangleEngine::updateSurface` element-tree construction and the full
`ElementToPointData::apply()` post-processing frame on the real fixture
geometry, without executing the ray tracer. The caller-owned, test-only probe
`tests/neutralCpuReferenceDifferential/neutral_cpu_oracle_kdtree_probe.cpp`
builds the same `MakePlane` domain (`gridDelta=0.5`, extents 4/4), generates
the disk mesh through `viennals::ToDiskMesh` with a translator exactly as
`FluxProcessStrategy::setupProcess` does, builds the element KDTree through
`CreateSurfaceMesh` plus `convertLinesToTriangles` exactly as
`updateSurface` does (`minNodeDistanceFactor=0.05`), validates one
`findNearestWithinRadius` per disk node (`radius=1.0`, indices and distances
contract-checked), and then runs `ElementToPointData<float,float,float,1,0>`
`apply()` — the historical `prepare$omp$1 -> findNearestWithinRadius ->
traverseDown` frame. The runner gained one-action probe steps
(`BuildProbeMod`/`BuildProbeReference`/`RunProbeMod`/`RunProbeReference`); the
probe links the same `embree4.lib` closure but never executes a trace.

Results under the serialized validation mutex, required flags
`/O2 /Ob2 /DNDEBUG /openmp:llvm /MD /Zi`, Hostx64/x64 MSVC 19.44.35207:

- Mod probe: exit 0 at OMP 1, 2, 4 and 8; 9 disk nodes, 16 elements, 60
  radius-query results, `PROBE_PASS`.
- Reference probe: exit 0 at OMP 1 and 8 with the same observables.
- Normalized outputs are byte-identical across both header roots and all
  thread counts (raw SHA256: Mod OMP=1
  `ab20a29a1c62a3e0ef9a86d6fa1f675839927d00bbba14c2a90274c6b31cf052`,
  reference OMP=1
  `e189eb6df0b398c60e7c849ec4d1739764b6bc7fcb0eee045eb7b211c649ac4f`; the
  6-byte raw delta is exactly the `source=mod`/`source=reference` line).
- Evidence bundle: `.tmp_p5_n1e_probe_20260819/` (probe binaries, outputs and
  `.phase.log` files); no compiler, linker, fixture, or probe process
  remained after each step.

Classification: the real-data built tree and the exact post-processing frame
are exonerated. This narrows the boundary but is not a repair and
does not unlock `P5-N2`.

## P5-N1F root-cause identification (2026-08-20)

The approved post-trace probe extension added a runtime mode selector
(`notrace`/`trace`) and an SEH/DbgHelp capture variant
(`neutral_cpu_oracle_kdtree_probe_capture.cpp`) to the same caller-owned
probe. Unexpectedly, the rebuilt `notrace` mode reproduced the composed
failure exactly: exit `-1073741819`, fault at
`KDTree<float,std::array<float,3>>::traverseDown<ClampedPQueue<...>>+0x52`
(`vcKDTree.hpp:331`, the `axis` load), reached through
`findNearestWithinRadius+0x8c` from the probe's direct query loop — serial
code, no OpenMP region, no ray tracer executed.

Serialized bisection under the validation mutex then excluded the new
elements one at a time: removing the second (`pointKdTree`) build still
crashed (E1), and removing the `TraceTriangle` object construction still
crashed (E2). The pass/fail flip between the N1E and N1F binaries is
therefore caused by semantically irrelevant surrounding code, not by any
runtime action.

The existing `KDTreeAudit` overlay was then applied to the probe build. In
the crashing binary itself the audit reported `KDTree audit valid nodes=16`
immediately before the fatal query: every child pointer lies inside the node
storage with vector-aligned addresses, every `axis < D`, every
`index < nodeCount`, and the walk visits all 16 nodes. The in-memory tree is
structurally perfect; the defect is in the query code path.

Disassembly of the crashing capture binary (`dumpbin /SYMBOLS` +
`/DISASM`, function at preferred VA `0x1400459F0`, fault at `+0x52` =
`0x140045A42`) shows:

- `0x...5A42: movsxd rbx, dword ptr [rdi+18h]` — the `axis` load with
  `rdi = currentNode`; the captured registers show `rdi=0`.
- Function entry `0x...59F0: test rdx,rdx; je ...` — the source-level
  `if (currentNode == nullptr) return;` guard.
- The second (tail) recursion was compiled into a loop:
  `0x...5B00: mov rdi, qword ptr [rdi+rax*8+20h]` (load the `left`/`right`
  child) followed by `jmp 0x...5A42` — a back-edge that re-enters AFTER the
  null check. A null leaf child is therefore dereferenced at the `axis`
  load, producing `0xC0000005` on address `0x18`.

Root cause: MSVC 14.44.35207 at `/O2 /Ob2 /openmp:llvm` miscompiles the tail
recursion in `viennacore::KDTree::traverseDown` (queue-based overload; the
`findNearest` pair overload shares the same shape) by dropping the dominating
null check from the tail-call loop. The ViennaCore source is correct; the
defect is external toolchain code generation triggered by this recursion
shape. This single mechanism explains every recorded observation: the
constant `+0x52` fault offset across Mod/reference/composed/probe binaries,
OMP-count independence, full determinism, the default-flags pass, the pure
and synthetic fixture passes (different codegen of the same function), and
the preflight pass (tree built but never queried).

This is a root-cause classification with direct evidence, not a repair.
`P5-N2` remains locked until an approved ViennaCore repair candidate
(loop-form `traverseDown` overlay) passes Lane C (probe) and Lane E (paired
fixture) under the ordered Mod-first gates.

## P5-N1G repair-candidate validation (2026-08-20)

The user approved the `IterativeTraverse` candidate: both `traverseDown`
overloads (pair and queue) are rewritten from tail recursion into an
explicit `while` loop, applied as a runner-owned include overlay
(`-Candidate IterativeTraverse`) without touching `.cpm-cache`, the
reference tree, or production defaults.

The first loop-form overlay still crashed identically. Disassembly of the
rebuilt binary (`neutral_cpu_oracle_kdtree_probe_capture_mod.exe`, function
at preferred VA `0x140045100`) showed the SAME defective shape: entry
`test rdx,rdx; je` at `0x...50B0`, axis load at `+0x52` (`0x...5102`), and
a back-edge `0x...51C5: jmp 0x...5102` that skips the entry null check.
Adding an explicit `if (currentNode == nullptr) break;` on the freshly
loaded child did not help either: MSVC deleted that check as well and
emitted byte-equivalent defective code. The optimizer treats the loop-head
and interior null tests as redundant with the entry test and rotates them
out of the back-edge path.

The final overlay therefore forces the check through a `volatile` store/load
of the continuation child:

```cpp
if (distanceToHyperplane < best.first /* resp. intersects */) {
  Node *volatile nextNode =
      isLeft ? currentNode->right : currentNode->left;
  if (nextNode == nullptr)
    break;
  currentNode = nextNode;
} else {
  break;
}
```

Validation under the serialized mutex (all at
`/O2 /Ob2 /DNDEBUG /openmp:llvm /MD /Zi`):

- Lane C probe, Mod build: `notrace` and `trace` modes at OMP 1/2/4/8,
  8/8 exit 0 (`notrace` OMP=1 previously crashed deterministically).
- Lane C probe, reference build: `notrace`/`trace` at OMP 1/8, 4/4 exit 0.
- Probe outputs: all available Mod/reference pairs (OMP 1/8 both modes)
  byte-identical after removing the `source=` label; 60 radius-query
  results on both sides.
- Lane E paired fixture, both sides rebuilt with the overlay: OMP 1/2/4/8
  all `paired neutral CPU differential PASS empty=exact active=exact
  flux=exact geometry=exact process=exact max_ulp=0` — the overlay does
  not perturb a single ULP of the differential.

Status: the repair candidate is validated locally on both ordered gates.
Adoption (a `cmake/patches/` CPM patch against ViennaCore plus an upstream
MSVC/ViennaCore report) and the N2 acceptance-matrix requirement that the
default-flags MSVC differential remains unchanged are main-line decisions
and remain pending.

## P5-N1H patch adoption and default-flags re-check (2026-08-20)

The user approved adoption and deferred both upstream reports. The fix
landed as `cmake/patches/viennacore-v2.2.1-kdtree-traversedown-nullcheck.patch`
(content byte-identical to the validated N1G overlay), wired into the
ViennaCore `CPMAddPackage` call through `PATCHES` plus
`CUSTOM_CACHE_KEY "v2.2.1-kdtree-nullcheck-<sha256[0:16]>"`, following the
ViennaLS patch precedent; a local `CPM_ViennaCore_SOURCE` override is
exempt. GNU `patch --dry-run` against a pristine v2.2.1 copy passes, and a
fresh top-level configure (tests/examples/Python/VTK off, verified
`.cpm-cache`) fetched ViennaCore into the new cache key
`v2.2.1-kdtree-nullcheck-31f6423c177a320d` with the patched header
byte-identical to the validated overlay.

The runner gained two test-only controls: `-ViennaCoreOverride` (point at
the patched cache while keeping the existing configured build for
Embree/TBB) and `-FlagSet Release|Default` (Default drops the explicit
`/O2 /Ob2`, reproducing the 2026-08-09 baseline closure). With the patched
cache headers and NO overlay (`-Candidate Baseline`):

- Release flags: paired fixture OMP 1/2/4/8 all
  `PASS ... max_ulp=0` — the patch alone fixes the Release crash and
  preserves raw equality.
- Default flags: paired fixture OMP 1/2/4/8 all
  `PASS ... max_ulp=0` — the differential is unchanged against the
  2026-08-09 baseline, closing the N2 acceptance-matrix default-flags
  requirement.

Evidence directories: `.tmp_p5_n1_adopt_release_20260820` and
`.tmp_p5_n1_adopt_default_20260820` (outputs), `.tmp_p5_n1_adopt_20260820`
(configure tree). The N1G overlay path in the runner remains available for
forensics but is no longer required for a passing Release differential.
