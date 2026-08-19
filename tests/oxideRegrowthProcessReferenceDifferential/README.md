# OxideRegrowth CPU/reference differential (P5 model-matrix row 10)

This is a CPU-authority fallback row for the production
`OxideRegrowth<float, 2>` model. It runs the existing `Process` analytic
advection path and its `ByproductDynamics::applyPreAdvect` then
`applyPostAdvect` callback on a deterministic 2D stack with a generated dense
cell set. The fixture serializes the active `byproductSum`, `Material`, and
filling-fraction fields, finite/nonnegative guards, a byproduct mass residual,
and final dense-cell geometry (2D nodes/elements; this model has no flux
triangle mesh).

The paired runner builds the fixture against the Mod tree and the unmodified
`D:\Codex_lib\code_reference\ViennaPS` tree with Release `/O2 /Ob2 /DNDEBUG
/openmp:llvm /MD /Zi`, then checks raw output bytes at OMP 1/2/4/8. The row
uses `auto_route=CPU`; Manual Vulkan remains `fail_closed_no_publication`.
The `seed=42`, ray labels, and SOURCE normalization are recorded as frozen
fixture metadata for matrix consistency, but OxideRegrowth has no particle or
flux-engine path; this row does not claim ray, Vulkan, hardware, or full
surface acceptance.

The mass check is bounded to the callback's serialized nonnegative
`byproductSum` state (`initialByproductMass=0` and finite nonnegative final
residual). It is not a claim of a complete physical conservation oracle; the
production callback's diffusion/redeposition solver remains CPU-only.

## Manual validation

```powershell
pwsh -NoProfile -ExecutionPolicy Bypass -File .\tests\oxideRegrowthProcessReferenceDifferential\run_paired_oracle.ps1 -OutputDirectory .\.tmp_oxide_regrowth_row10_red_20260818
cmake -S tests/oxideRegrowthProcessReferenceDifferential -B .tmp_oxide_regrowth_cmake_vs_20260818 -G Ninja -DBUILD_TESTING=ON
ctest --test-dir .tmp_oxide_regrowth_cmake_vs_20260818 --output-on-failure
```

No production CPU formula, Process route, reference tree, cache, Vulkan
kernel, or model eligibility is changed by this row.
