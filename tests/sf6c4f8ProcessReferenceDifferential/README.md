# SF6C4F8 CPU reference differential

This card records only the rank-6 `SF6C4F8Etching<float, 2>` CPU/fallback row.
The fixture uses one deterministic `CPU_TRIANGLE` plane, fixed seed `42`, one
ray per point, `maxReflections=0`, source normalization, and the explicit
`passivationFlux=0` branch. It serializes all four active CPU labels:

- `ionSputterFlux`
- `ionEnhancedFlux`
- `ionEnhancedPassivationFlux`
- `etchantFlux`

The unmodified `D:\Codex_lib\code_reference\ViennaPS` and Mod trees are built
with the same Release flags and compared byte-for-byte at OMP 1/2/4/8. The
fixture checks finite/non-negative labels and deterministic triangle geometry.

This is not a Vulkan plasma/polymer transport or surface route. Auto remains
CPU-authoritative and Manual Vulkan remains fail-closed without publication; no
eligibility predicate, CUDA promotion, or production model behavior is changed
by this test.
