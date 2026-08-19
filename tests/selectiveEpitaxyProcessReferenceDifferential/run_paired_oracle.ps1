param(
  [string]$OutputDirectory = "$PSScriptRoot\.tmp_manual",
  [string]$ReferenceViennaPS = "D:\Codex_lib\code_reference\ViennaPS"
)

$ErrorActionPreference = 'Stop'
$root = (Resolve-Path "$PSScriptRoot\..\..").Path
$source = Join-Path $PSScriptRoot 'selective_epitaxy_process_cpu_fixture.cpp'
$checkerSource = Join-Path $PSScriptRoot 'selective_epitaxy_process_cpu_checker.cpp'
$cache = Join-Path $root 'build\CMakeCache.txt'
$vsDevCmd = 'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\Tools\VsDevCmd.bat'

function Quote([string]$Value) { '"' + $Value + '"' }
function Require([string]$Path, [string]$Label) {
  if (-not (Test-Path -LiteralPath $Path)) { throw "$Label missing: $Path" }
}
function CacheDir([string]$Name) {
  $entry = Select-String -LiteralPath $cache -Pattern ("^" + [regex]::Escape($Name) + ":STATIC=") |
    Select-Object -First 1
  if (-not $entry) { throw "CMake cache entry missing: $Name" }
  return (($entry.Line -split '=', 2)[1]).Replace('/', '\')
}
function Run([scriptblock]$Command, [string]$Label) {
  & $Command
  if ($LASTEXITCODE -ne 0) { throw "$Label failed with exit code $LASTEXITCODE" }
}

$viennaCore = CacheDir 'ViennaCore_SOURCE_DIR'
$viennaLS = CacheDir 'ViennaLS_SOURCE_DIR'
$viennaHRLE = CacheDir 'ViennaHRLE_SOURCE_DIR'
$viennaRay = CacheDir 'ViennaRay_SOURCE_DIR'
$viennaCS = CacheDir 'ViennaCS_SOURCE_DIR'
$embree = Join-Path $root '.cpm-cache\embree\9311'
$embreeLibrary = Join-Path $root 'build\_deps\embree-build\Release\embree4.lib'
$viennaLSLibrary = Join-Path $root 'build\_deps\viennals-build\Release\viennals.lib'
$modLibrary = Join-Path $root 'build\Release\viennaps.lib'
$crtBin = 'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Redist\MSVC\14.44.35112\x64\Microsoft.VC143.CRT'
$ompBin = 'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Redist\MSVC\14.44.35112\debug_nonredist\x64\Microsoft.VC143.OpenMP.LLVM'
$tbbBin = Join-Path $root 'build\gpu\vulkan\ray\Release'

Require $ReferenceViennaPS 'Reference ViennaPS'
Require $cache 'Configured CMake cache'
Require $embreeLibrary 'Embree import library'
Require $viennaLSLibrary 'ViennaLS import library'
Require $modLibrary 'Mod ViennaPS import library'
New-Item -ItemType Directory -Force $OutputDirectory | Out-Null

$commonDefines = '/DVIENNARAY_EMBREE_VERSION=4 /DVIENNARAY_USE_RAY_MASKING /DVIENNARAY_GPU_DOUBLE_PRECISION /DVIENNALS_USE_PRECOMPILED'
$includeRoots = @(
  (Join-Path $viennaCore 'include\viennacore'),
  (Join-Path $viennaLS 'include\viennals'),
  (Join-Path $viennaHRLE 'include\viennahrle'),
  (Join-Path $viennaRay 'include\viennaray'),
  (Join-Path $viennaCS 'include\viennacs'),
  (Join-Path $embree 'include'))

function BuildFixture([string]$Output, [string]$ViennaPS, [bool]$Mod) {
  $defines = $commonDefines
  if ($Mod) { $defines += ' /DVIENNAPS_SELECTIVE_EPITAXY_PROCESS_ORACLE_MOD' }
  $includes = @((Join-Path $ViennaPS 'include\viennaps')) + $includeRoots
  $cmd = 'call ' + (Quote $vsDevCmd) + ' -arch=x64 >nul && cl.exe /nologo /std:c++20 /EHsc /O2 /Ob2 /DNDEBUG /openmp:llvm /MD ' +
    $defines + ' ' + (($includes | ForEach-Object { '/I' + (Quote $_) }) -join ' ') + ' ' +
    (Quote $source) + ' /Fe' + (Quote $Output) + ' /link ' +
    ((@($viennaLSLibrary, $embreeLibrary) + $(if ($Mod) { @($modLibrary) } else { @() })) |
      ForEach-Object { Quote $_ }) -join ' '
  Run { & cmd.exe /d /s /c $cmd } "Fixture build ($Output)"
}

$modExe = Join-Path $OutputDirectory 'selective_epitaxy_process_mod.exe'
$referenceExe = Join-Path $OutputDirectory 'selective_epitaxy_process_reference.exe'
$checkerExe = Join-Path $OutputDirectory 'selective_epitaxy_process_cpu_checker.exe'
BuildFixture $modExe $root $true
BuildFixture $referenceExe $ReferenceViennaPS $false
$checkerCmd = 'call ' + (Quote $vsDevCmd) + ' -arch=x64 >nul && cl.exe /nologo /std:c++20 /EHsc /O2 /DNDEBUG /MD ' +
  (Quote $checkerSource) + ' /Fe' + (Quote $checkerExe)
Run { & cmd.exe /d /s /c $checkerCmd } 'Checker build'

$previousPath = $env:Path
try {
  $env:Path = "$crtBin;$ompBin;$tbbBin;$previousPath"
  foreach ($threads in 1, 2, 4, 8) {
    $referenceOutput = Join-Path $OutputDirectory "reference-omp$threads.txt"
    $modOutput = Join-Path $OutputDirectory "mod-omp$threads.txt"
    $env:OMP_NUM_THREADS = "$threads"
    Run { & $referenceExe $referenceOutput } "Reference OMP=$threads"
    Run { & $modExe $modOutput } "Mod OMP=$threads"
    Run { & $checkerExe $referenceOutput $modOutput } "Checker OMP=$threads"
  }
} finally {
  $env:Path = $previousPath
}
