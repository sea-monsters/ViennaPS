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

& (Join-Path $PSScriptRoot '..\common\run_reference_oracle.ps1') `
  -TestDirectory $PSScriptRoot `
  -FixtureSource 'sf6c4f8_cpu_fixture.cpp' `
  -CheckerSource 'sf6c4f8_cpu_checker.cpp' `
  -ModDefine '/DVIENNAPS_SF6C4F8_MOD' `
  -OutputDirectory $OutputDirectory `
  -ReferenceViennaPS $ReferenceViennaPS `
  -BuildDirectory $BuildDirectory