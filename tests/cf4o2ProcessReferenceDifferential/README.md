# CF4O2 CPU reference differential

This card records only the rank-4 `CF4O2Etching<float, 2>` CPU/fallback row.
The fixture uses one deterministic `CPU_TRIANGLE` plane, fixed seed `42`, one
ray per point, `maxReflections=0`, and source normalization. It serializes the
four ion CPU labels exposed by the model:

- `ionSputterFlux`
- `ionEnhancedFlux`
- `ionEnhancedOxidationFlux`
- `ionEnhancedPassivationFlux`

The unmodified `D:\Codex_lib\code_reference\ViennaPS` and Mod trees are built
with the same Release flags and compared byte-for-byte at OMP 1/2/4/8. The
fixture checks finite/non-negative labels and deterministic triangle geometry.

This is not a Vulkan model route. Auto remains CPU-authoritative and Manual
Vulkan remains fail-closed without publication; no eligibility predicate or
production model behavior is changed by this test.
