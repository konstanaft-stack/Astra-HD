$ErrorActionPreference='Stop'
$app=Split-Path $PSScriptRoot -Parent
$adapter=Join-Path $app 'GameProfiles.ps1'
if(-not(Test-Path -LiteralPath $adapter)){throw 'Profile integration missing: scoped adapter/backend actions are not implemented.'}
. (Join-Path $app 'Core.ps1')
. (Join-Path $app 'Components.ps1')
. $adapter
function Assert-Integration($Value,[string]$Why){if(-not $Value){throw $Why};Write-Output ('PASS: '+$Why)}
function Reject-Integration([scriptblock]$Work,[string]$Why){$failed=$false;try{& $Work|Out-Null}catch{$failed=$true};Assert-Integration $failed $Why}
function Put-Integration([string]$Path,[string]$Text){[IO.Directory]::CreateDirectory((Split-Path $Path -Parent))|Out-Null;[IO.File]::WriteAllText($Path,$Text,[Text.UTF8Encoding]::new($false))}
function Text-Integration([string]$Path){return [IO.File]::ReadAllText($Path)}
# Process queries are replaced only in this disposable fixture, never by a runtime config flag.
$script:fixtureRunning=@()
function Get-Process {param([string[]]$Name,[int]$Id);foreach($n in $script:fixtureRunning){if($n -in $Name){[pscustomobject]@{ProcessName=$n;Id=99001}}}}
$fixture=Join-Path $PSScriptRoot ('fixture-game-profile-integration-'+[guid]::NewGuid().ToString('N'))
$c=@{game="$fixture/game";data="$fixture/data";bundle="$fixture/bundle";hold="$fixture/hold.json";appDataPath="$fixture/appdata";settingsFile="$fixture/appdata/UserSettings.json";userStateFile="$fixture/saved-games/user.gls";vortex="$fixture/no-vortex.exe";vortexData="$fixture/vortex";gameProfilesEnabled=$true}
try{
    Put-Integration "$fixture/stock.exe" 'stock-executable';Put-Integration "$($c.game)/bin/x64/Cyberpunk2077.exe" 'stock-executable'
    $exeHash=(Get-FileHash "$fixture/stock.exe").Hash;$exeSha1=(Get-FileHash "$fixture/stock.exe" -Algorithm SHA1).Hash
    $stock=@{'bin/x64/cyberpunk2077.exe'=@{sha1=$exeSha1;size=16}}
    Put-Integration "$fixture/shared-stock.archive" 'shared-stock'
    $sharedSha1=(Get-FileHash "$fixture/shared-stock.archive" -Algorithm SHA1).Hash
    foreach($i in 1..61){$rel='archive/pc/content/stock-'+$i+'.archive';$stock[$rel]=@{sha1=$sharedSha1;size=12};Put-Integration "$($c.game)/$rel" 'shared-stock'}
    Write-Json "$($c.bundle)/stock-paths.json" $stock
    Put-Integration "$($c.bundle)/files/$exeHash" 'stock-executable';Put-Integration "$($c.bundle)/Start-Game.ps1" '# fixture launcher: never executed'
    $native=@{}
    foreach($rel in @('red4ext/red4ext.dll','red4ext/plugins/cyberpunkvr_stereo/cyberpunkvr_stereo.dll','red4ext/plugins/cyberpunkvr_stereo/usersettings.json')){
        $source="$fixture/native-"+[guid]::NewGuid().ToString('N');Put-Integration $source ('fixture-'+$rel);$h=(Get-FileHash $source).Hash
        Put-Integration "$($c.bundle)/files/$h" (Text-Integration $source);$native[$rel]=$h
    }
    Write-Json "$($c.bundle)/bundle.json" @{schemaVersion=1;exeSha256=$exeHash;versions=@(@{id='flat';kind='flat';label='Flat';available=$true;files=@{'bin/x64/cyberpunk2077.exe'=$exeHash}},@{id='tofu-aer-v1-candidate';kind='native';label='Astra';available=$true;files=$native});managedPaths=@();modules=@{}}
    Write-Json $c.hold @{active=$false}
    Assert-Integration (-not(Test-CyberpunkProfileMode @{}) -and -not(Test-CyberpunkProfileMode @{gameProfilesEnabled='true'}) -and (Test-CyberpunkProfileMode $c)) 'mode is explicit Boolean opt-in and absent flag remains legacy'
    $store=Get-CyberpunkProfileStoreConfig $c;$scope=@{};foreach($s in $store.scopes){$scope[$s.id]=$s}
    Assert-Integration ($store.data -eq [IO.Path]::GetFullPath($c.data) -and $scope.Count -eq 10 -and $scope.'launch-source'.kind -eq 'file' -and $scope.collection.kind -eq 'directory' -and $scope.'collection-cache'.kind -eq 'directory') 'store includes collection data, cache, launch source and separate CAS parent'
    Assert-Integration (@($scope.game.exclusions|Where-Object{$_ -like 'archive/pc/*'}).Count -eq 61 -and 'archive/pc/content' -notin $scope.game.exclusions) 'only exact shared stock paths are excluded'
    Assert-Integration ($scope.'user-state'.kind -eq 'directory' -and $scope.'user-state'.root -eq [IO.Path]::GetFullPath("$fixture/saved-games")) 'the complete Saved Games directory belongs to the selected profile'
    $effective=@{};foreach($key in $c.Keys){$effective[$key]=$c[$key]};$effective.settingsFile="$fixture/mo2/profiles/VR/UserSettings.json";$effective.profileStoreConfig=$c
    $effectiveStore=Get-CyberpunkProfileStoreConfig $effective
    Assert-Integration (@($effectiveStore.scopes|Where-Object id -eq 'appdata')[0].root -eq $scope.appdata.root) 'effective MO2 settings path does not redefine original AppData store scope'
    $initial=Get-CyberpunkGameProfilesState $c
    Assert-Integration ($initial.enabled -and $initial.available -and -not $initial.initialized -and $initial.activeId -eq 'current' -and $initial.profiles[0].name -eq 'Основной') 'status exposes lazy named current profile before materialization'
    Assert-Integration (-not(Test-Path "$($c.data)/profile-store")) 'status does not create catalog, CAS or snapshot'
    Reject-Integration {Invoke-CyberpunkGameProfileAction $c @{action='CreateGameProfile';name='Premature';vr=$false}} 'new profile requires an explicitly captured fresh baseline'
    Write-Json $c.settingsFile @{data=@(@{group_name='/graphics/presets';options=@()})}
    $null=Save-CyberpunkCleanBaseline $c -FirstLaunchConfirmed
    $bad=@{};foreach($k in $c.Keys){$bad[$k]=$c[$k]};$bad.appDataPath='relative-appdata'
    $badState=Get-CyberpunkGameProfilesState $bad
    Assert-Integration ($badState.enabled -and -not $badState.available -and $badState.reason) 'invalid trusted root returns structured unavailable reason without reviving legacy mode'
    Put-Integration "$($c.game)/archive/pc/content/stock-1.archive" 'shared-stock';Put-Integration "$($c.game)/archive/pc/content/user-extra.archive" 'private-extra'
    Put-Integration "$($c.game)/CPVR-Portable-Backups/keep.txt" 'backup-shared';Put-Integration "$($c.game)/CPVR-Candidate-Deltas/keep.txt" 'delta-shared'
    Put-Integration "$($c.game)/mods/manual.txt" 'manual-A';Put-Integration $c.settingsFile '{"owner":"A"}';Put-Integration "$($c.appDataPath)/modsettings.json" '{"mod":"A"}'
    Put-Integration "$($c.appDataPath)/cache/cache.dat" 'cache-A';Put-Integration $c.userStateFile 'state-A';Put-Integration "$fixture/saved-games/ManualSave-1/sav.dat" 'progress-preserved'
    Put-Integration "$($c.data)/game-settings/test/custom.json" 'center-A';Put-Integration "$($c.data)/game-settings/test/transactions/keep.txt" 'transaction-backup'
    Put-Integration "$($c.data)/mods-state.json" '{"enabled":false,"files":[]}';Put-Integration "$($c.data)/keybindings.json" '{"ports":{}}';Put-Integration "$($c.data)/usb-recovery.json" '{"enabled":false}'
    Put-Integration "$($c.data)/launch-source.json" '{"schema":1,"kind":"mo2","profile":"Saved external collection","executable":"Cyberpunk2077"}'
    Put-Integration "$($c.data)/collection/profiles/A/modlist.txt" '+collection-A';Put-Integration "$($c.data)/collection/overwrite/mod-config.json" '{"owner":"A"}';Put-Integration "$($c.data)/collection-cache/state.json" '{"cache":"A"}'
    $b=Invoke-CyberpunkGameProfileAction $c @{action='CreateGameProfile';name='B';vr=$false}
    $bCreated=@($b.profiles|Where-Object id -eq $b.activeId)[0].createdUtc
    Assert-Integration (-not(Test-Path "$($c.data)/launch-source.json")) 'clean profile defaults to direct route without retaining a previous collection descriptor'
    Assert-Integration (-not(Test-Path "$($c.data)/collection/profiles/A/modlist.txt") -and -not(Test-Path "$($c.data)/collection/overwrite/mod-config.json") -and -not(Test-Path "$($c.data)/collection-cache/state.json")) 'new clean profile removes previous collection contents and cache identity'
    Assert-Integration ($b.activeId -ne 'current' -and $b.profiles.Count -eq 2 -and $b.initialized) 'create autosaves current under same ID and creates one clean profile'
    Assert-Integration (-not(Test-Path "$($c.game)/mods/manual.txt") -and -not(Test-Path "$($c.appDataPath)/modsettings.json") -and -not(Test-Path "$($c.appDataPath)/cache/cache.dat") -and -not(Test-Path $c.userStateFile)) 'clean profile removes old manual mods, AppData and user-state settings'
    Assert-Integration (-not(Test-Path "$fixture/saved-games/ManualSave-1/sav.dat") -and (Text-Integration "$($c.game)/archive/pc/content/stock-1.archive") -eq 'shared-stock') 'new profile has no previous saves and retains the shared stock archives'
    Assert-Integration (-not(Test-Path "$($c.game)/archive/pc/content/user-extra.archive") -and (Test-Path "$($c.game)/CPVR-Portable-Backups/keep.txt") -and (Test-Path "$($c.data)/game-settings/test/transactions/keep.txt")) 'unknown content is private while registered backup paths remain shared'
    Put-Integration "$($c.game)/mods/manual-B.txt" 'manual-B';Put-Integration "$($c.appDataPath)/modsettings.json" 'settings-B'
    Put-Integration "$fixture/saved-games/ManualSave-1/sav.dat" 'different-progress-B'
    Put-Integration "$fixture/saved-games/AutoSave-9/metadata.9.json" '{"owner":"B"}'
    Put-Integration $c.userStateFile 'state-B'
    Put-Integration "$($c.data)/collection/overwrite/mod-config.json" '{"owner":"B"}';Put-Integration "$($c.data)/collection-cache/state.json" '{"cache":"B"}'
    $catalogBefore=(Get-FileHash "$($c.data)/profile-store/catalog.json").Hash
    Put-Integration "$($c.game)/archive/pc/content/stock-1.archive" 'bad-shared!!'
    $sharedRejected=$false
    try{$null=Invoke-CyberpunkGameProfileAction $c @{action='SwitchGameProfile';profileId='current'}}catch{if($_.Exception.Message -notmatch 'shared stock'){throw};$sharedRejected=$true}
    Assert-Integration $sharedRejected 'switch rejects changed shared originals before applying another profile'
    Assert-Integration ((Get-FileHash "$($c.data)/profile-store/catalog.json").Hash -eq $catalogBefore -and (Text-Integration "$($c.appDataPath)/modsettings.json") -eq 'settings-B') 'shared-base failure leaves active catalog and settings unchanged'
    Put-Integration "$($c.game)/archive/pc/content/stock-1.archive" 'shared-stock'
    $a=Invoke-CyberpunkGameProfileAction $c @{action='SwitchGameProfile';profileId='current'}
    Assert-Integration ((Text-Integration "$fixture/saved-games/ManualSave-1/sav.dat") -eq 'progress-preserved' -and -not(Test-Path "$fixture/saved-games/AutoSave-9/metadata.9.json")) 'returning to A restores its save bytes and removes saves created only in B'
    Assert-Integration ((Read-Json "$($c.data)/launch-source.json").kind -eq 'mo2') 'outgoing MO2 route is autosaved and restored from catalog independently of clean direct route'
    Assert-Integration ((Text-Integration "$($c.data)/collection/profiles/A/modlist.txt") -eq '+collection-A' -and (Text-Integration "$($c.data)/collection/overwrite/mod-config.json") -eq '{"owner":"A"}' -and (Text-Integration "$($c.data)/collection-cache/state.json") -eq '{"cache":"A"}') 'A restores exact collection list, writable files and cache identity'
    Assert-Integration ($a.activeId -eq 'current' -and (Text-Integration "$($c.game)/mods/manual.txt") -eq 'manual-A' -and (Text-Integration "$($c.appDataPath)/modsettings.json") -eq '{"mod":"A"}' -and (Text-Integration $c.userStateFile) -eq 'state-A') 'switch restores A across game, AppData, user.gls and center state'
    $back=Invoke-CyberpunkGameProfileAction $c @{action='SwitchGameProfile';profileId=$b.activeId}
    Assert-Integration ((Text-Integration "$fixture/saved-games/ManualSave-1/sav.dat") -eq 'different-progress-B' -and (Text-Integration "$fixture/saved-games/AutoSave-9/metadata.9.json") -eq '{"owner":"B"}' -and (Text-Integration $c.userStateFile) -eq 'state-B') 'returning to B restores its own same-name save, metadata and preferences'
    Assert-Integration (@($back.profiles|Where-Object id -eq $b.activeId)[0].createdUtc -ceq $bCreated) 'switch and outgoing autosave preserve original createdUtc'
    Assert-Integration (-not(Test-Path "$($c.data)/collection/profiles/A/modlist.txt") -and (Text-Integration "$($c.data)/collection/overwrite/mod-config.json") -eq '{"owner":"B"}' -and (Text-Integration "$($c.data)/collection-cache/state.json") -eq '{"cache":"B"}') 'B restores its own collection files and removes A-only list'
    Assert-Integration ($back.profiles.Count -eq 2 -and (Text-Integration "$($c.game)/mods/manual-B.txt") -eq 'manual-B' -and (Text-Integration "$($c.appDataPath)/modsettings.json") -eq 'settings-B') 'A to B to A autosaves without duplicate profile IDs'
    $vr=Invoke-CyberpunkGameProfileAction $c @{action='CreateGameProfile';name='VR';vr=$true}
    Assert-Integration ((Test-Path "$($c.game)/red4ext/plugins/cyberpunkvr_stereo/cyberpunkvr_stereo.dll") -and @($vr.profiles|Where-Object id -eq $vr.activeId)[0].vr) 'VR creation uses separate clean-vr template'
    $removed=Invoke-CyberpunkGameProfileAction $c @{action='DeleteGameProfile';profileId=$b.activeId}
    Assert-Integration ($removed.profiles.Count -eq 2 -and $removed.activeId -eq $vr.activeId) 'inactive delete retains active and other profiles'
    Reject-Integration {Invoke-CyberpunkGameProfileAction $c @{action='DeleteGameProfile';profileId=$vr.activeId}} 'active delete is rejected'
    foreach($name in @('Cyberpunk2077','Vortex','ModOrganizer','Wabbajack')){
        $script:fixtureRunning=@($name)
        Reject-Integration {Invoke-CyberpunkGameProfileAction $c @{action='CreateGameProfile';name='blocked';vr=$false}} ('profile mutation blocks running '+$name)
        Reject-Integration {Assert-GameWritable $c} ('component mutation blocks running '+$name)
    }
    $script:fixtureRunning=@();Write-Json $c.hold @{active=$true}
    Reject-Integration {Invoke-CyberpunkGameProfileAction $c @{action='SwitchGameProfile';profileId='current'}} 'profile switch respects first-launch hold'
    Write-Json $c.hold @{active=$false}
    $repair=Invoke-CyberpunkGameProfileAction $c @{action='RepairGameProfiles'}
    Assert-Integration ($repair.activeId -eq $vr.activeId -and -not $repair.recoveryRequired.Count) 'repair with no pending journals does not snapshot or switch'
    Put-Integration "$($c.game)/archive/pc/mod/manual.archive" 'user-mod';Put-Integration "$($c.game)/r6/scripts/manual.reds" 'user-script'
    Write-Json "$($c.data)/mods-state.json" @{enabled=$false;files=@(@{path='archive/pc/mod/stale.archive';source="$fixture/stale.archive";sha256=('0'*64)})}
    $parkHash=(Get-FileHash "$($c.data)/mods-state.json").Hash
    Ensure-LaunchModProfile $c (Get-PortState $c)
    Set-LibrariesAndMods $c @{mods=$false;libraries=@{}}
    Set-LibrariesAndMods $c @{mods=$true;libraries=@{}}
    Assert-Integration ((Text-Integration "$($c.game)/archive/pc/mod/manual.archive") -eq 'user-mod' -and (Text-Integration "$($c.game)/r6/scripts/manual.reds") -eq 'user-script' -and (Get-FileHash "$($c.data)/mods-state.json").Hash -eq $parkHash -and -not(Test-Path "$($c.data)/mod-files")) 'profile mode bypasses all legacy parking, restore and mods-state writes'
    Put-Integration "$fixture/new-library.dll" 'new-library';$libHash=(Get-FileHash "$fixture/new-library.dll").Hash;Put-Integration "$($c.bundle)/files/$libHash" 'new-library'
    $bundle=Read-Json "$($c.bundle)/bundle.json";$bundle.versions[1].files['bin/x64/nvngx_dlss.dll']=$libHash;Write-Json "$($c.bundle)/bundle.json" $bundle
    Set-LibrariesAndMods $c @{mods=$false;libraries=@{'nvngx_dlss.dll'=$libHash}}
    Assert-Integration ((Get-FileHash "$($c.game)/bin/x64/nvngx_dlss.dll").Hash -eq $libHash -and (Get-FileHash "$($c.data)/mods-state.json").Hash -eq $parkHash) 'library SHA/receipt application remains available without mod parking'
    $journalId=[guid]::NewGuid().ToString('N');$journalRoot="$($c.data)/profile-store/journals/$journalId"
    $manifest=Read-Json "$($c.data)/profile-store/profiles/$($vr.activeId).json"
    $manualHash=(Get-FileHash "$($c.game)/archive/pc/mod/manual.archive").Hash
    $null=Import-GameProfileObject -StoreConfig $store -SourcePath ([IO.Path]::GetFullPath("$($c.game)/archive/pc/mod/manual.archive")) -ExpectedSha256 $manualHash
    $targetManifest=$manifest|ConvertTo-Json -Depth 25|ConvertFrom-Json -AsHashtable
    $manualEntry=@{scope='game';path='archive/pc/mod/manual.archive';sha256=$manualHash;size=8};$targetManifest.files+=,$manualEntry
    Write-Json "$journalRoot/receipt.json" @{schema=1;id=$journalId;state='prepared';cleanupComplete=$false;newProfile=$false;oldActiveId=$vr.activeId;targetId=$vr.activeId}
    Write-Json "$journalRoot/before.json" $manifest;Write-Json "$journalRoot/target.json" $targetManifest
    Write-GameProfileJson "$journalRoot/plan.json" @(@{index=0;scope='game';path=$manualEntry.path;before=$null;after=$manualEntry;phase='installed';suffix=('.profile-'+$journalId+'-0')})
    Put-Integration "$($c.game)/CPVR-Portable/installed.json" '{"truncated":'
    $pending=Get-CyberpunkGameProfilesState $c
    Assert-Integration ($pending.enabled -and $pending.available -and $journalId -in $pending.recoveryRequired) 'malformed optional port receipt does not hide pending recovery'
    Reject-Integration {Assert-GameWritable $c} 'pending journal blocks component changes until recovery'
    $recoveryError='';try{Invoke-CyberpunkGameProfileAction $c @{action='RepairGameProfiles'}|Out-Null}catch{$recoveryError=$_.Exception.Message}
    Assert-Integration ($recoveryError -like '*Recovery preserved foreign data*' -and (Text-Integration "$($c.game)/CPVR-Portable/installed.json") -eq '{"truncated":' -and (Text-Integration "$($c.game)/archive/pc/mod/manual.archive") -eq 'user-mod') ('repair reaches safe kernel conflict and preserves foreign bytes with malformed port metadata; result='+$recoveryError)
    $backend=[IO.File]::ReadAllText((Join-Path $app 'Backend.ps1'))
    foreach($name in @('CreateGameProfile','SwitchGameProfile','DeleteGameProfile','RepairGameProfiles')){Assert-Integration ($backend.Contains("'$name'")) ('backend routes '+$name)}
    Assert-Integration ($backend.Contains('Get-CyberpunkGameProfilesState') -and $backend.Contains('gameProfiles=') -and $backend.Contains('Local.VRPorts.Apply')) 'backend exposes refreshed profile state and recovery on failure under existing mutex'
    Assert-Integration ($backend.Contains('Test-CyberpunkProfileMode') -and $backend.Contains('$install.clean=$false')) 'port reinstall in profile mode does not inherit legacy clean parking'
    # Exercise the actual dispatcher in a wholly synthetic app. Port inspection is
    # deliberately impossible until repair has run; no production config is read.
    $dispatcher="$fixture/dispatcher";Put-Integration "$dispatcher/Backend.ps1" $backend
    foreach($module in @('Components','Updates','LibraryActions','KeyBindings','SettingsProfiles','GraphicsSettings','UsbRecovery','LauncherProcess','GameLaunch')){Put-Integration "$dispatcher/$module.ps1" ''}
    Put-Integration "$dispatcher/GameLaunch.ps1" 'function Get-GameLaunchEffectiveConfig($Config){return $Config};function Sync-GameLaunchRendererOverride($Config){return @{changed=$false}};function Get-GameLaunchSource($Config){return @{kind="direct"}}'
    Put-Integration "$dispatcher/Core.ps1" @'
function Read-Json($Path){if(Test-Path -LiteralPath $Path){Get-Content -LiteralPath $Path -Raw|ConvertFrom-Json -AsHashtable}}
function Write-Json($Path,$Value){[IO.Directory]::CreateDirectory((Split-Path $Path -Parent))|Out-Null;$Value|ConvertTo-Json -Depth 40|Set-Content -LiteralPath $Path}
function Test-CyberpunkProfileMode($Config){$Config.gameProfilesEnabled -eq $true}
function Get-PortState($Config){if(-not(Test-Path "$($Config.data)/repair-ran")){throw 'partial deployment prevents port scan'};@{receipt=@{};versions=@()}}
function Save-PortInstallTime($Config,$Receipt){}
function Get-LibraryCatalog($Config){@()}
function Get-ModProfileState($Config,$State){@{enabled=$true;active=@();parked=@()}}
function Get-RecommendedSettingsMode($Config,$State){$false}
function Get-UsbRecoveryState($Config){@{enabled=$false}}
function Get-CachedPortUpdates($Config){@()}
function Read-OptiMenuKey($Path){$null}
'@
    Put-Integration "$dispatcher/GameProfiles.ps1" @'
function Get-CyberpunkGameProfilesState($Config){@{enabled=$true;available=$true;initialized=$true;activeId='current';profiles=@();recoveryRequired=@('fixture-journal')}}
function Get-CyberpunkBaselineSummary($Config){@{captured=$true}}
function Invoke-CyberpunkGameProfileAction($Config,$Request){[IO.File]::WriteAllText("$($Config.data)/repair-ran",'called');if($Config.fixtureFail){throw 'fixture pending repair'};Get-CyberpunkGameProfilesState $Config}
'@
    $dc=@{data="$dispatcher/data";game="$dispatcher/game";bundle="$dispatcher/bundle";gameProfilesEnabled=$true;fixtureFail=$false}
    [IO.Directory]::CreateDirectory($dc.data)|Out-Null
    Write-Json "$dispatcher/config.json" $dc;Write-Json "$dispatcher/request.json" @{action='RepairGameProfiles'}
    & (Join-Path $PSHOME 'pwsh.exe') -NoProfile -File "$dispatcher/Backend.ps1" -Request "$dispatcher/request.json" -Result "$dispatcher/result.json" *> "$dispatcher/output.txt"
    $answer=Read-Json "$dispatcher/result.json"
    Assert-Integration ($LASTEXITCODE -eq 0 -and $answer.ok -and $answer.state.gameProfiles.enabled -and (Test-Path "$($dc.data)/repair-ran")) 'repair dispatcher runs before full port scan and refreshes state after success'
    $dc.fixtureFail=$true;Write-Json "$dispatcher/config.json" $dc
    & (Join-Path $PSHOME 'pwsh.exe') -NoProfile -File "$dispatcher/Backend.ps1" -Request "$dispatcher/request.json" -Result "$dispatcher/result.json" *> "$dispatcher/output.txt"
    $answer=Read-Json "$dispatcher/result.json"
    Assert-Integration ($LASTEXITCODE -eq 1 -and -not $answer.ok -and $answer.error -eq 'fixture pending repair' -and $answer.gameProfiles.enabled -and $answer.gameProfiles.recoveryRequired.Count -eq 1) 'failed profile action returns top-level recovery state for UI'
    $dc.data="$dispatcher/status-data";[IO.Directory]::CreateDirectory($dc.data)|Out-Null;Write-Json "$dispatcher/config.json" $dc;Write-Json "$dispatcher/request.json" @{action='Status'}
    & (Join-Path $PSHOME 'pwsh.exe') -NoProfile -File "$dispatcher/Backend.ps1" -Request "$dispatcher/request.json" -Result "$dispatcher/result.json" *> "$dispatcher/output.txt"
    $answer=Read-Json "$dispatcher/result.json"
    Assert-Integration ($LASTEXITCODE -eq 1 -and -not $answer.ok -and $answer.error -eq 'partial deployment prevents port scan' -and $answer.gameProfiles.enabled -and $answer.gameProfiles.recoveryRequired.Count -eq 1 -and -not(Test-Path "$($dc.data)/repair-ran")) 'startup Status failure exposes recovery independently of port scan'
    Write-Output 'PASS: full profile adapter lifecycle, lazy state, scoped roots, progress, guards, parking bypass and backend wiring.'
}finally{
    $script:fixtureRunning=@()
    $absolute=[IO.Path]::GetFullPath($fixture)
    if(-not $absolute.StartsWith([IO.Path]::GetFullPath($PSScriptRoot).TrimEnd('\')+'\fixture-game-profile-integration-',[StringComparison]::OrdinalIgnoreCase)){throw 'Unsafe integration fixture cleanup'}
    if(Test-Path -LiteralPath $absolute){Remove-Item -LiteralPath $absolute -Recurse -Force}
}
