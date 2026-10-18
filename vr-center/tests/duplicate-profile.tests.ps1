$ErrorActionPreference='Stop'
$app=Split-Path $PSScriptRoot -Parent
. (Join-Path $app 'GameProfileStore.ps1')
function Put($p,$text){[IO.Directory]::CreateDirectory((Split-Path $p -Parent))|Out-Null;[IO.File]::WriteAllText($p,$text)}
function Check($value,$why){if(-not $value){throw $why};Write-Output ('PASS: '+$why)}
$fixture=Join-Path $app ('audit/duplicate-profile-'+[guid]::NewGuid().ToString('N'))
$store=@{data="$fixture/data";initialName='Original';initialVR=$true;scopes=@(@{id='game';root="$fixture/game";kind='directory';requiredFiles=@('game.exe');exclusions=@()})}
Put "$fixture/game/game.exe" 'fixture';Put "$fixture/game/settings.ini" 'original-settings';Put "$fixture/game/save.dat" 'original-save'
Put "$fixture/common/mod.archive" 'common-resource'
Put "$fixture/game/modlist.txt" "+first`n+second"
Put "$fixture/game/library.txt" "$fixture/common"
$null=Save-GameProfile -StoreConfig $store
$before=@(Get-ChildItem "$fixture/data/profile-store/objects" -File).Count
$copy=Copy-GameProfile -StoreConfig $store -Name 'VR copy'
Check ($copy.activeId -ne 'current' -and $copy.profiles.Count -eq 2) 'duplicate is a separate active named profile'
Check (@(Get-ChildItem "$fixture/data/profile-store/objects" -File).Count -eq $before) 'duplicate reuses existing objects without another payload copy'
Check (($copy.profiles|Where-Object id -eq $copy.activeId).vr) 'duplicate preserves original VR metadata'
Put "$fixture/game/settings.ini" 'copy-settings';Put "$fixture/game/save.dat" 'copy-save';Put "$fixture/game/modlist.txt" '+first'
$null=Activate-GameProfile -StoreConfig $store -Id 'current'
Check ([IO.File]::ReadAllText("$fixture/game/settings.ini") -ceq 'original-settings' -and [IO.File]::ReadAllText("$fixture/game/save.dat") -ceq 'original-save' -and [IO.File]::ReadAllText("$fixture/game/modlist.txt") -ceq "+first`n+second") 'original settings, save and enabled mods survive changes in duplicate'
$null=Activate-GameProfile -StoreConfig $store -Id $copy.activeId
Check ([IO.File]::ReadAllText("$fixture/game/settings.ini") -ceq 'copy-settings' -and [IO.File]::ReadAllText("$fixture/game/save.dat") -ceq 'copy-save') 'return to duplicate restores its independent changes'
Check ([IO.File]::ReadAllText("$fixture/game/library.txt") -ceq "$fixture/common" -and @(Get-ChildItem $fixture -Filter '*.archive' -File -Recurse).Count -eq 1) 'both profiles keep one shared archive reference'
$null=Remove-GameProfile -StoreConfig $store -Id 'current'
Check ((Test-Path "$fixture/common/mod.archive") -and [IO.File]::ReadAllText("$fixture/game/settings.ini") -ceq 'copy-settings') 'deleting inactive original preserves common assets and active duplicate'
Write-Output ('PASS: evidence '+$fixture)
