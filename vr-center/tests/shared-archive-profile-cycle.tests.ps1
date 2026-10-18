param([string]$Runtime=(Join-Path (Split-Path $PSScriptRoot -Parent) 'components/usvfs'),[string]$Dotnet=(Get-Command dotnet -ErrorAction Stop).Source)
$ErrorActionPreference='Stop'
$app=Split-Path $PSScriptRoot -Parent
. (Join-Path $app 'GameLaunch.ps1');. (Join-Path $app 'GameProfileStore.ps1')
# Dotnet and usvfs runtime are explicit inputs or existing PATH/package defaults.
$hostDll=Join-Path $app 'helpers/VfsHost/bin/Release/net8.0/VRPorts.VfsHost.dll'
$consumer=Join-Path $app 'tests/VfsConsumer/bin/Release/net8.0/VfsConsumer.dll'
$fixture=Join-Path $app ('audit/shared-archive-vfs-'+[guid]::NewGuid().ToString('N'))
$c=@{game="$fixture/game";data="$fixture/data";vfs=@{runtime=$Runtime}}
function Put($p,$text){[IO.Directory]::CreateDirectory((Split-Path $p -Parent))|Out-Null;[IO.File]::WriteAllText($p,$text)}
function Check($value,$why){if(-not $value){throw $why};Write-Output ('PASS: '+$why)}
Put "$($c.game)/bin/x64/Cyberpunk2077.exe" 'never executed';Put "$($c.game)/stock.txt" 'stock';Put "$($c.game)/conflict.txt" 'base'
$source="$fixture/common-library";Put "$source/ModOrganizer.ini" "[General]`ngameName=Cyberpunk 2077"
Put "$source/profiles/Main/modlist.txt" "+high`n+low"
Put "$source/mods/high/conflict.txt" 'winner-A';Put "$source/mods/high/settings.txt" 'authored';Put "$source/mods/low/low-only.txt" 'low';Put "$source/overwrite/existing.txt" 'previous-overwrite'
Put "$source/mods/high/archive/pc/mod/shared.archive" 'shared-high';Put "$source/mods/low/archive/pc/mod/shared.archive" 'shared-low'
$null=Import-Mo2Collection $c $source 'Main'
$store=@{data=$c.data;initialName='Original';scopes=@(
 @{id='game';root=$c.game;kind='directory';requiredFiles=@('bin/x64/cyberpunk2077.exe');exclusions=@()},
 @{id='collection';root="$($c.data)/collection";kind='directory';requiredFiles=@();exclusions=@()},
 @{id='source';root="$($c.data)/launch-source.json";kind='file';requiredFiles=@();exclusions=@()}
)}
function Run-Consumer($label,$write){
 $plan=Get-GameLaunchPlan $c -InspectOnly
 $request=New-StandaloneVfsLaunchPlan $c $plan "$fixture/$label.usvfs.log"
 $request.exe=$dotnet;$request.arguments=@($consumer,$c.game,"$fixture/$label.json",$write);$request.workingDirectory=$c.game;$request.timeoutSeconds=30
 Write-Json "$fixture/$label.request.json" $request
 & $dotnet $hostDll "$fixture/$label.request.json" *> "$fixture/$label.host.log"
 if($LASTEXITCODE -ne 0){throw ('Hooked consumer failed: '+$label+'; '+$fixture)}
 return Read-Json "$fixture/$label.json"
}
$a=Run-Consumer 'original' 'written-A'
Check ($a.sharedArchive -ceq 'shared-high' -and $a.mutable -ceq 'authored') 'real usvfs consumer reads shared archive winner and profile-owned settings'
$null=Save-GameProfile -StoreConfig $store
$objectsBefore=@(Get-ChildItem "$($c.data)/profile-store/objects" -File).Count
$copy=Copy-GameProfile -StoreConfig $store -Name 'VR copy'
Check (@(Get-ChildItem "$($c.data)/profile-store/objects" -File).Count -eq $objectsBefore) 'duplicating the real hooked profile creates no new payload objects'
Put "$($c.data)/collection/profiles/Main/modlist.txt" '+low'
# Keep a writable config in overwrite when the original supplying mod is disabled.
Put "$($c.data)/collection/overwrite/settings.txt" 'copy-config'
$b=Run-Consumer 'copy' 'written-B'
Check ($b.sharedArchive -ceq 'shared-low' -and $b.conflict -ceq 'base' -and $b.mutable -ceq 'copy-config') 'duplicate selects its own archive list and independent settings through usvfs'
$null=Activate-GameProfile -StoreConfig $store -Id 'current'
$returned=Run-Consumer 'original-return' 'written-A'
Check ($returned.sharedArchive -ceq 'shared-high' -and $returned.conflict -ceq 'winner-A' -and $returned.mutable -ceq 'written-A') 'original archive priority and runtime-written settings survive editing the duplicate'
Put "$($c.data)/collection/overwrite/archive/pc/mod/shared.archive" 'private-patch'
$patched=Run-Consumer 'private-patch' 'written-A'
Check ($patched.sharedArchive -ceq 'private-patch') 'private override remains higher priority than linked common resource'
Check ([IO.File]::ReadAllText("$source/mods/high/settings.txt") -ceq 'authored' -and [IO.File]::ReadAllText("$source/mods/high/archive/pc/mod/shared.archive") -ceq 'shared-high') 'common library is unchanged by all hooked runs'
$manifest=Read-GameProfileJson "$($c.data)/profile-store/profiles/current.json"
Check (@($manifest.files|Where-Object {$_.path.EndsWith('.archive')}).Count -eq 0) 'shared resource archives never enter profile snapshots or CAS payload'
Write-Output ('PASS: real shared-library evidence '+$fixture)
