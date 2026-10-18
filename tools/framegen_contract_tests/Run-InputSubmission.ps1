param([string]$RunName='green',[switch]$ExpectFailure)
$ErrorActionPreference='Stop'
if($RunName -notmatch '^[A-Za-z0-9_-]+$'){throw 'Invalid run name'}
$project='<WORKSPACE_ROOT>/research/2026-09-30-cpvr-open-nvidia';$base=(Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
$output=Join-Path '<USER_HOME>/Documents/Codex/VR-Developer-Toolkit/runs' ('input-submission-20261004-'+$RunName)
[IO.Directory]::CreateDirectory($output)|Out-Null
$gate=[Threading.Mutex]::new($false,'Local\IRD.SingleBuildTestDeploy.v1');$held=$false;$saved=@{}
foreach($name in @('PATH','INCLUDE','LIB')){$saved[$name]=[Environment]::GetEnvironmentVariable($name,'Process')}
try {
 try{$held=$gate.WaitOne(0)}catch [Threading.AbandonedMutexException]{$held=$true}
 if(-not $held){throw 'HEAVY_OWNER_BUSY'}
 if(@(Get-Process -Name NMS,Cyberpunk2077,Vortex -ErrorAction SilentlyContinue).Count){throw 'HOLD_USER_GAME'}
 $vc='<WORKSPACE_ROOT>/tools/Cyberpunk-Modding/MSVC-2022/msvc/VC/Tools/MSVC/14.44.35207';$sdk='<WORKSPACE_ROOT>/tools/Cyberpunk-Modding/MSVC-2022/msvc/Windows Kits/10';$v='10.0.26100.0'
 $env:PATH=(Join-Path $vc 'bin/Hostx64/x64')+';'+$saved.PATH
 $env:INCLUDE=((Join-Path $vc 'include'),(Join-Path $sdk "Include/$v/ucrt"),(Join-Path $sdk "Include/$v/shared"),(Join-Path $sdk "Include/$v/um"),(Join-Path $sdk "Include/$v/winrt")) -join ';'
 $env:LIB=((Join-Path $vc 'lib/x64'),(Join-Path $sdk "Lib/$v/ucrt/x64"),(Join-Path $sdk "Lib/$v/um/x64")) -join ';'
 $ninja=Get-Content -LiteralPath (Join-Path $project 'build/tofu-ru/build.ninja') -Raw
 $block=[regex]::Match($ninja,'(?ms)^build CMakeFiles\\cyberpunkvrport_stereo.dir\\src\\Framegen\\Inputs.cpp.obj:.*?(?=^\s*$)').Value
 $flags=@();foreach($key in @('DEFINES','INCLUDES')){$line=[regex]::Match($block,('(?m)^  '+$key+' = (.+)$')).Groups[1].Value.Trim();if(-not $line){throw "Missing $key"};$flags+=($line -split ' '|Where-Object{$_})}
 $flags=@(("/I"+(Join-Path $base 'include')))+$flags
 $flags+=("/I"+(Join-Path $project 'native/nvidia_backend/snapshot/include'))
 $exe=Join-Path $output 'input-submission.exe'
 Push-Location $output
 try{& (Join-Path $vc 'bin/Hostx64/x64/cl.exe') /nologo /EHsc /std:c++20 /utf-8 /W4 /O2 /Gy /Gw /MT @flags ("/Fe:"+$exe) (Join-Path $PSScriptRoot 'input_submission.cpp') /link /OPT:REF *> (Join-Path $output 'build.log')}finally{Pop-Location}
 if($LASTEXITCODE -ne 0){Get-Content -LiteralPath (Join-Path $output 'build.log') -Tail 25;throw 'Compile failed'}
 & $exe *> (Join-Path $output 'run.log');$result=$LASTEXITCODE;Get-Content -LiteralPath (Join-Path $output 'run.log')
 @{exit_code=$result;expected_failure=[bool]$ExpectFailure;actual_input_registry=$true;gpu=$false;game=$false;exe_sha256=(Get-FileHash -LiteralPath $exe).Hash}|ConvertTo-Json|Set-Content -LiteralPath (Join-Path $output 'result.json') -Encoding utf8
 if(($ExpectFailure -and $result -ne 1) -or (-not $ExpectFailure -and $result -ne 0)){throw 'Unexpected result'}
}finally{foreach($name in $saved.Keys){[Environment]::SetEnvironmentVariable($name,$saved[$name],'Process')};if($held){$gate.ReleaseMutex()};$gate.Dispose()}
