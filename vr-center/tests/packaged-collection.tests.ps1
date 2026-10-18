param([string]$Release='3.1.0-collections-candidate-2',[switch]$PrepareForUi)
$ErrorActionPreference='Stop'
$app=Split-Path $PSScriptRoot -Parent
$package=Join-Path $app ('releases/'+$Release)
$pwsh=(Get-Command pwsh -ErrorAction Stop).Source
$fixture=Join-Path $app ('audit/packaged-collection-'+[guid]::NewGuid().ToString('N'))
function Put($p,$text){[IO.Directory]::CreateDirectory((Split-Path $p -Parent))|Out-Null;[IO.File]::WriteAllText($p,$text)}
function Json($p,$obj){Put $p ($obj|ConvertTo-Json -Depth 30)}
function Check($value,$why){if(-not $value){throw $why};Write-Output ('PASS: '+$why)}
$c=@{game="$fixture/game";data="$fixture/data";bundle="$fixture/bundle";hold="$fixture/hold.json";settingsFile="$fixture/appdata/UserSettings.json";appDataPath="$fixture/appdata";userStateFile="$fixture/saves/user.gls";vortex="$fixture/no-vortex.exe";vortexData="$fixture/vortex";gameProfilesEnabled=$false;pwsh=$pwsh}
Put "$($c.game)/bin/x64/Cyberpunk2077.exe" 'fixture'
Json "$($c.bundle)/bundle.json" @{schemaVersion=1;versions=@(@{id='flat';kind='flat';label='Плоская игра';available=$true;files=@{}});modules=@{};managedPaths=@()}
Json $c.hold @{active=$false}
Json "$fixture/config.json" $c
$mo="$fixture/MO2 collection"
Put "$mo/ModOrganizer.ini" "[General]`ngameName=Cyberpunk 2077"
Put "$mo/profiles/Collection/modlist.txt" '+mod'
Put "$mo/mods/mod/mod.txt" 'enabled'
function Request($body){
    Json "$fixture/request.json" $body
    & $pwsh -NoProfile -File "$package/Backend.ps1" -Request "$fixture/request.json" -Result "$fixture/result.json" -ConfigPath "$fixture/config.json" *> "$fixture/backend-output.log"
    $answer=Get-Content -LiteralPath "$fixture/result.json" -Raw|ConvertFrom-Json -AsHashtable
    if($LASTEXITCODE -ne 0 -or -not $answer.ok){throw ($body.action+': '+$answer.error+'; '+$fixture)}
    return $answer
}
$answer=Request @{action='InspectCollection';root=$mo}
Check ($answer.state.collectionCatalog.profiles[0] -ceq 'Collection') 'packaged backend reads collection with explicit external config path'
$answer=Request @{action='ImportGameCollection';root=$mo;profile='Collection';redmod=$false}
Check ($answer.state.collection.kind -ceq 'mo2' -and $answer.state.collection.profile -ceq 'Collection') 'packaged backend imports and returns actual selected source'
$answer=Request @{action='Status'}
Check ($answer.state.collection.kind -ceq 'mo2') 'selected source persists across backend restarts'
$answer=Request @{action='UseDirectCollection'}
Check ($answer.state.collection.kind -ceq 'direct' -and [IO.File]::ReadAllText("$mo/mods/mod/mod.txt") -ceq 'enabled') 'direct-source button disconnects without deleting collection'
Json $c.settingsFile @{data=@(@{group_name='/graphics/presets';options=@(@{name='FrameGeneration';type='string_list';is_dynamic=$true;value='Off';index=0},@{name='DLSSFrameGen';type='bool';value=$false},@{name='DLSS';type='string_list';is_dynamic=$true;value='Balanced';index=3},@{name='TextureQuality';type='string_list';value='High';index=2;values=@('Low','Medium','High')})},@{group_name='/graphics/raytracing';options=@(@{name='RayTracing';type='bool';value=$true})});untouched='keep'}
$answer=Request @{action='ReadGraphicsSettings'}
Check ($answer.state.graphics.available) 'packaged backend reads supported graphics settings'
$answer=Request @{action='ApplyGraphicsSettings';hash=$answer.state.graphics.hash;values=@{DLSS='Quality';RayTracing=$false}}
$saved=Get-Content -LiteralPath $c.settingsFile -Raw|ConvertFrom-Json
Check (@($saved.data[0].options|Where-Object name -eq 'DLSS')[0].value -ceq 'Quality' -and @($saved.data[0].options|Where-Object name -eq 'DLSS')[0].index -eq 2 -and -not $saved.data[1].options[0].value -and $saved.untouched -ceq 'keep') 'packaged graphics action saves matched DLSS value/index and preserves unrelated fields'
Check ((Test-Path "$package/helpers/vfs/VRPorts.VfsHost.exe") -and (Test-Path "$package/components/usvfs/usvfs_x64.dll") -and -not(Test-Path "$package/ModOrganizer.exe")) 'release includes independent helper and usvfs, not ModOrganizer'
Put "$($c.game)/archive/pc/content/base.archive" 'shared'
$stock=@{}
foreach($file in Get-ChildItem -LiteralPath $c.game -Recurse -File){$stock[[IO.Path]::GetRelativePath($c.game,$file.FullName).Replace('\','/')]=@{sha1=(Get-FileHash -LiteralPath $file.FullName -Algorithm SHA1).Hash;size=[long]$file.Length}}
Json "$($c.bundle)/stock-paths.json" $stock
if($PrepareForUi){Write-Output ('PASS: capture UI fixture '+$fixture);exit 0}
$answer=Request @{action='CaptureCleanBaseline';firstLaunchConfirmed=$true}
Check ($answer.state.cleanBaseline.captured -and $answer.state.gameProfiles.enabled) 'packaged capture action enables full profiles after saving explicit clean baseline'
Put "$($c.game)/mods/a.txt" 'profile-A';Put "$fixture/saves/ManualSave-1/sav.dat" 'A-progress'
$answer=Request @{action='CreateGameProfile';name='Clean B';vr=$false}
Check ($answer.state.gameProfiles.activeId -ne 'current' -and -not(Test-Path "$($c.game)/mods/a.txt") -and -not(Test-Path "$fixture/saves/ManualSave-1/sav.dat")) 'packaged create applies exact clean base and removes old mods and progress'
$answer=Request @{action='SwitchGameProfile';profileId='current'}
Check ([IO.File]::ReadAllText("$($c.game)/mods/a.txt") -ceq 'profile-A' -and [IO.File]::ReadAllText("$fixture/saves/ManualSave-1/sav.dat") -ceq 'A-progress') 'packaged return restores original profile files and progress'
Write-Output ('PASS: packaged collection evidence '+$fixture)
