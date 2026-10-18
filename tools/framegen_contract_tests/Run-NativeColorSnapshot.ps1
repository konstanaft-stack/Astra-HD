param([Parameter(Mandatory=$true)][string]$OutputDirectory,[switch]$ExpectFailure)
$ErrorActionPreference='Stop';$sourceTask=Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
if(Get-Process -Name Cyberpunk2077,NMS -ErrorAction SilentlyContinue){throw 'Game active'}
$mutexTask=[Threading.Mutex]::new($false,'Local\IRD.SingleBuildTestDeploy.v1');$lockedTask=$false
try {
    try {$lockedTask=$mutexTask.WaitOne(0)}catch [Threading.AbandonedMutexException]{$lockedTask=$true}
    if(-not $lockedTask){throw 'Build/test gate busy'}
    New-Item -ItemType Directory -Path $OutputDirectory -Force|Out-Null;Push-Location $OutputDirectory
    try {
        $vcTask='<WORKSPACE_ROOT>/tools/Cyberpunk-Modding/MSVC-2022/msvc/VC/Tools/MSVC/14.44.35207';$sdkTask='<WORKSPACE_ROOT>/tools/Cyberpunk-Modding/MSVC-2022/msvc/Windows Kits/10';$verTask='10.0.26100.0'
        $env:INCLUDE="$vcTask/include;"+((@('ucrt','shared','um','winrt')|ForEach-Object{"$sdkTask/Include/$verTask/$_"})-join ';')
        $env:LIB="$vcTask/lib/x64;$sdkTask/Lib/$verTask/ucrt/x64;$sdkTask/Lib/$verTask/um/x64"
        & "$vcTask/bin/Hostx64/x64/cl.exe" /nologo /EHsc /std:c++20 /utf-8 /MT /DNOMINMAX /DWIN32_LEAN_AND_MEAN "/I$sourceTask/include" "$sourceTask/tools/framegen_contract_tests/native_color_snapshot_gpu.cpp" "$sourceTask/src/Render/NativeColorSnapshots.cpp" "$sourceTask/src/Render/CommandResources.cpp" /Fe:native_color_snapshot.exe /link d3d12.lib dxgi.lib *> build.log
        if($LASTEXITCODE -ne 0){Get-Content build.log -Tail 18;throw 'Snapshot test compile failed'}
        & './native_color_snapshot.exe' *> run.log;$resultTask=$LASTEXITCODE;Get-Content run.log
        @{exit_code=$resultTask;expected_failure=[bool]$ExpectFailure;adapter='WARP'}|ConvertTo-Json|Set-Content result.json -Encoding UTF8
        if($ExpectFailure){if($resultTask -ne 1){throw 'Expected pixel preservation test to fail'}}elseif($resultTask -ne 0){throw 'Snapshot test failed'}
    }finally{Pop-Location}
}finally{if($lockedTask){$mutexTask.ReleaseMutex()};$mutexTask.Dispose()}
