$ErrorActionPreference='Stop'
. "$PSScriptRoot/../Components.ps1"
function Assert($value,$why){if(-not $value){throw $why}}
$root=Join-Path $PSScriptRoot ('fixture-profile-'+[guid]::NewGuid().ToString('N'))
$c=@{game="$root/game";data="$root/data";bundle="$root/bundle";hold="$root/hold.json";vortexData="$root/vortex";vortex="$root/missing.exe"}
New-Item -ItemType Directory -Force "$root/game/archive/pc/mod","$root/game/CPVR-Portable","$root/data","$root/bundle"|Out-Null
Write-Json $c.hold @{active=$false}
Write-Json "$root/bundle/bundle.json" @{versions=@();managedPaths=@()}
Write-Json "$root/game/CPVR-Portable/installed.json" @{version='fixture';clean=$false;paths=@('archive/pc/mod/required.archive');critical=@()}
[IO.File]::WriteAllText("$root/game/archive/pc/mod/required.archive",'required')
[IO.File]::WriteAllText("$root/game/archive/pc/mod/optional.archive",'optional')
$s=Get-PortState $c
Assert (-not (Get-ModProfileState $c $s).enabled) 'New profile must default off'
Set-LibrariesAndMods $c @{mods=$false;libraries=@{}}
Assert (Test-Path "$root/game/archive/pc/mod/required.archive") 'Required dependency lost'
Assert (-not (Test-Path "$root/game/archive/pc/mod/optional.archive")) 'Optional mod active in clean profile'
# A later external deployment must not turn the saved switch back on.
[IO.File]::WriteAllText("$root/game/archive/pc/mod/optional.archive",'optional')
$s=Get-PortState $c;$p=Get-ModProfileState $c $s
Assert (-not $p.enabled -and $p.active -eq 1) 'External files changed saved preference'
Ensure-LaunchModProfile $c $s
Assert (-not (Test-Path "$root/game/archive/pc/mod/optional.archive")) 'Launch did not restore clean profile'
Assert ([IO.File]::ReadAllText("$root/game/archive/pc/mod/required.archive") -eq 'required') 'Required dependency modified'
Set-LibrariesAndMods $c @{mods=$true;libraries=@{}}
Assert ((Get-ModProfileState $c (Get-PortState $c)).enabled) 'Explicit enable not remembered'
Assert ([IO.File]::ReadAllText("$root/game/archive/pc/mod/optional.archive") -eq 'optional') 'Optional mod not restored'
Set-LibrariesAndMods $c @{mods=$false;libraries=@{}}
Assert (-not (Get-ModProfileState $c (Get-PortState $c)).enabled) 'Disable not remembered on fresh read'
Write-Output 'PASS: default off; explicit on/off persists; external redeployment cannot enable; clean launch preserves required files; optional files restore.'
