# P5 frontier reference differential — observer seam checkpoint

Status: `DONE-LOCAL-NARROW` for
`P5-RAY-MULTIBOUNCE-FRONTIER-REFERENCE-ORACLE` (2026-08-13). The authorized
test-only observer yields an independently compiled, paired ViennaRay
raw-trace oracle. It is not a full ViennaPS `Process`, model, or Vulkan claim.

## Authority and seam

The reference CPU triangle engine owns a `viennaray::TraceTriangle` and only
configures source, geometry, material IDs, limits, and particle type before
calling `rayTracer_.apply()`
(`D:\Codex_lib\code_reference\ViennaPS\include\viennaps\process\psCPUTriangleEngine.hpp`,
lines 11 and 277). The locked ViennaRay `TraceKernel::apply` owns the Embree
`do/while`, per-ray RNG, callbacks, reflection, limits, and roulette
(`.cpm-cache\viennaray\0fe9\include\viennaray\rayTraceKernel.hpp`, lines 32,
118, 155, 297, and 380).

Before this card there was no public fixed-hit frontier injection or event
observer. The new `VIENNAPS_P5_TRACE_OBSERVER` macro adds an append-only,
test-only observer at real intersection/reflection boundaries. It records
fixed-width raw FP32 hit/state fields, identity, sequence, action, and
reflection count without mutating ray state, callbacks, geometry, or RNG. The
macro is absent by default, so production behavior and public APIs are unchanged.

The reference emitter resolves `D:\Codex_lib\code_reference\ViennaPS` plus
`.tmp_reference_viennaray`; the Mod emitter resolves
`D:\Codex_lib\ViennaPSMod` plus `.cpm-cache\viennaray\0fe9`. The two ViennaRay
source trees matched before instrumentation and remain independent test build
paths. The checker compares complete serialized byte streams, including actual
Philox post-branch state; Vulkan output is not an oracle.

## RED/GREEN evidence

RED previously proved that a same-header helper or copied TraceKernel would not
be independent evidence. The seam now avoids that violation without changing
the TraceKernel branch logic. The paired test is reproducible with:

```text
& tests\multibounceFrontierReferenceDifferential\run_paired_oracle.ps1 -OutputDirectory .tmp_p5_trace_script
```

The command compiles two test translation units with the corresponding
instrumented ViennaRay include tree and the existing validated Embree 4.3.3
library, avoiding a redundant dependency rebuild. It passed with
`multibounce frontier reference differential PASS raw_bytes=3072`. The checker
requires four actual hits/collision/reflection/weight transitions, two
high-weight continuations, a low-weight roulette branch that advances Philox,
and a third-hit reflection-limit termination that does not. Reference and Mod
records were raw-byte equal. The known Release KDTree crash is unrelated
because this fixture does not enter `Process` mesh construction.

The exact seam contract is frozen in
[`SEAM_DESIGN.md`](SEAM_DESIGN.md). The remaining gates are a CMake/CTest
registration run without rebuilding dependencies, the existing Intel Arc
frontier consumer smoke, then the separate Process/model acceptance cards.
