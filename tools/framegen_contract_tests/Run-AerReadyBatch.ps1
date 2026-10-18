param([string]$RunName='green',[switch]$ExpectFailure)
$ErrorActionPreference='Stop'
if($RunName -notmatch '^[A-Za-z0-9_-]+$'){throw 'Invalid run name'}
$project='<WORKSPACE_ROOT>/research/2026-09-30-cpvr-open-nvidia';$base=(Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
$output=Join-Path $project ('build/tofu-aer-ready-batch/'+$RunName);New-Item -ItemType Directory -Path $output -Force|Out-Null
if(@(Get-Process -Name NMS,Cyberpunk2077 -ErrorAction SilentlyContinue).Count){throw 'HOLD_USER_GAME'}
$mutex=[Threading.Mutex]::new($false,'Local\IRD.SingleBuildTestDeploy.v1');$owned=$false;$saved=@{}
foreach($name in @('PATH','INCLUDE','LIB')){$saved[$name]=[Environment]::GetEnvironmentVariable($name,'Process')}
try{
 try{$owned=$mutex.WaitOne(0)}catch [Threading.AbandonedMutexException]{$owned=$true}
 if(-not $owned){throw 'Build/test owner busy'}
 $tool='<WORKSPACE_ROOT>/tools/Cyberpunk-Modding/MSVC-2022/msvc';$vc=Join-Path $tool 'VC/Tools/MSVC/14.44.35207';$sdk=Join-Path $tool 'Windows Kits/10';$v='10.0.26100.0'
 $env:PATH=(Join-Path $vc 'bin/Hostx64/x64')+';'+$saved.PATH
 $env:INCLUDE=((Join-Path $vc 'include'),(Join-Path $sdk "Include/$v/ucrt"),(Join-Path $sdk "Include/$v/shared"),(Join-Path $sdk "Include/$v/um"),(Join-Path $sdk "Include/$v/winrt")) -join ';'
 $env:LIB=((Join-Path $vc 'lib/x64'),(Join-Path $sdk "Lib/$v/ucrt/x64"),(Join-Path $sdk "Lib/$v/um/x64")) -join ';'
 $exe=Join-Path $output 'aer-real-presenter.exe'
 Push-Location $output
 try{& (Join-Path $vc 'bin/Hostx64/x64/cl.exe') /nologo /EHsc /O2 /std:c++20 /W4 /WX /wd4100 /MT /DNOMINMAX /DWIN32_LEAN_AND_MEAN /DXR_USE_PLATFORM_WIN32 /DXR_USE_GRAPHICS_API_D3D12 ("/I"+(Join-Path $base 'include')) ("/I"+(Join-Path $project 'native/include')) '/I<WORKSPACE_ROOT>/tools/VR-Debug/OpenXR-Simulator-8de3457/include' ("/Fe:"+$exe) (Join-Path $PSScriptRoot 'aer_ready_batch.cpp') (Join-Path $base 'src/Framegen/AerRealPresenter.cpp') /link d3d12.lib dxgi.lib *> (Join-Path $output 'build.log')}finally{Pop-Location}
 if($LASTEXITCODE -ne 0){Get-Content -LiteralPath (Join-Path $output 'build.log');throw 'Build failed'}
 if(@(Get-Process -Name NMS,Cyberpunk2077 -ErrorAction SilentlyContinue).Count){throw 'HOLD_USER_GAME before CPU fixture'}
 & $exe *> (Join-Path $output 'run.log');$result=$LASTEXITCODE;Get-Content -LiteralPath (Join-Path $output 'run.log')
 @{exit_code=$result;expected_failure=[bool]$ExpectFailure;exe_sha256=(Get-FileHash -LiteralPath $exe).Hash;gpu_execution=$false;game_launched=$false}|ConvertTo-Json|Set-Content -LiteralPath (Join-Path $output 'run-result.json') -Encoding utf8
 if(($ExpectFailure -and $result -ne 1) -or (-not $ExpectFailure -and $result -ne 0)){throw 'Unexpected fixture result'}
}finally{foreach($name in $saved.Keys){[Environment]::SetEnvironmentVariable($name,$saved[$name],'Process')};if($owned){$mutex.ReleaseMutex()};$mutex.Dispose()}
