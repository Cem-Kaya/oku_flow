#requires -Version 7.0
[CmdletBinding()]
param()
$ErrorActionPreference='Stop'
$root=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
$json=[IO.File]::ReadAllText((Join-Path $PSScriptRoot 'formula-packs.json')).Replace("`r`n","`n")
$parsed=$json|ConvertFrom-Json -AsHashtable
if($parsed.schema-ne1 -or $parsed.recipe-ne'okuflow-formula-tf32-partitions-v1' -or $parsed.models.Count-ne4){throw 'Unsupported formula catalog.'}
if($json.Contains(')FORMULA"')){throw 'Formula catalog contains the C++ delimiter.'}
# MSVC limits each literal. Independent QByteArray chunks avoid both literal
# and concatenated-literal limits without altering pinned JSON bytes.
$parts=@()
for($offset=0;$offset-lt$json.Length;$offset+=8000){
    $chunk=$json.Substring($offset,[Math]::Min(8000,$json.Length-$offset))
    $parts+='QByteArray(R"FORMULA('+$chunk+')FORMULA")'
}
$header='#pragma once'+"`n"+'#include <QByteArray>'+"`n"+
    '// Native partition identities. No public graph download is enabled yet.'+"`n"+
    'namespace okuflow::uct::formula {'+"`n"+'inline const QByteArray kCatalog = '+
    ($parts-join(" +`n"))+';'+"`n"+'}'+"`n"
[IO.File]::WriteAllText((Join-Path $root 'src/common/uct_formula_catalog.hpp'),$header,[Text.UTF8Encoding]::new($false))
