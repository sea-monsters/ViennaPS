# P5 Neutral CPU Oracle: Pure ViennaCore Upstream Reproduction

- Card: `P5-NEUTRAL-CPU-ORACLE-EXTERNAL-UPSTREAM-REPRO-DESIGN`
- State: `SUPERSEDED / CLOSED` (2026-08-20; no current validation work remains)
- Owner: P5 CPU-oracle isolation
- Owned file: this document only

## Current disposition (2026-08-21)

This design was superseded by the adopted ViennaCore CPM patch
`cmake/patches/viennacore-v2.2.1-kdtree-traversedown-nullcheck.patch` and the
accepted `P5-N2` paired matrix. Mod and unmodified-reference fixtures now pass
the required Release OMP 1/2/4/8 runs with raw equality and `max_ulp=0`.
The pure-upstream reproducer remains useful historical evidence, but it is no
longer a prerequisite and must not be reopened as a current blocker.

## Historical design snapshot (superseded by P5-N1H/N2)

All design, predecessor, diagnostic-matrix, and implementation sections below
are retained historical evidence for the superseded experiment. Their older
`blocked`/`unlock` wording is not the current P5 gate state.

### Global position (approximately 150 words)

P5 may replace a compute operation, never the CPU definition of
`NeutralTransport`. The paired Mod/reference fixture is exact at the default
MSVC closure, but the required `/O2 /Ob2 /DNDEBUG /openmp:llvm` run fails in
the same ViennaCore `KDTree::traverseDown` frame for both trees. This card
therefore moves the next experiment below ViennaPS: a pure, independently
identified ViennaCore reproducer that constructs a KDTree, calls `build()`,
and performs the same radius lookup shape used by
`ElementToPointData::prepare`. It must use no ViennaPS, ViennaRay, Embree,
Process, fixture, or Mod headers. The experiment is diagnostic only; it cannot
unlock Vulkan, change CPU formulas, or turn a crash into a pass. A legitimate
upstream repair, dependency upgrade, or runtime lock is admissible only after
the reproducer, ABI, compiler, and loaded-binary identities are independently
recorded and the full paired oracle is rerun with the required optimization
and OpenMP flags. At the time of this design snapshot, the Release CPU oracle
remained blocked.

## Predecessor, downstream, and handoff

Predecessor evidence is [p5-neutral-cpu-oracle.md](p5-neutral-cpu-oracle.md),
[p5-cpu-reference-conformance.md](p5-cpu-reference-conformance.md), and the
`P5-NEUTRAL-CPU-ORACLE` row in
[vulkan-compute-acceleration-status.md](vulkan-compute-acceleration-status.md).
They establish identical Mod/reference crash stacks and a shared external
closure. The failed boundary is `KDTree::traverseDown` during
`ElementToPointData::prepare`, before ray tracing or model update.

The downstream gate is the existing paired fixture and independent checker;
no model-matrix, Vulkan Process route, Neutral velocity substage, promotion,
or release claim may consume this card as acceptance. Handoff is a raw-run
bundle plus a decision (`upstream defect`, `runtime identity defect`, or
`not reproduced`) to the coordinator. A pure-repro pass alone is not a CPU
oracle pass.

## Independent source and binary identity (required before a run)

The reproduction source must be an independently obtained, clean ViennaCore
snapshot, not `D:\Codex_lib\ViennaPSMod\.cpm-cache` and not a modified
`D:\Codex_lib\code_reference\ViennaPS` checkout. Record:

1. upstream URL, commit/tag, `git status --short`, and a tree/archive SHA256;
2. ViennaCore version from `CMakeLists.txt` and the exact `vcKDTree.hpp`,
   `vcQueues.hpp`, and `vcLogger.hpp` hashes;
3. MSVC `cl.exe` full path/banner, x64 host/target, CMake generator, and
   `/O2 /Ob2 /DNDEBUG /openmp:llvm /MD /Zi /std:c++20 /EHsc` flags;
4. `libomp140.x86_64.dll`, MSVC CRT, TBB, and any other loaded DLL paths and
   SHA256 values; verify with `dumpbin /DEPENDENTS` and a clean process PATH;
5. a source/include manifest proving no `viennaps`, ViennaLS, ViennaHRLE,
   ViennaCS, ViennaRay, Embree, Vulkan, or Mod include entered the command.

The Mod and reference trees remain untouched. Any local patch must be a
separate, named upstream candidate snapshot with its own commit/hash.

## Minimal pure-upstream reproducer (fixed ABI and input)

Create a temporary `.tmp_p5_neutral_upstream_repro_<date>` project containing
one C++20 translation unit and a tiny CMake file. Include only ViennaCore
headers. The program shall:

