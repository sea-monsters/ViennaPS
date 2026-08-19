# P5 strict Row-01 CPU reference differential

This test freezes the CPU authority for exactly one narrow P5 model-matrix
row: `SingleParticleProcess<float,2>`, one default particle-data label,
default source, fixed seed 42, `raysPerPoint=1`, SOURCE normalization, and
`maxReflections=0`. It does not dispatch Vulkan or claim support for another
model, source, precision, dimension, label count, reflection mode, surface
model, or Process route.

`run_paired_oracle.ps1` compiles two independent Release translation units
under `/O2 /Ob2 /DNDEBUG /openmp:llvm /MD /Zi`: one includes this workspace's
ViennaPS, the other includes the unmodified
`D:\Codex_lib\code_reference\ViennaPS` authority tree. Both use the CMake
locked ViennaCore/ViennaLS/ViennaHRLE/ViennaRay/ViennaCS/Embree closure. They
serialize the CPU `Process::calculateFlux()` flux cell-data, output geometry,
and basic Process observables as raw FP32 bytes. The checker requires byte
equality for OMP 1, 2, 4, and 8.

The reference compatibility VTK declarations are parse-only definitions for
uninstantiated no-VTK members; neither is constructed. The test changes no
production or reference headers. Passing it supplies CPU-reference evidence
only. Intel Arc CPU/Vulkan Process parity, conservation, and fail-closed
fallback are covered by separate P5 Row-01 smokes; aggregate model support
remains blocked by the remaining model rows and the Neutral surface oracle.
