# Read-only inventory tests against an isolated fake game tree.
$ErrorActionPreference='Stop'
. "$PSScriptRoot/../Core.ps1"
function Assert($value,$message){if(-not $value){throw $message}}
$fixture=Join-Path $PSScriptRoot ('runtime-fixture-'+[guid]::NewGuid().ToString('N'))
$c=@{game="$fixture/game";data="$fixture/data";bundle="$fixture/bundle";hold="$fixture/hold.json";vortex="$fixture/missing.exe";vortexData="$fixture/vortex"}
$owned=@('bin/x64/plugins/cyber_engine_tweaks/mods/our_port/init.lua','red4ext/plugins/codeware/codeware.dll','archive/pc/mod/core.archive')
$runtime=@('bin/x64/plugins/cyber_engine_tweaks/mods/our_port/db.sqlite3','bin/x64/plugins/cyber_engine_tweaks/mods/our_port/user_settings.ini','red4ext/plugins/codeware/Persistent/ScriptableServiceContainer.dat')
$external=@('bin/x64/plugins/cyber_engine_tweaks/mods/other_mod/init.lua','red4ext/plugins/other_plugin/other.dll','archive/pc/mod/optional.archive')
foreach($relative in @($owned)+@($runtime)+@($external)){$p=Join-Path $c.game $relative;New-Item -ItemType Directory (Split-Path $p) -Force|Out-Null;[IO.File]::WriteAllText($p,'fixture')}
Write-Json "$fixture/hold.json" @{active=$false}
Write-Json "$fixture/bundle/bundle.json" @{versions=@();managedPaths=@()}
Write-Json "$fixture/game/CPVR-Portable/installed.json" @{version='test';clean=$true;paths=$owned;critical=@()}
$s=Get-PortState $c
foreach($p in $runtime){Assert ($p -notin $s.externalFiles) "Required port runtime mislabelled as optional mod: $p"}
foreach($p in $external){Assert ($p -in $s.externalFiles) "Real optional mod hidden: $p"}
Assert ($s.externalFiles.Count -eq 3) 'Unexpected inventory count'
Write-Output 'PASS: required CET/RED4ext runtime retained; unrelated modules and archives detected.'
