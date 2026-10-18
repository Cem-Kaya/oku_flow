#Requires -Version 7.0
[CmdletBinding()]
param(
    [string]$CacheRoot = (Join-Path $env:LOCALAPPDATA 'OkuFlow/ocr-deps'),
    [string]$RuntimeRoot = (Join-Path $env:LOCALAPPDATA 'OkuFlow/ocr-runtime'),
    [switch]$TensorRT
)
$ErrorActionPreference='Stop'
$pins=Get-Content -LiteralPath (Join-Path $PSScriptRoot 'uct/pins.json') -Raw | ConvertFrom-Json -AsHashtable
function Get-Verified([string]$Url,[string]$Path,[string]$Hash) {
    if (!(Test-Path -LiteralPath $Path)) {
        New-Item -ItemType Directory -Path (Split-Path $Path -Parent) -Force | Out-Null
        Invoke-WebRequest -Uri $Url -OutFile $Path
    }
    if ((Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash -ine $Hash) {
        throw "UCT artifact hash mismatch; preserved for inspection: $Path"
    }
}
function Expand-Verified([hashtable]$Pin) {
    $archive=Join-Path $CacheRoot $Pin.archive
    Get-Verified $Pin.url $archive $Pin.sha256
    $root=Join-Path $CacheRoot ([IO.Path]::GetFileNameWithoutExtension($Pin.archive))
    if (!(Test-Path -LiteralPath $root)) {Expand-Archive -LiteralPath $archive -DestinationPath $root}
    $complete=Join-Path $root '.okuflow-verified-extraction'
    if (!(Test-Path -LiteralPath $complete) -or (Get-Content -LiteralPath $complete -Raw).Trim() -ne $Pin.sha256) {
        # An existing directory alone does not prove extraction completed.
        # Check exact uncompressed bytes against the verified upstream archive.
        $zip=[IO.Compression.ZipFile]::OpenRead($archive)
        try {
            $prefix=[IO.Path]::GetFullPath($root)+[IO.Path]::DirectorySeparatorChar
            foreach($entry in $zip.Entries) {
                if(!$entry.Name){continue}
                $path=[IO.Path]::GetFullPath((Join-Path $root $entry.FullName))
                if(!$path.StartsWith($prefix,[StringComparison]::OrdinalIgnoreCase)){throw 'Unsafe OCR archive entry.'}
                if(!(Test-Path -LiteralPath $path) -or (Get-Item -LiteralPath $path).Length -ne $entry.Length){
                    throw "Incomplete OCR SDK extraction, preserved for inspection: $path"
                }
                $stream=$entry.Open()
                try{$hash=[Convert]::ToHexString([Security.Cryptography.SHA256]::HashData($stream))}finally{$stream.Dispose()}
                if((Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash -ine $hash){throw "Changed OCR SDK extraction: $path"}
            }
            [IO.File]::WriteAllText($complete,$Pin.sha256)
        } finally {$zip.Dispose()}
    }
    return $root
}
$ortExtract=Expand-Verified $pins.ort
$ort=Join-Path $ortExtract 'onnxruntime-win-x64-1.31.0'
$dll=Join-Path $ort 'lib/onnxruntime.dll'
Get-Verified '' $dll $pins.ort.dllSha256
New-Item -ItemType Directory -Path (Join-Path $RuntimeRoot 'cpu') -Force | Out-Null
Copy-Item -LiteralPath $dll -Destination (Join-Path $RuntimeRoot 'cpu/onnxruntime.dll') -Force
foreach($notice in @('LICENSE','ThirdPartyNotices.txt','Privacy.md')) {
    Copy-Item -LiteralPath (Join-Path $ort $notice) -Destination (Join-Path $RuntimeRoot "cpu/$notice") -Force
}
foreach($model in $pins.models) {
    foreach($name in $model.files.Keys) {
        $cached=Join-Path $CacheRoot "models/$($model.directory)/$name"
        Get-Verified "https://huggingface.co/$($model.repository)/resolve/$($model.revision)/$name" $cached $model.files[$name]
        $target=Join-Path $RuntimeRoot "$($model.directory)/$name"
        New-Item -ItemType Directory -Path (Split-Path $target -Parent) -Force | Out-Null
        Copy-Item -LiteralPath $cached -Destination $target -Force
    }
}
# Keep the full Apache license with models as well as the exact model cards.
$apache=Join-Path $PSScriptRoot '../third_party/paddleocr/LICENSE'
Copy-Item -LiteralPath $apache -Destination (Join-Path $RuntimeRoot 'MODEL_LICENSES.txt') -Force
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'uct/pins.json') -Destination (Join-Path $RuntimeRoot 'pins.json') -Force
if($TensorRT) {
    $sdk=Join-Path (Expand-Verified $pins.tensorrt) 'TensorRT-11.4.0.106'
    Write-Host "Optional external TensorRT SDK: $sdk"
    Write-Host "NVIDIA SDK terms: $($pins.tensorrt.license)"
    Write-Host 'The SDK is a development/runtime installation, not a release staging inventory.'
}
Write-Host "UCT models and CPU runtime verified: $RuntimeRoot"
Write-Host "Set OKUFLOW_UCT_ROOT for another runtime directory. Reconfigure CMake after SDK preparation."
