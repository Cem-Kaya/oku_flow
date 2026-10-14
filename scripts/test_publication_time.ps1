#Requires -Version 7.0
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'windows-installer/publication_time.ps1')
$expected = [DateTime]::new(2026, 10, 14, 14, 9, 18, [DateTimeKind]::Utc)
$cases = @(
    '2026-10-14T14:09:18Z',
    '2026-10-14T17:09:18+03:00',
    '2026-10-14T10:09:18-04:00',
    $expected,
    [DateTimeOffset]::new(2026, 10, 14, 17, 9, 18, [TimeSpan]::FromHours(3)),
    (('{"generatedUtc":"2026-10-14T14:09:18Z"}' | ConvertFrom-Json).generatedUtc)
)
foreach ($value in $cases) {
    $actual = Get-PublicationUtcTimestamp $value
    if ($actual -ne $expected -or $actual.Kind -ne [DateTimeKind]::Utc) { throw 'UTC instant changed.' }
    if ($actual -lt $expected.AddSeconds(-1)) { throw 'A fresh manifest was treated as stale.' }
    if ($actual -ge $expected.AddSeconds(1)) { throw 'A stale manifest was treated as fresh.' }
}
foreach ($value in @($null, '', '2026-10-14T14:09:18', 'invalid',
                     [DateTime]::SpecifyKind($expected, [DateTimeKind]::Unspecified))) {
    $rejected = $false
    try { [void](Get-PublicationUtcTimestamp $value) } catch { $rejected = $true }
    if (!$rejected) { throw 'Ambiguous timestamp was accepted.' }
}
Write-Output 'PASS: 6 UTC/offset/JSON forms preserve freshness; 5 ambiguous timestamps rejected.'
