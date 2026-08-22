# SPDX-License-Identifier: MIT
#
# Shared paired CPU reference-oracle runner (P5-K1-R1-RUNNER-PARAM).
#
# One engine for the fourteen process-level reference-differential tests.
# Each per-test run_paired_oracle.ps1 is a thin wrapper supplying its fixture
# source, checker source, and optional Mod define macro. The compile recipe is
# the N2-recorded Release flag set (/O2 /Ob2 /DNDEBUG /openmp:llvm /MD /Zi).
#
# -BuildDirectory points at a configured CMake tree containing CMakeCache.txt,
# the Embree import library, the ViennaLS import library, and (for the Mod
# side) viennaps.lib. It defaults to the historical '<repoRoot>\build' so
# manually recorded commands remain valid; ctest-driven runs receive it
# explicitly once the test trees provide those artifacts.

param(
  [Parameter(Mandatory = $true)]
  [string]$TestDirectory,
  [Parameter(Mandatory = $true)]
  [string]$FixtureSource,
  [Parameter(Mandatory = $true)]
  [string]$CheckerSource,
  [string]$ModDefine = '',
  [string]$OutputDirectory = '',
  [string]$ReferenceViennaPS = 'D:\Codex_lib\code_reference\ViennaPS',
  [string]$BuildDirectory = '',
  # Optional ViennaCS include root for the REFERENCE-side fixture. The
  # reference ViennaPS snapshot pairs with ViennaCS v2.1.2 while the Mod tree
  # pins v2.0.1; fixtures touching embedded-boundary APIs need the matching
  # reference headers. Empty keeps the shared cache-derived include.
  [string]$ReferenceViennaCS = ''
)

$ErrorActionPreference = 'Stop'
$root = (Resolve-Path "$PSScriptRoot\..\..").Path
if (-not $BuildDirectory) { $BuildDirectory = Join-Path $root 'build' }
if (-not $OutputDirectory) { $OutputDirectory = Join-Path $TestDirectory '.tmp_manual' }

$source = Join-Path $TestDirectory $FixtureSource
$checkerSourcePath = Join-Path $TestDirectory $CheckerSource
$vsDevCmd = 'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\Tools\VsDevCmd.bat'

$cache = Join-Path $BuildDirectory 'CMakeCache.txt'

function Quote-Arg([string]$Value) { '"' + $Value + '"' }
function Invoke-Checked([scriptblock]$Command, [string]$Label) {
  & $Command
  if ($LASTEXITCODE -ne 0) { throw "$Label failed with exit code $LASTEXITCODE" }
}
function Require-Path([string]$Path, [string]$Label) {
  if (-not (Test-Path -LiteralPath $Path)) { throw "$Label missing: $Path" }
}
function Cache-SourceDir([string]$Name) {
  $entry = Select-String -LiteralPath $cache -Pattern ("^" + [regex]::Escape($Name) + ":STATIC=") |
    Select-Object -First 1
  if (-not $entry) { throw "CMake cache entry missing: $Name" }
  return (($entry.Line -split '=', 2)[1]).Replace('/', '\\')
}

$viennaCore = Cache-SourceDir 'ViennaCore_SOURCE_DIR'
$viennaLS = Cache-SourceDir 'ViennaLS_SOURCE_DIR'
$viennaHRLE = Cache-SourceDir 'ViennaHRLE_SOURCE_DIR'
$viennaRay = Cache-SourceDir 'ViennaRay_SOURCE_DIR'
$viennaCS = Cache-SourceDir 'ViennaCS_SOURCE_DIR'

# Embree include root: three-level resolution so the engine works from any
# verified worktree, not only from the repository root.
#   1. '<repoRoot>\.cpm-cache\embree\9311'   (main-tree layout)
#   2. '<BuildDirectory>\_deps\embree-src'    (CPM in-tree source)
#   3. "$env:CPM_SOURCE_CACHE\embree\9311"    (shared CPM cache)
function Resolve-EmbreeRoot {
  # Generic resolution: locate the MAIN repository through git so linked
  # worktrees reuse the primary cache without any machine-specific path in
  # tracked files or environment.
  $candidates = @((Join-Path $root '.cpm-cache\embree\9311'))
  $commonDir = (& git rev-parse --path-format=absolute --git-common-dir 2>$null)
  if ($LASTEXITCODE -eq 0 -and $commonDir) {
    $mainRepoRoot = Split-Path -Parent ([System.IO.Path]::GetFullPath($commonDir))
    $candidates += (Join-Path $mainRepoRoot '.cpm-cache\embree\9311')
  }
  $candidates += (Join-Path $BuildDirectory '_deps\embree-src')
  if ($env:CPM_SOURCE_CACHE) {
    $candidates += (Join-Path $env:CPM_SOURCE_CACHE 'embree\9311')
  }
  foreach ($candidate in $candidates) {
    if ($candidate -and
        (Test-Path (Join-Path $candidate 'include\embree4\rtcore.h'))) {
      return $candidate
    }
  }
  throw ('Embree include root not found; searched: ' +
         ($candidates -join '; '))
}
$embree = Resolve-EmbreeRoot
$embreeLibrary = Join-Path $BuildDirectory '_deps\embree-build\Release\embree4.lib'
$viennaLSLibrary = Join-Path $BuildDirectory '_deps\viennals-build\Release\viennals.lib'
$modLibrary = Join-Path $BuildDirectory 'Release\viennaps.lib'
$embreeBin = Split-Path $embreeLibrary
$tbbBinLegacy = Join-Path $BuildDirectory 'gpu\vulkan\ray\Release'
$tbbBinTests = Join-Path $BuildDirectory 'tests'
$tbbBin = if (Test-Path (Join-Path $tbbBinTests 'tbb12.dll')) { $tbbBinTests } else { $tbbBinLegacy }
$crtBin = 'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Redist\MSVC\14.44.35112\x64\Microsoft.VC143.CRT'
$ompBin = 'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Redist\MSVC\14.44.35112\debug_nonredist\x64\Microsoft.VC143.OpenMP.LLVM'

