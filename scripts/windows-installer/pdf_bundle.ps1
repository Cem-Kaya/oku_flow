function Get-OkuFlowPdfBundleMetadata {
    param([string]$PayloadRoot,[string]$RepositoryRoot,[string]$Prefix)
    $pin = Get-Content (Join-Path $RepositoryRoot 'scripts/qpdf/pins.json') -Raw | ConvertFrom-Json
    if (!(Test-Path -LiteralPath (Join-Path $PayloadRoot 'qpdf'))) { return $null }
    $files = @(); $relationships = @(); $packages = @()
    foreach ($record in @($pin.files) + @($pin.notices)) {
        $relative = "qpdf/$($record.path)"
        $path = Join-Path $PayloadRoot $relative
        if (!(Test-Path -LiteralPath $path -PathType Leaf) -or
            (Get-FileHash -LiteralPath $path).Hash.ToLowerInvariant() -cne $record.sha256) { throw "PDF export pin mismatch: $relative" }
        $id = 'SPDXRef-File-qpdf-' + ($record.path -replace '[^A-Za-z0-9.-]','-')
        $files += [ordered]@{SPDXID=$id;fileName='./'+$Prefix+$relative;checksums=@([ordered]@{algorithm='SHA256';checksumValue=$record.sha256});licenseConcluded='NOASSERTION';copyrightText='NOASSERTION'}
        $relationships += [ordered]@{spdxElementId='SPDXRef-Package-qpdf';relationshipType='CONTAINS';relatedSpdxElement=$id}
    }
    $expected = @(@($pin.files) + @($pin.notices) | ForEach-Object { "qpdf/$($_.path)" })
    foreach ($file in Get-ChildItem (Join-Path $PayloadRoot 'qpdf') -File -Recurse) {
        if ([IO.Path]::GetRelativePath($PayloadRoot,$file.FullName).Replace('\','/') -cnotin $expected) { throw 'Unexpected PDF export runtime file.' }
    }
    foreach ($component in @(
        @('qpdf',$pin.version,'Apache-2.0','https://github.com/qpdf/qpdf'),
        @('OpenSSL','3.6.4','Apache-2.0','https://openssl-library.org/'),
        @('zlib','1.3.2','Zlib','https://zlib.net/'),
        @('libjpeg-turbo','3.2.0','IJG AND BSD-3-Clause AND Zlib','https://libjpeg-turbo.org/'))) {
        $id = 'SPDXRef-Package-'+$component[0]
        $packages += [ordered]@{SPDXID=$id;name=$component[0];versionInfo=$component[1];downloadLocation=$component[3];filesAnalyzed=$false;licenseDeclared=$component[2];licenseConcluded=$component[2];copyrightText='NOASSERTION';comment='Unmodified upstream Windows runtime. Exact notices retained under qpdf/licenses; dependency versions from the upstream release vcpkg SPDX inventory.'}
        $relationships += [ordered]@{spdxElementId= $(if ($component[0] -eq 'qpdf') {'SPDXRef-Package-OkuFlow'}else{'SPDXRef-Package-qpdf'});relationshipType=$(if($component[0] -eq 'qpdf'){'DEPENDS_ON'}else{'STATIC_LINK'});relatedSpdxElement=$id}
    }
    return @{Packages=$packages;Files=$files;Relationships=$relationships;Version=$pin.version}
}
