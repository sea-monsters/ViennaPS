[CmdletBinding()]
param(
  [Parameter(Mandatory = $true)]
  [string]$BuildDir,

  [ValidateRange(1, 20)]
  [int]$Iterations = 3,

  [string]$OutputPath
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$repoRoot = Split-Path -Parent $PSScriptRoot
$resolvedBuildDir = [System.IO.Path]::GetFullPath((Join-Path $repoRoot $BuildDir))
if (-not (Test-Path -LiteralPath $resolvedBuildDir -PathType Container)) {
  throw "Build directory does not exist: $resolvedBuildDir"
}

if ([string]::IsNullOrWhiteSpace($OutputPath)) {
  $stamp = Get-Date -Format "yyyyMMdd"
  $OutputPath = Join-Path $repoRoot "docs/design/pd4-perf-baseline-$stamp.json"
}
$resolvedOutputPath = [System.IO.Path]::GetFullPath((Join-Path $repoRoot $OutputPath))

$cases = @(
  [pscustomobject]@{
    Name = "coverage"
    Executable = "gpu/vulkan/surface/viennaps-vulkan-coverage-delta-metric-smoke.exe"
    Sources = @("gpu/vulkan/surface/coverage_delta_metric.cpp")
    Correctness = "CPU/Vulkan exact PASS"
  }
  [pscustomobject]@{
    Name = "diffusion"
    Executable = "gpu/vulkan/surface/viennaps-vulkan-graph-diffusion-smoke.exe"
    Sources = @("gpu/vulkan/surface/graph_diffusion.cpp")
    Correctness = "CPU/Vulkan exact PASS"
  }
  [pscustomobject]@{
    Name = "neutral"
    Executable = "gpu/vulkan/surface/viennaps-vulkan-neutral-transport-surface-smoke.exe"
    Sources = @("gpu/vulkan/surface/neutral_transport_surface.cpp")
    Correctness = "CPU/Vulkan bit-exact PASS"
  }
  [pscustomobject]@{
    Name = "levelset-update"
    Executable = "gpu/vulkan/levelset/viennaps-vulkan-levelset-update-smoke.exe"
    Sources = @("gpu/vulkan/levelset/levelset_update.cpp")
    Correctness = "PASS"
  }
  [pscustomobject]@{
    Name = "levelset-composition"
    Executable = "gpu/vulkan/levelset/viennaps-vulkan-levelset-surface-composition-execution-smoke.exe"
    Sources = @(
      "gpu/vulkan/surface/coverage_delta_metric.cpp"
      "gpu/vulkan/surface/graph_diffusion.cpp"
      "gpu/vulkan/surface/neutral_transport_surface.cpp"
      "gpu/vulkan/levelset/levelset_update.cpp"
      "gpu/vulkan/levelset/hrle_rebuild_classification.cpp"
      "gpu/vulkan/levelset/hrle_rebuild_compaction.cpp"
    )
    Correctness = "CPU oracle PASS"
  }
)

function Get-ContractCount {
  param(
    [Parameter(Mandatory = $true)]
    [string[]]$Paths,
    [Parameter(Mandatory = $true)]
    [string]$Pattern
  )

  $count = 0
  foreach ($path in $Paths) {
    $resolvedPath = Join-Path $repoRoot $path
    if (-not (Test-Path -LiteralPath $resolvedPath -PathType Leaf)) {
      throw "Metric source does not exist: $resolvedPath"
    }
    $source = Get-Content -LiteralPath $resolvedPath -Raw
    $count += [regex]::Matches($source, $Pattern).Count
  }
  return $count
}

function Invoke-BaselineCase {
  param(
    [Parameter(Mandatory = $true)]
    [pscustomobject]$Case,
    [Parameter(Mandatory = $true)]
    [int]$Iteration
  )

  $executablePath = Join-Path $resolvedBuildDir $Case.Executable
  if (-not (Test-Path -LiteralPath $executablePath -PathType Leaf)) {
    throw "Baseline executable does not exist: $executablePath"
  }

  $start = [System.Diagnostics.Stopwatch]::StartNew()
  $processInfo = New-Object System.Diagnostics.ProcessStartInfo
  $processInfo.FileName = $executablePath
  $processInfo.WorkingDirectory = $resolvedBuildDir
  $processInfo.UseShellExecute = $false
  $processInfo.CreateNoWindow = $true
  $processInfo.RedirectStandardOutput = $true
  $processInfo.RedirectStandardError = $true
  $process = New-Object System.Diagnostics.Process
  $process.StartInfo = $processInfo
  if (-not $process.Start()) {
    throw "Could not start baseline executable: $executablePath"
  }
  $stdout = $process.StandardOutput.ReadToEnd()
  $stderr = $process.StandardError.ReadToEnd()
  $process.WaitForExit()
  $start.Stop()

  $combined = ($stdout + "`n" + $stderr).Trim()
  $correctnessPassed = $process.ExitCode -eq 0 -and
    $combined -match [regex]::Escape($Case.Correctness)
  $observedLine = ($combined -split "`r?`n" |
      Where-Object { $_ -match [regex]::Escape($Case.Correctness) } |
      Select-Object -First 1)

  [pscustomobject]@{
    suite = $Case.Name
    iteration = $Iteration
    exit_code = $process.ExitCode
    cpu_correctness = $correctnessPassed
    elapsed_ms = [math]::Round($start.Elapsed.TotalMilliseconds, 3)
    observed = $observedLine
  }
}

$results = @()
foreach ($case in $cases) {
  $dispatchSites = Get-ContractCount -Paths $case.Sources -Pattern "\bvkCmdDispatch\s*\("
  $submitSites = Get-ContractCount -Paths $case.Sources -Pattern "\bvkQueueSubmit\s*\("
  $bufferSites = Get-ContractCount -Paths $case.Sources -Pattern "\bcreate(?:Float|Index|UInt32|Int)Buffer\s*\(|\b(?:buffer|buffers|updated|previous|output|coverage|materialIds|velocity|rowOffsets|columnIndices|weights|field|indexInput|compactIndexOutput|flagsInput|offsets|count)\s*\.\s*create\s*\("

  foreach ($iteration in 1..$Iterations) {
    $result = Invoke-BaselineCase -Case $case -Iteration $iteration
    $results += [pscustomobject]@{
      suite = $result.suite
      iteration = $result.iteration
      exit_code = $result.exit_code
      cpu_correctness = $result.cpu_correctness
      elapsed_ms = $result.elapsed_ms
      observed = $result.observed
      contract_metrics = [pscustomobject]@{
        dispatch_sites = $dispatchSites
        submit_sites = $submitSites
        buffer_create_sites = $bufferSites
      }
    }
    if (-not $result.cpu_correctness) {
      throw "CPU/Vulkan correctness gate failed for $($case.Name) iteration $iteration."
    }
  }
}

$document = [pscustomobject]@{
  schema = "pd4-perf-baseline.v1"
  recorded_at_utc = (Get-Date).ToUniversalTime().ToString("o")
  build_directory = Split-Path -Leaf $resolvedBuildDir
  iterations = $Iterations
  correctness_gate = "all selected CPU/Vulkan smoke runs exit 0 and emit their oracle PASS marker"
  metric_boundary = "contract-site counts are source inventories; elapsed_ms is measured per process run; no runtime counter is inferred"
  deterministic_correctness = (@($results | Where-Object { -not $_.cpu_correctness }).Count -eq 0)
  results = $results
}

$parent = Split-Path -Parent $resolvedOutputPath
if (-not (Test-Path -LiteralPath $parent -PathType Container)) {
  New-Item -ItemType Directory -Path $parent -Force | Out-Null
}
$document | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $resolvedOutputPath -Encoding utf8
Write-Output ("PD4 baseline written: " + $resolvedOutputPath)