function Build-Fixture([string]$Output, [string]$ViennaPS, [bool]$Mod) {
  $defines = @('/DVIENNARAY_EMBREE_VERSION=4', '/DVIENNARAY_USE_RAY_MASKING',
               '/DVIENNARAY_GPU_DOUBLE_PRECISION', '/DVIENNALS_USE_PRECOMPILED')
  if ($Mod -and $ModDefine) { $defines += $ModDefine }
  $csInclude = $viennaCS
  if (-not $Mod -and $ReferenceViennaCS) { $csInclude = $ReferenceViennaCS }
  $includeArgs = @(
    (Join-Path $ViennaPS 'include\viennaps'),
    (Join-Path $viennaCore 'include\viennacore'),
    (Join-Path $viennaLS 'include\viennals'),
    (Join-Path $viennaHRLE 'include\viennahrle'),
    (Join-Path $viennaRay 'include\viennaray'),
    (Join-Path $csInclude 'include\viennacs'),
    (Join-Path $embree 'include')
  ) | ForEach-Object { '/I' + (Quote-Arg $_) }
  $libraries = @($viennaLSLibrary, $embreeLibrary)
  if ($Mod) { $libraries = @($modLibrary) + $libraries }
  $cmd = 'call ' + (Quote-Arg $vsDevCmd) + ' -arch=x64 >nul && cl.exe /nologo ' +
    '/std:c++20 /EHsc /Zi /O2 /Ob2 /DNDEBUG /openmp:llvm /MD ' +
    ($defines -join ' ') + ' ' + ($includeArgs -join ' ') + ' ' +
    (Quote-Arg $source) + ' /Fe' + (Quote-Arg $Output) + ' /link ' +
    (($libraries | ForEach-Object { Quote-Arg $_ }) -join ' ')
  Invoke-Checked { & cmd.exe /d /s /c $cmd } "Fixture build ($Output)"
}
function Build-Checker([string]$Output) {
  $cmd = 'call ' + (Quote-Arg $vsDevCmd) + ' -arch=x64 >nul && cl.exe /nologo ' +
    '/std:c++20 /EHsc /O2 /DNDEBUG /MD ' + (Quote-Arg $checkerSourcePath) +
    ' /Fe' + (Quote-Arg $Output)
  Invoke-Checked { & cmd.exe /d /s /c $cmd } 'Checker build'
}

Require-Path $ReferenceViennaPS 'Reference ViennaPS'
Require-Path $cache 'Configured CMake cache'
Require-Path $embreeLibrary 'Pinned Embree import library'
Require-Path $viennaLSLibrary 'Pinned ViennaLS import library'
Require-Path $modLibrary 'Mod ViennaPS import library'
New-Item -ItemType Directory -Force $OutputDirectory | Out-Null

$fixtureBase = [IO.Path]::GetFileNameWithoutExtension($FixtureSource)
$modExe = Join-Path $OutputDirectory "${fixtureBase}_mod.exe"
$referenceExe = Join-Path $OutputDirectory "${fixtureBase}_reference.exe"
$checker = Join-Path $OutputDirectory ([IO.Path]::GetFileNameWithoutExtension($CheckerSource) + '.exe')

Build-Fixture $modExe $root $true
Build-Fixture $referenceExe $ReferenceViennaPS $false
Build-Checker $checker

$previousPath = $env:Path
try {
  $env:Path = "$embreeBin;$tbbBin;$crtBin;$ompBin;$previousPath"
  foreach ($threads in 1, 2, 4, 8) {
    $env:OMP_NUM_THREADS = "$threads"
    $referenceOutput = Join-Path $OutputDirectory "reference-omp$threads.txt"
    $modOutput = Join-Path $OutputDirectory "mod-omp$threads.txt"
    Invoke-Checked { & $referenceExe $referenceOutput } "Reference OMP=$threads"
    Invoke-Checked { & $modExe $modOutput } "Mod OMP=$threads"
    Invoke-Checked { & $checker $referenceOutput $modOutput } "Raw checker OMP=$threads"
  }
} finally {
  $env:Path = $previousPath
}
