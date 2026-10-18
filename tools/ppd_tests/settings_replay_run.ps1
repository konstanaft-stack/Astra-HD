param([Parameter(Mandatory)][string]$AuditDirectory)
$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..\..')).Path
$audit = [IO.Path]::GetFullPath($AuditDirectory)
New-Item -ItemType Directory -Path $audit -Force | Out-Null
$source = Get-Content -LiteralPath (Join-Path $repo 'src\Hooks\SettingsRes.cpp') -Raw
$begin = $source.IndexOf('    code[pos++] = 0x39; code[pos++] = 0x41; code[pos++] = 0x18;')
$end = $source.IndexOf('    DWORD oldProtect;', $begin)
if ($begin -lt 0 -or $end -le $begin) { throw 'Production replay boundary not found' }
$header = "inline bool EmitProductionReplay(uint8_t* code, int& pos, uint8_t* found) {`nconstexpr int replaceLen=14;`n" + $source.Substring($begin,$end-$begin) + "`nreturn true;`n}`n"
$header | Set-Content -LiteralPath (Join-Path $audit 'settings-replay-under-test.hpp') -Encoding utf8
$vc = '<WORKSPACE_ROOT>\tools\Cyberpunk-Modding\MSVC-2022\msvc\VC\Tools\MSVC\14.44.35207'
$sdk = '<WORKSPACE_ROOT>\tools\Cyberpunk-Modding\MSVC-2022\msvc\Windows Kits\10'
$savedInclude=$env:INCLUDE; $savedLib=$env:LIB
try {
    $env:INCLUDE="$vc\include;$sdk\Include\10.0.26100.0\ucrt;$sdk\Include\10.0.26100.0\shared;$sdk\Include\10.0.26100.0\um"
    $env:LIB="$vc\lib\x64;$sdk\Lib\10.0.26100.0\ucrt\x64;$sdk\Lib\10.0.26100.0\um\x64"
    & "$vc\bin\Hostx64\x64\cl.exe" /nologo /std:c++20 /EHsc /W4 "/I$audit" (Join-Path $PSScriptRoot 'settings_replay_test.cpp') "/Fo$audit\replay.obj" "/Fe$audit\replay.exe" 2>&1 | Set-Content -LiteralPath (Join-Path $audit 'build.log') -Encoding utf8
    if($LASTEXITCODE -ne 0){throw 'Replay fixture did not compile'}
    $output = & "$audit\replay.exe" 2>&1; $result=$LASTEXITCODE
    $output | Set-Content -LiteralPath (Join-Path $audit 'test.log') -Encoding utf8
    [ordered]@{time=[DateTimeOffset]::Now.ToString('o');exit=$result;scope='Actual production emitter executed against pinned native predicate bytes, no game run';source_sha256=(Get-FileHash -LiteralPath (Join-Path $repo 'src\Hooks\SettingsRes.cpp')).Hash;result="$output"} | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $audit 'RESULT.json') -Encoding utf8
    $output
    exit $result
} finally { $env:INCLUDE=$savedInclude; $env:LIB=$savedLib }
