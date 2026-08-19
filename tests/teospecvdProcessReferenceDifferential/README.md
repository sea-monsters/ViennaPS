# TEOSPECVD CPU fallback differential

This is the rank-10 CPU-authority/fallback row for the concrete production
`TEOSPECVD<float, 2>` model. The CPU constructor always installs both radical
and ion particles; therefore the fixture keeps both active labels and freezes
the surface reaction as radical-only with `ionRate=0`. It does not invent an
ion-disabled model or change production behavior.

The fixture uses a deterministic 2-D `CPU_TRIANGLE` plane with grid delta
`0.5`, extents `10 x 10`, seed `42`, one ray per point, zero reflections, and
`SOURCE` normalization. Parameters are `radicalSticking=1`, `radicalRate=1`,
`radicalOrder=1`, `ionRate=0`, `ionSticking=1`, `ionExponent=300`,
`ionOrder=1`, and `ionMinAngle=85` (the production CPU constructor's
parameter convention).

The active CPU labels are:

```text
radicalFlux, ionFlux
```

Both labels are checked for finite, nonnegative values, equal cardinality,
and the output serializes finite triangle geometry. Mod and the unmodified
`D:\Codex_lib\code_reference\ViennaPS` tree are compiled with
`/O2 /Ob2 /DNDEBUG /openmp:llvm /MD /Zi` and compared byte-for-byte at OMP
1/2/4/8.

This row is CPU-only fallback evidence: Auto selects CPU and Manual Vulkan is
fail-closed without publication. It does not add a Vulkan PECVD transport or
surface seam, promote CUDA, widen ray eligibility, alter radical/ion labels,
reaction powers, sticking, or Process order, or claim hardware/full-surface
acceptance.
