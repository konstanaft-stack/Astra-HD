param([Parameter(Mandatory)][string]$OutputDirectory)
$ErrorActionPreference='Stop'
$source=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
$out=[IO.Path]::GetFullPath($OutputDirectory)
[void][IO.Directory]::CreateDirectory($out)
$python='<USER_HOME>\.cache\codex-runtimes\codex-primary-runtime\dependencies\python\python.exe'
$tool='<WORKSPACE_ROOT>\tools\Cyberpunk-Modding\MSVC-2022\msvc'
$vc=Join-Path $tool 'VC\Tools\MSVC\14.44.35207'
$sdk=Join-Path $tool 'Windows Kits\10';$version='10.0.26100.0'
$savedInclude=$env:INCLUDE;$savedLib=$env:LIB
try {
    & $python -X utf8 (Join-Path $PSScriptRoot 'native_options_observer_test.py') (Join-Path $out 'observer.cpp')
    if($LASTEXITCODE -ne 0){throw 'Observer fixture generation failed'}
    $env:INCLUDE=(@("$vc\include")+@('ucrt','shared','um','winrt'|ForEach-Object{"$sdk\Include\$version\$_"})) -join ';'
    $env:LIB="$vc\lib\x64;$sdk\Lib\$version\ucrt\x64;$sdk\Lib\$version\um\x64"
    & "$vc\bin\Hostx64\x64\cl.exe" /nologo /std:c++20 /EHsc /MT /DNOMINMAX /I"$source\include" /I"$source\externals\streamline\include" "$out\observer.cpp" /Fo"$out\observer.obj" /Fe"$out\observer.exe" 2>&1 | Tee-Object -FilePath "$out\compile.log"
    if($LASTEXITCODE -ne 0){throw 'Observer fixture compilation failed'}
    & "$out\observer.exe" 2>&1 | Tee-Object -FilePath "$out\RESULT.log"
    if($LASTEXITCODE -ne 0){throw 'Observer fixture failed'}
} finally {$env:INCLUDE=$savedInclude;$env:LIB=$savedLib}
