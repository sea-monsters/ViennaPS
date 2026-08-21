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
  -FixtureSource 'single_particle_ald_cpu_fixture.cpp' `
  -CheckerSource 'single_particle_ald_cpu_checker.cpp' `
  -ModDefine '/DVIENNAPS_SINGLE_PARTICLE_ALD_MOD' `
  -OutputDirectory $OutputDirectory `
  -ReferenceViennaPS $ReferenceViennaPS `
  -BuildDirectory $BuildDirectory