param(
  [ValidateSet('BuildMod', 'BuildReference', 'BuildChecker', 'RunMod',
               'RunReference', 'Check')]
  [string]$Step,
  [string]$OutputDirectory = "$PSScriptRoot\.tmp_paired",
  [string]$ConfiguredBuildDirectory =
      "$PSScriptRoot\..\..\build",
  [string]$ReferenceViennaPS = 'D:\Codex_lib\code_reference\ViennaPS',
  [ValidateSet('Baseline', 'ExplicitKDTree')]
  [string]$Candidate = 'Baseline',
  [ValidateSet(1, 2, 4, 8)]
  [int]$Threads = 1,
  [int]$TimeoutSeconds = 180
)

$ErrorActionPreference = 'Stop'
$root = (Resolve-Path "$PSScriptRoot\..\..").Path
$cache = Join-Path $ConfiguredBuildDirectory 'CMakeCache.txt'
$fixtureMain = Join-Path $PSScriptRoot 'neutral_cpu_oracle_fixture.cpp'
$fixtureProcess = Join-Path $PSScriptRoot 'neutral_cpu_oracle_process.cpp'
$kdTreeSpecialization =
    Join-Path $PSScriptRoot 'neutral_cpu_oracle_kdtree_specialization.cpp'
$checkerSource = Join-Path $PSScriptRoot 'neutral_cpu_oracle_checker.cpp'
$vsDevCmd =
    'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\Tools\VsDevCmd.bat'
$ompBin =
    'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Redist\MSVC\14.44.35112\debug_nonredist\x64\Microsoft.VC143.OpenMP.LLVM'
$crtBin =
    'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Redist\MSVC\14.44.35112\x64\Microsoft.VC143.CRT'

function Quote-Arg([string]$Value) { return '"' + $Value + '"' }

function Require-Path([string]$Path, [string]$Label) {
  if (-not (Test-Path -LiteralPath $Path)) {
    throw "$Label missing: $Path"
  }
}

