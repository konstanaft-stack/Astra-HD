param(
    [Parameter(Mandatory)][string]$AuditDirectory,
    [string]$CompilerRoot = '<WORKSPACE_ROOT>\tools\Cyberpunk-Modding\MSVC-2022\msvc\VC\Tools\MSVC\14.44.35207',
    [string]$SdkRoot = '<WORKSPACE_ROOT>\tools\Cyberpunk-Modding\MSVC-2022\msvc\Windows Kits\10',
    [string]$SdkVersion = '10.0.26100.0',
    [switch]$VerifyRegression
)
$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..\..')).Path
$audit = [IO.Path]::GetFullPath($AuditDirectory)
New-Item -ItemType Directory -Path $audit -Force | Out-Null
$priorInclude = $env:INCLUDE
$priorLib = $env:LIB
try {
    $env:INCLUDE = "$CompilerRoot\include;$SdkRoot\Include\$SdkVersion\ucrt;$SdkRoot\Include\$SdkVersion\shared;$SdkRoot\Include\$SdkVersion\um"
    $env:LIB = "$CompilerRoot\lib\x64;$SdkRoot\Lib\$SdkVersion\ucrt\x64;$SdkRoot\Lib\$SdkVersion\um\x64"
    function Compile-Test([string]$Name, [string]$IncludeRoot, [string]$SourceName = 'main.cpp') {
        $exe = Join-Path $audit "$Name.exe"
        $objectRoot = Join-Path $audit "$Name-objects"
        New-Item -ItemType Directory -Path $objectRoot -Force | Out-Null
        $sources = @((Join-Path $PSScriptRoot $SourceName))
        if ($SourceName -in @('native_boundary.cpp','apply_transaction.cpp')) { $sources += (Join-Path $repo 'src\Overlay\PpdNativeApply.cpp') }
        & "$CompilerRoot\bin\Hostx64\x64\cl.exe" /nologo /EHsc /std:c++20 /W4 "/I$IncludeRoot" `
            @sources "/Fo$objectRoot\\" "/Fe$exe" 2>&1 |
            Set-Content -LiteralPath (Join-Path $audit "$Name-build.log") -Encoding utf8
        if ($LASTEXITCODE -ne 0) { throw "Compilation failed: $Name-build.log" }
        return $exe
    }
    $exe = Compile-Test 'production' (Join-Path $repo 'include')
    $output = & $exe 2>&1
    $testExit = $LASTEXITCODE
    $output | Set-Content -LiteralPath (Join-Path $audit 'production-result.log') -Encoding utf8
    if ($testExit -ne 0) { throw "Production regression failed: $output" }
    $dispatchExe = Compile-Test 'dispatch' (Join-Path $repo 'include') 'dispatch.cpp'
    $dispatchOutput = & $dispatchExe 2>&1
    $dispatchExit = $LASTEXITCODE
    $dispatchOutput | Set-Content -LiteralPath (Join-Path $audit 'dispatch-result.log') -Encoding utf8
    if ($dispatchExit -ne 0) { throw "Dispatch regression failed: $dispatchOutput" }
    $reservationExe = Compile-Test 'reservation' (Join-Path $repo 'include') 'reservation.cpp'
    $reservationOutput = & $reservationExe 2>&1
    $reservationExit = $LASTEXITCODE
    $reservationOutput | Set-Content -LiteralPath (Join-Path $audit 'reservation-result.log') -Encoding utf8
    if ($reservationExit -ne 0) { throw "Reservation regression failed: $reservationOutput" }
    $nativeExe = Compile-Test 'native-boundary' (Join-Path $repo 'include') 'native_boundary.cpp'
    $nativeOutput = & $nativeExe 2>&1
    $nativeExit = $LASTEXITCODE
    $nativeOutput | Set-Content -LiteralPath (Join-Path $audit 'native-boundary-result.log') -Encoding utf8
    if ($nativeExit -ne 0) { throw "Native write boundary regression failed: $nativeOutput" }
    $terminalExe = Compile-Test 'terminal' (Join-Path $repo 'include') 'terminal.cpp'
    $terminalOutput = & $terminalExe 2>&1
    $terminalExit = $LASTEXITCODE
    $terminalOutput | Set-Content -LiteralPath (Join-Path $audit 'terminal-result.log') -Encoding utf8
    if ($terminalExit -ne 0) { throw "Terminal lifecycle regression failed: $terminalOutput" }
    $applyExe = Compile-Test 'apply-transaction' (Join-Path $repo 'include') 'apply_transaction.cpp'
    $applyOutput = & $applyExe 2>&1
    $applyOutput | Set-Content -LiteralPath (Join-Path $audit 'apply-transaction-result.log') -Encoding utf8
    if ($LASTEXITCODE -ne 0) { throw "Native apply transaction regression failed: $applyOutput" }
    $copyExe = Compile-Test 'color-copy' (Join-Path $repo 'include') 'color_copy.cpp'
    $copyOutput = & $copyExe 2>&1
    $copyOutput | Set-Content -LiteralPath (Join-Path $audit 'color-copy-result.log') -Encoding utf8
    if ($LASTEXITCODE -ne 0) { throw "Menu color copy regression failed: $copyOutput" }
    $mutationResult = 'NOT_RUN'
    if ($VerifyRegression) {
        # Only this audit copy bypasses the CPU wait. Repository code is intact.
        $mutantRoot = Join-Path $audit 'mutant-include'
        New-Item -ItemType Directory -Path (Join-Path $mutantRoot 'Render') -Force | Out-Null
        $header = Get-Content -LiteralPath (Join-Path $repo 'include\Render\ProducerResetGate.hpp') -Raw
        $needle = 'return active_ == 0;'
        if (($header.Split($needle).Count - 1) -ne 1) { throw 'CPU-wait mutation target is not unique' }
        $header.Replace($needle, 'return true;') | Set-Content -LiteralPath (Join-Path $mutantRoot 'Render\ProducerResetGate.hpp') -Encoding utf8
        $mutant = Compile-Test 'skip-cpu-wait' $mutantRoot
        $mutantOutput = & $mutant 2>&1
        $mutantExit = $LASTEXITCODE
        $mutantOutput | Set-Content -LiteralPath (Join-Path $audit 'skip-cpu-wait-result.log') -Encoding utf8
        if ($mutantExit -ne 1 -or "$mutantOutput" -notmatch 'GPU drain ran before pending CPU recording was submitted') {
            throw "Expected behavioral regression was not detected: exit=$mutantExit $mutantOutput"
        }
        $mutationResult = 'EXPECTED_FAILURE_DETECTED'
    }
    [ordered]@{
        time = [DateTimeOffset]::Now.ToString('o')
        scope = 'Portable CPU admission fixture; no CET hooks, real GPU fences, game run or HMD acceptance'
        production = 'PASS'
        dispatch = 'PASS'
        reservation = 'PASS'
        native_write_boundary = 'PASS'
        terminal_lifecycle = 'PASS'
        native_apply_transaction = 'PASS'
        menu_color_copy = 'PASS'
        skipped_cpu_wait = $mutationResult
        header_sha256 = (Get-FileHash -LiteralPath (Join-Path $repo 'include\Render\ProducerResetGate.hpp')).Hash
        test_sha256 = (Get-FileHash -LiteralPath (Join-Path $PSScriptRoot 'main.cpp')).Hash
    } | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $audit 'RESULT.json') -Encoding utf8
    Get-Content -LiteralPath (Join-Path $audit 'RESULT.json')
} finally {
    $env:INCLUDE = $priorInclude
    $env:LIB = $priorLib
}
