function Get-OkuFlowReleaseBuildFacts([string]$BuildDirectory, [string]$Configuration = 'Release', [string]$ApplicationPath = '') {
    if ($Configuration -cnotmatch '^[A-Za-z0-9_-]+$') { throw 'Invalid build-facts configuration.' }
    $build = (Resolve-Path -LiteralPath $BuildDirectory).Path
    $path = Join-Path $build "okuflow-build-facts/$Configuration.json"
    $facts = Get-Content -LiteralPath $path -Raw | ConvertFrom-Json -Depth 8
    $cache = @{}
    foreach ($line in Get-Content -LiteralPath (Join-Path $build 'CMakeCache.txt')) {
        if ($line -match '^([^#/][^:]*):[^=]+=(.*)$') { $cache[$Matches[1]] = $Matches[2] }
    }
    $enabled = $cache['OKUFLOW_ENABLE_CUDA'] -in @('1','ON','TRUE','YES')
    if ($facts.schema -ne 1 -or $facts.target -cne 'oku_flow' -or
        $facts.configuration -cne $Configuration -or $facts.cuda.enabled -isnot [bool] -or
        $facts.cuda.enabled -ne $enabled -or -not $cache.ContainsKey('OKUFLOW_ENABLE_CUDA') -or
        [IO.Path]::GetFullPath($facts.buildDirectory) -ne $build -or
        [IO.Path]::GetFullPath($facts.sourceDirectory) -ne [IO.Path]::GetFullPath($cache['CMAKE_HOME_DIRECTORY']) -or
        $facts.generator -cne $cache['CMAKE_GENERATOR']) {
        throw 'Release build facts do not match the configured target/cache.'
    }
    if (($cache['CMAKE_CONFIGURATION_TYPES'] -and $Configuration -cnotin $cache['CMAKE_CONFIGURATION_TYPES'].Split(';')) -or
        (-not $cache['CMAKE_CONFIGURATION_TYPES'] -and $cache['CMAKE_BUILD_TYPE'] -cne $Configuration)) {
        throw 'Release build facts do not match the selected build configuration.'
    }
    $application = [IO.Path]::GetFullPath($facts.application)
    if (-not $application.StartsWith($build.TrimEnd('\','/') + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase) -or
        -not (Test-Path -LiteralPath $application -PathType Leaf)) {
        throw 'Release build facts require the actual built application inside this build directory.'
    }
    $seal = Get-Content -LiteralPath (Join-Path $build "okuflow-build-facts/$Configuration.seal.json") -Raw | ConvertFrom-Json -Depth 4
    $applicationHash = (Get-FileHash -LiteralPath $application -Algorithm SHA256).Hash.ToLowerInvariant()
    if ($seal.schema -ne 1 -or $seal.factsSha256 -cne (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant() -or
        $seal.applicationSha256 -cne $applicationHash) {
        throw 'Release facts/application lack a matching successful-build seal; rebuild the application target.'
    }
    if ($ApplicationPath -and $applicationHash -cne (Get-FileHash -LiteralPath $ApplicationPath).Hash.ToLowerInvariant()) {
        throw 'Staged application does not match the configured build-facts target.'
    }
    if ($enabled) {
        if ($facts.cuda.compilerVersion -notmatch '^\d+\.\d+' -or $facts.cuda.toolkitVersion -notmatch '^\d+\.\d+' -or
            $facts.cuda.runtimeLibrary -cne 'Static' -or @($facts.cuda.architectures).Count -eq 0 -or
            @($facts.cuda.architectures | Where-Object { $_ -notmatch '^\d+(-real|-virtual)?$' }).Count -ne 0 -or
            [IO.Path]::GetFullPath($facts.cuda.compiler) -ne [IO.Path]::GetFullPath($cache['CMAKE_CUDA_COMPILER']) -or
            (Get-FileHash -LiteralPath $facts.cuda.compiler).Hash.ToLowerInvariant() -cne $facts.cuda.compilerSha256 -or
            (Get-FileHash -LiteralPath $facts.cuda.runtimePath).Hash.ToLowerInvariant() -cne $facts.cuda.runtimeSha256 -or
            -not $facts.cuda.license -or (Get-FileHash -LiteralPath $facts.cuda.license).Hash.ToLowerInvariant() -cne $facts.cuda.licenseSha256) {
            throw 'Release CUDA compiler, target facts or exact toolkit license could not be verified.'
        }
        $root = [IO.Path]::GetFullPath($facts.cuda.toolkitRoot).TrimEnd('\','/') + [IO.Path]::DirectorySeparatorChar
        foreach ($file in @($facts.cuda.compiler, $facts.cuda.runtimePath, $facts.cuda.license)) {
            if (-not [IO.Path]::GetFullPath($file).StartsWith($root, [StringComparison]::OrdinalIgnoreCase)) {
                throw 'Release CUDA compiler/license is outside the configured toolkit.'
            }
        }
    } else {
        foreach ($name in @('compiler','compilerVersion','compilerSha256','toolkitRoot','toolkitVersion','runtimeLibrary','runtimePath','runtimeSha256','license','licenseSha256')) {
            if ($null -ne $facts.cuda.$name) { throw 'CPU build facts must not declare CUDA provenance.' }
        }
        if (@($facts.cuda.architectures).Count -ne 0) { throw 'CPU build facts must not declare CUDA architectures.' }
    }
    return $facts
}
