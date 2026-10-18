param([string]$RunName='green',[switch]$ExpectFailure,[switch]$CompileProduction)
$ErrorActionPreference='Stop'
if($RunName -notmatch '^[A-Za-z0-9_-]+$'){throw 'Invalid run name'}
$project='<WORKSPACE_ROOT>/research/2026-09-30-cpvr-open-nvidia';$base=(Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
$output=Join-Path '<USER_HOME>/Documents/Codex/VR-Developer-Toolkit/runs' ('sr-history-integration-20261004-'+$RunName)
[IO.Directory]::CreateDirectory($output)|Out-Null
function Assert-Closed {if(@(Get-Process -Name NMS,Cyberpunk2077,Vortex -ErrorAction SilentlyContinue).Count){throw 'HOLD_USER_GAME'}}
$gate=[Threading.Mutex]::new($false,'Local\IRD.SingleBuildTestDeploy.v1');$held=$false;$saved=@{}
foreach($name in @('PATH','INCLUDE','LIB')){$saved[$name]=[Environment]::GetEnvironmentVariable($name,'Process')}
try {
 try{$held=$gate.WaitOne(0)}catch [Threading.AbandonedMutexException]{$held=$true}
 if(-not $held){throw 'HEAVY_OWNER_BUSY'};Assert-Closed
 $vc='<WORKSPACE_ROOT>/tools/Cyberpunk-Modding/MSVC-2022/msvc/VC/Tools/MSVC/14.44.35207';$sdk='<WORKSPACE_ROOT>/tools/Cyberpunk-Modding/MSVC-2022/msvc/Windows Kits/10';$v='10.0.26100.0'
 $env:PATH=(Join-Path $vc 'bin/Hostx64/x64')+';'+$saved.PATH
 $env:INCLUDE=((Join-Path $vc 'include'),(Join-Path $sdk "Include/$v/ucrt"),(Join-Path $sdk "Include/$v/shared"),(Join-Path $sdk "Include/$v/um"),(Join-Path $sdk "Include/$v/winrt")) -join ';'
 $env:LIB=((Join-Path $vc 'lib/x64'),(Join-Path $sdk "Lib/$v/ucrt/x64"),(Join-Path $sdk "Lib/$v/um/x64")) -join ';'
 $ninja=Get-Content -LiteralPath (Join-Path $project 'build/tofu-ru/build.ninja') -Raw
 $block=[regex]::Match($ninja,'(?ms)^build CMakeFiles\\cyberpunkvrport_stereo.dir\\src\\Framegen\\Inputs.cpp.obj:.*?(?=^\s*$)').Value
 $flags=@();foreach($key in @('DEFINES','INCLUDES')){$line=[regex]::Match($block,('(?m)^  '+$key+' = (.+)$')).Groups[1].Value.Trim();if(-not $line){throw "Missing $key"};$flags+=($line -split ' '|Where-Object{$_})}
 $flags+=("/I"+(Join-Path $project 'native/nvidia_backend/snapshot/include'))
 $exe=Join-Path $output 'sr-history-integration.exe'
 Push-Location $output
 try{& (Join-Path $vc 'bin/Hostx64/x64/cl.exe') /nologo /EHsc /std:c++20 /utf-8 /W4 /WX /O2 /fp:strict /MT @flags ("/Fe:"+$exe) (Join-Path $PSScriptRoot 'sr_history_integration.cpp') (Join-Path $base 'src/Framegen/SrHistoryIntegration.cpp') (Join-Path $base 'src/Framegen/SrIdentityDiagnostics.cpp') *> (Join-Path $output 'build.log')}finally{Pop-Location}
 if($LASTEXITCODE -ne 0){Get-Content -LiteralPath (Join-Path $output 'build.log') -Tail 35;throw 'Compile failed'}
 $ini=Join-Path $output 'fixture.ini';[IO.File]::WriteAllText($ini,"unrelated=4`nxr_aer_v2=1`n",[Text.UTF8Encoding]::new($false))
 Assert-Closed
 & $exe $ini *> (Join-Path $output 'run.log');$result=$LASTEXITCODE
 Get-Content -LiteralPath (Join-Path $output 'run.log')
 @{exit_code=$result;expected_failure=[bool]$ExpectFailure;scope='CPU integration boundary with mock history owner; not production GPU/history acceptance';game=$false;gpu=$false;exe_sha256=(Get-FileHash -LiteralPath $exe).Hash}|ConvertTo-Json|Set-Content -LiteralPath (Join-Path $output 'result.json') -Encoding utf8
 if(($ExpectFailure -and $result -ne 1) -or (-not $ExpectFailure -and $result -ne 0)){throw 'Unexpected test result'}
 if($CompileProduction){
  $compiled=@()
  foreach($unit in @('Framegen/SrHistoryIntegration.cpp','Framegen/Framegen.cpp','Framegen/Inputs.cpp','Hooks/Ngx.cpp','Stereo/DeviceHooks.cpp','Stereo/Dlss.cpp')){
   $file=Join-Path $base ('src/'+$unit);$name=$unit.Replace('/','-');$hash=(Get-FileHash -LiteralPath $file).Hash
   & (Join-Path $vc 'bin/Hostx64/x64/cl.exe') /nologo /c /EHsc /std:c++20 /utf-8 /W4 /MD @flags ("/Fo:"+(Join-Path $output ($name+'.obj'))) $file *> (Join-Path $output ($name+'.log'))
   if($LASTEXITCODE -ne 0){Get-Content -LiteralPath (Join-Path $output ($name+'.log')) -Tail 25;throw "Production compile failed $unit"}
   $compiled+=@{path=$file;sha256=$hash;source_unchanged=($hash -eq (Get-FileHash -LiteralPath $file).Hash);exit=0}
  }
  $compiled|ConvertTo-Json -Depth 4|Set-Content -LiteralPath (Join-Path $output 'production-tus.json') -Encoding utf8
 }
}finally{foreach($name in $saved.Keys){[Environment]::SetEnvironmentVariable($name,$saved[$name],'Process')};if($held){$gate.ReleaseMutex()};$gate.Dispose()}
