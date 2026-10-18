param([string]$Runtime=(Join-Path (Split-Path $PSScriptRoot -Parent) 'components/usvfs'),[string]$Dotnet=(Get-Command dotnet -ErrorAction Stop).Source)
$ErrorActionPreference='Stop'
$app=Split-Path $PSScriptRoot -Parent
. (Join-Path $app 'GameProfiles.ps1');. (Join-Path $app 'GameLaunch.ps1')
# Dotnet and usvfs runtime are explicit inputs or existing PATH/package defaults.
# Runtime comes from the parameter above.
$hostDll=Join-Path $app 'helpers/VfsHost/bin/Release/net8.0/VRPorts.VfsHost.dll'
$consumer=Join-Path $app 'tests/VfsConsumer/bin/Release/net8.0/VfsConsumer.dll'
$fixture=Join-Path $app ('audit/full-profile-vfs-'+[guid]::NewGuid().ToString('N'))
$c=@{game="$fixture/game";data="$fixture/data";bundle="$fixture/bundle";appDataPath="$fixture/appdata";settingsFile="$fixture/appdata/UserSettings.json";userStateFile="$fixture/saves/user.gls";hold="$fixture/hold.json";gameProfilesEnabled=$true;vfs=@{runtime=$runtime}}
function Put($p,$text){[IO.Directory]::CreateDirectory((Split-Path $p -Parent))|Out-Null;[IO.File]::WriteAllText($p,$text)}
function Check($value,$why){if(-not $value){throw $why};Write-Output ('PASS: '+$why)}
Put "$($c.game)/bin/x64/Cyberpunk2077.exe" 'not executed';Put "$($c.game)/stock.txt" 'stock';Put "$($c.game)/conflict.txt" 'base';Put "$($c.game)/archive/pc/content/base.archive" 'shared'
$stock=@{};foreach($file in Get-ChildItem -LiteralPath $c.game -Recurse -File){$stock[[IO.Path]::GetRelativePath($c.game,$file.FullName).Replace('\','/')]=@{sha1=(Get-FileHash -LiteralPath $file.FullName -Algorithm SHA1).Hash;size=[long]$file.Length}}
Write-Json "$($c.bundle)/stock-paths.json" $stock;Write-Json $c.hold @{active=$false}
Write-Json $c.settingsFile @{data=@(@{group_name='/graphics/presets';options=@()})}
$null=Save-CyberpunkCleanBaseline $c -FirstLaunchConfirmed
$source="$fixture/original-collection";Put "$source/ModOrganizer.ini" "[General]`ngameName=Cyberpunk 2077"
Put "$source/profiles/Main/modlist.txt" "+high`n+low"
Put "$source/mods/high/conflict.txt" 'winner-A';Put "$source/mods/high/settings.txt" 'authored';Put "$source/mods/low/low-only.txt" 'low';Put "$source/overwrite/existing.txt" 'previous-overwrite'
Put "$source/mods/high/archive/pc/mod/shared.archive" 'shared-high';Put "$source/mods/low/archive/pc/mod/shared.archive" 'shared-low'
$null=Import-Mo2Collection $c $source 'Main'
$before=@(Get-Process ModOrganizer,ModOrganizer2 -ErrorAction SilentlyContinue|ForEach-Object Id)
function Run-Consumer($label,$write){
 $plan=Get-GameLaunchPlan $c -InspectOnly
 $request=New-StandaloneVfsLaunchPlan $c $plan "$fixture/$label.usvfs.log"
 $request.exe=$dotnet;$request.arguments=@($consumer,$c.game,"$fixture/$label.json",$write);$request.workingDirectory=$c.game;$request.timeoutSeconds=30
 Write-Json "$fixture/$label.request.json" $request
 & $dotnet $hostDll "$fixture/$label.request.json" *> "$fixture/$label.host.log"
 if($LASTEXITCODE -ne 0){throw ('Hooked consumer failed: '+$label+'; '+$fixture)}
 return Read-Json "$fixture/$label.json"
}
$a=Run-Consumer 'a-first' 'written-A'
Check ($a.sharedArchive -ceq 'shared-high' -and -not(Test-Path "$($c.data)/collection/mods/high/archive/pc/mod/shared.archive")) 'real usvfs launch sees the highest priority shared archive without a private archive copy'
Check ($a.conflict -ceq 'winner-A' -and $a.mutable -ceq 'authored') 'actual hooked launch reads imported A and writes existing config'
Put "$fixture/saves/ManualSave-1/sav.dat" 'save-A'
$b=Invoke-CyberpunkGameProfileAction $c @{action='CreateGameProfile';name='B';vr=$false}
Check (-not(Test-Path "$($c.data)/collection/mods/high/settings.txt") -and -not(Test-Path "$fixture/saves/ManualSave-1/sav.dat")) 'new clean profile removes A collection and progress'
$null=Import-Mo2Collection $c $source 'Main';Put "$($c.data)/collection/mods/high/conflict.txt" 'winner-B';Put "$fixture/saves/ManualSave-1/sav.dat" 'save-B'
$bRun=Run-Consumer 'b-first' 'written-B'
Check ($bRun.conflict -ceq 'winner-B' -and $bRun.mutable -ceq 'authored') 'actual hooked launch of B starts with its own untouched settings'
$null=Invoke-CyberpunkGameProfileAction $c @{action='SwitchGameProfile';profileId='current'}
$aReturn=Run-Consumer 'a-return' 'written-A'
Check ($aReturn.conflict -ceq 'winner-A' -and $aReturn.mutable -ceq 'written-A' -and [IO.File]::ReadAllText("$fixture/saves/ManualSave-1/sav.dat") -ceq 'save-A') 'return to A restores runtime-written config, winner and save before hooked launch'
$null=Invoke-CyberpunkGameProfileAction $c @{action='SwitchGameProfile';profileId=$b.activeId}
$bReturn=Run-Consumer 'b-return' 'written-B'
Check ($bReturn.conflict -ceq 'winner-B' -and $bReturn.mutable -ceq 'written-B' -and [IO.File]::ReadAllText("$fixture/saves/ManualSave-1/sav.dat") -ceq 'save-B') 'return to B restores its runtime-written config, winner and same-name save'
$null=Save-GameProfile -StoreConfig (Get-CyberpunkProfileStoreConfig $c)
$objectsBefore=@(Get-ChildItem "$($c.data)/profile-store/objects" -File).Count
$copy=Invoke-CyberpunkGameProfileAction $c @{action='DuplicateGameProfile';name='B VR copy'}
Check ($copy.activeId -ne $b.activeId -and @(Get-ChildItem "$($c.data)/profile-store/objects" -File).Count -eq $objectsBefore) 'duplicate activates an independent profile referencing the same CAS objects with no repeated payload'
Put "$($c.data)/collection/profiles/Main/modlist.txt" '+high';Put "$fixture/saves/ManualSave-1/sav.dat" 'save-C'
$copyRun=Run-Consumer 'copy-edited' 'written-C'
Check ($null -eq $copyRun.low -and $copyRun.sharedArchive -ceq 'shared-high' -and $copyRun.mutable -ceq 'written-B') 'copy can disable a mod and retain its own writable settings while sharing resource archives'
$null=Invoke-CyberpunkGameProfileAction $c @{action='SwitchGameProfile';profileId=$b.activeId}
$bAfterCopy=Run-Consumer 'b-after-copy' 'written-B'
Check ($bAfterCopy.low -ceq 'low' -and $bAfterCopy.mutable -ceq 'written-B' -and [IO.File]::ReadAllText("$fixture/saves/ManualSave-1/sav.dat") -ceq 'save-B') 'original profile keeps enabled mods, settings and saves after editing its duplicate'
$after=@(Get-Process ModOrganizer,ModOrganizer2 -ErrorAction SilentlyContinue|ForEach-Object Id)
Check ([IO.File]::ReadAllText("$source/mods/high/settings.txt") -ceq 'authored' -and [IO.File]::ReadAllText("$($c.game)/conflict.txt") -ceq 'base' -and ($before -join ',') -ceq ($after -join ',')) 'original collection/base stay unchanged and no ModOrganizer process starts'
Write-Output ('PASS: full native consumer cycle evidence '+$fixture)
