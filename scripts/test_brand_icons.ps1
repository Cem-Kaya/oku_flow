#requires -Version 7.0
[CmdletBinding()]
param([Parameter(Mandatory)][string]$BundlePath)
$ErrorActionPreference='Stop'
$repository=(Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$ico=[IO.File]::ReadAllBytes((Join-Path $repository 'assets\okuflow.ico'))
Add-Type @'
using System;
using System.Runtime.InteropServices;
public static class OkuFlowIconResources {
 [DllImport("kernel32.dll", CharSet=CharSet.Unicode, SetLastError=true)]
 static extern IntPtr LoadLibraryEx(string path, IntPtr file, uint flags);
 [DllImport("kernel32.dll", SetLastError=true)] static extern IntPtr FindResource(IntPtr module, IntPtr name, IntPtr type);
 [DllImport("kernel32.dll")] static extern uint SizeofResource(IntPtr module, IntPtr resource);
 [DllImport("kernel32.dll")] static extern IntPtr LoadResource(IntPtr module, IntPtr resource);
 [DllImport("kernel32.dll")] static extern IntPtr LockResource(IntPtr data);
 [DllImport("kernel32.dll")] static extern bool FreeLibrary(IntPtr module);
 public static byte[] Read(string path, int type, int id) {
  IntPtr module=LoadLibraryEx(path,IntPtr.Zero,0x22); // data/image only; never executes code
  if(module==IntPtr.Zero) throw new Exception("Cannot read executable resources");
  try {
   IntPtr resource=FindResource(module,(IntPtr)id,(IntPtr)type);
   if(resource==IntPtr.Zero) throw new Exception("Missing executable icon resource");
   byte[] bytes=new byte[SizeofResource(module,resource)];
   Marshal.Copy(LockResource(LoadResource(module,resource)),bytes,0,bytes.Length);
   return bytes;
  } finally {FreeLibrary(module);}
 }
}
'@
$count=[BitConverter]::ToUInt16($ico,4)
if($count -ne 7){throw 'Expected the seven reviewed icon sizes.'}
foreach($name in @('oku_flow.exe','OkuFlowApp.exe')){
    $path=(Resolve-Path -LiteralPath (Join-Path $BundlePath $name)).Path
    $group=[OkuFlowIconResources]::Read($path,14,101)
    if([BitConverter]::ToUInt16($group,4) -ne $count){throw "Incorrect embedded icon count: $name"}
    for($index=0;$index -lt $count;$index++){
        $entry=6+16*$index;$groupEntry=6+14*$index
        $length=[BitConverter]::ToUInt32($ico,$entry+8);$offset=[BitConverter]::ToUInt32($ico,$entry+12)
        $resource=[OkuFlowIconResources]::Read($path,3,[BitConverter]::ToUInt16($group,$groupEntry+12))
        $expected=$ico[$offset..($offset+$length-1)]
        if($resource.Length -ne $length -or [Convert]::ToHexString([Security.Cryptography.SHA256]::HashData($resource)) -ne
            [Convert]::ToHexString([Security.Cryptography.SHA256]::HashData([byte[]]$expected))){throw "Stale executable icon frame $index in $name"}
    }
}
Write-Output 'PASS: both deployed executables embed all seven exact current cyan icon frames.'
