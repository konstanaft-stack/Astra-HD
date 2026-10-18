param([Parameter(Mandatory)][string]$AuditDirectory)
$ErrorActionPreference='Stop'
$repo=Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
$compiler='<WORKSPACE_ROOT>\tools\Cyberpunk-Modding\MSVC-2022\msvc\VC\Tools\MSVC\14.44.35207'
$sdk='<WORKSPACE_ROOT>\tools\Cyberpunk-Modding\MSVC-2022\msvc\Windows Kits\10'
$oldInclude=$env:INCLUDE;$oldLib=$env:LIB
try{
    New-Item -ItemType Directory -Path $AuditDirectory -Force|Out-Null
    $env:INCLUDE="$compiler\include;$sdk\Include\10.0.26100.0\ucrt;$sdk\Include\10.0.26100.0\shared;$sdk\Include\10.0.26100.0\um"
    $env:LIB="$compiler\lib\x64;$sdk\Lib\10.0.26100.0\ucrt\x64;$sdk\Lib\10.0.26100.0\um\x64"
    & "$compiler\bin\Hostx64\x64\cl.exe" /nologo /EHsc /std:c++20 /W4 /WX "/I$repo\include" "$PSScriptRoot\main.cpp" "/Fo$AuditDirectory\test.obj" "/Fe$AuditDirectory\test.exe" *> "$AuditDirectory\build.log"
    if($LASTEXITCODE -ne 0){throw 'CPU fixture build failed'}
    & "$AuditDirectory\test.exe" *> "$AuditDirectory\result.log"
    $code=$LASTEXITCODE
    [ordered]@{scope='CPU scalar timeline only; no native hook, GPU or HMD acceptance';exit_code=$code;header_sha256=(Get-FileHash "$repo\include\Stereo\NativeSlFrameTimeline.hpp").Hash;test_sha256=(Get-FileHash "$PSScriptRoot\main.cpp").Hash}|ConvertTo-Json|Set-Content "$AuditDirectory\RESULT.json" -Encoding utf8
    if($code -ne 0){throw 'CPU fixture failed'}
    Get-Content "$AuditDirectory\result.log"
}finally{$env:INCLUDE=$oldInclude;$env:LIB=$oldLib}
