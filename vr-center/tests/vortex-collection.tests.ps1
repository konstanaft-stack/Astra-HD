$ErrorActionPreference='Stop'
$app=Split-Path $PSScriptRoot -Parent
. (Join-Path $app 'GameLaunch.ps1')
function Put($p,$text){[IO.Directory]::CreateDirectory((Split-Path $p -Parent))|Out-Null;[IO.File]::WriteAllText($p,$text,[Text.UTF8Encoding]::new($false))}
function Check($x,$message){if(-not $x){throw $message};Write-Output ('PASS: '+$message)}
$f=Join-Path $app ('audit/vortex-collection-'+[guid]::NewGuid().ToString('N'))
$config=@{game="$f/game";data="$f/data";vortexData="$f/vortex";vortexCatalogFile="$f/catalog.json";settingsFile="$f/settings/UserSettings.json"}
Put "$f/game/bin/x64/Cyberpunk2077.exe" 'fixture-not-launched'
Put "$f/settings/UserSettings.json" '{"dlss":"balanced"}'
Put "$f/vortex/cyberpunk2077/mods/mod-one/archive/pc/mod/one.archive" 'shared archive bytes'
Put "$f/vortex/cyberpunk2077/mods/mod-one/r6/scripts/one.reds" 'original script'
Put "$f/game/archive/pc/mod/one.archive" 'physical vortex file'
$files=@(Get-ChildItem -LiteralPath "$f/vortex/cyberpunk2077/mods/mod-one" -File -Recurse|ForEach-Object{@{relative=[IO.Path]::GetRelativePath("$f/vortex/cyberpunk2077/mods/mod-one",$_.FullName).Replace('\','/');source=$_.FullName;size=$_.Length;modId='mod-one'}})
$catalog=@{schema=1;root="$f/vortex";stagingPath="$f/vortex/cyberpunk2077/mods";activeProfileId='stable-id';profiles=@(@{id='stable-id';name='SuperCyberpunkAI';enabledCount=1;active=$true;available=$true;reason='';files=$files},@{id='empty-id';name='Empty';enabledCount=0;active=$false;available=$true;reason='';files=@()})}
Write-Json "$f/catalog.json" $catalog
Write-Json "$f/game/vortex.deployment.json" @{gameId='cyberpunk2077';targetPath="$f/game";stagingPath=$catalog.stagingPath;files=@(@{relPath='archive/pc/mod/one.archive';source='mod-one';target=''})}
$before=(Get-FileHash -LiteralPath "$f/vortex/cyberpunk2077/mods/mod-one/r6/scripts/one.reds").Hash
$list=Get-VortexCollectionCatalog $config
Check ($list.provider -eq 'vortex' -and $list.profiles[0].id -eq 'stable-id' -and -not $list.profiles[0].ContainsKey('files')) 'UI receives friendly profile catalog without exposing full file inventory'
$source=Import-VortexCollection $config 'stable-id'
Check ($source.kind -eq 'vortex' -and $source.profileName -eq 'SuperCyberpunkAI' -and $source.origin.profileId -eq 'stable-id') 'stable Vortex identity and friendly name preserved'
$plan=Get-GameLaunchPlan $config -InspectOnly
Check ($plan.kind -eq 'vortex' -and $plan.engine -eq 'usvfs') 'same autonomous VFS launch engine used for imported Vortex'
$script=Resolve-GameLaunchEffectiveFile $config $plan 'r6/scripts/one.reds'
Check ([IO.File]::ReadAllText($script) -eq 'original script' -and $script -like '*data*collection*') 'scripts resolve from private imported collection'
[IO.File]::WriteAllText($script,'private edit')
Check ((Get-FileHash -LiteralPath "$f/vortex/cyberpunk2077/mods/mod-one/r6/scripts/one.reds").Hash -eq $before) 'editing private script does not change Vortex library'
$links=@(Get-StandaloneSharedArchiveLinks $config $plan)
Check ($links.Count -eq 1 -and (Get-FileHash -LiteralPath $links[0].source).Hash -eq (Get-FileHash -LiteralPath "$f/vortex/cyberpunk2077/mods/mod-one/archive/pc/mod/one.archive").Hash) 'large archive is a shared resource outside private snapshot'
Check (-not(Test-Path -LiteralPath "$f/data/collection/mods/Resolved/archive/pc/mod/one.archive")) 'profile snapshot does not include the archive payload'
Check (Test-Path -LiteralPath "$f/data/collection/mods/Resolved/archive/pc/mod" -PathType Container) 'shared archive parent exists in private merged tree for actual usvfs mapping'
$config.Remove('vortexCatalogFile');$config.vortexData="$f/not-installed";$config.vortex=''
$independent=Get-GameLaunchPlan $config -InspectOnly
Check ($independent.engine -eq 'usvfs' -and @(Get-StandaloneSharedArchiveLinks $config $independent).Count -eq 1) 'after import launcher does not need Vortex executable or database'
$config.vortexCatalogFile="$f/catalog.json";$config.vortexData="$f/vortex"
$empty=Import-VortexCollection $config 'empty-id'
Check (-not(Test-Path -LiteralPath "$f/game/archive/pc/mod/one.archive") -and (Test-Path -LiteralPath "$f/vortex/cyberpunk2077/mods/mod-one/archive/pc/mod/one.archive")) 'excluded physical Vortex mod is parked; source library stays intact'
Restore-VortexCollectionPark $config $empty
Check ([IO.File]::ReadAllText("$f/game/archive/pc/mod/one.archive") -eq 'physical vortex file') 'disconnect restores exact original physical file'
Check ([IO.File]::ReadAllText("$f/settings/UserSettings.json") -eq '{"dlss":"balanced"}') 'collection selection does not change graphics settings'
Write-Output ('Fixture: '+$f)