```cpp
using T = float;
using Point = std::array<T, 3>;
std::vector<Point> points{{-1.5f, 0.f, 0.f}, {-0.5f, 0.f, 0.f},
                          {0.5f, 0.f, 0.f},  {1.5f, 0.f, 0.f},
                          {2.5f, 0.f, 0.f},  {3.5f, 0.f, 0.f}};
viennacore::KDTree<T, Point> tree(points);
tree.build();
for (const auto &p : points)
  (void)tree.findNearestWithinRadius(p, T(0.5));
```

The ABI is x64 MSVC, `float`, `std::array<float,3>`, six stable points,
`build()` once, six deterministic queries, and no OpenMP thread-count
override. A second diagnostic mode may query the fixed coordinates from the
neutral fixture, but may not alter the production fixture or use its source.
The source must print `sizeof`, alignment, point count, build completion, and
query result counts before exit; it must not catch or mask access violations.

Required commands (run from a VS 2022 x64 developer shell):

```powershell
cl /nologo /TP /EHsc /O2 /Ob2 /DNDEBUG /openmp:llvm /MD /Zi /std:c++20 `
  /I <independent-viennacore> pure_kdtree_repro.cpp `
  /Fe:pure_kdtree_repro.exe
dumpbin /DEPENDENTS pure_kdtree_repro.exe
& .\pure_kdtree_repro.exe 2>&1 | Tee-Object raw-run.log
```

The exact command line, working directory, environment PATH, exit code, and
all stdout/stderr are part of the evidence bundle.

## Oracle and acceptance

The pure repro oracle is structural and deterministic: `build()` completes,
the root is non-null through a successful query, every query returns a valid
index `< 6`, every reported distance is finite and `<= 0.5`, and the process
exits `0` without SEH. The expected output is recorded as raw text plus a
machine-readable JSON summary; no tolerance is used for counts/indexes.

Acceptance requires all of the following:

- the independent source and loaded-binary manifests are complete;
- the pure repro either reproduces `0xc0000005` in ViennaCore or exits 0 with
  an explicit `NOT_REPRODUCED` classification;
- if it reproduces, the symbolized frame is in `KDTree::traverseDown` and the
  fault occurs with the required flags, not a substituted build;
- the unchanged paired Mod/reference fixture is rerun afterward, preserving
  the same input, checker, and Release flags; and
- no acceptance text claims NeutralTransport or Vulkan parity from the pure
  repro alone.

## Explicit exclusions and invariants

Prohibited: changing `include/viennaps/**`, CPU formulas, fixture points,
ray parameters, `ElementToPointData`, OpenMP mode/count, optimization level,
compiler family, ABI, or exception handling to avoid the fault; disabling
OpenMP; `/Od`, `/Ob0`, debug-only builds; replacing KDTree with another
algorithm; adding a null/exception fallback; editing the reference tree,
`.cpm-cache`, CMake, status board, or tests; and downloading or writing to
external locations without coordinator approval.

The reproducer must not link Embree, TBB, ViennaRay, or any ViennaPS target.
If ViennaCore itself requires a runtime DLL, that DLL is recorded and loaded
from the declared clean PATH; mixed-version resolution is a failed run.

## Diagnostic matrix

| Lane | Source | Flags/runtime | Required result | Interpretation |
|---|---|---|---|---|
| A | independent clean ViennaCore | required Release + declared runtime | crash or PASS | primary upstream boundary |
| B | same source, same flags, alternate clean runtime only | exact ABI; no source change | compare raw logs/hashes | runtime defect candidate |
| C | independent upstream candidate patch | required Release + same runtime | no crash plus deterministic oracle | candidate repair only |
| D | current Mod/reference paired fixture | unchanged source and checker | same crash or exact PASS | parity gate; never weakened |
| E | candidate dependency in paired fixture | same flags/input/runtime manifest | checker exact PASS | unlock consideration only |

Only one variable may change between lanes. A lane that changes optimization,
OpenMP, compiler, input, or ABI is invalid evidence and must be rerun.

## Candidate repair, upgrade, and runtime-lock approval boundary

An upstream patch or version upgrade may be proposed only after Lane A produces
a reproducible fault and the candidate is identified by upstream commit,
clean diff, and source/tree hashes. It must first pass Lane C, then Lane E,
the existing default-MSVC differential, and the affected non-benchmark test
gate. The coordinator must approve any dependency pin or production CPM
change; this card itself cannot modify CMake or cache contents.

