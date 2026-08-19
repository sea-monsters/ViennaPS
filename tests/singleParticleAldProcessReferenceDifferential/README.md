# SingleParticleALD CPU fallback differential

This is the rank-8 CPU-authority/fallback row for the concrete production
`SingleParticleALD<float, 2>` model. It freezes ballistic transport
(`gasMeanFreePath=-1`) with no evaporation and no surface diffusion, while
serializing the CPU-owned ALD coverage field.

The fixture uses a deterministic 2-D `CPU_TRIANGLE` plane with grid delta
`0.5`, extents `10 x 10`, seed `42`, one ray per point, zero reflections, and
`SOURCE` normalization. Parameters are `stickingProbability=1`,
`growthPerCycle=0`, `evaporationFlux=0`, `incomingFlux=1`, `s0=1`, and
`coverageDiffusionCoefficient=0`.

The active CPU particle label is:

```text
ParticleFlux
```

The CPU coverage field is also serialized and checked as `Coverage`; it must
be finite and within `[0,1]`. Triangle geometry is checked for finite nodes
and serialized for the paired comparison.

Mod and the unmodified `D:\Codex_lib\code_reference\ViennaPS` tree are
compiled with `/O2 /Ob2 /DNDEBUG /openmp:llvm /MD /Zi` and compared
byte-for-byte at OMP 1/2/4/8.

This row is CPU-only fallback evidence: Auto selects CPU and Manual Vulkan is
fail-closed without publication. It does not add a Vulkan ALD transport or
coverage seam, promote CUDA, widen ray eligibility, alter ALD cycle/coverage
semantics, or claim hardware/full-surface acceptance.
