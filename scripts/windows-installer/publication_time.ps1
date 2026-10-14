# First-party UTC conversion shared by the publisher and its offline regression.
function Get-PublicationUtcTimestamp([object]$Value) {
    if ($Value -is [DateTimeOffset]) { return $Value.UtcDateTime }
    if ($Value -is [DateTime]) {
        if ($Value.Kind -eq [DateTimeKind]::Unspecified) {
            throw 'Publication timestamps require an explicit UTC/offset kind.'
        }
        return $Value.ToUniversalTime()
    }
    if ($Value -isnot [string] -or $Value -notmatch '^\d{4}-\d{2}-\d{2}T.+(?:Z|[+-]\d{2}:\d{2})$') {
        throw 'Publication timestamps require ISO-8601 with an explicit timezone.'
    }
    $parsed = [DateTimeOffset]::MinValue
    if (![DateTimeOffset]::TryParse($Value, [Globalization.CultureInfo]::InvariantCulture,
        [Globalization.DateTimeStyles]::None, [ref]$parsed)) {
        throw 'Invalid publication timestamp.'
    }
    $parsed.UtcDateTime
}
