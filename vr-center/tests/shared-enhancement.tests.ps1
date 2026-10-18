$ErrorActionPreference='Stop'
# Real process guard precedes all fixture work; no game/device/vendor invocation.
if(Microsoft.PowerShell.Management\Get-Process Cyberpunk2077 -ErrorAction SilentlyContinue){throw 'Physical game live; CPU fixture deferred.'}
$app=Split-Path $PSScriptRoot -Parent
. "$app/EnhancementPackage.ps1"
. "$app/GameProfileStore.ps1"
. "$app/GameProfiles.ps1"
. "$app/GameLaunch.ps1"
function Put($path,$text){[IO.Directory]::CreateDirectory((Split-Path $path -Parent))|Out-Null;[IO.File]::WriteAllText($path,$text)}
function Check($value,$why){if(-not $value){throw $why};Write-Output ('PASS: '+$why)}
function Hash($path){(Get-FileHash -LiteralPath $path).Hash}
$f=Join-Path ([IO.Path]::GetTempPath()) ('shared-enhancement-'+[guid]::NewGuid().ToString('N'))
$c=@{game="$f/game";data="$f/data";hold="$f/hold.json";bundle="$f/bundle";imagePackage="$f/package.json"}
try{
    Write-Json $c.hold @{active=$false}
    Put "$f/game/bin/x64/Cyberpunk2077.exe" 'not-an-executable'
    Put "$f/game/bin/x64/dbghelp.dll" 'Microsoft-original'
    $userIni="[Menu]`nShortcutKey=45`n[DlssNr]`nEnabled=false`nUserPreset=keep"
    Put "$f/game/bin/x64/OptiScaler.ini" $userIni
    Put "$f/game/noncommon.txt" 'A'
    $files=@(foreach($name in @('dbghelp.dll','OptiScaler.ini','nvngx_dlssnr.dll','OptiScaler/streamline/nvngx_dlssg.dll')){
        Put "$f/source/$name" ('package-'+$name)
        @{path='bin/x64/'+$name;source="$f/source/$name";sha256=(Hash "$f/source/$name");mutable=($name -eq 'OptiScaler.ini')}
    })
    Write-Json $c.imagePackage @{schema=1;scope='game-shared';id='shared-fixture';loaderPath='bin/x64/dbghelp.dll';disabledLoaderPath='bin/x64/dbghelp.dll.disabled';files=$files;runtimeMutablePaths=@('bin/x64/RTXMFG-Universal.json','bin/x64/RTXMFG-UI.ini')}
    Put "$f/game/bin/x64/RTXMFG-Universal.json" '{"user":"preserve"}'
    $literalDate='2026-10-15T01:51:46.3314130+04:00'
    Write-Json "$f/game/CPVR-Portable/installed.json" @{version='fixture-port';kind='native';sourceDelta=@{time=$literalDate};paths=@('bin/x64/dbghelp.dll','noncommon.txt');critical=@(@{path='bin/x64/dbghelp.dll';sha256=(Hash "$f/game/bin/x64/dbghelp.dll")},@{path='noncommon.txt';sha256=(Hash "$f/game/noncommon.txt")})}
    $store=@{data=$c.data;scopes=@(@{id='game';root=$c.game;kind='directory';exclusions=@();requiredFiles=@('bin/x64/Cyberpunk2077.exe')})}
    $a=Save-GameProfile -StoreConfig $store;$idA=$a.activeId
    $b=Copy-GameProfile -StoreConfig $store -Name 'B';$idB=$b.activeId
    Put "$f/game/noncommon.txt" 'B';$null=Save-GameProfile -StoreConfig $store
    $paths=@(Get-SharedEnhancementPaths $c)
    $null=Move-GameProfileEnhancementToShared -StoreConfig $store -Paths $paths -BackupDirectory "$f/migration"
    $migrated=[IO.File]::ReadAllText("$f/data/profile-store/profiles/$idA.json")|ConvertFrom-Json -AsHashtable -DateKind String
    $receiptEntry=@($migrated.files|Where-Object path -eq 'CPVR-Portable/installed.json')[0]
    $migratedReceipt=[IO.File]::ReadAllText("$f/data/profile-store/objects/$($receiptEntry.sha256)")|ConvertFrom-Json -AsHashtable -DateKind String
    Check ($migratedReceipt.sourceDelta.time -is [string] -and $migratedReceipt.sourceDelta.time -ceq $literalDate) 'migration preserves exact receipt date string including trailing fractional zero and offset'
    $store.scopes[0].exclusions=$paths
    Check (Test-Path "$f/migration/profiles/$idA.json") 'legacy profile backed up before exact path migration'
    Install-SharedEnhancementPackage $c
    $installedReceipt=[IO.File]::ReadAllText("$f/game/CPVR-Portable/installed.json")|ConvertFrom-Json -AsHashtable -DateKind String
    Check ($installedReceipt.sourceDelta.time -ceq $literalDate) 'installation preserves exact unrelated receipt date string'
    $state=Read-Json "$f/data/enhancement-common/state.json"
    Check ($state.phase -eq 'ready' -and (Get-EnhancementPackageState $c).enabled) 'shared install uses independent ready state'
    $pending=$state.Clone();$pending.phase='prepared';Write-Json "$f/data/enhancement-common/state.json" $pending
    $pendingStatus=Get-EnhancementPackageState $c
    Check (-not $pendingStatus.enabled -and -not $pendingStatus.healthy) 'prepared install never reports green even with the loader physically present'
    Write-Json "$f/data/enhancement-common/state.json" $state
    Check ([IO.File]::ReadAllText("$f/game/bin/x64/OptiScaler.ini") -ceq $userIni) 'initial install preserves existing user menu settings'
    Check (@((Read-Json (Join-Path $state.backup 'plan.json'))|Where-Object { $_.path -eq 'bin/x64/dbghelp.dll' -and [IO.File]::ReadAllText($_.saved) -ceq 'Microsoft-original'}).Count -eq 1) 'Microsoft dbghelp backed up by real transaction'
    $before=@{};foreach($file in $files){$before[$file.path]=Hash (Join-Path $c.game $file.path)}
    $receiptHash=Hash "$f/game/CPVR-Portable/installed.json"
    Set-EnhancementPackage $c $false
    Check (-not(Test-Path "$f/game/bin/x64/dbghelp.dll") -and (Hash "$f/game/bin/x64/dbghelp.dll.disabled") -eq $before['bin/x64/dbghelp.dll']) 'Off is loader rename with identical bytes'
    foreach($file in $files|Where-Object path -ne 'bin/x64/dbghelp.dll'){Check ((Hash (Join-Path $c.game $file.path)) -eq $before[$file.path]) ('Off preserves '+$file.path)}
    $plan=@{game=$c.game;kind='direct';source=@{collection=@{root="$f/collection"}}};$selected=Get-GameLaunchSelectedFiles $plan $c
    Check ($selected['bin/x64/nvngx_dlssnr.dll'] -and [IO.Path]::GetFullPath($selected['bin/x64/nvngx_dlssnr.dll']) -eq [IO.Path]::GetFullPath("$f/game/bin/x64/nvngx_dlssnr.dll")) 'common NR physical override persists while Off'
    Check ($selected.ContainsKey('bin/x64/RTXMFG-Universal.json') -and -not $selected.ContainsKey('bin/x64/RTXMFG-UI.ini')) 'optional runtime config is overridden only when present and is not health-required'
    # Supported legacy MO2 shape uses Config.mo2 rather than source.collection.
    $c.mo2=@{root="$f/legacy-mo2";modsRoot="$f/legacy-mo2/mods";overwriteRoot="$f/legacy-mo2/overwrite"}
    Put "$f/legacy-mo2/profiles/P/modlist.txt" '+Shadow'
    Put "$f/legacy-mo2/mods/Shadow/bin/x64/dbghelp.dll" 'unknown-private-Opti-loader'
    $legacyPlan=@{game=$c.game;kind='mo2';source=@{kind='mo2'};profileRoot="$f/legacy-mo2/profiles/P"}
    $rejected=$false;try{$null=Get-GameLaunchSelectedFiles $legacyPlan $c}catch{$rejected=$_.Exception.Message.Contains('Disabled shared loader is shadowed')}
    Check $rejected 'legacy MO2 Off rejects unknown entry shadow without inline collection'
    Set-EnhancementPackage $c $true
    $legacySelected=Get-GameLaunchSelectedFiles $legacyPlan $c
    $legacyPlan.selectedFiles=$legacySelected
    $winner=Resolve-GameLaunchEffectiveFile $c $legacyPlan 'bin/x64/dbghelp.dll'
    Check ([IO.Path]::GetFullPath($winner) -eq [IO.Path]::GetFullPath("$f/game/bin/x64/dbghelp.dll")) 'legacy MO2 On uses physical common loader over private shadow'
    Check ($legacySelected.ContainsKey('bin/x64/nvngx_dlssnr.dll')) 'legacy MO2 includes common NR override without inline collection'
    Set-EnhancementPackage $c $false
    $null=Activate-GameProfile -StoreConfig $store -Id $idA
    Check ([IO.File]::ReadAllText("$f/game/noncommon.txt") -ceq 'A') 'real profile apply restored A noncommon data'
    Check (-not(Test-Path "$f/game/bin/x64/dbghelp.dll") -and (Hash "$f/game/bin/x64/dbghelp.dll.disabled") -eq $before['bin/x64/dbghelp.dll']) 'A apply leaves Off shared loader unchanged'
    $null=Activate-GameProfile -StoreConfig $store -Id $idB
    Check ([IO.File]::ReadAllText("$f/game/noncommon.txt") -ceq 'B') 'real profile apply restored B noncommon data'
    Check ([IO.File]::ReadAllText("$f/game/bin/x64/RTXMFG-Universal.json") -ceq '{"user":"preserve"}') 'optional MFG runtime settings persist across real A/B apply'
    foreach($file in $files|Where-Object path -ne 'bin/x64/dbghelp.dll'){Check ((Hash (Join-Path $c.game $file.path)) -eq $before[$file.path]) ('A/B preserves '+$file.path)}
    Set-EnhancementPackage $c $true
    Check ((Hash "$f/game/bin/x64/dbghelp.dll") -eq $before['bin/x64/dbghelp.dll']) 'On renames the same loader back'
    $afterReceipt=Read-Json "$f/game/CPVR-Portable/installed.json"
    Check ($afterReceipt.version -eq 'fixture-port' -and -not @($afterReceipt.critical|Where-Object path -eq 'bin/x64/dbghelp.dll').Count) 'profile receipt retains port identity without common critical owner'
    $toggleReceipt=Hash "$f/game/CPVR-Portable/installed.json";Set-EnhancementPackage $c $false;Set-EnhancementPackage $c $true
    Check ((Hash "$f/game/CPVR-Portable/installed.json") -eq $toggleReceipt) 'repeated toggles never rewrite port receipt'
    $h='A'*64;$p='red4ext/plugins/cyberpunkvr_stereo/cyberpunkvr_stereo.dll'
    $bundle=@{versions=@(@{id='flat';kind='flat';files=@{'bin/x64/dbghelp.dll'=$h}},@{id='port';kind='native';files=@{$p=$h;'bin/x64/dbghelp.dll'=$h;'bin/x64/nvngx_dlssg.dll'=$h};portPaths=@($p,'bin/x64/dbghelp.dll')});managedPaths=@($p,'bin/x64/dbghelp.dll');modules=@{opti=@{'bin/x64/dbghelp.dll'=$h}}}
    $filtered=Get-CollectionSafePortDefinition $bundle 'port' -SharedPaths $paths
    Check (-not $filtered.versions[1].files.Contains('bin/x64/dbghelp.dll') -and $filtered.versions[1].files.Contains('bin/x64/nvngx_dlssg.dll') -and 'bin/x64/dbghelp.dll' -notin $filtered.managedPaths -and -not $filtered.modules.opti.Count) 'port copy/remove maps exclude only shared paths; root G remains port-owned'
    $legacy=Get-CollectionSafePortDefinition $bundle 'port'
    Check ($legacy.versions[1].files.Contains('bin/x64/dbghelp.dll')) 'nonshared legacy packaging remains unchanged'
    # Exercise the actual Backend Apply tail, including stale per-port NR/key requests.
    $c.gameProfilesEnabled=$true;$c.appDataPath="$f/appdata";$c.savedGamesPath="$f/saves";$c.userStateFile="$f/saves/user.gls"
    Write-Json "$f/bundle/stock-paths.json" @{'archive/pc/content/stock.archive'=@{sha1=('A'*40);size=1}}
    Write-Json "$f/bundle/bundle.json" @{versions=@()}
    Write-Json "$f/data/keybindings.json" @{ports=@{'fixture-port'=@{optiKey=119}}}
    $iniBefore=[IO.File]::ReadAllBytes("$f/game/bin/x64/OptiScaler.ini")
    Set-LibrariesAndMods $c @{version='fixture-port';opti=$true;mods=$true;libraries=@{}}
    Check ([Convert]::ToHexString([IO.File]::ReadAllBytes("$f/game/bin/x64/OptiScaler.ini")) -ceq [Convert]::ToHexString($iniBefore)) 'real Apply component tail never reapplies per-port shared menu key'
    Set-LibrariesAndMods $c @{version='fixture-port';opti=$true;nr=$true;mods=$true;libraries=@{}}
    Check ([Convert]::ToHexString([IO.File]::ReadAllBytes("$f/game/bin/x64/OptiScaler.ini")) -ceq [Convert]::ToHexString($iniBefore)) 'real Apply component tail never reapplies shared NR settings or per-port menu key'
    $fault=$c.Clone();$fault.gameProfilesEnabled=$false;$fault.game="$f/fault-game";$fault.data="$f/fault-data"
    Put "$f/fault-game/bin/x64/dbghelp.dll" 'Microsoft-before-failure'
    Write-Json "$f/fault-game/CPVR-Portable/installed.json" @{version='fault-port';kind='native';critical=@(@{path='bin/x64/dbghelp.dll';sha256=(Hash "$f/fault-game/bin/x64/dbghelp.dll")})}
    $faultReceipt=Hash "$f/fault-game/CPVR-Portable/installed.json"
    $originalWriter=(Get-Command Write-Json).ScriptBlock
    function Write-Json($Path,$Value){if([IO.Path]::GetFullPath($Path) -eq [IO.Path]::GetFullPath("$f/fault-data/enhancement-common/state.json")){throw 'injected shared state publication failure'};& $originalWriter $Path $Value}
    $rolledBack=$false
    try{Install-SharedEnhancementPackage $fault}catch{$rolledBack=$_.Exception.Message.Contains('shared payload and port receipt restored')}
    finally{Set-Item Function:Write-Json -Value $originalWriter}
    Check ($rolledBack -and [IO.File]::ReadAllText("$f/fault-game/bin/x64/dbghelp.dll") -ceq 'Microsoft-before-failure' -and
        (Hash "$f/fault-game/CPVR-Portable/installed.json") -eq $faultReceipt -and -not(Test-Path "$f/fault-game/bin/x64/nvngx_dlssnr.dll") -and
        -not(Test-Path "$f/fault-data/enhancement-common/state.json")) 'actual state-write failure rolls back payload and exact port receipt without reporting ready'
    Write-Output 'Shared enhancement production-function CPU contracts passed; no game/GPU proof.'
}finally{
    # Only this newly-created exact temp fixture is eligible for cleanup.
    $resolved=[IO.Path]::GetFullPath($f)
    if($resolved.StartsWith([IO.Path]::GetFullPath([IO.Path]::GetTempPath()),[StringComparison]::OrdinalIgnoreCase) -and (Split-Path $resolved -Leaf) -like 'shared-enhancement-*') {Remove-Item -LiteralPath $resolved -Recurse -Force -ErrorAction SilentlyContinue}
}
