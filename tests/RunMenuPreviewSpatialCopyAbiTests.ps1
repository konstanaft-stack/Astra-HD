param([Parameter(Mandatory)][string]$OutputDirectory)
$ErrorActionPreference='Stop'
[IO.Directory]::CreateDirectory($OutputDirectory)|Out-Null
Push-Location (Split-Path $PSScriptRoot -Parent)
try{
 $thunk=Join-Path $OutputDirectory 'editor-copy.obj';$fixture=Join-Path $OutputDirectory 'copy-fixture.obj';$test=Join-Path $OutputDirectory 'test.obj';$exe=Join-Path $OutputDirectory 'spatial-copy-abi.exe'
 & cmd /c "`"<WORKSPACE_ROOT>\tools\Cyberpunk-Modding\MSVC-2022\msvc\setup_x64.bat`" && ml64 /nologo /c /Fo`"$thunk`" src\MenuPreview\SpatialEditorCopyThunk.asm && ml64 /nologo /c /Fo`"$fixture`" tests\SpatialEditorCopyAbi.asm && cl /nologo /std:c++20 /EHsc /DNOMINMAX /I include /I <WORKSPACE_ROOT>\tools\VR-Debug\OpenXR-Simulator-8de3457\include tests\MenuPreviewSpatialCopyAbiTests.cpp `"$thunk`" `"$fixture`" /Fe:`"$exe`" /Fo:`"$test`""
 if($LASTEXITCODE -ne 0){throw 'Spatial editor ABI fixture compile failed'}
 & $exe
 if($LASTEXITCODE -ne 0){throw 'Spatial editor ABI fixture failed'}
}finally{Pop-Location}
