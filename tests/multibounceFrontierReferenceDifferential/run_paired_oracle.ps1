param(
  [string]$OutputDirectory = "$PSScriptRoot\.tmp_manual",
  [string]$ReferenceViennaPS = "D:\Codex_lib\code_reference\ViennaPS",
  [string]$ReferenceViennaRay = "$PSScriptRoot\..\..\.tmp_reference_viennaray",
  [string]$BuildDirectory = ""
)
$ErrorActionPreference = 'Stop'
$root = (Resolve-Path "$PSScriptRoot\..\..").Path
if (-not $BuildDirectory) { $BuildDirectory = Join-Path $root 'build' }
$observerDirectory = (Resolve-Path $PSScriptRoot).Path
# Cache-resident dependencies: resolve through the MAIN repository when
# running from a linked worktree (generic git-based discovery).
$commonDir = (& git rev-parse --path-format=absolute --git-common-dir 2>$null)
$cacheRoots = @($root)
if ($LASTEXITCODE -eq 0 -and $commonDir) {
  $mainRepoRoot = Split-Path -Parent ([System.IO.Path]::GetFullPath($commonDir))
  if ($mainRepoRoot -ne $root)
    { $cacheRoots += $mainRepoRoot }
}
function Resolve-CacheDir([string]$relative) {
  foreach ($cacheRoot in $cacheRoots) {
    $candidate = Join-Path $cacheRoot $relative
    if (Test-Path $candidate) {
      return $candidate
    }
  }
  throw "Cache directory not found: $relative (searched: $($cacheRoots -join '; '))"
}
$modViennaRay = Resolve-CacheDir '.cpm-cache\viennaray\0fe9'
$viennaCore = Resolve-CacheDir '.cpm-cache\viennacore\edac'
# Embree include root: three-level resolution (repo root cache -> build tree
# CPM source -> shared CPM cache), mirroring the shared engine resolver.
$embreeCandidates = @(
  (Join-Path $root '.cpm-cache\embree\9311')
)
$commonDir = (& git rev-parse --path-format=absolute --git-common-dir 2>$null)
if ($LASTEXITCODE -eq 0 -and $commonDir) {
  $mainRepoRoot = Split-Path -Parent ([System.IO.Path]::GetFullPath($commonDir))
  $embreeCandidates += (Join-Path $mainRepoRoot '.cpm-cache\embree\9311')
}
$embreeCandidates += (Join-Path $BuildDirectory '_deps\embree-src')
if ($env:CPM_SOURCE_CACHE) {
  $embreeCandidates += (Join-Path $env:CPM_SOURCE_CACHE 'embree\9311')
}
$embree = $null
foreach ($candidate in $embreeCandidates) {
  if ($candidate -and
      (Test-Path (Join-Path $candidate 'include\embree4\rtcore.h'))) {
    $embree = $candidate
    break
  }
}
if (-not $embree) {
  throw ('Embree include root not found; searched: ' +
         ($embreeCandidates -join '; '))
}
$embreeLibrary = Join-Path $BuildDirectory '_deps\embree-build\Release\embree4.lib'
$embreeBin = Split-Path $embreeLibrary
$tbbBinLegacy = Join-Path $BuildDirectory 'gpu\vulkan\ray\Release'
$tbbBinTests = Join-Path $BuildDirectory 'tests'
$tbbBin = if (Test-Path (Join-Path $tbbBinTests 'tbb12.dll')) { $tbbBinTests } else { $tbbBinLegacy }
$source = Join-Path $observerDirectory 'trace_emitter.cpp'
$checkerSource = Join-Path $observerDirectory 'trace_checker.cpp'
$vsDevCmd = 'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\Tools\VsDevCmd.bat'

function Quote-Arg([string]$Value) { '"' + $Value + '"' }
function Invoke-Checked([scriptblock]$Command, [string]$Label) {
  & $Command
  if ($LASTEXITCODE -ne 0) { throw "$Label failed with exit code $LASTEXITCODE" }
}
function Require-Path([string]$Path, [string]$Label) {
  if (-not (Test-Path $Path)) { throw "$Label missing: $Path" }
}
function Build-Emitter([string]$Output, [string]$ViennaPS, [string]$ViennaRay,
                       [bool]$Reference) {
  $definitions = @('/DVIENNAPS_P5_TRACE_OBSERVER=1',
                   '/DVIENNARAY_EMBREE_VERSION=4',
                   '/DVIENNARAY_USE_RAY_MASKING=1')
  if ($Reference) { $definitions += '/DVIENNAPS_P5_REFERENCE_EMITTER=1' }
  $includeArgs = @($observerDirectory, (Join-Path $ViennaPS 'include'),
                   (Join-Path $ViennaRay 'include\viennaray'),
                   (Join-Path $viennaCore 'include\viennacore'),
                   (Join-Path $embree 'include')) |
    ForEach-Object { '/I ' + (Quote-Arg $_) }
  $cmd = 'call ' + (Quote-Arg $vsDevCmd) + ' -arch=x64 >nul && cl /nologo ' +
    '/std:c++20 /EHsc /openmp:llvm /MD /O2 /DNDEBUG ' +
    ($definitions -join ' ') + ' ' + ($includeArgs -join ' ') + ' /Fe:' +
    (Quote-Arg $Output) + ' ' + (Quote-Arg $source) + ' /link ' +
    (Quote-Arg $embreeLibrary)
  Invoke-Checked { & cmd.exe /d /s /c $cmd } "Emitter build ($Output)"
}
function Build-Checker([string]$Output) {
  $cmd = 'call ' + (Quote-Arg $vsDevCmd) + ' -arch=x64 >nul && cl /nologo ' +
    '/std:c++20 /EHsc /MD /O2 /DNDEBUG /I ' + (Quote-Arg $observerDirectory) +
    ' /Fe:' + (Quote-Arg $Output) + ' ' + (Quote-Arg $checkerSource)
  Invoke-Checked { & cmd.exe /d /s /c $cmd } 'Trace checker build'
}

Require-Path $ReferenceViennaPS 'Reference ViennaPS'
Require-Path $ReferenceViennaRay 'Reference ViennaRay'
Require-Path $modViennaRay 'Mod ViennaRay'
Require-Path $viennaCore 'Shared ViennaCore'
Require-Path $embreeLibrary 'Pinned Embree import library'
New-Item -ItemType Directory -Force $OutputDirectory | Out-Null
$modExe = Join-Path $OutputDirectory 'multibounce_frontier_mod_emitter.exe'
$refExe = Join-Path $OutputDirectory 'multibounce_frontier_reference_emitter.exe'
$checker = Join-Path $OutputDirectory 'multibounce_frontier_trace_checker.exe'
$refTrace = Join-Path $OutputDirectory 'reference.trace'
$modTrace = Join-Path $OutputDirectory 'mod.trace'

# This is deliberately a manual, two-TU test build: a fresh CMake tree would
# rebuild the pinned Embree source. Both emitters use the same existing Embree
# binary but different ViennaRay source trees and compile definitions matching
# the ViennaRay interface target (including ray masking).
Build-Emitter $modExe $root $modViennaRay $false
Build-Emitter $refExe $ReferenceViennaPS $ReferenceViennaRay $true
Build-Checker $checker

$previousPath = $env:Path
try {
  $env:Path = "$embreeBin;$tbbBin;$previousPath"
  Invoke-Checked { & $refExe $refTrace } 'Reference emitter'
  Invoke-Checked { & $modExe $modTrace } 'Mod emitter'
  Invoke-Checked { & $checker $refTrace $modTrace } 'Raw trace checker'
} finally {
  $env:Path = $previousPath
}
