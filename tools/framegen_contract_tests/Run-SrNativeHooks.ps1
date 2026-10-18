param([string]$RunName='green',[switch]$NegativeControl,[switch]$Lifecycle,[switch]$ExpectFailure)
$ErrorActionPreference='Stop'
if($RunName -notmatch '^[A-Za-z0-9_-]+$'){throw 'Invalid run name'}
$project='<WORKSPACE_ROOT>/research/2026-09-30-cpvr-open-nvidia';$base=(Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
$output=Join-Path $project ('build/tofu-sr-native-hooks/'+$RunName);New-Item -ItemType Directory -Path $output -Force|Out-Null
if(@(Get-Process -Name NMS,Cyberpunk2077 -ErrorAction SilentlyContinue).Count){throw 'HOLD_USER_GAME'}
$mutex=[Threading.Mutex]::new($false,'Local\IRD.SingleBuildTestDeploy.v1');$owned=$false;$saved=@{}
foreach($name in @('PATH','INCLUDE','LIB')){$saved[$name]=[Environment]::GetEnvironmentVariable($name,'Process')}
try{
 try{$owned=$mutex.WaitOne(0)}catch [Threading.AbandonedMutexException]{$owned=$true}
 if(-not $owned){throw 'Build/test owner busy'}
 $vc='<WORKSPACE_ROOT>/tools/Cyberpunk-Modding/MSVC-2022/msvc/VC/Tools/MSVC/14.44.35207';$sdk='<WORKSPACE_ROOT>/tools/Cyberpunk-Modding/MSVC-2022/msvc/Windows Kits/10';$v='10.0.26100.0'
 $env:PATH=(Join-Path $vc 'bin/Hostx64/x64')+';'+$saved.PATH
 $env:INCLUDE=((Join-Path $vc 'include'),(Join-Path $sdk "Include/$v/ucrt"),(Join-Path $sdk "Include/$v/shared"),(Join-Path $sdk "Include/$v/um"),(Join-Path $sdk "Include/$v/winrt")) -join ';'
 $env:LIB=((Join-Path $vc 'lib/x64'),(Join-Path $sdk "Lib/$v/ucrt/x64"),(Join-Path $sdk "Lib/$v/um/x64")) -join ';'
 $extra=@(("/I"+(Join-Path $project 'native/nvidia_backend/snapshot/include')),("/I"+(Join-Path $base 'include')),("/I"+(Join-Path $base 'externals/streamline/include')),("/I"+(Join-Path $base 'externals/minhook/include')),("/I"+$PSScriptRoot))
 $exe=Join-Path $output 'sr-native-hooks.exe'
 $negative=@();if($NegativeControl){$negative+='/DCVR_SR_NGX_NEGATIVE_RELEASE'}
 Push-Location $output
 $fixtureName=if($Lifecycle){'sr_native_lifecycle.cpp'}else{'sr_native_hooks.cpp'}
 try{& (Join-Path $vc 'bin/Hostx64/x64/cl.exe') /nologo /EHsc /O2 /std:c++20 /W4 /WX /wd4100 /MT /DNOMINMAX /DWIN32_LEAN_AND_MEAN /DCVR_SR_NGX_TESTING @negative @extra ("/Fe:"+$exe) (Join-Path $PSScriptRoot $fixtureName) (Join-Path $base 'src/Hooks/SrNgxHooks.cpp') *> (Join-Path $output 'build.log')}finally{Pop-Location}
 if($LASTEXITCODE -ne 0){Get-Content (Join-Path $output 'build.log');throw 'Fixture compile failed'}
 if(@(Get-Process -Name NMS,Cyberpunk2077 -ErrorAction SilentlyContinue).Count){throw 'HOLD_USER_GAME before CPU fixture'}
 @{checked_utc=[DateTime]::UtcNow.ToString('o');games=@();gpu=$false;actual_hooks=$false}|ConvertTo-Json|Set-Content (Join-Path $output 'guard.json') -Encoding utf8
 $fixture=Start-Process -FilePath $exe -WorkingDirectory $output -WindowStyle Hidden -PassThru -RedirectStandardOutput (Join-Path $output 'run.log') -RedirectStandardError (Join-Path $output 'run-error.log')
 if(-not $fixture.WaitForExit(30000)){$fixture.Kill();$fixture.WaitForExit();throw 'Owned CPU fixture timeout'}
 $fixture.Refresh();$result=$fixture.ExitCode;Get-Content (Join-Path $output 'run.log');Get-Content (Join-Path $output 'run-error.log')
 @{exit_code=$result;negative_control=[bool]$NegativeControl;lifecycle=[bool]$Lifecycle;expected_failure=[bool]$ExpectFailure;exe_sha256=(Get-FileHash $exe).Hash;gpu=$false;game=$false;actual_hooks=$false}|ConvertTo-Json|Set-Content (Join-Path $output 'result.json') -Encoding utf8
 if($result -ne $(if($NegativeControl -or $ExpectFailure){1}else{0})){throw 'Unexpected CPU fixture result'}
 if($NegativeControl -or $ExpectFailure){return}
 # Compile changed production TUs only, not the DLL. Use existing configured flags.
 $ninja=Get-Content (Join-Path $project 'build/tofu-ru/build.ninja') -Raw
 $block=[regex]::Match($ninja,'(?ms)^build CMakeFiles\\cyberpunkvrport_stereo.dir\\src\\Framegen\\Inputs.cpp.obj:.*?(?=^\s*$)').Value
 if(-not $block){throw 'Existing Inputs rule missing'}
 $args=@('/nologo','/c');foreach($key in @('DEFINES','FLAGS','INCLUDES')){$line=[regex]::Match($block,('(?m)^  '+$key+' = (.+)$')).Groups[1].Value.Trim();if(-not $line){throw "Missing $key"};$args+=($line -split ' '|Where-Object{$_})}
 $args+=("/I"+(Join-Path $project 'native/nvidia_backend/snapshot/include'))
 $compiled=@();foreach($file in @('src/Hooks/SrNgxHooks.cpp','src/Framegen/Inputs.cpp','src/Hooks/Ngx.cpp')){
  $name=[IO.Path]::GetFileName($file)
  & (Join-Path $vc 'bin/Hostx64/x64/cl.exe') @args ("/Fo:"+(Join-Path $output ($name+'.obj'))) ("/Fd:"+(Join-Path $output ($name+'.pdb'))) (Join-Path $base $file) *> (Join-Path $output ($name+'-build.log'))
  if($LASTEXITCODE -ne 0){Get-Content (Join-Path $output ($name+'-build.log'));throw "Production compile failed: $name"}
  $compiled+=@{path=(Join-Path $base $file);source_sha256=(Get-FileHash (Join-Path $base $file)).Hash;object_sha256=(Get-FileHash (Join-Path $output ($name+'.obj'))).Hash;compiler_exit=0}
 }
 @{full_dll_link=$false;compiled=$compiled}|ConvertTo-Json -Depth 5|Set-Content (Join-Path $output 'production-tus.json') -Encoding utf8
}finally{foreach($name in $saved.Keys){[Environment]::SetEnvironmentVariable($name,$saved[$name],'Process')};if($owned){$mutex.ReleaseMutex()};$mutex.Dispose()}
