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

$startProcessArguments = @{
  FilePath = "cmake.exe"
  ArgumentList = $CMakeArguments
  Environment = @{ Path = $canonicalPath }
  NoNewWindow = $true
  PassThru = $true
  Wait = $true
}
$process = Start-Process @startProcessArguments
exit $process.ExitCode
