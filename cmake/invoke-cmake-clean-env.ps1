# SPDX-License-Identifier: MIT
#
# Run CMake in a child environment with one canonical Path entry. Some host
# applications expose both PATH and Path with different values; MSBuild then
# rejects the inherited environment before it can build CPM dependencies.
# SDK and dependency locations remain caller-controlled environment variables.

[CmdletBinding()]
param(
  [Parameter(Mandatory = $true, ValueFromRemainingArguments = $true)]
  [string[]]$CMakeArguments
)

$canonicalPath = $env:Path
if ([string]::IsNullOrWhiteSpace($canonicalPath)) {
  throw "A non-empty Path environment variable is required to invoke CMake."
}

# Start-Process merges the supplied map into the inherited environment.  If
# the host supplied both `Path` and `PATH`, that merge retains both spellings
# and MSBuild rejects the duplicate when launching cl.exe.  Rebuild the child
# environment through a case-insensitive dictionary so the process receives
# exactly one canonical Path entry while retaining caller-provided SDK/cache
# variables.
$childEnvironment = [System.Collections.Generic.Dictionary[string, string]]::new(
  [System.StringComparer]::OrdinalIgnoreCase)
foreach ($entry in [System.Environment]::GetEnvironmentVariables().GetEnumerator()) {
  $childEnvironment[[string]$entry.Key] = [string]$entry.Value
}
$childEnvironment["Path"] = $canonicalPath

$startInfo = [System.Diagnostics.ProcessStartInfo]::new()
$startInfo.FileName = "cmake.exe"
$startInfo.UseShellExecute = $false
$startInfo.WorkingDirectory = (Get-Location).Path
foreach ($argument in $CMakeArguments) {
  [void]$startInfo.ArgumentList.Add($argument)
}
$startInfo.Environment.Clear()
foreach ($entry in $childEnvironment.GetEnumerator()) {
  [void]$startInfo.Environment.Add($entry.Key, $entry.Value)
}

$process = [System.Diagnostics.Process]::Start($startInfo)
$process.WaitForExit()
exit $process.ExitCode
