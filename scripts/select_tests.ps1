#Requires -Version 7.0
[CmdletBinding(DefaultParameterSetName = 'Preset')]
param(
    [Parameter(ParameterSetName = 'Preset')][string]$Preset = 'msvc-cpu-tests',
    [Parameter(Mandatory, ParameterSetName = 'Build')][string]$BuildDirectory,
    [string]$Base,
    [string[]]$Files,
    [string]$RepositoryRoot = (Split-Path $PSScriptRoot -Parent),
    [string]$RulesFile = (Join-Path $PSScriptRoot '../tests/test_impact.json')
)

$ErrorActionPreference = 'Stop'
$PSNativeCommandUseErrorActionPreference = $false
$repo = [IO.Path]::GetFullPath($RepositoryRoot)
$result = [ordered]@{ mode = 'none'; reasons = @(); tests = @(); guiTests = @(); targets = @(); extraChecks = @() }
$selected = [Collections.Generic.HashSet[string]]::new([StringComparer]::Ordinal)
$targets = [Collections.Generic.HashSet[string]]::new([StringComparer]::Ordinal)
$extra = [Collections.Generic.HashSet[string]]::new([StringComparer]::Ordinal)

function Full([string]$Reason) {
    $result.mode = 'full'
    $result.reasons += $Reason
}
function Canonical([string]$Path, [string]$Root) {
    [IO.Path]::GetFullPath($Path.Replace('\', '/'), $Root).Replace('\', '/').ToLowerInvariant()
}
function Matches-Any([string]$Path, $Patterns) {
    foreach ($pattern in $Patterns) { if ($Path -match $pattern) { return $true } }
    return $false
}
function Native([string]$Program, [string[]]$Arguments) {
    $output = @(& $Program @Arguments)
    if ($LASTEXITCODE) { throw "$Program failed ($LASTEXITCODE): $($Arguments -join ' ')" }
    return $output
}
function Read-Presets([string]$Path, $Documents, $Visited) {
    $path = [IO.Path]::GetFullPath($Path)
    if (!$Visited.Add($path)) { return }
    $document = Get-Content -LiteralPath $path -Raw | ConvertFrom-Json
    $Documents.Add($document)
    foreach ($include in $document.include) {
        Read-Presets (Join-Path (Split-Path $path -Parent) $include) $Documents $Visited
    }
}
function Preset-Binary($Configure, [string]$Name, $Seen) {
    if (!$Seen.Add($Name)) { throw "Cyclic configure preset: $Name" }
    $entry = @($Configure | Where-Object name -eq $Name)[0]
    if (!$entry) { throw "Missing configure preset: $Name" }
    if ($entry.binaryDir) { return [string]$entry.binaryDir }
    foreach ($parent in $entry.inherits) {
        $binary = Preset-Binary $Configure $parent $Seen
        if ($binary) { return $binary }
    }
}

Push-Location -LiteralPath $repo
try {
    $rules = Get-Content -LiteralPath $RulesFile -Raw | ConvertFrom-Json
    if ($rules.version -ne 1) { throw 'Unsupported impact rules version.' }
    if (!$PSBoundParameters.ContainsKey('Files')) {
        if (!$Base) { $Base = (Native git @('merge-base', 'HEAD', 'origin/main')) -join '' }
        # Diff against the worktree includes committed, staged and unstaged changes.
        # --no-renames retains both old and new paths so removed inputs are selected.
        $Files = @(Native git @('-c', 'core.quotepath=false', 'diff', '--name-only', '--no-renames', $Base, '--')) +
                 @(Native git @('-c', 'core.quotepath=false', 'ls-files', '--others', '--exclude-standard'))
    }
    $changes = @($Files | ForEach-Object {
        [IO.Path]::GetRelativePath($repo, [IO.Path]::GetFullPath($_, $repo)).Replace('\', '/')
    } | Sort-Object -Unique)
    $graphChanges = [Collections.Generic.List[string]]::new()
    $ruleChanges = [Collections.Generic.List[object]]::new()
    foreach ($path in $changes) {
        if (Matches-Any $path $rules.full) { Full "Validation infrastructure changed: $path"; continue }
        if (Matches-Any $path $rules.none) { $result.reasons += "Documentation/site only: $path"; continue }
        $matched = $false
        $hasTestRule = $false
        foreach ($rule in $rules.rules) {
            if (Matches-Any $path $rule.paths) {
                $matched = $true
                $ruleChanges.Add($rule)
                if ($rule.tests.Count) { $hasTestRule = $true }
                foreach ($check in $rule.extraChecks) { [void]$extra.Add($check) }
                $result.reasons += "Explicit impact rule: $path"
            }
        }
        # Also use build dependencies for rules with CTest inputs (e.g. resources).
        if (!$matched -or $hasTestRule) {
            $graphChanges.Add($path)
        }
    }
    $needsInventory = $graphChanges.Count -gt 0 -or @($ruleChanges | Where-Object { $_.tests.Count }).Count -gt 0
    if ($result.mode -ne 'full' -and $needsInventory) {
        if (!$BuildDirectory) {
            $documents = [Collections.Generic.List[object]]::new()
            $visited = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
            Read-Presets (Join-Path $repo 'CMakePresets.json') $documents $visited
            if (Test-Path -LiteralPath (Join-Path $repo 'CMakeUserPresets.json')) {
                Read-Presets (Join-Path $repo 'CMakeUserPresets.json') $documents $visited
            }
            $testPreset = @($documents.testPresets | Where-Object name -eq $Preset)[0]
            if (!$testPreset.configurePreset) { throw "Test preset has no configurePreset: $Preset" }
            $seen = [Collections.Generic.HashSet[string]]::new()
            $BuildDirectory = Preset-Binary $documents.configurePresets $testPreset.configurePreset $seen
            $BuildDirectory = $BuildDirectory.Replace('${sourceDir}', $repo).Replace('${presetName}', $testPreset.configurePreset)
            if ($BuildDirectory -match '\$') { throw 'Unsupported preset binaryDir macro; use -BuildDirectory.' }
        }
        $build = [IO.Path]::GetFullPath($BuildDirectory, $repo)
        if (!(Test-Path -LiteralPath "$build/build.ninja") -or !(Test-Path -LiteralPath "$build/.ninja_deps")) {
            throw "Missing Ninja build/dependency inventory: $build. Configure and build first."
        }
        $cache = Get-Content -LiteralPath "$build/CMakeCache.txt" -Raw
        $source = [regex]::Match($cache, '(?m)^CMAKE_HOME_DIRECTORY:INTERNAL=(.+)\r?$').Groups[1].Value.Trim()
        if (!$source -or (Canonical $source $repo) -ne (Canonical $repo $repo)) { throw 'Build cache belongs to another source directory.' }
        $ninja = [regex]::Match($cache, '(?m)^CMAKE_MAKE_PROGRAM:[^=]+=(.+)\r?$').Groups[1].Value.Trim()
        if (!$ninja) { $ninja = 'ninja' }
        $ctest = [regex]::Match($cache, '(?m)^CMAKE_CTEST_COMMAND:[^=]+=(.+)\r?$').Groups[1].Value.Trim()
        if (!$ctest) {
            $ctest = Join-Path (Split-Path (Get-Command cmake -ErrorAction Stop).Source -Parent) $(if ($IsWindows) { 'ctest.exe' } else { 'ctest' })
        }
        $inventory = (Native $ctest @('--test-dir', $build, '-C', 'Release', '--show-only=json-v1')) -join "`n" | ConvertFrom-Json
        if (!$inventory.tests.Count) { throw 'Configured CTest inventory is empty.' }
        $cases = @($inventory.tests)
        # Query every output, including EXCLUDE_FROM_ALL executables and helper binaries.
        $outputs = @(Native $ninja @('-C', $build, '-t', 'targets', 'all'))
        $exeOutputs = @($outputs | Where-Object { $_ -match '^(.+\.(exe|dll)): ' } | ForEach-Object { $Matches[1] } |
            Where-Object { Test-Path -LiteralPath ([IO.Path]::GetFullPath($_, $build)) })
        $graph = @(Native $ninja (@('-C', $build, '-t', 'graph', 'all') + $exeOutputs))
        $nodes = @{}
        $paths = @{}
        $forward = @{}
        foreach ($line in $graph) {
            if ($line -match '^"([^\"]+)" \[label="((?:\\.|[^"\\])*)"(.*)\]') {
                $id = $Matches[1]; $label = $Matches[2].Replace('\"', '"').Replace('\\', '\')
                if ($Matches[3] -notmatch 'shape=ellipse') {
                    $key = Canonical $label $build
                    $nodes[$id] = $label
                    if (!$paths.ContainsKey($key)) { $paths[$key] = [Collections.Generic.List[string]]::new() }
                    $paths[$key].Add($id)
                }
            } elseif ($line -match '^"([^\"]+)" -> "([^\"]+)"(.*)') {
                $from = $Matches[1]; $to = $Matches[2]
                # Order-only directory/autogen ordering does not propagate source impact.
                if ($Matches[3] -match 'style=dotted') { continue }
                if (!$forward.ContainsKey($from)) { $forward[$from] = [Collections.Generic.List[string]]::new() }
                $forward[$from].Add($to)
            }
        }
        $dependencies = @{}
        $depObjects = [Collections.Generic.HashSet[string]]::new()
        $object = $null
        $stale = $false
        foreach ($line in (Native $ninja @('-C', $build, '-t', 'deps'))) {
            if ($line -match '^(.+): #deps \d+, deps mtime .+ \((VALID|STALE)\)') {
                $object = Canonical $Matches[1] $build
                [void]$depObjects.Add($object)
                if ($Matches[2] -ne 'VALID') { $stale = $true }
            } elseif ($object -and $line -match '^    (.+)$') {
                $key = Canonical $Matches[1] $build
                if (!$dependencies.ContainsKey($key)) { $dependencies[$key] = [Collections.Generic.HashSet[string]]::new() }
                [void]$dependencies[$key].Add($object)
            }
        }
        if (!$dependencies.Count -or $stale) { throw 'Ninja dependency inventory is empty or stale; rebuild first.' }
        foreach ($label in $nodes.Values) {
            if ($label -match '\.(obj|o)$' -and !$depObjects.Contains((Canonical $label $build))) {
                throw "Incomplete Ninja dependency inventory: $label. Build the complete configuration first."
            }
        }
        $caseExecutables = @{}
        $caseInputs = @{}
        foreach ($case in $cases) {
            if (!$case.command.Count) { throw "CTest command is absent; build first: $($case.name)" }
            $exe = Canonical $case.command[0] $build
            $caseExecutables[$case.name] = $exe
            $working = @($case.properties | Where-Object name -eq WORKING_DIRECTORY | ForEach-Object value)
            $workingRoot = if ($working.Count) { $working[0] } else { $build }
            $required = @($case.properties | Where-Object name -eq REQUIRED_FILES | ForEach-Object value)
            $inputs = @($case.command | Where-Object { [IO.Path]::IsPathRooted($_) }) + $required
            foreach ($inputPath in $inputs) {
                $inputKey = Canonical $inputPath $workingRoot
                if (!$caseInputs.ContainsKey($inputKey)) { $caseInputs[$inputKey] = [Collections.Generic.List[string]]::new() }
                $caseInputs[$inputKey].Add($case.name)
            }
        }
        foreach ($rule in $ruleChanges) {
            foreach ($pattern in $rule.tests) {
                $matches = @($cases | Where-Object name -match $pattern)
                if (!$matches.Count) { throw "Impact rule matches no configured tests: $pattern" }
                foreach ($case in $matches) { [void]$selected.Add($case.name) }
            }
        }
        foreach ($path in $graphChanges) {
            $key = Canonical $path $repo
            $seeds = [Collections.Generic.HashSet[string]]::new()
            if ($paths.ContainsKey($key)) { foreach ($id in $paths[$key]) { [void]$seeds.Add($id) } }
            if ($dependencies.ContainsKey($key)) {
                foreach ($obj in $dependencies[$key]) {
                    if ($paths.ContainsKey($obj)) { foreach ($id in $paths[$obj]) { [void]$seeds.Add($id) } }
                }
            }
            $known = $seeds.Count -gt 0
            foreach ($case in $cases) {
                $working = @($case.properties | Where-Object name -eq WORKING_DIRECTORY | ForEach-Object value)
                $workingRoot = if ($working.Count) { $working[0] } else { $build }
                $required = @($case.properties | Where-Object name -eq REQUIRED_FILES | ForEach-Object value)
                $inputs = @($case.command | Where-Object { [IO.Path]::IsPathRooted($_) }) + $required
                foreach ($inputPath in $inputs) {
                    $inputKey = Canonical $inputPath $workingRoot
                    if ($key -eq $inputKey -or $key.StartsWith($inputKey.TrimEnd('/') + '/')) {
                        $known = $true; [void]$selected.Add($case.name)
                    }
                }
            }
            $queue = [Collections.Generic.Queue[string]]::new()
            $seen = [Collections.Generic.HashSet[string]]::new()
            foreach ($seed in $seeds) { $queue.Enqueue($seed) }
            while ($queue.Count) {
                $node = $queue.Dequeue()
                if (!$seen.Add($node)) { continue }
                foreach ($next in $forward[$node]) { $queue.Enqueue($next) }
            }
            foreach ($case in $cases) {
                $exe = $caseExecutables[$case.name]
                foreach ($id in $paths[$exe]) { if ($seen.Contains($id)) { [void]$selected.Add($case.name) } }
            }
            # Compile affected production outputs even when no test wraps them.
            $hasBuildOutput = $false
            foreach ($node in $seen) {
                if (!$nodes.ContainsKey($node)) { continue }
                if ($nodes[$node] -match '\.(exe|dll|lib|a)$') {
                    [void]$targets.Add($nodes[$node])
                    $hasBuildOutput = $true
                }
                $inputKey = Canonical $nodes[$node] $build
                foreach ($name in $caseInputs[$inputKey]) { [void]$selected.Add($name) }
            }
            if (!$hasBuildOutput) {
                foreach ($node in $seen) {
                    if ($nodes.ContainsKey($node) -and $nodes[$node] -match '\.(obj|o)$') { [void]$targets.Add($nodes[$node]) }
                }
            }
            if (!$known) {
                $covered = @($rules.rules | Where-Object { Matches-Any $path $_.paths }).Count -gt 0
                if (!$covered) { Full "Unmapped input: $path" }
            } else { $result.reasons += "Ninja/CTest input: $path" }
        }
        foreach ($case in $cases) {
            if (!$selected.Contains($case.name)) { continue }
            $exe = $caseExecutables[$case.name]
            if ($paths.ContainsKey($exe)) {
                [void]$targets.Add($nodes[$paths[$exe][0]])
            } elseif ($case.command[0] -match '\.(exe|dll)$' -and $exe.StartsWith((Canonical $build $repo) + '/')) {
                throw "No Ninja target for selected executable: $($case.command[0])"
            }
            $labels = @($case.properties | Where-Object name -eq LABELS | ForEach-Object value)
            if ('gui' -in $labels) { $result.guiTests += $case.name }
        }
    }
    if ($result.mode -ne 'full' -and ($selected.Count -or $targets.Count -or $extra.Count)) { $result.mode = 'scoped' }
    if (!$changes.Count) { $result.reasons += 'No changed files.' }
} catch {
    Full $_.Exception.Message
} finally {
    Pop-Location
}
$result.tests = @($selected | Sort-Object)
$result.guiTests = @($result.guiTests | Sort-Object -Unique)
$result.targets = @($targets | Sort-Object)
$result.extraChecks = @($extra | Sort-Object)
$result | ConvertTo-Json -Depth 8