A runtime lock is admissible only when the exact DLL set is documented,
`dumpbin /DEPENDENTS` and PATH resolution show no mixed versions, Lane A is
stable across three fresh processes, and Lane E remains bit-exact. Runtime
locking may not lower optimization, disable OpenMP, or alter CPU semantics.
No candidate is accepted on a single lucky exit, a changed thread count, or a
debug/diagnostic-only pass.

## Raw-run evidence package

Store only under the temporary card directory: source manifest, compiler
banner, full command, CMake cache (if used), PATH, dependency dump, binary
hashes, stdout/stderr, exit code, symbolized stack or PASS summary, and a
lane-by-lane comparison table. The evidence must include the existing crash
identity: `0xc0000005`, `KDTree::traverseDown` (`vcKDTree.hpp` lines 331/358),
`findNearestWithinRadius`, and the caller boundary in
`psElementToPointData.hpp` when the paired fixture is rerun.

## Behavior regression after a candidate

After a candidate passes the pure repro, rerun the unchanged paired checker:
default MSVC must remain `empty=exact active=exact flux=exact geometry=exact
process=exact max_ulp=0`; required Release must produce two non-crashing
outputs and the same exact checker result. Also run the focused neutral CTest
target and the named non-benchmark gate from the repository instructions.
Capture fresh output hashes and confirm that unsupported Vulkan/model rows
remain CPU fallback or Manual fail-closed. A pure ViennaCore PASS without
these regressions is insufficient.

## Rollback and correction/reclaim rule

All candidate builds are disposable `.tmp_*` trees. On any failed lane,
restore the prior dependency identity and delete only the candidate temporary
tree after processes exit; never alter production, reference, or cache files.
The owner may make one evidence-targeted correction (for example, fix a
manifest or isolate a mixed DLL) and rerun only the named lane. If that
correction fails, or if the same failure recurs without a new boundary,
reclaim the card to the coordinator with the complete logs and no further
retry. The Release CPU oracle stays `EVIDENCE-GAP-MAIN` and all downstream
promotion/model gates remain locked.

## Links and source anchors

- [Neutral paired oracle](p5-neutral-cpu-oracle.md)
- [CPU conformance inventory](p5-cpu-reference-conformance.md)
- [P5 status board](vulkan-compute-acceleration-status.md)
- `D:\Codex_lib\ViennaPSMod\.cpm-cache\viennacore\edac\include\viennacore\vcKDTree.hpp`
  (`build`, `findNearestWithinRadius`, `traverseDown`)
- `include/viennaps/psElementToPointData.hpp` (`prepare`, KDTree lookup)
- `tests/neutralCpuReferenceDifferential/neutral_cpu_oracle_crash_capture.cpp`

## Historical implementation evidence (2026-08-13; superseded by P5-N1H/N2)

The caller-owned temporary root was
`.tmp_p5_neutral_upstream_repro_20260813`; no production, reference, CMake,
status, or `.cpm-cache` path was written. ViennaCore was fetched independently
from `https://github.com/ViennaTools/ViennaCore.git`, tag `v2.2.0`, commit
`4d0c013655fbbf471291737bd6939e7fd98208f0`, tree
`8ccfdcb930b2c2c07e5e4fd9622550e69056519e`. `git status --short` was empty.
The source archive SHA256 is
`9AB925BE3E37A895D541DA709006DAFA55EA83CAC2FC0A14E6FBAB14600A0BFA`.
The source reports ViennaCore `2.2.0`; header hashes are:

```text
vcKDTree.hpp CA71180468668BB27508F18BC56DC3E333C2698AFD359495AEFA7D0872CBE5DD
vcQueues.hpp 30CBDF751AF77812599C8FF9493782C8E88CD538A1540AA18EE8CE058EA64AB5
vcLogger.hpp 0C2B43B5AD98CEC3C2A30BA2283A34338C96D811EC2ABE60623912FEBE3E6117
CMakeLists.txt 2668A002BB91ABD685F27B795A8B95BF5EEC3F6C1C6D5F2C54FEA462CAAF80B2
```

The only non-standard include is `viennacore/vcKDTree.hpp`; no ViennaPS,
ViennaRay, ViennaLS, ViennaHRLE, ViennaCS, Embree, Vulkan, or Mod include was
present. The exact build command was recorded in
`.tmp_p5_neutral_upstream_repro_20260813/build-command.txt` and used:

```text
MSVC cl.exe 19.44.35223 (14.44.35207 Hostx64/x64)
/nologo /TP /EHsc /O2 /Ob2 /DNDEBUG /openmp:llvm /MD /Zi /std:c++20
```

