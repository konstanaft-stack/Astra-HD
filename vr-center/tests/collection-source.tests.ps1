$ErrorActionPreference='Stop'
$app=Split-Path $PSScriptRoot -Parent
. (Join-Path $app 'GameLaunch.ps1')
. (Join-Path $app 'CollectionSources.ps1')
function Put($p,$text){[IO.Directory]::CreateDirectory((Split-Path $p -Parent))|Out-Null;[IO.File]::WriteAllText($p,$text)}
function Check($value,$text){if(-not $value){throw $text};Write-Output ('PASS: '+$text)}
$fixture=Join-Path ([IO.Path]::GetTempPath()) ('import-collection-'+[guid]::NewGuid().ToString('N'))
$root="$fixture/MO source";$c=@{game="$fixture/game";data="$fixture/data"}
Put "$root/ModOrganizer.ini" @'
[General]
gameName=Cyberpunk 2077
selected_profile=@ByteArray(My collection)
[Settings]
mod_directory=%BASE_DIR%/mod storage
[Plugins]
Cyberpunk%202077%20Support%20Plugin\enforce_archive_load_order=true
Cyberpunk%202077%20Support%20Plugin\reverse_archive_load_order=true
'@
Put "$root/profiles/My collection/modlist.txt" "+Winner`n-Loser"
Put "$root/profiles/My collection/UserSettings.json" '{"quality":"authored"}'
Put "$root/mod storage/Winner/r6/scripts/mod.reds" 'active'
Put "$root/mod storage/Winner/bin/x64/dxgi.dll" 'collection-loader'
Put "$root/mod storage/Winner/archive/pc/mod/shared.archive" 'shared-resource'
Put "$root/mod storage/Dormant/settings.json" '{"enabledLater":true}'
Put "$root/mod storage/Dormant/archive/pc/mod/dormant.archive" 'dormant-resource'
Put "$root/overwrite/config/generated.json" '{"original":true}'
Put "$fixture/game/bin/x64/Cyberpunk2077.exe" 'fixture'
$catalog=Get-Mo2CollectionCatalog $root
Check ($catalog.profiles.Count -eq 1 -and $catalog.profiles[0] -ceq 'My collection') 'collection discovered without ModOrganizer executable'
$import=Import-Mo2Collection $c $root 'My collection' -Arguments @('--launcher-skip')
$source=Get-GameLaunchSource $c
Check (-not(Test-Path "$($c.data)/collection/mods/Winner/archive/pc/mod/shared.archive") -and $source.collection.sharedModsRoot -eq $catalog.modsRoot) 'large archives stay in the original common library instead of being copied into each profile'
Check ($source.kind -ceq 'mo2' -and $source.collection.modsRoot -eq [IO.Path]::GetFullPath("$($c.data)/collection/mods") -and $source.origin.modsRoot -eq [IO.Path]::GetFullPath("$root/mod storage")) 'enabled mods are owned by the profile while original MO2 paths are retained for explicit refresh'
Check ($source.cyberpunk.enforceArchiveOrder -and $source.cyberpunk.reverseArchiveOrder) 'authored Cyberpunk order options retained'
Check ([IO.File]::ReadAllText("$root/profiles/My collection/UserSettings.json") -ceq '{"quality":"authored"}') 'source profile settings remain unchanged'
$plan=Get-GameLaunchPlan $c -InspectOnly
Check ($plan.engine -ceq 'usvfs' -and $plan.arguments[0] -ceq '--launcher-skip') 'saved source reopens without global MO2 configuration or executable'
Check ((Get-GameLaunchModRoots $c $plan).Count -eq 2) 'overwrite and enabled winner resolved from imported descriptor'
Put (Join-Path $plan.profileRoot 'modlist.txt') "+Winner`n+Dormant"
Check ((Get-GameLaunchModRoots $c $plan).Count -eq 3 -and [IO.File]::ReadAllText((Resolve-GameLaunchEffectiveFile $c $plan 'settings.json')).Contains('enabledLater')) 'duplicate can enable a previously inactive library mod with its private sidecars present'
Put (Join-Path $plan.profileRoot 'modlist.txt') "+Winner`n-Loser"
$sharedArchive="$root/mod storage/Winner/archive/pc/mod/shared.archive"
[IO.File]::Move($sharedArchive,$sharedArchive+'.temporarily-missing')
$missingRejected=$false
try{$null=Get-StandaloneSharedArchiveLinks $c $plan}catch{$missingRejected=$true}
[IO.File]::Move($sharedArchive+'.temporarily-missing',$sharedArchive)
Check $missingRejected 'missing individual shared archive rejects launch instead of silently falling back'
Put "$root/mod storage/Winner/archive/pc/mod/unimported.archive" 'later-added-archive'
$extraRejected=$false
try{$null=Get-StandaloneSharedArchiveLinks $c $plan}catch{$extraRejected=$true}
[IO.File]::Delete("$root/mod storage/Winner/archive/pc/mod/unimported.archive")
Check $extraRejected 'unrefreshed extra shared archive cannot diverge between load order and mapped files'
Put "$root/mod storage/Winner/r6/scripts/mod.reds" 'external later edit'
Check ([IO.File]::ReadAllText((Join-Path $source.collection.modsRoot 'Winner/r6/scripts/mod.reds')) -ceq 'active') 'external mod library edits cannot leak into the imported profile'
Put (Join-Path $source.collection.modsRoot 'Winner/r6/scripts/mod.reds') 'profile-local edit'
Check ([IO.File]::ReadAllText("$root/mod storage/Winner/r6/scripts/mod.reds") -ceq 'external later edit') 'writes into imported mod files do not alter original library'
Put "$root/profiles/My collection/modlist.txt" '+Different later change'
Check ([IO.File]::ReadAllText((Join-Path $plan.profileRoot 'modlist.txt')) -ceq "+Winner`n-Loser") 'import snapshot is stable until explicit refresh'
Check ((Read-Json (Join-Path $source.collection.overwriteRoot 'config/generated.json')).original) 'existing overwrite is preserved in the imported profile'
Put (Join-Path $source.collection.overwriteRoot 'config/generated.json') '{"player":true}'
Put $plan.settingsFile '{"quality":"player"}'
Put "$root/profiles/My collection/modlist.txt" "+Winner`n+New"
Put "$root/mod storage/New/new.txt" 'new'
$null=Import-Mo2Collection $c $root 'My collection' -Refresh
Check ([IO.File]::ReadAllText($plan.settingsFile) -ceq '{"quality":"player"}' -and (Read-Json (Join-Path $source.collection.overwriteRoot 'config/generated.json')).player) 'explicit list refresh preserves player settings and runtime overwrite'
Check ([IO.File]::ReadAllText((Join-Path $plan.profileRoot 'modlist.txt')) -ceq "+Winner`n+New") 'explicit refresh imports the changed list'
Check ([IO.File]::ReadAllText((Join-Path $source.collection.modsRoot 'Winner/r6/scripts/mod.reds')) -ceq 'external later edit' -and (Test-Path (Join-Path $source.collection.modsRoot 'New/new.txt'))) 'explicit refresh updates owned mod files from source'
$beforeHash=(Get-FileHash (Join-Path $c.data 'launch-source.json')).Hash
Put "$root/profiles/My collection/modlist.txt" '+Missing required mod'
$rejected=$false;try{$null=Import-Mo2Collection $c $root 'My collection' -Refresh}catch{$rejected=$true}
Check ($rejected -and (Get-FileHash (Join-Path $c.data 'launch-source.json')).Hash -ceq $beforeHash -and [IO.File]::ReadAllText((Join-Path $plan.profileRoot 'modlist.txt')) -ceq "+Winner`n+New") 'failed import restores the previous source and exact list'
Put "$($c.game)/bin/x64/dxgi.dll" 'selected-luke'
Put "$root/mod storage/Winner/bin/x64/dxgi.dll" 'collection-loader'
Write-Json "$($c.game)/CPVR-Portable/installed.json" @{kind='luke';version='luke-test';critical=@(@{path='bin/x64/dxgi.dll';sha256=(Get-FileHash "$($c.game)/bin/x64/dxgi.dll").Hash})}
$plan=Get-GameLaunchPlan $c -InspectOnly
Assert-GameLaunchComposition $c $plan
Check ((Resolve-GameLaunchEffectiveFile $c $plan 'bin/x64/dxgi.dll') -eq [IO.Path]::GetFullPath("$($c.game)/bin/x64/dxgi.dll")) 'explicitly installed renderer wins over the collection loader without modifying collection files'
Check ([IO.File]::ReadAllText("$root/mod storage/Winner/bin/x64/dxgi.dll") -ceq 'collection-loader') 'renderer binding leaves source collection loader intact'
Put "$root/profiles/My collection/modlist.txt" "+Winner`n+New"
$null=Import-Mo2Collection $c $root 'My collection' -Refresh
Write-Json "$($c.game)/CPVR-Portable/installed.json" @{kind='flat';version='flat';critical=@(@{path='bin/x64/Cyberpunk2077.exe';sha256=(Get-FileHash "$($c.game)/bin/x64/Cyberpunk2077.exe").Hash})}
[IO.File]::Delete("$($c.game)/bin/x64/dxgi.dll")
$flat=Get-GameLaunchPlan $c -InspectOnly
Assert-GameLaunchComposition $c $flat
Check ((Resolve-GameLaunchEffectiveFile $c $flat 'bin/x64/dxgi.dll') -eq [IO.Path]::GetFullPath("$($source.collection.modsRoot)/Winner/bin/x64/dxgi.dll")) 'refresh while in VR still permits returning to original collection loader in flat mode'
Check (@(Get-ChildItem -LiteralPath $c.data -Directory | Where-Object Name -like 'collection-before-*').Count -eq 0) 'successful refresh does not accumulate complete previous collection copies'
$copy=Join-Path $c.data ('collection-before-'+[guid]::NewGuid().ToString('N'));Put "$copy/owned.txt" 'original'
$expected=Get-CollectionCopyInventory $copy;Put "$copy/foreign.txt" 'do not delete'
$rejected=$false;try{Remove-VerifiedCollectionCopy $c.data $copy $expected}catch{$rejected=$true}
Check ($rejected -and [IO.File]::ReadAllText("$copy/owned.txt") -ceq 'original' -and [IO.File]::ReadAllText("$copy/foreign.txt") -ceq 'do not delete') 'verified copy cleanup preserves every file when an unknown file appears'
$copy2=Join-Path $c.data ('collection-before-'+[guid]::NewGuid().ToString('N'));Put "$copy2/owned.txt" 'original'
$expected=Get-CollectionCopyInventory $copy2;Put "$copy2/owned.txt" 'edited'
$rejected=$false;try{Remove-VerifiedCollectionCopy $c.data $copy2 $expected}catch{$rejected=$true}
Check ($rejected -and [IO.File]::ReadAllText("$copy2/owned.txt") -ceq 'edited') 'verified copy cleanup preserves changed bytes'
$rejected=$false;try{Remove-VerifiedCollectionCopy $c.data $root @()}catch{$rejected=$true}
Check ($rejected -and (Test-Path "$root/ModOrganizer.ini")) 'cleanup cannot target original collection outside generated sibling'
Write-Output ('PASS: evidence '+$fixture)
