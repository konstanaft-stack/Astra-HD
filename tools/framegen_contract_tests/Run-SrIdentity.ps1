param([string]$RunName='green',[switch]$NegativeControl)
$ErrorActionPreference='Stop'
if($RunName -notmatch '^[A-Za-z0-9_-]+$'){throw 'Invalid run name'}
$project='<WORKSPACE_ROOT>/research/2026-09-30-cpvr-open-nvidia';$base=(Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
$output=Join-Path $project ('build/tofu-sr-identity/'+$RunName);New-Item -ItemType Directory -Path $output -Force|Out-Null
$mutex=[Threading.Mutex]::new($false,'Local\IRD.SingleBuildTestDeploy.v1');$owned=$false;$saved=@{}
foreach($name in @('PATH','INCLUDE','LIB')){$saved[$name]=[Environment]::GetEnvironmentVariable($name,'Process')}
try{
 try{$owned=$mutex.WaitOne(0)}catch [Threading.AbandonedMutexException]{$owned=$true}
 if(-not $owned){throw 'Build/test owner busy'}
 $tool='<WORKSPACE_ROOT>/tools/Cyberpunk-Modding/MSVC-2022/msvc';$vc=Join-Path $tool 'VC/Tools/MSVC/14.44.35207';$sdk=Join-Path $tool 'Windows Kits/10';$v='10.0.26100.0'
 $env:PATH=(Join-Path $vc 'bin/Hostx64/x64')+';'+$saved.PATH
 $env:INCLUDE=((Join-Path $vc 'include'),(Join-Path $sdk "Include/$v/ucrt"),(Join-Path $sdk "Include/$v/shared"),(Join-Path $sdk "Include/$v/um"),(Join-Path $sdk "Include/$v/winrt")) -join ';'
 $env:LIB=((Join-Path $vc 'lib/x64'),(Join-Path $sdk "Lib/$v/ucrt/x64"),(Join-Path $sdk "Lib/$v/um/x64")) -join ';'
 $exe=Join-Path $output 'sr-identity.exe';$extra=@();if($NegativeControl){$extra+='/DSR_IDENTITY_NEGATIVE_CONTROL'}
 Push-Location $output
 try{& (Join-Path $vc 'bin/Hostx64/x64/cl.exe') /nologo /EHsc /O2 /std:c++20 /W4 /WX /wd4100 /MT /DNOMINMAX /DWIN32_LEAN_AND_MEAN @extra ("/I"+(Join-Path $base 'include')) ("/I"+(Join-Path $base 'externals/streamline/include')) ("/Fe:"+$exe) (Join-Path $PSScriptRoot 'sr_identity.cpp') (Join-Path $base 'src/Framegen/SrIdentityDiagnostics.cpp') /link d3d12.lib dxgi.lib *> (Join-Path $output 'build.log')}finally{Pop-Location}
 if($LASTEXITCODE -ne 0){Get-Content -LiteralPath (Join-Path $output 'build.log');throw 'Build failed'}
 & $exe (Join-Path $output 'wire-fixture.bin') *> (Join-Path $output 'run.log');$result=$LASTEXITCODE;Get-Content -LiteralPath (Join-Path $output 'run.log')
 @{exit_code=$result;negative_control=[bool]$NegativeControl;exe_sha256=(Get-FileHash -LiteralPath $exe).Hash;gpu_execution=$false;game_launched=$false}|ConvertTo-Json|Set-Content -LiteralPath (Join-Path $output 'run-result.json') -Encoding utf8
 if(($NegativeControl -and $result -ne 1) -or (-not $NegativeControl -and $result -ne 0)){throw 'Unexpected fixture result'}
 if(-not $NegativeControl){
  # Compile only the changed production TU, into our evidence directory. No DLL link.
  $ninja=Get-Content -LiteralPath (Join-Path $project 'build/tofu-ru/build.ninja') -Raw
  $block=[regex]::Match($ninja,'(?ms)^build CMakeFiles\\cyberpunkvrport_stereo.dir\\src\\Framegen\\Inputs.cpp.obj:.*?(?=^\s*$)').Value
  if(-not $block){throw 'Missing existing Inputs.cpp build rule'}
  $compileArgs=@('/nologo','/c')
  foreach($key in @('DEFINES','FLAGS','INCLUDES')){
   $line=[regex]::Match($block,('(?m)^  '+$key+' = (.+)$')).Groups[1].Value.Trim()
   if(-not $line){throw "Missing $key"};$compileArgs+=($line -split ' ' | Where-Object {$_})
  }
  & (Join-Path $vc 'bin/Hostx64/x64/cl.exe') @compileArgs ("/Fo:"+(Join-Path $output 'Inputs.cpp.obj')) ("/Fd:"+(Join-Path $output 'Inputs.pdb')) (Join-Path $base 'src/Framegen/Inputs.cpp') *> (Join-Path $output 'inputs-build.log')
  if($LASTEXITCODE -ne 0){Get-Content -LiteralPath (Join-Path $output 'inputs-build.log');throw 'Inputs production TU failed'}
  @{compiler_exit_code=0;full_dll_link=$false;source_sha256=(Get-FileHash -LiteralPath (Join-Path $base 'src/Framegen/Inputs.cpp')).Hash;object_sha256=(Get-FileHash -LiteralPath (Join-Path $output 'Inputs.cpp.obj')).Hash}|ConvertTo-Json|Set-Content -LiteralPath (Join-Path $output 'inputs-result.json') -Encoding utf8
 }
}finally{foreach($name in $saved.Keys){[Environment]::SetEnvironmentVariable($name,$saved[$name],'Process')};if($owned){$mutex.ReleaseMutex()};$mutex.Dispose()}