The link command had no explicit libraries beyond the CRT/OpenMP closure and
produced `pure_kdtree_repro.exe` (SHA256
`87A1420BA927DE782A6985DA6F9F18C9CB9B8BF97E191CBBAEA1790D674FA5EB`).
`dumpbin /DEPENDENTS` listed only `MSVCP140.dll`,
`libomp140.x86_64.dll`, `KERNEL32.dll`, `VCRUNTIME140.dll`,
`VCRUNTIME140_1.dll`, and the API-set CRT DLLs; Embree and TBB were absent.
The clean process PATH put the MSVC 14.44.35112 CRT directory first, then the
LLVM OpenMP redist directory, then `C:\Windows\System32` and `C:\Windows`.
Resolved runtime hashes include `msvcp140.dll`
`0F885B509A685D2BBFA652FED26B5FB31D88FBDAB0A978C641D1C7B8AA460AA9`,
`vcruntime140.dll`
`D5E4D9A3E835FA679450145D6A7D94E36573A509317111904D9B3712C30D9066`,
`vcruntime140_1.dll`
`1F2D41C4AA5DB0BC33EBF7B66D72943A817D7CE6CBE880502A9403823633093F`, and
`libomp140.x86_64.dll`
`9AB1CB787E52B2A36133899C01C1DB1F876067D1471CD224EAD85F40A8152B99`.

The fixed six-point `float/std::array<float,3>` program completed `build()`
and six radius queries in all four required lanes. Each lane exited `0`,
reported `sizeof_point=12`, `alignof_point=4`, six one-result queries with
indexes `0..5`, `total_results=6`, and `oracle=PASS`. The four stdout files
were byte-identical (SHA256
`6B30C4E0120932CE8E8394AECDF38D8FF5D07B74E35E7137CF19A72569C56741`); the
empty stderr hash was
`E3B0C44298FC1C149AFBF4C8996FB92427AE41E4649B934CA495991B7852B855`.
Machine-readable evidence is `matrix-summary.json`, with four entries and
`all_pass=True`.

This is an explicit `NOT_REPRODUCED` result for Lane A: the pure upstream
KDTree does not crash under the required flags. As required by Lane D, the
unchanged paired wrappers were then rerun with the Embree/TBB/CRT/OpenMP
runtime closure. Both wrote zero-byte fixture outputs and exited `128` after
catching `0xc0000005`: Mod address `0x00007FF7970603F2`, reference address
`0x00007FF678590022`; both symbolized through
`KDTree::traverseDown+0x52` -> `findNearestWithinRadius+0x8c` and the same
`ElementToPointData`/`CPUTriangleEngine` caller chain. The paired logs and
zero-byte outputs are `paired_mod.log`, `paired_reference.log`,
`paired_mod.out`, and `paired_reference.out` under the temporary root.

The first paired-rerun shell command had an invalid inline PowerShell `if`
expression and produced no run; one named correction replaced it with an
explicit executable map, after which both wrappers ran as above. No second
correction was attempted. The evidence therefore narrows the fault to the
composed ViennaPS/ElementToPointData deployment boundary or its interaction
with the shared closure; it is not a standalone upstream ViennaCore KDTree
reproduction. No upstream patch, runtime lock, dependency update, or oracle
unlock is authorized by this result.

### Lane B runtime-only rerun (2026-08-18)

To isolate the CRT without changing source, compiler flags, OpenMP mode, input,
or ABI, the unchanged paired crash-capture executables were rerun with
`C:\Windows\System32` first on `PATH`, followed by the existing Embree/TBB
directories. This selected the System32 CRT hashes
`msvcp140.dll=7C26614E1D733892C2DEAC7E245CE115504B1D80592DD0A01B08E3E5A55F89CA`
and
`vcruntime140.dll=D1F4225DF2CD877DBF130D5668A021DCE3F94118455FF5EC952061C30AFC9CE7`;
`libomp140.x86_64.dll` remained the same
`9AB1CB787E52B2A36133899C01C1DB1F876067D1471CD224EAD85F40A8152B99`.

The Mod and reference wrappers both exited `128` in OMP 1/2/4/8. Every lane
captured the same `0xc0000005` chain through
`KDTree::traverseDown+0x52` -> `findNearestWithinRadius` ->
`ElementToPointData::prepare$omp$1`, with no accepted fixture output. The
machine-readable matrix and raw logs are under
`.tmp_p5_neutral_upstream_repro_20260813/laneB_system32_crt_20260818/`.
This runtime-only lane therefore does not explain or repair the composed
boundary fault; the parent Release CPU oracle remains `EVIDENCE-GAP-MAIN` and
no CRT/runtime lock is authorized.
