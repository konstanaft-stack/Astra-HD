param([Parameter(Mandatory=$true)][string]$OutputDirectory)
$ErrorActionPreference='Stop'
$repo=Split-Path $PSScriptRoot -Parent
New-Item -ItemType Directory -Force -Path $OutputDirectory | Out-Null
Push-Location $repo
try {
    $testExe=Join-Path $OutputDirectory 'ui-consumer-tests.exe'
    $testObj=Join-Path $OutputDirectory 'ui-consumer-tests.obj'
    & cmd /c "`"<WORKSPACE_ROOT>\tools\Cyberpunk-Modding\MSVC-2022\msvc\setup_x64.bat`" && cl /nologo /std:c++20 /EHsc /DNOMINMAX /I include tests\MenuPreviewUiConsumerTests.cpp /Fe:`"$testExe`" /Fo:`"$testObj`""
    if($LASTEXITCODE -ne 0){throw 'UI consumer policy compile failed'}
    & $testExe
    if($LASTEXITCODE -ne 0){throw 'UI consumer policy tests failed'}
    'PASS ordered same-queue writer/consumer, scope/setup/role rejection and table bounds' | Set-Content (Join-Path $OutputDirectory 'RESULT.txt')
} finally {Pop-Location}
