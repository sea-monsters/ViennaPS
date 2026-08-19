# P5 reference trace-observer seam design

Status: `DONE-LOCAL-NARROW` for `P5-RAY-MULTIBOUNCE-REFERENCE-SEAM-AUTHORITY`
(2026-08-13). The test-only seam and a paired raw-trace run are complete; this
does not exercise a ViennaPS `Process` or unlock a model row.

## Milestone and boundary

The milestone is an independently compiled three-bounce CPU trace pair: the
reference emitter resolves `D:\Codex_lib\code_reference\ViennaPS` and the
test-only `.tmp_reference_viennaray` tree, while the Mod emitter resolves the
workspace and `.cpm-cache\viennaray\0fe9`. Both execute the real ViennaRay
`TraceKernel`; neither copies its branch logic, uses Vulkan output, or replaces
callbacks/RNG with a fixture loop. This is a ViennaRay state-order oracle, not
a complete ViennaPS `Process` differential. The next unlock is its narrow
frontier conformance evidence, never Process/model promotion.

The preferred seam is an observer compiled only for a test target. It is
disabled by default and has no production API or link dependency. At branch
boundaries the real `TraceKernel` calls an observer with a fixed-width record:
`rayId`, `bounce`, `sequence`, kind, primitive/material ID, callback/action,
reflection count, FP32 raw bits for `t/u/v`, origin, direction, weight,
next-weight and successor. For the locked Philox RNG it also records the real
post-branch `counter`, `key` and `output_index` state without taking a draw.
The observer is append-only and receives no authority to mutate ray state,
callbacks, geometry, or RNG. Output I/O failure produces a malformed/missing
artifact and the checker fails; it never changes the trace transition.

The seam must be injected at the TraceKernel call sites that already own the
ordered state: after intersection/classification, after `surfaceCollision`,
after `surfaceReflection`/weight update, after reflection-limit and roulette,
and at termination. The host callback and RNG remain the implementation; the
observer only serializes their actual arguments/results. A fixed-hit seam is a
fallback only if observer instrumentation cannot be added: it must be an
explicit test constructor/input, bounded to a declared hit sequence, and prove
that no source, boundary, callback, or RNG branch is bypassed. No fixed-hit seam
may become a public/default TraceKernel behavior.

## Ownership, ABI, security, and default behavior

ViennaRay owns observer invocation and record timing; the reference and Mod
test emitters own output files and comparison. On the locked MSVC ABI a
`static_assert` fixes `Record` to 124 bytes; each record is prefixed by the
little-endian `0x50355452` magic and FP32 fields are stored as `uint32` bits.
The observer pointer is null by default, and production builds never define the
test macro. The macro is private to test emitters; no public ViennaPS header
exposes it. The observer accepts no callbacks or input-controlled function
pointers, and output paths are caller-selected test artifacts only.

## Ordered exits and acceptance

1. RED: prove the current unmodified reference/Mod trees have no observer or
   fixed-hit seam and that a same-header helper is not independent evidence.
2. Add the smallest test-only observer seam, preserving the default ABI and
   exact TraceKernel branch order.
3. Build and run separate emitters. Compare ordered raw records for three
   high-weight bounces plus a low-weight roulette rejection: hit `t/u/v`,
   callback order, successor vectors, actions/reasons, weights, reflection
   counts and real Philox post-branch state. The checker additionally verifies
   that high-weight continuation and reflection-limit termination preserve RNG
   state, while the low-weight roulette branch advances it.
4. Run the existing frontier smoke unchanged; its GPU result is a consumer check,
   not a CPU oracle. Release/Intel evidence is additive and does not unlock
   Process or model matrix.

If the authorized reference/ViennaRay seam cannot be safely isolated, the exact
path/error must be reported and the card remains blocked; no TraceKernel
reimplementation, compiler-flag workaround, or weakened raw-bit assertion is
permitted.

## Local paired evidence

`run_paired_oracle.ps1 -OutputDirectory .tmp_p5_trace_script` compiles two
separate test translation units with their respective ViennaRay trees and the
same already-validated Embree 4.3.3 binary. It completed with
`multibounce frontier reference differential PASS raw_bytes=3072`. The 24
records contain four real geometry hits, four collision/reflection/weight
transitions, two high-weight continuations, one roulette rejection that
advances Philox state, and one third-hit reflection-limit termination that does
not. This evidence is intentionally local CPU trace conformance only.
