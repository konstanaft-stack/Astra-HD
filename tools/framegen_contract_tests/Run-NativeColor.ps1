param([Parameter(Mandatory=$true)][string]$OutputDirectory,[switch]$ExpectFailure)
$ErrorActionPreference='Stop'
$sourceTask=Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
if(Get-Process -Name Cyberpunk2077,NMS -ErrorAction SilentlyContinue){throw 'Game active'}
$mutexTask=[Threading.Mutex]::new($false,'Local\IRD.SingleBuildTestDeploy.v1')
$lockedTask=$false
try {
    try {$lockedTask=$mutexTask.WaitOne(0)}catch [Threading.AbandonedMutexException]{$lockedTask=$true}
    if(-not $lockedTask){throw 'Build/test/deploy gate busy'}
    New-Item -ItemType Directory -Path $OutputDirectory -Force|Out-Null
    Push-Location $OutputDirectory
    try {
        $vcTask='<WORKSPACE_ROOT>/tools/Cyberpunk-Modding/MSVC-2022/msvc/VC/Tools/MSVC/14.44.35207'
        $sdkTask='<WORKSPACE_ROOT>/tools/Cyberpunk-Modding/MSVC-2022/msvc/Windows Kits/10';$verTask='10.0.26100.0'
        $env:INCLUDE="$vcTask/include;$sdkTask/Include/$verTask/ucrt;$sdkTask/Include/$verTask/shared;$sdkTask/Include/$verTask/um"
        $env:LIB="$vcTask/lib/x64;$sdkTask/Lib/$verTask/ucrt/x64;$sdkTask/Lib/$verTask/um/x64"
        & "$vcTask/bin/Hostx64/x64/cl.exe" /nologo /EHsc /std:c++20 /utf-8 /MT /DNOMINMAX /DWIN32_LEAN_AND_MEAN "/I$sourceTask/include" "$sourceTask/tools/framegen_contract_tests/native_color_policy.cpp" /Fe:native_color_policy.exe *> build.log
        if($LASTEXITCODE -ne 0){Get-Content build.log -Tail 20;throw 'Native source test compile failed'}
        & './native_color_policy.exe' *> run.log;$resultTask=$LASTEXITCODE;Get-Content run.log
        [ordered]@{exit_code=$resultTask;expected_failure=[bool]$ExpectFailure}|ConvertTo-Json|Set-Content result.json -Encoding UTF8
        if($ExpectFailure){if($resultTask -eq 0){throw 'Expected missing-behavior test to fail'}}elseif($resultTask -ne 0){throw 'Native source contract failed'}
    }finally{Pop-Location}
}finally{if($lockedTask){$mutexTask.ReleaseMutex()};$mutexTask.Dispose()}
