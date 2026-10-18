param([Parameter(Mandatory)][string]$BundleRoot)
$ErrorActionPreference='Stop'
. (Join-Path (Split-Path $PSScriptRoot -Parent) 'GameLaunch.ps1')
function Put($p,$v){[IO.Directory]::CreateDirectory((Split-Path $p -Parent))|Out-Null;[IO.File]::WriteAllText($p,$v)}
function Check($v,$why){if(-not $v){throw $why};Write-Output ('PASS: '+$why)}
$root=Join-Path ([IO.Path]::GetTempPath()) ('redmod-'+[guid]::NewGuid().ToString('N'))
$c=@{game="$root/game";data="$root/data";hold="$root/hold.json";redmodPackage="$root/package.json"}
Write-Json $c.hold @{active=$false}
Put "$root/game/bin/x64/Cyberpunk2077.exe" 'not-executed'
Put "$root/stock/redMod.exe" 'tool-bytes';Put "$root/stock/runtime.dll" 'runtime-bytes'
$files=@(@{path='bin/redMod.exe';source="$root/stock/redMod.exe";sha256=(Get-FileHash "$root/stock/redMod.exe").Hash},@{path='bin/runtime.dll';source="$root/stock/runtime.dll";sha256=(Get-FileHash "$root/stock/runtime.dll").Hash})
Write-Json $c.redmodPackage @{schema=1;files=$files}
Write-Json "$root/data/launch-source.json" @{schema=1;kind='direct';arguments=@('--launcher-skip','-modded');custom='preserve'}
Check (-not(Get-RedmodState $c).installed -and (Get-RedmodState $c).enabled) 'enabled flag is distinct from installed REDmod tool'
Set-RedmodEnabled $c $true
Check ((Get-RedmodState $c).installed -and [IO.File]::ReadAllText("$root/game/tools/redmod/bin/runtime.dll") -eq 'runtime-bytes') 'enabling restores complete saved tool with dependencies'
Set-RedmodEnabled $c $false
$source=Get-GameLaunchSource $c
Check (-not(Get-RedmodState $c).enabled -and $source.custom -eq 'preserve' -and '--launcher-skip' -in $source.arguments) 'off changes only REDmod flag and keeps tool and source descriptor'
Set-RedmodEnabled $c $true
$plan=Get-GameLaunchPlan $c -InspectOnly
Check ('-modded' -in $plan.arguments -and $plan.kind -eq 'direct') 'plain direct launch receives REDmod argument too'
$noPackage=$c.Clone();$noPackage.data="$root/other-data";$noPackage.game="$root/other-game";$noPackage.redmodPackage="$root/missing-package.json"
$failed=$false;try{Set-RedmodEnabled $noPackage $true}catch{$failed=$_.Exception.Message -match 'не установлен'}
Check ($failed -and -not(Test-Path "$root/other-data/launch-source.json")) 'missing actual tool rejects on without falsely setting enabled'
# Exercise the real managed launcher environment without starting its game.
$launcher=[IO.File]::ReadAllText((Join-Path $BundleRoot 'Start-Game.ps1'))
$needle='$process=[Diagnostics.Process]::Start($start)'
if(-not $launcher.Contains($needle)){throw 'Launcher fixture contract changed'}
$launcher=$launcher.Replace($needle,'Write-Output ($start.ArgumentList -join "|"); exit 0; '+$needle)
Put "$root/game/CPVR-Portable/Start-Game.ps1" $launcher
Write-Json "$root/game/CPVR-Portable/installed.json" @{version='flat';kind='flat';exeSha256=(Get-FileHash "$root/game/bin/x64/Cyberpunk2077.exe").Hash;critical=@();paths=@()}
$c.pwsh=(Get-Process -Id $PID).Path;$c.launchArguments=@('--launcher-skip','-modded')
$check=Invoke-VrLauncher $c "$root/launcher.log"
Check ($check.output -match '--launcher-skip\|-modded') 'managed direct launcher preserves environment and forwards REDmod arguments without game execution'
Write-Output ('Fixture: '+$root)
