param([Parameter(Mandatory)][string]$OutputDirectory)
$ErrorActionPreference='Stop'
[IO.Directory]::CreateDirectory($OutputDirectory)|Out-Null
$repo=Split-Path $PSScriptRoot -Parent
Push-Location $repo
try{
 $exe=Join-Path $OutputDirectory 'spatial-editor-tests.exe'
 $obj=Join-Path $OutputDirectory 'spatial-editor-tests.obj'
 & cmd /c "`"<WORKSPACE_ROOT>\tools\Cyberpunk-Modding\MSVC-2022\msvc\setup_x64.bat`" && cl /nologo /std:c++20 /EHsc /DNOMINMAX /I include /I <WORKSPACE_ROOT>\tools\VR-Debug\OpenXR-Simulator-8de3457\include tests\MenuPreviewSpatialEditorTests.cpp /Fe:`"$exe`" /Fo:`"$obj`""
 if($LASTEXITCODE -ne 0){throw 'Spatial editor policy compile failed'}
 & $exe
 if($LASTEXITCODE -ne 0){throw 'Spatial editor policy failed'}
}finally{Pop-Location}
