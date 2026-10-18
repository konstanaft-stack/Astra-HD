param([string]$RunName='green',[switch]$ExpectFailure)
$ErrorActionPreference='Stop'
if($RunName -notmatch '^[A-Za-z0-9_-]+$'){throw 'Invalid run name'}
$project='<WORKSPACE_ROOT>/research/2026-09-30-cpvr-open-nvidia';$base=(Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
$output=Join-Path $project ('build/tofu-aer-native-split/'+$RunName);New-Item -ItemType Directory -Path $output -Force|Out-Null
if(@(Get-Process -Name NMS,Cyberpunk2077 -ErrorAction SilentlyContinue).Count){throw 'HOLD_USER_GAME'}
$mutex=[Threading.Mutex]::new($false,'Local\IRD.SingleBuildTestDeploy.v1');$owned=$false;$saved=@{}
foreach($name in @('PATH','INCLUDE','LIB')){$saved[$name]=[Environment]::GetEnvironmentVariable($name,'Process')}
try{
 try{$owned=$mutex.WaitOne(0)}catch [Threading.AbandonedMutexException]{$owned=$true}
 if(-not $owned){throw 'Build/test owner busy'}
 if(@(Get-Process -Name NMS,Cyberpunk2077 -ErrorAction SilentlyContinue).Count){throw 'HOLD_USER_GAME under mutex'}
 $tool='<WORKSPACE_ROOT>/tools/Cyberpunk-Modding/MSVC-2022/msvc';$vc=Join-Path $tool 'VC/Tools/MSVC/14.44.35207';$sdk=Join-Path $tool 'Windows Kits/10';$v='10.0.26100.0'
 $env:PATH=(Join-Path $vc 'bin/Hostx64/x64')+';'+$saved.PATH
 $env:INCLUDE=((Join-Path $vc 'include'),(Join-Path $sdk "Include/$v/ucrt"),(Join-Path $sdk "Include/$v/shared"),(Join-Path $sdk "Include/$v/um"),(Join-Path $sdk "Include/$v/winrt")) -join ';'
 $env:LIB=((Join-Path $vc 'lib/x64'),(Join-Path $sdk "Lib/$v/ucrt/x64"),(Join-Path $sdk "Lib/$v/um/x64")) -join ';'
 $exe=Join-Path $output 'aer-native-split.exe'
 $files=@((Join-Path $PSScriptRoot 'aer_native_split.cpp'),(Join-Path $base 'src/Framegen/NativeGenerationBridge.cpp'),(Join-Path $base 'src/Render/SrgbToLinearPass.cpp'))+
   @('generation.cpp','core.cpp','phase_metrics.cpp','resource_metrics.cpp','resource_pool.cpp'|ForEach-Object{Join-Path $project ('native/src/'+$_)})
 $includes=@(('/I'+(Join-Path $base 'include')),('/I'+(Join-Path $project 'native/include')),'/I<WORKSPACE_ROOT>/tools/VR-Debug/OpenXR-Simulator-8de3457/include')
 Push-Location $output
 try{& (Join-Path $vc 'bin/Hostx64/x64/cl.exe') /nologo /EHsc /std:c++20 /utf-8 /MT /Od /DNOMINMAX /DWIN32_LEAN_AND_MEAN /DXR_USE_PLATFORM_WIN32 /DXR_USE_GRAPHICS_API_D3D12 @includes @files ("/Fe:"+$exe) /link d3d12.lib dxgi.lib d3dcompiler.lib *> (Join-Path $output 'build.log')}finally{Pop-Location}
 if($LASTEXITCODE -ne 0){Get-Content -LiteralPath (Join-Path $output 'build.log');throw 'Build failed'}
 if(@(Get-Process -Name NMS,Cyberpunk2077 -ErrorAction SilentlyContinue).Count){throw 'HOLD_USER_GAME before CPU fixture'}
 & $exe *> (Join-Path $output 'run.log');$result=$LASTEXITCODE;Get-Content -LiteralPath (Join-Path $output 'run.log')
 $sources=@($files)+(Join-Path $base 'include/Framegen/NativeGenerationBridge.hpp')+(Join-Path $PSScriptRoot 'aer_native_input.cpp')+$PSCommandPath
 @{exit_code=$result;expected_failure=[bool]$ExpectFailure;exe_sha256=(Get-FileHash -LiteralPath $exe).Hash;gpu_execution=$false;game_launched=$false;
 sources=@($sources|ForEach-Object{@{path=$_;sha256=(Get-FileHash -LiteralPath $_).Hash}})}|ConvertTo-Json -Depth 5|Set-Content -LiteralPath (Join-Path $output 'run-result.json') -Encoding utf8
 if(($ExpectFailure -and $result -ne 1) -or (-not $ExpectFailure -and $result -ne 0)){throw 'Unexpected fixture result'}
}finally{foreach($name in $saved.Keys){[Environment]::SetEnvironmentVariable($name,$saved[$name],'Process')};if($owned){$mutex.ReleaseMutex()};$mutex.Dispose()}
