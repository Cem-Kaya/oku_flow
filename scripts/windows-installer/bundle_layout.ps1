# Shared first-party bundle paths. Legacy published layouts remain readable.
function Get-OkuFlowBundleLayout([string]$BundlePath) {
    $root = (Resolve-Path -LiteralPath $BundlePath).Path
    $prefix = if (Test-Path -LiteralPath (Join-Path $root 'app') -PathType Container) { 'app/' } else { '' }
    [pscustomobject]@{
        Root = $root
        PayloadRoot = if ($prefix) { Join-Path $root 'app' } else { $root }
        Prefix = $prefix
        ApplicationRelativePath = $prefix + 'OkuFlowApp.exe'
    }
}
