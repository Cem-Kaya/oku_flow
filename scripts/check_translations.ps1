[CmdletBinding()]
param(
    [switch]$UpdateManifest,
    [string]$QtPrefix = $env:QT_PREFIX
)

$ErrorActionPreference = "Stop"

$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$translationDirectory = Join-Path $repoRoot "translations"
$manifestPath = Join-Path $repoRoot "src\ui\translation_catalog.cpp"
$catalogPaths = @(
    (Join-Path $translationDirectory "openzoom_tr.ts"),
    (Join-Path $translationDirectory "openzoom_de.ts")
)

function Read-Catalog {
    param([string]$Path)

    $bytes = [IO.File]::ReadAllBytes($Path)
    for ($i = 0; $i -lt $bytes.Length; ++$i) {
        if ($bytes[$i] -eq 13) {
            throw "$Path contains CRLF or bare CR line endings; translation files must use LF."
        }
    }

    [xml]$document = [Text.Encoding]::UTF8.GetString($bytes)
    $messages = @($document.SelectNodes("/TS/context/message"))
    if ($messages.Count -eq 0) {
        throw "$Path contains no translation messages."
    }

    $translations =
        [Collections.Generic.Dictionary[string, string]]::new(
            [StringComparer]::Ordinal)
    foreach ($message in $messages) {
        $sourceNode = $message.SelectSingleNode("source")
        $source = [string]$sourceNode.InnerText
        if ([string]::IsNullOrWhiteSpace($source)) {
            throw "$Path contains a message with an empty source string."
        }
        if ($translations.ContainsKey($source)) {
            throw "$Path contains duplicate source text: $source"
        }

        $translationNode = $message.SelectSingleNode("translation")
        $translation = [string]$translationNode.InnerText
        if ($translationNode.GetAttribute("type") -eq "unfinished" -or
            [string]::IsNullOrWhiteSpace($translation)) {
            throw "$Path has an empty or unfinished translation for: $source"
        }
        $translations[$source] = $translation
    }
    return $translations
}

function Write-Manifest {
    param(
        [string]$Path,
        [string[]]$Sources
    )

    $lines = [Collections.Generic.List[string]]::new()
    $lines.Add("// Generated translation extraction manifest. Keep this file in sync with")
    $lines.Add("// translations/*.ts by running scripts/check_translations.ps1 -UpdateManifest.")
    $lines.Add("")
    $lines.Add("#include <QtCore/qglobal.h>")
    $lines.Add("")
    $lines.Add("#include <cstddef>")
    $lines.Add("")
    $lines.Add("namespace openzoom {")
    $lines.Add("namespace {")
    $lines.Add("[[maybe_unused]] const char* const kOpenZoomTranslationSources[] = {")
    foreach ($source in $Sources) {
        $encoded = ConvertTo-Json $source -Compress
        $lines.Add("    QT_TRANSLATE_NOOP(`"OpenZoom`", $encoded),")
    }
    $lines.Add("};")
    $lines.Add("} // namespace")
    $lines.Add("")
    $lines.Add("const char* const* TranslationCatalogSources(std::size_t* count)")
    $lines.Add("{")
    $lines.Add("    if (count) {")
    $lines.Add("        *count = sizeof(kOpenZoomTranslationSources) /")
    $lines.Add("                 sizeof(kOpenZoomTranslationSources[0]);")
    $lines.Add("    }")
    $lines.Add("    return kOpenZoomTranslationSources;")
    $lines.Add("}")
    $lines.Add("")
    $lines.Add("} // namespace openzoom")

    [IO.File]::WriteAllText(
        $Path,
        (($lines -join "`n") + "`n"),
        [Text.UTF8Encoding]::new($false))
}

function Read-ManifestSources {
    param([string]$Path)

    if (-not (Test-Path $Path)) {
        throw "Translation extraction manifest is missing: $Path"
    }
    $bytes = [IO.File]::ReadAllBytes($Path)
    for ($i = 0; $i -lt $bytes.Length; ++$i) {
        if ($bytes[$i] -eq 13) {
            throw "$Path contains CRLF or bare CR line endings; generated sources must use LF."
        }
    }

    $text = [Text.Encoding]::UTF8.GetString($bytes)
    $pattern = 'QT_TRANSLATE_NOOP\("OpenZoom",\s*("(?:\\.|[^"\\])*")\)'
    $sources =
        [Collections.Generic.Dictionary[string, bool]]::new(
            [StringComparer]::Ordinal)
    foreach ($match in [regex]::Matches($text, $pattern)) {
        $source = [string](ConvertFrom-Json $match.Groups[1].Value)
        if ($sources.ContainsKey($source)) {
            throw "$Path contains duplicate source text: $source"
        }
        $sources[$source] = $true
    }
    if ($sources.Count -eq 0) {
        throw "$Path contains no QT_TRANSLATE_NOOP entries."
    }
    return $sources
}

function Compare-SourceSets {
    param(
        [string]$LeftName,
        [object]$Left,
        [string]$RightName,
        [object]$Right
    )

    $missing = @($Left.Keys | Where-Object { -not $Right.ContainsKey($_) } | Sort-Object)
    $extra = @($Right.Keys | Where-Object { -not $Left.ContainsKey($_) } | Sort-Object)
    if ($missing.Count -gt 0 -or $extra.Count -gt 0) {
        $details = [Collections.Generic.List[string]]::new()
        foreach ($source in $missing) {
            $details.Add("missing from ${RightName}: $source")
        }
        foreach ($source in $extra) {
            $details.Add("missing from ${LeftName}: $source")
        }
        throw "Translation source sets differ:`n$($details -join "`n")"
    }
}

$turkish = Read-Catalog $catalogPaths[0]
$german = Read-Catalog $catalogPaths[1]
Compare-SourceSets "Turkish catalog" $turkish "German catalog" $german

$sortedSources = @($turkish.Keys | Sort-Object)
if ($UpdateManifest) {
    Write-Manifest $manifestPath $sortedSources
    Write-Host "Updated translation extraction manifest with $($sortedSources.Count) sources."
}

$manifest = Read-ManifestSources $manifestPath
Compare-SourceSets "translation manifest" $manifest "catalogs" $turkish

if ([string]::IsNullOrWhiteSpace($QtPrefix)) {
    $QtPrefix = "C:\Qt\6.9.3\msvc2022_64"
}
$lrelease = Join-Path $QtPrefix "bin\lrelease.exe"
if (-not (Test-Path $lrelease)) {
    $command = Get-Command lrelease.exe -ErrorAction SilentlyContinue
    if ($null -eq $command) {
        throw "lrelease.exe was not found. Set QT_PREFIX to the Qt msvc2022_64 root."
    }
    $lrelease = $command.Source
}

$temporaryDirectory =
    Join-Path ([IO.Path]::GetTempPath()) ("openzoom-translations-" + [guid]::NewGuid())
[IO.Directory]::CreateDirectory($temporaryDirectory) | Out-Null
try {
    foreach ($catalogPath in $catalogPaths) {
        $qmPath =
            Join-Path $temporaryDirectory (([IO.Path]::GetFileNameWithoutExtension($catalogPath)) + ".qm")
        & $lrelease $catalogPath -qm $qmPath -nounfinished
        if ($LASTEXITCODE -ne 0 -or -not (Test-Path $qmPath)) {
            throw "lrelease failed for $catalogPath."
        }
    }
}
finally {
    Remove-Item $temporaryDirectory -Recurse -Force -ErrorAction SilentlyContinue
}

Write-Host "PASS: $($sortedSources.Count) complete Turkish/German translations match the extraction manifest."
