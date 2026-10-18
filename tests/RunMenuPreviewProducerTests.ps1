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
    $exe=Join-Path $out 'MenuPreviewProducerTests.exe'
    & $cl /nologo /std:c++20 /EHsc /utf-8 ("/I"+(Join-Path $root 'include')) `
        (Join-Path $PSScriptRoot 'MenuPreviewProducerTests.cpp') ("/Fo"+(Join-Path $out 'tests.obj')) ("/Fe"+$exe)
    if($LASTEXITCODE){throw 'Preview producer CPU compilation failed'}
    & $exe;if($LASTEXITCODE){throw 'Preview producer CPU test failed'}
    $files=@('include/MenuPreview/Scope.hpp','include/MenuPreview/Producer.hpp','include/MenuPreview/CameraApplication.hpp','include/MenuPreview/ProducerRoute.hpp','include/MenuPreview/LocalComponentPose.hpp','include/MenuPreview/ImageAliasObservation.hpp','include/MenuPreview/WidgetAliasObservation.hpp','include/MenuPreview/ReaderObservation.hpp','tests/MenuPreviewProducerTests.cpp','tests/RunMenuPreviewProducerTests.ps1')
    $proof=@{status='PASS';scope='Pure executable descriptor/frame/mode/zero/producer-match policy; no DLL/game/stereo acceptance';inputs=@($files|ForEach-Object {@{path=$_;sha256=(Get-FileHash -LiteralPath (Join-Path $root $_)).Hash}});test_sha256=(Get-FileHash -LiteralPath $exe).Hash}
    [IO.File]::WriteAllText((Join-Path $out 'RESULT.json'),($proof|ConvertTo-Json -Depth 6),[Text.UTF8Encoding]::new($false))
} finally {foreach($key in $saved.Keys){[Environment]::SetEnvironmentVariable($key,$saved[$key],'Process')}}
