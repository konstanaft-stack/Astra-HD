$ErrorActionPreference='Stop'
. "$PSScriptRoot/../Components.ps1"
function Get-Process { return $null } # Isolated fixture has no game process.
$d=Join-Path $PSScriptRoot ('fixture-'+[guid]::NewGuid().ToString('N'))
$c=@{game="$d/game";data="$d/data";bundle="$d/bundle";hold="$d/hold.json";vortex="$d/no.exe";vortexData="$d/vortex"}
New-Item -ItemType Directory -Force "$d/game/bin/x64"|Out-Null
Write-Json "$d/hold.json" @{active=$false};Write-Json "$d/bundle/bundle.json" @{versions=@()}
Write-Json "$d/game/CPVR-Portable/installed.json" @{paths=@('bin/x64/OptiScaler.ini');critical=@();opti=$true}
$ini="$d/game/bin/x64/OptiScaler.ini";[IO.File]::WriteAllText($ini,"[DlssNr]`nEnabled=false`n[Menu]`nShortcutKey=0x75`n")
$req=@{opti=$true;nr=$true;mods=$true;libraries=@{}}
Set-LibrariesAndMods $c $req
if(-not [IO.File]::ReadAllText($ini).Contains('Enabled=true')){throw 'NR was not enabled'}
$req.nr=$false;Set-LibrariesAndMods $c $req
$text=[IO.File]::ReadAllText($ini);if(-not $text.Contains('Enabled=false') -or -not $text.Contains('ShortcutKey=0x75')){throw 'NR off or preservation failed'}
'PASS: DLSS-NR configuration on/off, unrelated key preserved; no real game touched.'
