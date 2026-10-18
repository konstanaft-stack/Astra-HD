param([string]$RunName='green',[switch]$ExpectFailure,[switch]$Warp,[switch]$ActualFormats,[switch]$Evidence,[string]$OutputDirectory='')
$ErrorActionPreference='Stop'
if($RunName -notmatch '^[A-Za-z0-9_-]+$'){throw 'Invalid run name'}
$project='<WORKSPACE_ROOT>/research/2026-09-30-cpvr-open-nvidia';$base=(Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
$output=if($OutputDirectory){[IO.Path]::GetFullPath($OutputDirectory)}else{Join-Path $project ('build/tofu-sr-history-owner/'+$RunName)};New-Item -ItemType Directory -Path $output -Force|Out-Null
function Assert-Closed {if(@(Get-Process -Name NMS,Cyberpunk2077 -ErrorAction SilentlyContinue).Count){throw 'HOLD_USER_GAME'}}
Assert-Closed
$mutex=[Threading.Mutex]::new($false,'Local\IRD.SingleBuildTestDeploy.v1');$owned=$false;$saved=@{}
foreach($name in @('PATH','INCLUDE','LIB')){$saved[$name]=[Environment]::GetEnvironmentVariable($name,'Process')}
try{
 try{$owned=$mutex.WaitOne(0)}catch [Threading.AbandonedMutexException]{$owned=$true}
 if(-not $owned){throw 'HEAVY_OWNER_BUSY'};Assert-Closed
 $vc='<WORKSPACE_ROOT>/tools/Cyberpunk-Modding/MSVC-2022/msvc/VC/Tools/MSVC/14.44.35207';$sdk='<WORKSPACE_ROOT>/tools/Cyberpunk-Modding/MSVC-2022/msvc/Windows Kits/10';$v='10.0.26100.0'
 $env:PATH=(Join-Path $vc 'bin/Hostx64/x64')+';'+$saved.PATH
 $env:INCLUDE=((Join-Path $vc 'include'),(Join-Path $sdk "Include/$v/ucrt"),(Join-Path $sdk "Include/$v/shared"),(Join-Path $sdk "Include/$v/um"),(Join-Path $sdk "Include/$v/winrt")) -join ';'
 $env:LIB=((Join-Path $vc 'lib/x64'),(Join-Path $sdk "Lib/$v/ucrt/x64"),(Join-Path $sdk "Lib/$v/um/x64")) -join ';'
 $exe=Join-Path $output 'sr-history-owner.exe'
 $fixture=if($Warp){'sr_history_owner_warp.cpp'}else{'sr_history_owner.cpp'}
 $definitions=@();if($ActualFormats){$definitions+='/DCVR_SR_ACTUAL_FORMATS'};if($Evidence){$definitions+='/DCVR_SR_EVIDENCE_TEST'}
 $definitions+=('/I'+(Join-Path $project 'build/tofu-ru/_deps/openxr-src/include'))
 $definitions+=@('/DXR_USE_PLATFORM_WIN32','/DXR_USE_GRAPHICS_API_D3D12')
 $definitions+=(Join-Path $base 'src/Framegen/NativeFgPairedSource.cpp')
 $definitions+=(Join-Path $PSScriptRoot 'SrOwnerFgObservationStubs.cpp')
 Push-Location $output
 try{& (Join-Path $vc 'bin/Hostx64/x64/cl.exe') /nologo /EHsc /std:c++20 /utf-8 /W4 /WX /O2 /fp:strict /MT /DNOMINMAX /DWIN32_LEAN_AND_MEAN @definitions ("/I"+(Join-Path $base 'include')) ("/I"+(Join-Path $project 'native/nvidia_backend/snapshot/include')) ("/I"+(Join-Path $project 'audit/luke-eye-cycle-20261003/sr-motion-shader')) ("/Fe:"+$exe) (Join-Path $PSScriptRoot $fixture) (Join-Path $base 'src/Framegen/SrHistoryOwner.cpp') (Join-Path $base 'src/Framegen/SrCameraAdapter.cpp') (Join-Path $base 'src/Render/SrTemporalMotionPass.cpp') /link d3d12.lib dxgi.lib d3dcompiler.lib *> (Join-Path $output 'build.log')}finally{Pop-Location}
 if($LASTEXITCODE -ne 0){Get-Content -LiteralPath (Join-Path $output 'build.log');throw 'Compile failed'}
 Assert-Closed
 Push-Location $output
 try{& $exe *> (Join-Path $output 'run.log');$result=$LASTEXITCODE}finally{Pop-Location}
 Get-Content -LiteralPath (Join-Path $output 'run.log')
 @{exit_code=$result;expected_failure=[bool]$ExpectFailure;game_execution=$false;gpu_execution=[bool]$Warp;adapter=$(if($Warp){'WARP'}else{'none'});exe_sha256=(Get-FileHash -LiteralPath $exe).Hash}|ConvertTo-Json|Set-Content -LiteralPath (Join-Path $output 'result.json') -Encoding utf8
 if(($ExpectFailure -and $result -ne 1) -or (-not $ExpectFailure -and $result -ne 0)){throw 'Unexpected test result'}
}finally{foreach($name in $saved.Keys){[Environment]::SetEnvironmentVariable($name,$saved[$name],'Process')};if($owned){$mutex.ReleaseMutex()};$mutex.Dispose()}
