param([Parameter(Mandatory)][string]$AuditDirectory)
$ErrorActionPreference='Stop'
$repo=(Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '../..')).Path
$audit=[IO.Path]::GetFullPath($AuditDirectory)
[IO.Directory]::CreateDirectory($audit)|Out-Null
$compiler='<WORKSPACE_ROOT>/tools/Cyberpunk-Modding/MSVC-2022/msvc/VC/Tools/MSVC/14.44.35207'
$sdk='<WORKSPACE_ROOT>/tools/Cyberpunk-Modding/MSVC-2022/msvc/Windows Kits/10'
$savedInclude=$env:INCLUDE;$savedLib=$env:LIB
try {
    $env:INCLUDE="$compiler/include;$sdk/Include/10.0.26100.0/ucrt;$sdk/Include/10.0.26100.0/shared;$sdk/Include/10.0.26100.0/um"
    $env:LIB="$compiler/lib/x64;$sdk/Lib/10.0.26100.0/ucrt/x64;$sdk/Lib/10.0.26100.0/um/x64"
    foreach($mode in @('production','mutant')) {
        $include=Join-Path $repo 'include'
        if($mode -eq 'mutant') {
            $include=Join-Path $audit 'mutant-include'
            [IO.Directory]::CreateDirectory((Join-Path $include 'Framegen'))|Out-Null
            $text=[IO.File]::ReadAllText((Join-Path $repo 'include/Framegen/PresentationStatus.hpp'))
            $needle='before.revision==after.revision'
            if(($text.Split($needle).Count-1) -ne 1){throw 'Mutation target changed'}
            [IO.File]::WriteAllText((Join-Path $include 'Framegen/PresentationStatus.hpp'),$text.Replace($needle,'true'),[Text.UTF8Encoding]::new($false))
            Copy-Item -LiteralPath (Join-Path $repo 'include/Framegen/HardwareTelemetry.hpp') -Destination (Join-Path $include 'Framegen/HardwareTelemetry.hpp')
        }
        & "$compiler/bin/Hostx64/x64/cl.exe" /nologo /EHsc /std:c++20 /W4 "/I$include" (Join-Path $PSScriptRoot 'presentation_status.cpp') "/Fo$audit/$mode.obj" "/Fe$audit/$mode.exe" 2>&1 | Set-Content -LiteralPath "$audit/$mode-build.log"
        if($LASTEXITCODE){throw "Compile failure: $mode"}
        $output=& "$audit/$mode.exe" 2>&1;$result=$LASTEXITCODE
        $output | Set-Content -LiteralPath "$audit/$mode-result.log"
        if($mode -eq 'production' -and $result){throw "Regression failed: $output"}
        if($mode -eq 'mutant' -and (!$result -or "$output" -notmatch 'policy change during production')){throw "Mutation not caught correctly: $output"}
    }
    [ordered]@{production='PASS';producer_revision_mutation='EXPECTED_FAIL';time=(Get-Date -Format o)}|ConvertTo-Json|Tee-Object -FilePath "$audit/RESULT.json"
}finally{$env:INCLUDE=$savedInclude;$env:LIB=$savedLib}
