param([string]$Zig='zig')
$ErrorActionPreference='Stop'
Push-Location $PSScriptRoot
try {
    if(-not(Test-Path -LiteralPath 'snapshot\nv_backend.c')) { & .\Prepare-Backend.ps1 }
    New-Item -ItemType Directory -Force -Path out | Out-Null
    & $Zig cc -target x86_64-windows-gnu -std=c11 -O2 -shared -Wall -Wextra -Werror -Wno-unused-function -Wno-unused-parameter -Wno-unused-variable -Wno-missing-field-initializers -Isnapshot/include -Isnapshot snapshot/nv_backend.c cpvr_nv_backend.def -lbcrypt -lversion -ld3d12 -ldxgi -ldxguid -luuid -o out/cpvr_nv_backend.dll
    if($LASTEXITCODE){throw 'Own NVIDIA backend C ABI build failed'}
    Get-FileHash -LiteralPath 'out\cpvr_nv_backend.dll'
} finally {Pop-Location}
