param(
  [string]$OutputDirectory = "$PSScriptRoot\.tmp_manual",
  [string]$ReferenceViennaPS = "D:\Codex_lib\code_reference\ViennaPS"
)

$ErrorActionPreference = 'Stop'
$root = (Resolve-Path "$PSScriptRoot\..\..").Path
$source = Join-Path $PSScriptRoot 'sf6o2_cpu_fixture.cpp'
$checkerSource = Join-Path $PSScriptRoot 'sf6o2_cpu_checker.cpp'
$cache = Join-Path $root 'build\CMakeCache.txt'
$vsDevCmd = 'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\Tools\VsDevCmd.bat'

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
  return (($entry.Line -split '=', 2)[1]).Replace('/', '\')
}

$viennaCore = Cache-SourceDir 'ViennaCore_SOURCE_DIR'
$viennaLS = Cache-SourceDir 'ViennaLS_SOURCE_DIR'
$viennaHRLE = Cache-SourceDir 'ViennaHRLE_SOURCE_DIR'
$viennaRay = Cache-SourceDir 'ViennaRay_SOURCE_DIR'
$viennaCS = Cache-SourceDir 'ViennaCS_SOURCE_DIR'
$embree = Join-Path $root '.cpm-cache\embree\9311'
$embreeLibrary = Join-Path $root 'build\_deps\embree-build\Release\embree4.lib'
$viennaLSLibrary = Join-Path $root 'build\_deps\viennals-build\Release\viennals.lib'
$modLibrary = Join-Path $root 'build\Release\viennaps.lib'
$embreeBin = Split-Path $embreeLibrary
$tbbBin = Join-Path $root 'build\gpu\vulkan\ray\Release'
$crtBin = 'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Redist\MSVC\14.44.35112\x64\Microsoft.VC143.CRT'
$ompBin = 'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Redist\MSVC\14.44.35112\debug_nonredist\x64\Microsoft.VC143.OpenMP.LLVM'

function Build-Fixture([string]$Output, [string]$ViennaPS, [bool]$Mod) {
  $defines = @('/DVIENNARAY_EMBREE_VERSION=4', '/DVIENNARAY_USE_RAY_MASKING',
               '/DVIENNARAY_GPU_DOUBLE_PRECISION', '/DVIENNALS_USE_PRECOMPILED')
  if ($Mod) { $defines += '/DVIENNAPS_SF6O2_MOD' }
  $includeArgs = @(
    (Join-Path $ViennaPS 'include\viennaps'),
    (Join-Path $viennaCore 'include\viennacore'),
    (Join-Path $viennaLS 'include\viennals'),
    (Join-Path $viennaHRLE 'include\viennahrle'),
    (Join-Path $viennaRay 'include\viennaray'),
    (Join-Path $viennaCS 'include\viennacs'),
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
    '/std:c++20 /EHsc /O2 /DNDEBUG /MD ' + (Quote-Arg $checkerSource) +
    ' /Fe' + (Quote-Arg $Output)
  Invoke-Checked { & cmd.exe /d /s /c $cmd } 'Checker build'
}

Require-Path $ReferenceViennaPS 'Reference ViennaPS'
Require-Path $cache 'Configured CMake cache'
Require-Path $embreeLibrary 'Pinned Embree import library'
Require-Path $viennaLSLibrary 'Pinned ViennaLS import library'
Require-Path $modLibrary 'Mod ViennaPS import library'
New-Item -ItemType Directory -Force $OutputDirectory | Out-Null
$modExe = Join-Path $OutputDirectory 'sf6o2_mod.exe'
$referenceExe = Join-Path $OutputDirectory 'sf6o2_reference.exe'
$checker = Join-Path $OutputDirectory 'sf6o2_cpu_checker.exe'
Build-Fixture $modExe $root $true
Build-Fixture $referenceExe $ReferenceViennaPS $false
Build-Checker $checker

$previousPath = $env:Path
try {
  $env:Path = "$embreeBin;$tbbBin;$crtBin;$ompBin;$previousPath"
  foreach ($threads in 1, 2, 4, 8) {
    $referenceOutput = Join-Path $OutputDirectory "reference-omp$threads.txt"
    $modOutput = Join-Path $OutputDirectory "mod-omp$threads.txt"
    $env:OMP_NUM_THREADS = "$threads"
    Invoke-Checked { & $referenceExe $referenceOutput } "Reference OMP=$threads"
    Invoke-Checked { & $modExe $modOutput } "Mod OMP=$threads"
    Invoke-Checked { & $checker $referenceOutput $modOutput } "Raw checker OMP=$threads"
  }
} finally {
  $env:Path = $previousPath
}
