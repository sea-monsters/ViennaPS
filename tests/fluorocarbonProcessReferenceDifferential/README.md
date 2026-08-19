# FluorocarbonEtching CPU fallback differential

This is the rank-7 CPU-authority/fallback row for the concrete production
`FluorocarbonEtching<float, 2>` model. The inventory also lists
`PlasmaEtching` beside `FluorocarbonEtching`; neither tree contains a
production `PlasmaEtching<float, 2>` class, so this evidence does not cover or
rename that absent type.

The fixture uses a deterministic 2-D `CPU_TRIANGLE` plane with grid delta
`0.5`, extents `10 x 10`, seed `42`, one ray per point, zero reflections, and
`SOURCE` normalization. It freezes ion/etchant/polymer fluxes `56/500/100`,
`delta_p=1`, temperature `300`, `k_ie=2`, `k_ev=2`, ion energy `100`, zero
energy sigma, exponent `300`, and explicit Si/Mask/Polymer material entries.

All five active CPU labels are serialized:

```text
ionSputterFlux, ionEnhancedFlux, ionpeFlux, etchantFlux, polyFlux
```

Each label is checked for finite, nonnegative values, equal cardinality, and
the output also serializes finite triangle geometry. Mod and the unmodified
`D:\Codex_lib\code_reference\ViennaPS` tree are compiled with
`/O2 /Ob2 /DNDEBUG /openmp:llvm /MD /Zi` and compared byte-for-byte at OMP
1/2/4/8.

This row is CPU-only fallback evidence: Auto selects CPU and Manual Vulkan is
fail-closed without publication. It does not widen ray eligibility, promote
CUDA, add a Vulkan seam, alter production formulas/Process order, or claim
hardware/full-surface acceptance. A separate PlasmaEtching production row
cannot be claimed until that type exists and is separately evidenced.
