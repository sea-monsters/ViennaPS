# P5 WetEtching velocity CPU reference differential

This fixture freezes only the isolated `WetEtchingVelocityField<float,2>`
crystal-direction arithmetic. It compiles the same caller-owned source once
against the Mod headers and once against the unmodified
`D:\Codex_lib\code_reference\ViennaPS` headers, under the Release
`/O2 /Ob2 /DNDEBUG /openmp:llvm /MD` closure. The serialized FP32 values must
match byte-for-byte for OMP 1, 2, 4 and 8.

The fixture is a CPU semantic oracle only. It does not install a Process or
TranslationField callback, change Level Set ownership, or claim WetEtching
model support. The Vulkan executor smoke separately checks the same inputs on
the real adapter, transactional malformed-input/reset sentinels, and a named
32-ULP device-vs-CPU candidate bound.