function Cache-Value([string]$Name) {
  $pattern = '^' + [regex]::Escape($Name) + ':[^=]+=(.*)$'
  $entry = Select-String -LiteralPath $cache -Pattern $pattern |
      Select-Object -First 1
  if (-not $entry) {
    throw "CMake cache entry missing: $Name"
  }
  return $entry.Matches[0].Groups[1].Value.Replace('/', '\')
}

function Invoke-Checked([string]$Command, [string]$Label) {
  & cmd.exe /d /s /c $Command
  if ($LASTEXITCODE -ne 0) {
    throw "$Label failed with exit code $LASTEXITCODE"
  }
}

Require-Path $cache 'Configured build cache'
Require-Path $ReferenceViennaPS 'Reference ViennaPS'
Require-Path $vsDevCmd 'Visual Studio developer environment'
Require-Path $ompBin 'LLVM OpenMP runtime directory'
Require-Path $crtBin 'MSVC runtime directory'

$viennaCore = Cache-Value 'ViennaCore_SOURCE_DIR'
$viennaLS = Cache-Value 'ViennaLS_SOURCE_DIR'
$viennaHRLE = Cache-Value 'ViennaHRLE_SOURCE_DIR'
$viennaRay = Cache-Value 'ViennaRay_SOURCE_DIR'
$viennaCS = Cache-Value 'ViennaCS_SOURCE_DIR'
$embreeSource = Cache-Value 'embree4_SOURCE_DIR'
$tbbDll = Cache-Value '_tbb_release_dll'
$embreeLibrary =
    Join-Path $ConfiguredBuildDirectory '_deps\embree-build\embree4.lib'
$embreeBin = Split-Path $embreeLibrary
$tbbBin = Split-Path $tbbDll

Require-Path $viennaCore 'ViennaCore source'
Require-Path $viennaLS 'ViennaLS source'
Require-Path $viennaHRLE 'ViennaHRLE source'
Require-Path $viennaRay 'ViennaRay source'
Require-Path $viennaCS 'ViennaCS source'
Require-Path $embreeSource 'Embree source'
Require-Path $embreeLibrary 'Embree import library'
Require-Path $tbbDll 'TBB runtime'
New-Item -ItemType Directory -Force -Path $OutputDirectory | Out-Null
$OutputDirectory = (Resolve-Path $OutputDirectory).Path

$commonDefinitions = @(
  '/DNOMINMAX',
  '/DVIENNARAY_EMBREE_VERSION=4',
  '/DVIENNARAY_GPU_DOUBLE_PRECISION',
  '/DVIENNARAY_USE_RAY_MASKING',
  '/DWIN32_LEAN_AND_MEAN',
  '/D_USE_MATH_DEFINES=1'
)
$dependencyIncludes = @(
  (Join-Path $viennaCore 'include\viennacore'),
  (Join-Path $viennaLS 'include\viennals'),
  (Join-Path $viennaHRLE 'include\viennahrle'),
  (Join-Path $viennaRay 'include\viennaray'),
  (Join-Path $viennaCS 'include\viennacs'),
  (Join-Path $embreeSource 'include')
)
$compileFlags =
    '/nologo /std:c++20 /EHsc /Zi /O2 /Ob2 /DNDEBUG /openmp:llvm /MD /bigobj'

function Build-Fixture([string]$Name, [string]$ViennaPS, [bool]$IsMod) {
  if ($Candidate -eq 'ExplicitKDTree') {
    $Name += '-kdtree'
  }
  $definitions = @($commonDefinitions)
  if ($IsMod) {
    $definitions += '/DVIENNAPS_NEUTRAL_ORACLE_MOD=1'
  }
  if ($Candidate -eq 'ExplicitKDTree') {
    $definitions += '/DVIENNAPS_NEUTRAL_ORACLE_EXPLICIT_KDTREE=1'
  }
  $includes = @((Join-Path $ViennaPS 'include\viennaps')) +
      $dependencyIncludes
  $includeArgs = ($includes | ForEach-Object { '/I' + (Quote-Arg $_) }) -join ' '
  $definitionArgs = $definitions -join ' '
  $mainObject = Join-Path $OutputDirectory "$Name-main.obj"
  $processObject = Join-Path $OutputDirectory "$Name-process.obj"
  $kdTreeObject = Join-Path $OutputDirectory "$Name-kdtree.obj"
  $pdb = Join-Path $OutputDirectory "$Name.pdb"
  $executable = Join-Path $OutputDirectory "$Name.exe"
  $prefix = 'call ' + (Quote-Arg $vsDevCmd) +
      ' -host_arch=x64 -arch=x64 >nul && cl.exe '
  Invoke-Checked ($prefix + $compileFlags + ' ' + $definitionArgs + ' ' +
      $includeArgs + ' /c ' + (Quote-Arg $fixtureMain) + ' /Fo' +
      (Quote-Arg $mainObject) + ' /Fd' + (Quote-Arg $pdb)) "$Name main build"
  Invoke-Checked ($prefix + $compileFlags + ' ' + $definitionArgs + ' ' +
      $includeArgs + ' /c ' + (Quote-Arg $fixtureProcess) + ' /Fo' +
      (Quote-Arg $processObject) + ' /Fd' + (Quote-Arg $pdb))
      "$Name Process build"
  $objects = @($mainObject, $processObject)
  if ($Candidate -eq 'ExplicitKDTree') {
    Invoke-Checked ($prefix + $compileFlags + ' ' + $definitionArgs + ' ' +
        $includeArgs + ' /c ' + (Quote-Arg $kdTreeSpecialization) + ' /Fo' +
        (Quote-Arg $kdTreeObject) + ' /Fd' + (Quote-Arg $pdb))
        "$Name KDTree specialization build"
    $objects += $kdTreeObject
  }
  $objectArgs = ($objects | ForEach-Object { Quote-Arg $_ }) -join ' '
  Invoke-Checked ($prefix + $compileFlags + ' ' + $objectArgs + ' /Fe' +
      (Quote-Arg $executable) + ' /link ' + (Quote-Arg $embreeLibrary))
      "$Name link"
}

function Build-Checker {
  $checker = Join-Path $OutputDirectory 'neutral_cpu_oracle_checker.exe'
  $command = 'call ' + (Quote-Arg $vsDevCmd) +
      ' -host_arch=x64 -arch=x64 >nul && cl.exe /nologo /std:c++20 ' +
      '/EHsc /O2 /DNDEBUG /MD ' + (Quote-Arg $checkerSource) + ' /Fe' +
      (Quote-Arg $checker)
  Invoke-Checked $command 'Checker build'
}

function Run-Fixture([string]$Name) {
  if ($Candidate -eq 'ExplicitKDTree') {
    $Name += '-kdtree'
  }
  $executable = Join-Path $OutputDirectory "$Name.exe"
  $output = Join-Path $OutputDirectory "$Name-omp$Threads.txt"
  Require-Path $executable "$Name executable"
  $childEnvironment =
      [System.Collections.Generic.Dictionary[string, string]]::new(
          [System.StringComparer]::OrdinalIgnoreCase)
  foreach ($entry in
      [System.Environment]::GetEnvironmentVariables().GetEnumerator()) {
    $childEnvironment[[string]$entry.Key] = [string]$entry.Value
  }
  $childEnvironment['Path'] =
      "$embreeBin;$tbbBin;$crtBin;$ompBin;$($env:Path)"
  $childEnvironment['OMP_NUM_THREADS'] = "$Threads"

  $startInfo = [System.Diagnostics.ProcessStartInfo]::new()
  $startInfo.FileName = $executable
  $startInfo.UseShellExecute = $false
  $startInfo.WorkingDirectory = (Get-Location).Path
  [void]$startInfo.ArgumentList.Add($output)
  $startInfo.Environment.Clear()
  foreach ($entry in $childEnvironment.GetEnumerator()) {
    [void]$startInfo.Environment.Add($entry.Key, $entry.Value)
  }

  $process = [System.Diagnostics.Process]::Start($startInfo)
  if (-not $process.WaitForExit($TimeoutSeconds * 1000)) {
    $process.Kill()
    $process.WaitForExit()
    throw "$Name OMP=$Threads timed out after $TimeoutSeconds seconds"
  }
  if ($process.ExitCode -ne 0) {
    throw "$Name OMP=$Threads failed with exit code $($process.ExitCode)"
  }
  $bytes = (Get-Item -LiteralPath $output).Length
  Write-Output "$Name OMP=$Threads exit=0 output_bytes=$bytes"
}

switch ($Step) {
  'BuildMod' { Build-Fixture 'neutral_cpu_oracle_mod' $root $true }
  'BuildReference' {
    Build-Fixture 'neutral_cpu_oracle_reference' $ReferenceViennaPS $false
  }
  'BuildChecker' { Build-Checker }
  'RunMod' { Run-Fixture 'neutral_cpu_oracle_mod' }
  'RunReference' { Run-Fixture 'neutral_cpu_oracle_reference' }
  'Check' {
    $checker = Join-Path $OutputDirectory 'neutral_cpu_oracle_checker.exe'
    $referenceOutput =
        Join-Path $OutputDirectory "neutral_cpu_oracle_reference-omp$Threads.txt"
    $modOutput =
        Join-Path $OutputDirectory "neutral_cpu_oracle_mod-omp$Threads.txt"
    Require-Path $checker 'Checker executable'
    Require-Path $referenceOutput 'Reference output'
    Require-Path $modOutput 'Mod output'
    & $checker $referenceOutput $modOutput
    if ($LASTEXITCODE -ne 0) {
      throw "Raw checker OMP=$Threads failed with exit code $LASTEXITCODE"
    }
  }
}
