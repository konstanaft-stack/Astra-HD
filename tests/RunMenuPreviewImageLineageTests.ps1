#requires -Version 7.4
param([Parameter(Mandatory)][string]$OutputDirectory)
$ErrorActionPreference='Stop'
$root=Split-Path $PSScriptRoot
$out=[IO.Path]::GetFullPath($OutputDirectory)
New-Item -ItemType Directory -Path $out -Force|Out-Null
$vc='<WORKSPACE_ROOT>\tools\Cyberpunk-Modding\MSVC-2022\msvc\VC\Tools\MSVC\14.44.35207'
$sdk='<WORKSPACE_ROOT>\tools\Cyberpunk-Modding\MSVC-2022\msvc\Windows Kits\10'
$saved=@{};foreach($key in @('INCLUDE','LIB')){$saved[$key]=[Environment]::GetEnvironmentVariable($key,'Process')}
try {
    $env:INCLUDE="$vc\include;$sdk\Include\10.0.26100.0\ucrt;$sdk\Include\10.0.26100.0\shared;$sdk\Include\10.0.26100.0\um"
    $env:LIB="$vc\lib\x64;$sdk\Lib\10.0.26100.0\ucrt\x64;$sdk\Lib\10.0.26100.0\um\x64"
    $cl=Join-Path $vc 'bin/Hostx64/x64/cl.exe'
    $exe=Join-Path $out 'MenuPreviewImageLineageTests.exe'
    & $cl /nologo /std:c++20 /EHsc /utf-8 ("/I"+(Join-Path $root 'include')) `
        (Join-Path $PSScriptRoot 'MenuPreviewImageLineageTests.cpp') ("/Fo"+(Join-Path $out 'tests.obj')) ("/Fe"+$exe)
    if($LASTEXITCODE){throw 'Preview lineage CPU compilation failed'}
    & $exe;if($LASTEXITCODE){throw 'Preview lineage CPU test failed'}
    # Compile only this new standalone adapter object; never build/stage the DLL.
    & $cl /nologo /std:c++20 /EHsc /utf-8 ("/I"+(Join-Path $root 'include')) /c `
        (Join-Path $root 'src/MenuPreview/ImageLineage.cpp') ("/Fo"+(Join-Path $out 'ImageLineage.obj'))
    if($LASTEXITCODE){throw 'Preview lineage D3D12 adapter compilation failed'}
    $files=@('include/MenuPreview/Scope.hpp','include/MenuPreview/ImageLineagePolicy.hpp','include/MenuPreview/ImageLineage.hpp','src/MenuPreview/ImageLineage.cpp','tests/MenuPreviewImageLineageTests.cpp')
    $proof=@{status='PASS';scope='Executable CPU lineage policy and standalone adapter object; no full DLL/game/UI consumption acceptance';inputs=@($files|ForEach-Object {@{path=$_;sha256=(Get-FileHash -LiteralPath (Join-Path $root $_)).Hash}});test_sha256=(Get-FileHash -LiteralPath $exe).Hash}
    [IO.File]::WriteAllText((Join-Path $out 'RESULT.json'),($proof|ConvertTo-Json -Depth 6),[Text.UTF8Encoding]::new($false))
} finally {foreach($key in $saved.Keys){[Environment]::SetEnvironmentVariable($key,$saved[$key],'Process')}}
