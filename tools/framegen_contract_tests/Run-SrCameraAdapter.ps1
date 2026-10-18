param([string]$RunName='green',[switch]$ExpectFailure)
$ErrorActionPreference='Stop'
if($RunName -notmatch '^[A-Za-z0-9_-]+$'){throw 'Invalid run name'}
$project='<WORKSPACE_ROOT>/research/2026-09-30-cpvr-open-nvidia';$base=(Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
$output=Join-Path $project ('build/tofu-sr-camera-adapter/'+$RunName);New-Item -ItemType Directory -Path $output -Force|Out-Null
if(@(Get-Process -Name NMS,Cyberpunk2077 -ErrorAction SilentlyContinue).Count){throw 'HOLD_USER_GAME'}
$mutex=[Threading.Mutex]::new($false,'Local\IRD.SingleBuildTestDeploy.v1');$owned=$false;$saved=@{}
foreach($name in @('PATH','INCLUDE','LIB')){$saved[$name]=[Environment]::GetEnvironmentVariable($name,'Process')}
try{
 try{$owned=$mutex.WaitOne(0)}catch [Threading.AbandonedMutexException]{$owned=$true}
 if(-not $owned){throw 'HEAVY_OWNER_BUSY'}
 if(@(Get-Process -Name NMS,Cyberpunk2077 -ErrorAction SilentlyContinue).Count){throw 'HOLD_USER_GAME under gate'}
 $tool='<WORKSPACE_ROOT>/tools/Cyberpunk-Modding/MSVC-2022/msvc';$vc=Join-Path $tool 'VC/Tools/MSVC/14.44.35207';$sdk=Join-Path $tool 'Windows Kits/10';$v='10.0.26100.0'
 $env:PATH=(Join-Path $vc 'bin/Hostx64/x64')+';'+$saved.PATH
 $env:INCLUDE=((Join-Path $vc 'include'),(Join-Path $sdk "Include/$v/ucrt"),(Join-Path $sdk "Include/$v/shared"),(Join-Path $sdk "Include/$v/um")) -join ';'
 $env:LIB=((Join-Path $vc 'lib/x64'),(Join-Path $sdk "Lib/$v/ucrt/x64"),(Join-Path $sdk "Lib/$v/um/x64")) -join ';'
 $paths=@('include/Framegen/SrTemporalConstants.hpp','include/Framegen/SrCameraAdapter.hpp','src/Framegen/SrCameraAdapter.cpp','tools/framegen_contract_tests/sr_temporal_constants.cpp','tools/framegen_contract_tests/sr_camera_adapter.cpp','tools/framegen_contract_tests/fixtures/sr_actual_120_122.hpp','tools/framegen_contract_tests/Run-SrCameraAdapter.ps1',
  'include/Camera/AppliedEyeReference.hpp','include/Camera/PoseAddressLedger.hpp','include/Framegen/ProducerContract.hpp','include/Framegen/MainEyeSourceLatch.hpp','include/Framegen/AerV2Policy.hpp','include/Framegen/SourceDisplayClock.hpp')
 $before=@{};foreach($path in $paths){$before[$path]=(Get-FileHash -LiteralPath (Join-Path $base $path)).Hash}
 $results=@()
 foreach($test in @('sr_temporal_constants','sr_camera_adapter')){
  $exe=Join-Path $output ($test+'.exe');$compilerArgs=@('/nologo','/EHsc','/std:c++20','/utf-8','/W4','/WX','/O2','/fp:strict','/MT','/DNOMINMAX',('/I'+(Join-Path $base 'include')),('/Fe:'+$exe),(Join-Path $PSScriptRoot ($test+'.cpp')))
  if($test -eq 'sr_camera_adapter'){$compilerArgs+=(Join-Path $base 'src/Framegen/SrCameraAdapter.cpp')}
  Push-Location $output
  try{& (Join-Path $vc 'bin/Hostx64/x64/cl.exe') @compilerArgs *> (Join-Path $output ($test+'-build.log'))}finally{Pop-Location}
  if($LASTEXITCODE -ne 0){Get-Content -LiteralPath (Join-Path $output ($test+'-build.log'));throw 'Build failed'}
  if(@(Get-Process -Name NMS,Cyberpunk2077 -ErrorAction SilentlyContinue).Count){throw 'HOLD_USER_GAME before CPU fixture'}
  & $exe *> (Join-Path $output ($test+'-run.log'));$result=$LASTEXITCODE;Get-Content -LiteralPath (Join-Path $output ($test+'-run.log'))
  $results+=@{test=$test;exit_code=$result;exe_sha256=(Get-FileHash -LiteralPath $exe).Hash}
  if($test -eq 'sr_temporal_constants' -and $result -ne 0){throw 'Promoted builder regression'}
 }
 foreach($path in $paths){if($before[$path] -ne (Get-FileHash -LiteralPath (Join-Path $base $path)).Hash){throw ('CPU source changed during test: '+$path)}}
 @{expected_failure=[bool]$ExpectFailure;gpu_execution=$false;game_execution=$false;results=$results;
  files=@($paths|ForEach-Object{@{path=(Join-Path $base $_);sha256=(Get-FileHash -LiteralPath (Join-Path $base $_)).Hash}})}|ConvertTo-Json -Depth 5|Set-Content -LiteralPath (Join-Path $output 'receipt.json') -Encoding utf8
 if(($ExpectFailure -and $result -ne 1) -or (-not $ExpectFailure -and $result -ne 0)){throw 'Unexpected adapter result'}
}finally{foreach($name in $saved.Keys){[Environment]::SetEnvironmentVariable($name,$saved[$name],'Process')};if($owned){$mutex.ReleaseMutex()};$mutex.Dispose()}
