# SPDX-License-Identifier: MIT
#
# Thin wrapper over the shared paired reference-oracle engine
# (tests/common/run_reference_oracle.ps1). Kept as a separate file so the
# recorded manual invocation surface (this script plus -OutputDirectory and
# -BuildDirectory) stays stable across the P5 records.

param(
  [string]$OutputDirectory = '',
  [string]$ReferenceViennaPS = 'D:\Codex_lib\code_reference\ViennaPS',
  [string]$BuildDirectory = ''
)

# The reference snapshot pairs with ViennaCS v2.1.2; its fixture uses the
# embedded-boundary DenseCellSet API absent from the Mod tree's v2.0.1.
& (Join-Path $PSScriptRoot '..\common\run_reference_oracle.ps1') `
  -TestDirectory $PSScriptRoot `
  -FixtureSource 'oxide_regrowth_cpu_fixture.cpp' `
  -CheckerSource 'oxide_regrowth_cpu_checker.cpp' `
  -ModDefine '/DVIENNAPS_OXIDEREGROWTH_MOD' `
  -OutputDirectory $OutputDirectory `
  -ReferenceViennaPS $ReferenceViennaPS `
  -BuildDirectory $BuildDirectory `
  -ReferenceViennaCS 'D:\Codex_lib\code_reference\ViennaCS'