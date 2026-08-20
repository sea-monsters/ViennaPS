param(
  [ValidateSet('BuildMod', 'BuildReference', 'BuildChecker', 'RunMod',
               'RunReference', 'Check', 'BuildProbeMod', 'BuildProbeReference',
               'RunProbeMod', 'RunProbeReference', 'BuildProbeCaptureMod',
               'BuildProbeCaptureReference', 'RunProbeCaptureMod',
               'RunProbeCaptureReference')]
  [string]$Step,
  [string]$OutputDirectory = "$PSScriptRoot\.tmp_paired",
  [string]$ConfiguredBuildDirectory =
      "$PSScriptRoot\..\..\build",
  [string]$ReferenceViennaPS = 'D:\Codex_lib\code_reference\ViennaPS',
  [ValidateSet('Baseline', 'ExplicitKDTree', 'KDTreeAudit', 'IterativeTraverse')]
  [string]$Candidate = 'Baseline',
  [ValidateSet(1, 2, 4, 8)]
  [int]$Threads = 1,
  [ValidateSet('notrace', 'trace')]
  [string]$ProbeMode = 'notrace',
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
$probeSource = Join-Path $PSScriptRoot 'neutral_cpu_oracle_kdtree_probe.cpp'
$probeCaptureSource =
    Join-Path $PSScriptRoot 'neutral_cpu_oracle_kdtree_probe_capture.cpp'
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

function Prepare-KDTreeAuditOverlay {
  $source = Join-Path $viennaCore 'include\viennacore\vcKDTree.hpp'
  Require-Path $source 'ViennaCore KDTree header'
  $overlayRoot = Join-Path $OutputDirectory 'kdtree-audit-include'
  $overlayDirectory = Join-Path $overlayRoot 'viennacore'
  New-Item -ItemType Directory -Force -Path $overlayDirectory | Out-Null
  $overlay = Join-Path $overlayDirectory 'vcKDTree.hpp'
  Copy-Item -LiteralPath $source -Destination $overlay -Force
  $text = Get-Content -LiteralPath $overlay -Raw
  $needle = '    rootNode = myRootNode;'
  $replacement = @'
    rootNode = myRootNode;
#ifdef VIENNAPS_NEUTRAL_ORACLE_KDTREE_AUDIT
    {
      const auto nodeCount = nodes.size();
      const auto beginAddress = reinterpret_cast<std::size_t>(nodes.data());
      const auto byteCount = nodeCount * sizeof(Node);
      const auto endAddress = beginAddress + byteCount;
      auto inNodeStorage = [&](Node *candidate) {
        if (candidate == nullptr || nodeCount == 0 ||
            endAddress < beginAddress)
          return false;
        const auto address = reinterpret_cast<std::size_t>(candidate);
        return address >= beginAddress && address < endAddress &&
               ((address - beginAddress) % sizeof(Node)) == 0;
      };

      std::vector<Node *> pending;
      std::vector<Node *> visited;
      if (rootNode != nullptr)
        pending.push_back(rootNode);
      bool valid = true;
      while (valid && !pending.empty()) {
        auto *current = pending.back();
        pending.pop_back();
        if (!inNodeStorage(current)) {
          valid = false;
          break;
        }
        if (std::find(visited.begin(), visited.end(), current) !=
            visited.end()) {
          valid = false;
          break;
        }
        visited.push_back(current);
        if (current->axis >= D || current->index >= nodeCount) {
          valid = false;
          break;
        }
        if (current->left != nullptr)
          pending.push_back(current->left);
        if (current->right != nullptr)
          pending.push_back(current->right);
      }
      valid = valid && visited.size() == nodeCount;
      if (!valid) {
        viennacore::Logger::getInstance()
            .addError("KDTree audit invalid")
            .print();
      }
      viennacore::Logger::getInstance()
          .addDebug("KDTree audit valid nodes=" +
                    std::to_string(visited.size()))
          .print();
    }
#endif
'@
  if (-not $text.Contains($needle)) {
    throw 'KDTree audit needle missing: rootNode = myRootNode;'
  }
  if (($text.Split($needle).Count - 1) -ne 1) {
    throw 'KDTree audit needle count is not exactly one'
  }
  $text = $text.Replace($needle, $replacement)
  Set-Content -LiteralPath $overlay -Value $text -NoNewline
  return $overlayRoot
}

function Prepare-IterativeTraverseOverlay {
  $source = Join-Path $viennaCore 'include\viennacore\vcKDTree.hpp'
  Require-Path $source 'ViennaCore KDTree header'
  $overlayRoot = Join-Path $OutputDirectory 'iterative-traverse-include'
  $overlayDirectory = Join-Path $overlayRoot 'viennacore'
  New-Item -ItemType Directory -Force -Path $overlayDirectory | Out-Null
  $overlay = Join-Path $overlayDirectory 'vcKDTree.hpp'
  Copy-Item -LiteralPath $source -Destination $overlay -Force
  $text = Get-Content -LiteralPath $overlay -Raw
  # Normalize line endings so the multi-line needles match regardless of
  # how this script was checked out.
  $text = $text -replace "\r\n", "`n"

  $needlePair = @'
  void traverseDown(Node *currentNode, std::pair<NumericType, Node *> &best,
                    const ValueType &x) const {
    if (currentNode == nullptr)
      return;

    auto axis = currentNode->axis;

    // For distance comparison operations we only use the "reduced" aka less
    // compute intensive, but order preserving version of the distance
    // function.
    auto distance = SquaredDistance(x, currentNode->value);
    if (distance < best.first)
      best = std::pair{distance, currentNode};

    bool isLeft;
    if (x[axis] < currentNode->value[axis]) {
      traverseDown(currentNode->left, best, x);
      isLeft = true;
    } else {
      traverseDown(currentNode->right, best, x);
      isLeft = false;
    }

    // If the hypersphere with origin at x and a radius of our current best
    // distance intersects the hyperplane defined by the partitioning of the
    // current node, we also have to search the other subtree, since there could
    // be points closer to x than our current best.
    auto distanceToHyperplane =
        scalingFactors[axis] * std::abs(x[axis] - currentNode->value[axis]);
    distanceToHyperplane *= distanceToHyperplane;
    if (distanceToHyperplane < best.first) {
      if (isLeft)
        traverseDown(currentNode->right, best, x);
      else
        traverseDown(currentNode->left, best, x);
    }
  }
'@
  $replacementPair = @'
  void traverseDown(Node *currentNode, std::pair<NumericType, Node *> &best,
                    const ValueType &x) const {
    // P5-N1F workaround: MSVC 14.44 (/O2 /Ob2) miscompiles both the tail
    // recursion and a plain while (currentNode != nullptr) loop here into
    // a loop whose backward jump skips the null check, dereferencing null
    // leaf children; it even deletes an explicit "if (nullptr) break" on
    // the freshly loaded child. The volatile store/load below forces the
    // check to be emitted. Load-bearing: do not "simplify" away.
    while (currentNode != nullptr) {
      auto axis = currentNode->axis;

      // For distance comparison operations we only use the "reduced" aka
      // less compute intensive, but order preserving version of the
      // distance function.
      auto distance = SquaredDistance(x, currentNode->value);
      if (distance < best.first)
        best = std::pair{distance, currentNode};

      bool isLeft;
      if (x[axis] < currentNode->value[axis]) {
        traverseDown(currentNode->left, best, x);
        isLeft = true;
      } else {
        traverseDown(currentNode->right, best, x);
        isLeft = false;
      }

      // If the hypersphere with origin at x and a radius of our current
      // best distance intersects the hyperplane defined by the
      // partitioning of the current node, we also have to search the other
      // subtree, since there could be points closer to x than our current
      // best.
      auto distanceToHyperplane =
          scalingFactors[axis] * std::abs(x[axis] - currentNode->value[axis]);
      distanceToHyperplane *= distanceToHyperplane;
      if (distanceToHyperplane < best.first) {
        Node *volatile nextNode =
            isLeft ? currentNode->right : currentNode->left;
        if (nextNode == nullptr)
          break;
        currentNode = nextNode;
      } else {
        break;
      }
    }
  }
'@

  $needleQueue = @'
  void traverseDown(Node *currentNode, Q &queue, const ValueType &x) const {
    if (currentNode == nullptr)
      return;

    int axis = currentNode->axis;

    queue.enqueue(std::pair{Distance(x, currentNode->value), currentNode});

    bool isLeft;
    if (x[axis] < currentNode->value[axis]) {
      traverseDown(currentNode->left, queue, x);
      isLeft = true;
    } else {
      traverseDown(currentNode->right, queue, x);
      isLeft = false;
    }

    // If the hypersphere with origin at x and a radius of our current best
    // distance intersects the hyperplane defined by the partitioning of the
    // current node, we also have to search the other subtree, since there could
    // be points closer to x than our current best.
    auto distanceToHyperplane =
        scalingFactors[axis] * std::abs(x[axis] - currentNode->value[axis]);
    distanceToHyperplane *= distanceToHyperplane;

    bool intersects = false;
    if constexpr (std::is_same_v<Q, BoundedPQueue<NumericType, Node *>>) {
      intersects = queue.size() < queue.maxSize() ||
                   distanceToHyperplane < queue.worst();
    } else if constexpr (std::is_same_v<Q,
                                        ClampedPQueue<NumericType, Node *>>) {
      intersects = distanceToHyperplane < queue.worst();
    }

    if (intersects) {
      if (isLeft)
        traverseDown(currentNode->right, queue, x);
      else
        traverseDown(currentNode->left, queue, x);
    }
  }
'@
  $replacementQueue = @'
  void traverseDown(Node *currentNode, Q &queue, const ValueType &x) const {
    // P5-N1F workaround: see the pair overload above. Same MSVC 14.44
    // miscompilation; the volatile child load below is load-bearing.
    while (currentNode != nullptr) {
      int axis = currentNode->axis;

      queue.enqueue(std::pair{Distance(x, currentNode->value), currentNode});

      bool isLeft;
      if (x[axis] < currentNode->value[axis]) {
        traverseDown(currentNode->left, queue, x);
        isLeft = true;
      } else {
        traverseDown(currentNode->right, queue, x);
        isLeft = false;
      }

      // If the hypersphere with origin at x and a radius of our current
      // best distance intersects the hyperplane defined by the
      // partitioning of the current node, we also have to search the other
      // subtree, since there could be points closer to x than our current
      // best.
      auto distanceToHyperplane =
          scalingFactors[axis] * std::abs(x[axis] - currentNode->value[axis]);
      distanceToHyperplane *= distanceToHyperplane;

      bool intersects = false;
      if constexpr (std::is_same_v<Q, BoundedPQueue<NumericType, Node *>>) {
        intersects = queue.size() < queue.maxSize() ||
                     distanceToHyperplane < queue.worst();
      } else if constexpr (std::is_same_v<
                           Q, ClampedPQueue<NumericType, Node *>>) {
        intersects = distanceToHyperplane < queue.worst();
      }

      if (intersects) {
        Node *volatile nextNode =
            isLeft ? currentNode->right : currentNode->left;
        if (nextNode == nullptr)
          break;
        currentNode = nextNode;
      } else {
        break;
      }
    }
  }
'@

  foreach ($pair in @(@($needlePair, $replacementPair, 'pair'),
                      @($needleQueue, $replacementQueue, 'queue'))) {
    $needle = $pair[0] -replace "\r\n", "`n"
    $replacement = $pair[1] -replace "\r\n", "`n"
    $label = $pair[2]
    if (($text.Split($needle).Count - 1) -ne 1) {
      throw "IterativeTraverse needle count for $label traverseDown is not exactly one"
    }
    $text = $text.Replace($needle, $replacement)
  }
  Set-Content -LiteralPath $overlay -Value $text -NoNewline
  return $overlayRoot
}

$kdTreeAuditInclude = $null
if ($Candidate -eq 'KDTreeAudit') {
  $kdTreeAuditInclude = Prepare-KDTreeAuditOverlay
}
$iterativeTraverseInclude = $null
if ($Candidate -eq 'IterativeTraverse') {
  $iterativeTraverseInclude = Prepare-IterativeTraverseOverlay
}

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
  if ($Candidate -eq 'KDTreeAudit') {
    $definitions += '/DVIENNAPS_NEUTRAL_ORACLE_KDTREE_AUDIT=1'
  }
  $candidateIncludes = @()
  if ($Candidate -eq 'KDTreeAudit') {
    $candidateIncludes = @((Join-Path $kdTreeAuditInclude 'viennacore'))
  } elseif ($Candidate -eq 'IterativeTraverse') {
    $candidateIncludes =
        @((Join-Path $iterativeTraverseInclude 'viennacore'))
  }
  $includes = $candidateIncludes +
      @((Join-Path $ViennaPS 'include\viennaps')) +
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

function Build-Probe([string]$Name, [string]$ViennaPS, [bool]$IsMod) {
  $definitions = @($commonDefinitions)
  if ($IsMod) {
    $definitions += '/DVIENNAPS_NEUTRAL_ORACLE_MOD=1'
  }
  $includes = @((Join-Path $ViennaPS 'include\viennaps')) +
      $dependencyIncludes
  if ($Candidate -eq 'KDTreeAudit') {
    $definitions += '/DVIENNAPS_NEUTRAL_ORACLE_KDTREE_AUDIT=1'
    $includes = @((Join-Path $kdTreeAuditInclude 'viennacore')) + $includes
  } elseif ($Candidate -eq 'IterativeTraverse') {
    $includes =
        @((Join-Path $iterativeTraverseInclude 'viennacore')) + $includes
  }
  $includeArgs = ($includes | ForEach-Object { '/I' + (Quote-Arg $_) }) -join ' '
  $definitionArgs = $definitions -join ' '
  $probeObject = Join-Path $OutputDirectory "$Name.obj"
  $pdb = Join-Path $OutputDirectory "$Name.pdb"
  $executable = Join-Path $OutputDirectory "$Name.exe"
  $prefix = 'call ' + (Quote-Arg $vsDevCmd) +
      ' -host_arch=x64 -arch=x64 >nul && cl.exe '
  Invoke-Checked ($prefix + $compileFlags + ' ' + $definitionArgs + ' ' +
      $includeArgs + ' /c ' + (Quote-Arg $probeSource) + ' /Fo' +
      (Quote-Arg $probeObject) + ' /Fd' + (Quote-Arg $pdb)) "$Name build"
  Invoke-Checked ($prefix + $compileFlags + ' ' + (Quote-Arg $probeObject) +
      ' /Fe' + (Quote-Arg $executable) + ' /link ' +
      (Quote-Arg $embreeLibrary)) "$Name link"
}

function Build-ProbeCapture([string]$Name, [string]$ViennaPS, [bool]$IsMod) {
  $definitions = @($commonDefinitions)
  if ($IsMod) {
    $definitions += '/DVIENNAPS_NEUTRAL_ORACLE_MOD=1'
  }
  $includes = @((Join-Path $ViennaPS 'include\viennaps')) +
      $dependencyIncludes
  if ($Candidate -eq 'KDTreeAudit') {
    $definitions += '/DVIENNAPS_NEUTRAL_ORACLE_KDTREE_AUDIT=1'
    $includes = @((Join-Path $kdTreeAuditInclude 'viennacore')) + $includes
  } elseif ($Candidate -eq 'IterativeTraverse') {
    $includes =
        @((Join-Path $iterativeTraverseInclude 'viennacore')) + $includes
  }
  $includeArgs = ($includes | ForEach-Object { '/I' + (Quote-Arg $_) }) -join ' '
  $definitionArgs = $definitions -join ' '
  $captureObject = Join-Path $OutputDirectory "$Name.obj"
  $pdb = Join-Path $OutputDirectory "$Name.pdb"
  $executable = Join-Path $OutputDirectory "$Name.exe"
  $prefix = 'call ' + (Quote-Arg $vsDevCmd) +
      ' -host_arch=x64 -arch=x64 >nul && cl.exe '
  Invoke-Checked ($prefix + $compileFlags + ' ' + $definitionArgs + ' ' +
      $includeArgs + ' /c ' + (Quote-Arg $probeCaptureSource) + ' /Fo' +
      (Quote-Arg $captureObject) + ' /Fd' + (Quote-Arg $pdb)) "$Name build"
  Invoke-Checked ($prefix + $compileFlags + ' ' + (Quote-Arg $captureObject) +
      ' /Fe' + (Quote-Arg $executable) + ' /link ' +
      (Quote-Arg $embreeLibrary) + ' dbghelp.lib') "$Name link"
}

function Run-Fixture([string]$Name, [string]$ExtraArg = '',
                     [string]$RunLabel = '') {
  if ($Candidate -eq 'ExplicitKDTree') {
    $Name += '-kdtree'
  }
  $executable = Join-Path $OutputDirectory "$Name.exe"
  $suffix = "omp$Threads"
  if ($RunLabel) {
    $suffix = "$RunLabel-$suffix"
  }
  $output = Join-Path $OutputDirectory "$Name-$suffix.txt"
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
  if ($ExtraArg) {
    [void]$startInfo.ArgumentList.Add($ExtraArg)
  }
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
  Write-Output "$Name $RunLabel OMP=$Threads exit=0 output_bytes=$bytes"
}

switch ($Step) {
  'BuildMod' { Build-Fixture 'neutral_cpu_oracle_mod' $root $true }
  'BuildReference' {
    Build-Fixture 'neutral_cpu_oracle_reference' $ReferenceViennaPS $false
  }
  'BuildChecker' { Build-Checker }
  'BuildProbeMod' {
    Build-Probe 'neutral_cpu_oracle_kdtree_probe_mod' $root $true
  }
  'BuildProbeReference' {
    Build-Probe 'neutral_cpu_oracle_kdtree_probe_reference' `
        $ReferenceViennaPS $false
  }
  'RunProbeMod' {
    Run-Fixture 'neutral_cpu_oracle_kdtree_probe_mod' $ProbeMode $ProbeMode
  }
  'RunProbeReference' {
    Run-Fixture 'neutral_cpu_oracle_kdtree_probe_reference' $ProbeMode `
        $ProbeMode
  }
  'BuildProbeCaptureMod' {
    Build-ProbeCapture 'neutral_cpu_oracle_kdtree_probe_capture_mod' $root `
        $true
  }
  'BuildProbeCaptureReference' {
    Build-ProbeCapture 'neutral_cpu_oracle_kdtree_probe_capture_reference' `
        $ReferenceViennaPS $false
  }
  'RunProbeCaptureMod' {
    Run-Fixture 'neutral_cpu_oracle_kdtree_probe_capture_mod' $ProbeMode `
        $ProbeMode
  }
  'RunProbeCaptureReference' {
    Run-Fixture 'neutral_cpu_oracle_kdtree_probe_capture_reference' `
        $ProbeMode $ProbeMode
  }
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
