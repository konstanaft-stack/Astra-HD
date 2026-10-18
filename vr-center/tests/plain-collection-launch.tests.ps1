$ErrorActionPreference='Stop'
$app=Split-Path $PSScriptRoot -Parent
. (Join-Path $app 'GameLaunch.ps1')
$root=Join-Path $app ('audit/plain-collection-'+[guid]::NewGuid().ToString('N'))
function Put($p,$t){[IO.Directory]::CreateDirectory((Split-Path $p -Parent))|Out-Null;[IO.File]::WriteAllText($p,$t)}
$config=@{game="$root/game";data="$root/data";mo2=@{root="$root/collection";game="$root/game";executables=@{Cyberpunk2077='bin/x64/Cyberpunk2077.exe'}}}
Put "$root/game/bin/x64/Cyberpunk2077.exe" 'fixture'
Put "$root/collection/profiles/Test/modlist.txt" '+mod'
Put "$root/collection/mods/mod/archive/pc/mod/test.archive" 'fixture'
Put "$root/data/launch-source.json" '{"schema":1,"kind":"mo2","profile":"Test","executable":"Cyberpunk2077"}'
$script:dispatched=$false
function Invoke-VrLauncher {throw 'Plain collection must not require CPVR-Portable or its VR launcher'}
function Start-GameLaunchDispatcher {param($Config,$Plan,$Log);$script:dispatched=$true;@{dispatcherPid=77000}}
function Get-GameLaunchProcesses {param($Name);if('Cyberpunk2077' -in $Name -and $script:dispatched){@{pid=77001;exe=[IO.Path]::GetFullPath("$root/game/bin/x64/Cyberpunk2077.exe");startedUtc=[datetime]::UtcNow.ToString('o')}}}
$result=Invoke-GameProfileLaunch $config "$root/log.txt"
if($result.game.pid -ne 77001 -or -not $script:dispatched){throw 'Plain collection was not dispatched'}
Write-Output 'PASS: plain collection starts without installed VR receipt or portable VR launcher'
