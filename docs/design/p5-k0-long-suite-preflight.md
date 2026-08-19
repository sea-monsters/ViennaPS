# P5-K0 top-level long-suite preflight

## Milestone and position

- **Milestone:** `P5-K0` diagnostic top-level preflight; it does not claim
  `P5-K1`, surface/matrix progress, or P5 completion.
- **Predecessor:** `P5-X0` accepted; the Wave 1 `P5-N1` lane is
  `BLOCKED-EXTERNAL` and does not prevent this diagnostic run.
- **Next unlock:** provide K1 with the observed build boundary, first failure,
  timeout/flaky classification, and remaining-suite risk. K1 still requires
  its own accepted `P5-S1`/`P5-M0` candidate and complete long-suite run.

### Global position (approximately 150 tokens)

The existing CPU-only Release configuration was reused in the dedicated
`codex/p5-k0-preflight` worktree at baseline `b9e23fb`; no reconfigure or
production edit was made. The resumed focused target completed after the
interrupted Embree compilation and produced `backendPolicy.exe`. CTest
discovery registered 94 tests. The focused test passed after one named
configuration correction. The next individually selected test could not run
because its executable was not built. This is diagnostic evidence only:
K0 did not establish a complete suite, a Vulkan result, or K1 acceptance.

## Validation mutex receipt

The global validation lock was held exclusively for this card after the
updated serialization rule took effect. The earlier `--parallel 2` focused
build predates that rule and is retained only as historical preparation
evidence; it is not a serialization-compliant Wave 1 build gate. After the
rule took effect, no build was started and each CTest invocation selected one
test by an anchored regular expression. After each action, process state was
audited. No post-rule command overlapped another `cmake`, compiler, CTest, or
Vulkan process.

## Evidence

Worktree: `codex/p5-k0-preflight`, HEAD `b9e23fb7c1d4bf0f00a4f6dbb83db94289a48bb8`.
The existing `.tmp_p5_k0_cpu/` configuration was reused; no configure was
rerun.

1. Focused build resume (existing configuration):

   ```powershell
   pwsh -File cmake/invoke-cmake-clean-env.ps1 --build .tmp_p5_k0_cpu --config Release --target backendPolicy --parallel 2
   ```

   Result: exit 0. Embree Release libraries and `tests/backendPolicy.exe`
   were built. The original interrupted log contained no compile or link
   error; the resumed log reaches the `backendPolicy.vcxproj` success line.

2. CTest inventory:

   ```powershell
   ctest --test-dir .tmp_p5_k0_cpu -N
   ```

   Result: exit 0; 94 tests registered.

3. Focused test, initial invocation:

   ```powershell
   ctest --test-dir .tmp_p5_k0_cpu --output-on-failure --timeout 300 -R '^backendPolicy$'
   ```

   Result: exit 8, `Not Run` because the Visual Studio configuration was
   omitted (`Missing "-C <config>"`). This was the one permitted named
   command correction, not a product failure.

4. Focused test after the named correction:

   ```powershell
   ctest --test-dir .tmp_p5_k0_cpu -C Release --output-on-failure --timeout 300 -R '^backendPolicy$'
   ```

   Result: exit 0; `backendPolicy` passed 1/1 in 0.14 seconds.

5. Next individual test:

   ```powershell
   ctest --test-dir .tmp_p5_k0_cpu -C Release --output-on-failure --timeout 300 -R '^CSVFileProcess$'
   ```

   Result: exit 8; `CSVFileProcess` was `Not Run` because
   `.tmp_p5_k0_cpu/tests/CSVFileProcess.exe` was absent. This is the second
   blocking condition, so the run stopped without another build or CTest
   invocation.

## Coverage and classification

- **Focused build:** historical pre-mutex PASS for the existing
  `backendPolicy` target; K1 must rebuild its selected target under the current
  one-command/serial policy before treating it as an accepted build gate.
- **Focused CTest:** PASS 1/1 after the allowed command correction.
- **First blocking suite boundary:** `CSVFileProcess` missing executable;
  classification is incomplete build coverage, not a source/test assertion
  failure and not a timeout or flaky result.
- **Timeout/flaky:** none observed.
- **Not covered:** the remaining 92 registered tests, including all Vulkan
  runtime/hardware tests, were not run. This CPU/no-SDK configuration provides
  no hardware acceptance and cannot unlock K1.

## Process cleanup and handoff

At the final audit, `Get-Process -Name cmake,msbuild,cl,ctest,ninja,devenv`
and the Vulkan/Vienna process filter returned no entries. The focused build
and all CTest processes were reaped; no orphan compiler, build, test, or
Vulkan descendant remains.

The generated `.tmp_p5_k0_cpu/` tree and `.tmp_p5_k0_*` logs are retained as
card evidence and are not source deliverables. K1 input is the successful
single focused target, 94-test inventory, `backendPolicy` 1/1 result, and the
first missing-executable boundary at `CSVFileProcess`; K1 must decide whether
to build the remaining targets before its own sequential long-suite gate.
