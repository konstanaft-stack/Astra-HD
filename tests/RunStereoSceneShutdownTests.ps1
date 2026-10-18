param([Parameter(Mandatory)][string]$OutputDirectory)
$ErrorActionPreference='Stop'
[IO.Directory]::CreateDirectory($OutputDirectory)|Out-Null
$repo=Split-Path $PSScriptRoot -Parent
Push-Location $repo
try{
 $exe=Join-Path $OutputDirectory 'scene-shutdown-tests.exe';$obj=Join-Path $OutputDirectory 'scene-shutdown-tests.obj'
 & cmd /c "`"<WORKSPACE_ROOT>\tools\Cyberpunk-Modding\MSVC-2022\msvc\setup_x64.bat`" && cl /nologo /std:c++20 /EHsc /DNOMINMAX /I include /I <WORKSPACE_ROOT>\tools\VR-Debug\OpenXR-Simulator-8de3457\include tests\StereoSceneShutdownTests.cpp d3d12.lib /Fe:`"$exe`" /Fo:`"$obj`""
 if($LASTEXITCODE -ne 0){throw 'Scene shutdown regression compile failed'}
 & $exe|Tee-Object -FilePath (Join-Path $OutputDirectory 'stdout.log')
 $code=$LASTEXITCODE
 @{exitCode=$code;status=$(if($code -eq 0){'PASS'}else{'FAIL'});scope='Actual production scene-state callback after CRT global cleanup'}|ConvertTo-Json|Set-Content -LiteralPath (Join-Path $OutputDirectory 'RESULT.json') -Encoding utf8
 if($code -ne 0){throw ('Late production scene callback failed with exit '+$code)}
}finally{Pop-Location}
