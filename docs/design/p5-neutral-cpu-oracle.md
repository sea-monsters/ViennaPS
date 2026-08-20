
## P5-N2 acceptance matrix: main-line gate rerun (2026-08-20)

The plan's main-line gate ("rebuild both sides independently and rerun the
full matrix before updating the board") was executed after the N1H
adoption. Both fixture sides and the checker were rebuilt from scratch
into a fresh evidence directory `.tmp_p5_n2_gate_20260820`, using the
CPM-patched ViennaCore cache headers natively fetched by the adopted
patch (`v2.2.1-kdtree-nullcheck-31f6423c177a320d`) with no overlay and no
ViennaCore source-tree edit.

Matrix result — every cell `PASS`:

| Item | Evidence |
|---|---|
| Mod and unmodified reference | `neutral_cpu_oracle_mod` / `neutral_cpu_oracle_reference` rebuilt independently |
| OMP 1/2/4/8 | 4/4 configurations run on both sides |
| Exact Release flags | `/O2 /Ob2 /DNDEBUG /openmp:llvm /MD /Zi` (runner `FlagSet=Release`) |
| Identical dependency/runtime closure | same patched ViennaCore cache, same cached ViennaLS/ViennaRay/ViennaHRLE/ViennaCS/Embree/TBB |
| exit 0, no SEH exception | all 8 fixture runs exit 0 |
| Raw serialized equality | `empty=exact active=exact flux=exact geometry=exact process=exact max_ulp=0` at every OMP count |
| Reproducible commands | listed below |

Reproducible command shape (per step; `-Threads` in {1,2,4,8}):

```text
pwsh -File tests/neutralCpuReferenceDifferential/run_paired_oracle.ps1 `
  -Step <BuildMod|BuildReference|BuildChecker|RunMod|RunReference|Check> `
  -ViennaCoreOverride <patched ViennaCore cache dir> `
  -ConfiguredBuildDirectory <build tree providing Embree/TBB closure> `
  -OutputDirectory .tmp_p5_n2_gate_20260820
```

`P5-NEUTRAL-CPU-ORACLE` is closed. The required Release paired oracle that
blocked `P5-SURFACE-INTEGRATION` now passes; the surface fixture enters
RED under `P5-S0` per the critical path.
