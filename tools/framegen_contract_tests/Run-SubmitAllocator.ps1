param([Parameter(Mandatory)][string]$RunName,[switch]$ExpectFailure)
$ErrorActionPreference='Stop'
if($RunName -notmatch '^[A-Za-z0-9_-]+$'){throw 'Invalid run name'}
if(Get-Process Cyberpunk2077,NMS -ErrorAction SilentlyContinue){throw 'Game running'}
$taskSource=(Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
$taskOut='<USER_HOME>\Documents\Codex\VR-Developer-Toolkit\runs\'+$RunName
[IO.Directory]::CreateDirectory($taskOut)|Out-Null
$taskMutex=[Threading.Mutex]::new($false,'Local\IRD.SingleBuildTestDeploy.v1');$taskHeld=$false;$taskEnv=@{}
foreach($n in @('PATH','INCLUDE','LIB')){$taskEnv[$n]=[Environment]::GetEnvironmentVariable($n,'Process')}
try {
    try{$taskHeld=$taskMutex.WaitOne(0)}catch [Threading.AbandonedMutexException]{$taskHeld=$true}
    if(-not $taskHeld){throw 'Shared owner busy'}
    & '<USER_HOME>\.cache\codex-runtimes\codex-primary-runtime\dependencies\python\python.exe' -X utf8 (Join-Path $PSScriptRoot 'extract_allocator_reuse.py') $taskSource $taskOut
    if($LASTEXITCODE){throw 'Extraction failed'}
    $vc='<WORKSPACE_ROOT>\tools\Cyberpunk-Modding\MSVC-2022\msvc\VC\Tools\MSVC\14.44.35207'
    $sdk='<WORKSPACE_ROOT>\tools\Cyberpunk-Modding\MSVC-2022\msvc\Windows Kits\10';$v='10.0.26100.0'
    $env:PATH=(Join-Path $vc 'bin/Hostx64/x64')+';'+$env:PATH
    $env:INCLUDE=((Join-Path $vc 'include'),(Join-Path $sdk "Include/$v/ucrt"),(Join-Path $sdk "Include/$v/shared"),(Join-Path $sdk "Include/$v/um"),(Join-Path $sdk "Include/$v/winrt")) -join ';'
    $env:LIB=((Join-Path $vc 'lib/x64'),(Join-Path $sdk "Lib/$v/ucrt/x64"),(Join-Path $sdk "Lib/$v/um/x64")) -join ';'
    Push-Location $taskOut
    try{& (Join-Path $vc 'bin/Hostx64/x64/cl.exe') /nologo /std:c++20 /EHsc /W4 /WX /O2 /DNOMINMAX ("/I"+(Join-Path $taskSource 'include')) ("/I"+$taskOut) (Join-Path $PSScriptRoot 'submit_allocator.cpp') /Fe:allocator.exe *> (Join-Path $taskOut 'build.log')}finally{Pop-Location}
    if($LASTEXITCODE){Get-Content (Join-Path $taskOut 'build.log');throw 'Compile failed'}
    & (Join-Path $taskOut 'allocator.exe') *> (Join-Path $taskOut 'run.log');$taskExit=$LASTEXITCODE
    Get-Content (Join-Path $taskOut 'run.log')
    @{exit_code=$taskExit;expected_failure=[bool]$ExpectFailure;gpu_execution=$false;production_fragment=$true}|ConvertTo-Json|Set-Content -LiteralPath (Join-Path $taskOut 'result.json') -Encoding utf8
    if(($ExpectFailure -and $taskExit -ne 1) -or (-not $ExpectFailure -and $taskExit -ne 0)){throw 'Unexpected test result'}
} finally {
    foreach($n in $taskEnv.Keys){[Environment]::SetEnvironmentVariable($n,$taskEnv[$n],'Process')}
    if($taskHeld){$taskMutex.ReleaseMutex()};$taskMutex.Dispose()
}
