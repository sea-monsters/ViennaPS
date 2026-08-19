# MultiParticle CPU reference differential

This card closes only the rank-2 CPU/fallback evidence for
`MultiParticleProcess<float, 2>`:

- the unmodified `D:\Codex_lib\code_reference\ViennaPS` and Mod CPU
  implementations are compiled with the same Release flags and compared as
  raw serialized flux/geometry output at OMP 1/2/4/8;
- the fixture freezes one neutral and one ion label, fully sticking particles,
  `maxReflections=0`, source normalization, and the CPU triangle engine;
- `model_matrix_fallback_smoke` remains the Vulkan contract: Auto stays on the
  CPU model and Manual Vulkan fails closed before publication.

This is not a Vulkan species/energy/material implementation and does not unlock
the aggregate model matrix, surface integration, or deployment exit.
