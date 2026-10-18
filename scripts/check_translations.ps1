[CmdletBinding()]
param(
    [switch]$UpdateManifest,
    [string]$QtPrefix = $env:QT_PREFIX
)

$ErrorActionPreference = "Stop"

$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$translationDirectory = Join-Path $repoRoot "translations"
$manifestPath = Join-Path $repoRoot "src\ui\translation_catalog.cpp"
# Every non-English UI language in src/app/app_language.cpp, in picker order.
# Turkish and German are the reference catalogs; every catalog must have the
# exact same source set as the generated extraction manifest.
$catalogLanguages = @(
    "tr", "de", "fr", "es", "it", "pt_BR", "ru", "uk", "ar", "hi", "ja", "ko",
    "zh_Hans", "nl", "pl", "cs", "sv", "id", "vi", "th", "bn", "fa", "he", "ur"
)
$catalogPaths = @($catalogLanguages | ForEach-Object {
    Join-Path $translationDirectory "okuflow_$_.ts"
})

# Sources whose established translation can legitimately equal the English
# text (product names, units, technical terms, internationally used words).
# Anything else identical to its source is reported as an untranslated
# English placeholder.
$identicalAllowed =
    [Collections.Generic.HashSet[string]]::new([StringComparer]::Ordinal)
foreach ($source in @(
    "Sigma", "Normal", "Model", "Codex CLI", "NVIDIA Video Effects",
    "NVIDIA Super Resolution", "Zoom", "Radius", "Backend", "Text",
    "Minimal", "Ellipse", "%1 px", "API Key", "Assistant", "Status",
    ("%1 " + [char]0x00D7 + " %2 @ %3 fps"), "OK")) {
    [void]$identicalAllowed.Add($source)
}

# These words are spelled identically in the named languages. Keep exceptions
# narrow and language-specific; untranslated English sentences still fail.
$identicalByLanguage = @{
    # Reviewed native video labels: these are established local UI terms.
    tr = @("Video")
    de = @("Details", "Video", "Live")
    fr = @("Simple", "Orientation", "Conversation", "Application", "Assistant", "Image", "Rectangle", "Microphone", "Photo", " [REC original, %1]")
    es = @(" [REC original, %1]")
    pt_BR = @(" [REC original, %1]")
    nl = @("Details", "Camera", "Stop", "Focus X:", "Focus Y:", "Pen", "Later", "Contrast", "Video", "Live")
    cs = @("Video")
    sv = @("Video", "Live", " [REC original, %1]")
    it = @("Video")
    id = @("Video")
    vi = @("Video")
}

function Get-FormatTokens {
    param([string]$Text)

    $tokens = @([regex]::Matches($Text, '%L?\d|%n') |
        ForEach-Object { $_.Value } | Sort-Object)
    $escapedAmpersands = ([regex]::Matches($Text, '&&')).Count
    $lineBreaks = ([regex]::Matches($Text, "`n")).Count
    return "$($tokens -join ',')|&&=$escapedAmpersands|nl=$lineBreaks"
}

function Read-Catalog {
    param([string]$Path)
    $language = [IO.Path]::GetFileNameWithoutExtension($Path).Substring("okuflow_".Length)

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
        $sourceTokens = Get-FormatTokens $source
        $translationTokens = Get-FormatTokens $translation
        if ($sourceTokens -ne $translationTokens) {
            throw "$Path changes placeholders, escaped ampersands, or line breaks ($sourceTokens -> $translationTokens) for: $source"
        }
        $legitimateIdentity = $identicalByLanguage.ContainsKey($language) -and
            $identicalByLanguage[$language] -ccontains $source
        if ($translation -ceq $source -and -not $identicalAllowed.Contains($source) -and -not $legitimateIdentity) {
            throw "$Path leaves an English placeholder translation for: $source"
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
    $lines.Add("namespace okuflow {")
    $lines.Add("namespace {")
    $lines.Add("[[maybe_unused]] const char* const kOkuFlowTranslationSources[] = {")
    foreach ($source in $Sources) {
        $encoded = ConvertTo-Json $source -Compress
        $lines.Add("    QT_TRANSLATE_NOOP(`"OkuFlow`", $encoded),")
    }
    $lines.Add("};")
    $lines.Add("} // namespace")
    $lines.Add("")
    $lines.Add("const char* const* TranslationCatalogSources(std::size_t* count)")
    $lines.Add("{")
    $lines.Add("    if (count) {")
    $lines.Add("        *count = sizeof(kOkuFlowTranslationSources) /")
    $lines.Add("                 sizeof(kOkuFlowTranslationSources[0]);")
    $lines.Add("    }")
    $lines.Add("    return kOkuFlowTranslationSources;")
    $lines.Add("}")
    $lines.Add("")
    $lines.Add("} // namespace okuflow")

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
    $pattern = 'QT_TRANSLATE_NOOP\("OkuFlow",\s*("(?:\\.|[^"\\])*")\)'
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

$unlisted = @(Get-ChildItem -LiteralPath $translationDirectory -Filter "okuflow_*.ts" |
    Where-Object { $catalogPaths -notcontains $_.FullName } |
    ForEach-Object { $_.Name })
if ($unlisted.Count -gt 0) {
    throw "Translation catalogs are not registered in check_translations.ps1: $($unlisted -join ', ')"
}

$turkish = Read-Catalog $catalogPaths[0]
for ($index = 1; $index -lt $catalogPaths.Count; ++$index) {
    $catalog = Read-Catalog $catalogPaths[$index]
    Compare-SourceSets "Turkish catalog" $turkish "$($catalogLanguages[$index]) catalog" $catalog
}

$sortedSources = @($turkish.Keys | Sort-Object)
if ($UpdateManifest) {
    Write-Manifest $manifestPath $sortedSources
    Write-Host "Updated translation extraction manifest with $($sortedSources.Count) sources."
}

$manifest = Read-ManifestSources $manifestPath
Compare-SourceSets "translation manifest" $manifest "catalogs" $turkish

if ([string]::IsNullOrWhiteSpace($QtPrefix)) {
    $QtPrefix = "C:\Qt\6.12.0\msvc2022_64"
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
    Join-Path ([IO.Path]::GetTempPath()) ("okuflow-translations-" + [guid]::NewGuid())
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

Write-Host "PASS: $($catalogPaths.Count) catalogs each have $($sortedSources.Count) complete translations matching the extraction manifest."
