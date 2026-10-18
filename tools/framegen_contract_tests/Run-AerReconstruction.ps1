param([switch]$Green,[switch]$ExpectFailure,[string]$RunName='')
$ErrorActionPreference='Stop'
$project='<WORKSPACE_ROOT>/research/2026-09-30-cpvr-open-nvidia'
$output=Join-Path $project 'build/tofu-aer-reconstruction'
if($RunName){if($RunName -notmatch '^[A-Za-z0-9_-]+$'){throw 'Invalid run name'};$output=Join-Path $output $RunName}
$audit=Join-Path $project 'audit/luke-eye-cycle-20261003'
$tofu=(Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
New-Item -ItemType Directory -Path $output -Force | Out-Null
$games=@(Get-Process -Name NMS,Cyberpunk2077 -ErrorAction SilentlyContinue)
if($games.Count){throw 'HOLD_USER_GAME'}
$oracles=@{
 'pixel-vs-warp-5778e0.dxbc'='F340B9A2190C9DB7329F4AC05702F1FFFC454CEAAFDD7B8AF3E33C79433B1AAD'
 'pixel-quality-20261004/shader-573b30.dxbc'='7BBBDE6E5EE1693F39D34A05D88B3C066F41C7F0DC0C6CB47D45C6CADC2AE9A2'
 'pixel-quality-20261004/shader-578800.dxbc'='C2F33337DBD653B8192985E4AB167AEC30F2E7AF3421FB258042D191F56239F5'
}
foreach($asset in $oracles.Keys){if((Get-FileHash -LiteralPath (Join-Path $audit $asset)).Hash -ne $oracles[$asset]){throw "Oracle hash mismatch: $asset"}}
$mutex=[Threading.Mutex]::new($false,'Local\IRD.SingleBuildTestDeploy.v1');$owned=$false
$saved=@{};foreach($name in @('PATH','INCLUDE','LIB')){$saved[$name]=[Environment]::GetEnvironmentVariable($name,'Process')}
try{
 try{$owned=$mutex.WaitOne(0)}catch [Threading.AbandonedMutexException]{$owned=$true}
 if(-not $owned){throw 'Build/test owner busy'}
 $tool='<WORKSPACE_ROOT>/tools/Cyberpunk-Modding/MSVC-2022/msvc';$vc=Join-Path $tool 'VC/Tools/MSVC/14.44.35207';$sdk=Join-Path $tool 'Windows Kits/10';$v='10.0.26100.0'
 $env:PATH=(Join-Path $vc 'bin/Hostx64/x64')+';'+$saved.PATH
 $env:INCLUDE=((Join-Path $vc 'include'),(Join-Path $sdk "Include/$v/ucrt"),(Join-Path $sdk "Include/$v/shared"),(Join-Path $sdk "Include/$v/um"),(Join-Path $sdk "Include/$v/winrt"),(Join-Path $sdk "Include/$v/cppwinrt")) -join ';'
 $env:LIB=((Join-Path $vc 'lib/x64'),(Join-Path $sdk "Lib/$v/ucrt/x64"),(Join-Path $sdk "Lib/$v/um/x64")) -join ';'
 $name=if($Green -and -not $ExpectFailure){'green'}else{'red'};$exe=Join-Path $output ('aer-'+$name+'.exe')
 $sources=@((Join-Path $PSScriptRoot 'aer_reconstruction_warp.cpp'),(Join-Path $tofu 'src/Render/SharpenPass.cpp'))
 if($Green){$sources+=(Join-Path $tofu 'src/Render/AerReconstructionPass.cpp')}
 Push-Location $output
 try{& (Join-Path $vc 'bin/Hostx64/x64/cl.exe') /nologo /EHsc /O2 /std:c++20 /W4 /WX /MT /DNOMINMAX /DWIN32_LEAN_AND_MEAN ("/DUSE_AER_PASS="+[int][bool]$Green) ("/I"+(Join-Path $tofu 'include')) ("/I"+$audit) ("/Fe:"+$exe) $sources /link d3d12.lib dxgi.lib d3dcompiler.lib *> (Join-Path $output ($name+'-build.log'))}finally{Pop-Location}
 if($LASTEXITCODE -ne 0){Get-Content -LiteralPath (Join-Path $output ($name+'-build.log'));throw 'Compilation failed'}
 $games=@(Get-Process -Name NMS,Cyberpunk2077 -ErrorAction SilentlyContinue);if($games.Count){throw 'HOLD_USER_GAME before WARP'}
 @{time=(Get-Date).ToUniversalTime().ToString('o');games=@($games);adapter='WARP software only';mode=$name;oracles=$oracles}|ConvertTo-Json -Depth 4|Set-Content -LiteralPath (Join-Path $output ($name+'-guard.json')) -Encoding utf8
 & $exe $audit *> (Join-Path $output ($name+'-run.log'));$result=$LASTEXITCODE
 Get-Content -LiteralPath (Join-Path $output ($name+'-run.log'))
 @{exit_code=$result;expected_red=(-not $Green -or $ExpectFailure);exe_sha256=(Get-FileHash -LiteralPath $exe).Hash}|ConvertTo-Json|Set-Content -LiteralPath (Join-Path $output ($name+'-result.json')) -Encoding utf8
 if($Green -and -not $ExpectFailure -and $result -ne 0){throw 'WARP contract failed'}
 if((-not $Green -or $ExpectFailure) -and $result -ne 1){throw 'RED did not fail as expected'}
}finally{foreach($name in $saved.Keys){[Environment]::SetEnvironmentVariable($name,$saved[$name],'Process')};if($owned){$mutex.ReleaseMutex()};$mutex.Dispose()}
