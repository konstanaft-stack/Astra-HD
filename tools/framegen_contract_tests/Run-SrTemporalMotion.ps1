param([string]$RunName='green',[switch]$NegativeControl)
$ErrorActionPreference='Stop'
if($RunName -notmatch '^[A-Za-z0-9_-]+$'){throw 'Invalid run name'}
$project='<WORKSPACE_ROOT>/research/2026-09-30-cpvr-open-nvidia';$base=(Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
$audit=Join-Path $project 'audit/luke-eye-cycle-20261003/sr-motion-shader';$output=Join-Path $project ('build/tofu-sr-temporal-motion/'+$RunName)
New-Item -ItemType Directory -Path $output -Force|Out-Null
function Assert-GamesClosed {
 $games=@(Get-Process -Name NMS,Cyberpunk2077 -ErrorAction SilentlyContinue)
 if($games.Count){throw 'HOLD_USER_GAME'}
}
Assert-GamesClosed
$oracle=Join-Path $audit 'combined-mv-110-1024.dxbc'
if((Get-FileHash -LiteralPath $oracle).Hash -ne '0BEBFF56E9D992CE01483A5D309C6D9188F029F4336A606F0EB846AF9FDA7DD3'){throw 'Oracle changed'}
if((Get-FileHash -LiteralPath (Join-Path $audit 'recovered.hlsl')).Hash -ne '6DD429584458F780D2E891BE322C3174F4ADC44993394B3789500390DAA87CC0'){throw 'Frozen HLSL changed'}
$mutex=[Threading.Mutex]::new($false,'Local\IRD.SingleBuildTestDeploy.v1');$owned=$false;$saved=@{}
foreach($name in @('PATH','INCLUDE','LIB')){$saved[$name]=[Environment]::GetEnvironmentVariable($name,'Process')}
try{
 try{$owned=$mutex.WaitOne(0)}catch [Threading.AbandonedMutexException]{$owned=$true}
 if(-not $owned){throw 'Build/test owner busy'}
 $vc='<WORKSPACE_ROOT>/tools/Cyberpunk-Modding/MSVC-2022/msvc/VC/Tools/MSVC/14.44.35207';$sdk='<WORKSPACE_ROOT>/tools/Cyberpunk-Modding/MSVC-2022/msvc/Windows Kits/10';$v='10.0.26100.0'
 $env:PATH=(Join-Path $vc 'bin/Hostx64/x64')+';'+$saved.PATH
 $env:INCLUDE=((Join-Path $vc 'include'),(Join-Path $sdk "Include/$v/ucrt"),(Join-Path $sdk "Include/$v/shared"),(Join-Path $sdk "Include/$v/um"),(Join-Path $sdk "Include/$v/winrt")) -join ';'
 $env:LIB=((Join-Path $vc 'lib/x64'),(Join-Path $sdk "Lib/$v/ucrt/x64"),(Join-Path $sdk "Lib/$v/um/x64")) -join ';'
 $paths=@((Join-Path $base 'include/Render/SrTemporalMotionPass.hpp'),(Join-Path $base 'src/Render/SrTemporalMotionPass.cpp'),(Join-Path $base 'src/Render/SrTemporalMotionShader.hpp'),(Join-Path $PSScriptRoot 'sr_temporal_motion_warp.cpp'),$PSCommandPath,(Join-Path $audit 'gpu_compare.cpp'),$oracle,(Join-Path $audit 'recovered.hlsl'))
 $before=@($paths|ForEach-Object{@{path=$_;sha256=(Get-FileHash -LiteralPath $_).Hash}})
 $exe=Join-Path $output 'sr-temporal-motion.exe'
 Push-Location $output
 try{& (Join-Path $vc 'bin/Hostx64/x64/cl.exe') /nologo /EHsc /O2 /std:c++20 /W4 /WX /MT /DNOMINMAX /DWIN32_LEAN_AND_MEAN ("/I"+(Join-Path $base 'include')) ("/I"+$audit) ("/Fe:"+$exe) (Join-Path $PSScriptRoot 'sr_temporal_motion_warp.cpp') (Join-Path $base 'src/Render/SrTemporalMotionPass.cpp') /link d3d12.lib dxgi.lib d3dcompiler.lib *> (Join-Path $output 'build.log')}finally{Pop-Location}
 if($LASTEXITCODE -ne 0){Get-Content -LiteralPath (Join-Path $output 'build.log');throw 'Compile failed'}
 Assert-GamesClosed
 @{checked_utc=[DateTime]::UtcNow.ToString('o');games=@();adapter='WARP software';target_dll_executed=$false}|ConvertTo-Json|Set-Content -LiteralPath (Join-Path $output 'guard.json') -Encoding utf8
 $extra=@();if($NegativeControl){$extra+='negative'}
 & $exe $oracle $output @extra *> (Join-Path $output 'run.log');$result=$LASTEXITCODE
 Get-Content -LiteralPath (Join-Path $output 'run.log') -Tail 25
 $after=@($paths|ForEach-Object{@{path=$_;sha256=(Get-FileHash -LiteralPath $_).Hash}})
 $changes=@(Compare-Object ($before|ForEach-Object{$_.path+'|'+$_.sha256}) ($after|ForEach-Object{$_.path+'|'+$_.sha256}))
 @{before=$before;after=$after;changes=$changes.Count}|ConvertTo-Json -Depth 5|Set-Content -LiteralPath (Join-Path $output 'source-manifest.json') -Encoding utf8
 @{exit_code=$result;negative_control=[bool]$NegativeControl;exe_sha256=(Get-FileHash -LiteralPath $exe).Hash;adapter='WARP';original_shader_oracle=$true;target_dll_executed=$false;game_launched=$false}|ConvertTo-Json|Set-Content -LiteralPath (Join-Path $output 'result.json') -Encoding utf8
 if($changes.Count){throw 'Fixture source changed'}
 if(($NegativeControl -and $result -ne 1) -or (-not $NegativeControl -and $result -ne 0)){throw 'Unexpected fixture result'}
}finally{foreach($name in $saved.Keys){[Environment]::SetEnvironmentVariable($name,$saved[$name],'Process')};if($owned){$mutex.ReleaseMutex()};$mutex.Dispose()}
