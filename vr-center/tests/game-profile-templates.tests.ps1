$ErrorActionPreference='Stop'
$appRoot=Split-Path $PSScriptRoot -Parent
$builder=Join-Path $appRoot 'GameProfileTemplates.ps1'
if(-not(Test-Path -LiteralPath $builder)){throw 'RED: clean profile template builder is not implemented'}
. (Join-Path $appRoot 'GameProfileStore.ps1')
. $builder
function Assert([bool]$Value,[string]$Message){if(-not $Value){throw $Message}}
function Write-Fixture([string]$Path,[string]$Text){[IO.Directory]::CreateDirectory((Split-Path $Path -Parent))|Out-Null;[IO.File]::WriteAllText($Path,$Text,[Text.UTF8Encoding]::new($false))}
function Write-FixtureJson([string]$Path,$Value){Write-Fixture $Path ($Value|ConvertTo-Json -Depth 20)}
function FixtureHash([string]$Path,[string]$Algorithm='SHA256'){return (Get-FileHash -LiteralPath $Path -Algorithm $Algorithm).Hash}
function FixtureCas([string]$Text){$temp=Join-Path $fixture 'seed.txt';Write-Fixture $temp $Text;$sha=FixtureHash $temp;Write-Fixture (Join-Path $bundleRoot ('files/'+$sha)) $Text;return $sha}
function FixtureInventory([string]$Root){return (@(Get-ChildItem -LiteralPath $Root -Recurse -File|Sort-Object FullName|ForEach-Object {$_.FullName.Substring($Root.Length)+'='+(FixtureHash $_.FullName)}) -join "`n")}
function Assert-Throws([scriptblock]$Action,[string]$Expected){try{& $Action|Out-Null}catch{if($_.Exception.Message -notlike ('*'+$Expected+'*')){throw};return $_.Exception};throw ('Expected failure containing: '+$Expected)}
$tempBase=[IO.Path]::GetFullPath($env:TEMP).TrimEnd('\')
$fixture=[IO.Path]::GetFullPath((Join-Path $tempBase ('cpvr-template-fixture-'+[guid]::NewGuid().ToString('N'))))
if(-not $fixture.StartsWith($tempBase+'\',[StringComparison]::OrdinalIgnoreCase)){throw 'Fixture escaped TEMP'}
try{
 $game=Join-Path $fixture 'game';$bundleRoot=Join-Path $fixture 'bundle';$data=Join-Path $fixture 'center-data'
 $exe=Join-Path $game 'bin/x64/Cyberpunk2077.exe';Write-Fixture $exe 'stock executable'
 $stockConfigHash=FixtureCas 'stock engine config';$stockConfigSource=Join-Path $bundleRoot ('files/'+$stockConfigHash)
 Write-Fixture (Join-Path $game 'engine/config/base/general.ini') 'collection modified config'
 Write-Fixture (Join-Path $game 'r6/config/inputContexts.xml') 'stock input contexts'
 Write-Fixture (Join-Path $game 'archive/pc/content/base.archive') 'shared content'
 Write-Fixture (Join-Path $game 'archive/pc/ep1/ep1.archive') 'shared expansion'
 Write-Fixture (Join-Path $game 'archive/pc/ep1/addcont.keystone') 'shared stock auxiliary identity'
 Write-Fixture (Join-Path $game 'archive/pc/content/collection-addon.archive') 'private extra archive from collection'
 Write-Fixture (Join-Path $game 'mods/HUDitor/user.json') 'foreign user mod'
 Write-Fixture (Join-Path $game 'bin/x64/vrport.ini') 'old personal calibration'
 Write-Fixture (Join-Path $game 'CPVR-Portable/preferences/old/@settings') 'old preference'
 Write-Fixture (Join-Path $game 'CPVR-Portable/installed.json') '{"version":"old","opti":true}'
 $appdata=Join-Path $fixture 'appdata';Write-Fixture (Join-Path $appdata 'UserSettings.json') '{"old":true}'
 $userState=Join-Path $fixture 'user-state/user.gls';Write-Fixture $userState 'old gameplay preferences'
 $settings=Join-Path $data 'game-settings';Write-Fixture (Join-Path $settings 'old/custom.json') 'old saved settings'
 $modsState=Join-Path $data 'mods-state.json';Write-Fixture $modsState '{"enabled":true}'
 $keys=Join-Path $data 'keybindings.json';Write-Fixture $keys '{"old":true}'
 $usb=Join-Path $data 'usb-state.json';Write-Fixture $usb '{"old":true}'
 $config=@{game=$game;bundle=$bundleRoot;data=$data}
 $storeConfig=@{data=$data;scopes=@(
  @{id='game';kind='directory';root=$game;requiredFiles=@('bin/x64/Cyberpunk2077.exe');exclusions=@('archive/pc/content/base.archive','archive/pc/ep1/ep1.archive','archive/pc/ep1/addcont.keystone')},
  @{id='appdata';kind='directory';root=$appdata;exclusions=@();requiredFiles=@()},
  @{id='user-state';kind='directory';root=(Split-Path $userState -Parent);exclusions=@();requiredFiles=@()},
  @{id='center-settings';kind='directory';root=$settings;exclusions=@();requiredFiles=@()},
  @{id='mods-state';kind='file';root=$modsState;exclusions=@();requiredFiles=@()},
  @{id='keybindings';kind='file';root=$keys;exclusions=@();requiredFiles=@()},
  @{id='usb-state';kind='file';root=$usb;exclusions=@();requiredFiles=@()}
 )}
 $stock=@{}
 foreach($path in @('bin/x64/Cyberpunk2077.exe','r6/config/inputContexts.xml','archive/pc/content/base.archive','archive/pc/ep1/ep1.archive','archive/pc/ep1/addcont.keystone')){
  $file=Join-Path $game $path;$stock[$path]=@{sha1=FixtureHash $file SHA1;size=[long](Get-Item -LiteralPath $file).Length}
 }
 $stock['engine/config/base/general.ini']=@{sha1=FixtureHash $stockConfigSource SHA1;size=[long](Get-Item -LiteralPath $stockConfigSource).Length}
 $authoredSettings='{"version":140,"data":[{"group_name":"/graphics","options":[{"name":"Resolution","value":"author"}]}]}'
 $payload=@{
  'red4ext/red4ext.dll'=(FixtureCas 'mandatory loader')
  'red4ext/plugins/cyberpunkvr_stereo/cyberpunkvr_stereo.dll'=(FixtureCas 'Astra HD renderer')
  'red4ext/plugins/cyberpunkvr_stereo/usersettings.json'=(FixtureCas $authoredSettings)
  'archive/pc/mod/cyberpunkvrport.archive'=(FixtureCas 'native mandatory assets')
  'bin/x64/vrport.ini'=(FixtureCas 'PERSONAL vrport calibration')
  'bin/x64/plugins/cyber_engine_tweaks/mods/cyberpunkvrport_stereo/vrcam.json'=(FixtureCas '{"component":"vrcam_1920x1920","virtualCamera":"vrcam_feed_1920x1920","components":["vrcam_1920x1920"]}')
  'bin/x64/vrport-launcher.ini'=(FixtureCas 'technical launcher defaults')
  '@settings'=(FixtureCas 'OLD USER GRAPHICS')
 }
 $bundle=@{schemaVersion=1;exeSha256=FixtureHash $exe;versions=@(
  @{id='flat';label='Плоская игра';available=$true;kind='flat';files=@{'engine/config/base/general.ini'=$stockConfigHash;'@settings'=(FixtureCas 'OLD FLAT GRAPHICS')}},
  @{id='tofu-aer-v1-candidate';label='Astra HD';available=$true;kind='native';files=$payload}
 )}
 Write-FixtureJson (Join-Path $bundleRoot 'stock-paths.json') $stock
 Write-FixtureJson (Join-Path $bundleRoot 'bundle.json') $bundle
 Write-Fixture (Join-Path $bundleRoot 'Start-Game.ps1') 'param([switch]$CheckOnly) # exact launcher fixture'
 $gameBefore=FixtureInventory $game;$appdataBefore=FixtureInventory $appdata;$userBefore=FixtureHash $userState
 $centerBefore=@((FixtureHash $modsState),(FixtureHash $keys),(FixtureHash $usb),(FixtureInventory $settings)) -join '|'
 Assert ((Ensure-CyberpunkProfileTemplate -Config $config -StoreConfig $storeConfig -VR:$false) -ceq 'clean-flat') 'Flat builder must return only template ID'
 $flat=Read-GameProfileJson (Join-Path $data 'profile-store/templates/clean-flat.json')
 Assert ($flat.complete -and $flat.files.Count -eq 5) 'Flat template must include every private stock file plus receipt and launcher'
 Assert (@($flat.files|Where-Object scope -ne 'game').Count -eq 0) 'Flat AppData, user state, cache and app preferences must start empty'
 Assert (@($flat.files|Where-Object path -match 'archive/pc/(content|ep1)|HUDitor|@settings|preferences|vrport.ini').Count -eq 0) 'No shared archives, mods or previous preferences in clean flat'
 $engine=@($flat.files|Where-Object path -eq 'engine/config/base/general.ini')[0]
 Assert ($engine.sha256 -eq $stockConfigHash) 'Modified game stock must use independently verified flat CAS fallback'
 $receiptFile=@($flat.files|Where-Object path -eq 'CPVR-Portable/installed.json')[0]
 $receipt=Read-GameProfileJson (Join-Path $data ('profile-store/objects/'+$receiptFile.sha256))
 Assert ($receipt.version -ceq 'flat' -and $receipt.kind -ceq 'flat' -and -not $receipt.opti -and -not $receipt.fg -and -not $receipt.hands -and $receipt.clean) 'Fresh flat receipt must match its real composition'
 Assert ($receipt.exeSha256 -eq $bundle.exeSha256) 'Launcher requires the verified exe identity'
 $contextsPath=Join-Path $game 'r6/config/inputContexts.xml';$contextsText=[IO.File]::ReadAllText($contextsPath);$contextsHash=FixtureHash $contextsPath
 Write-Fixture $contextsPath 'new collection overwrote an additional stock file'
 $null=Ensure-CyberpunkProfileTemplate -Config $config -StoreConfig $storeConfig
 $recreated=Read-GameProfileJson (Join-Path $data 'profile-store/templates/clean-flat.json')
 $contextsEntry=@($recreated.files|Where-Object path -eq 'r6/config/inputcontexts.xml')[0]
 Assert ($contextsEntry.sha256 -eq $contextsHash) 'An existing verified clean-flat CAS base must recover stock beyond portable flat.files'
 Write-Fixture $contextsPath $contextsText
 Assert ((Ensure-CyberpunkProfileTemplate -Config $config -StoreConfig $storeConfig -VR:$true) -ceq 'clean-vr') 'VR builder returns its ID'
 $vr=Read-GameProfileJson (Join-Path $data 'profile-store/templates/clean-vr.json')
 Assert (@($vr.files|Where-Object {$_.scope -eq 'game' -and $_.path -eq 'red4ext/red4ext.dll'}).Count -eq 1) 'Mandatory dependency remains part of exact native kit'
 Assert (@($vr.files|Where-Object path -match '(^|/)vrport.ini$').Count -eq 0) 'Runtime personalized calibration must not seed a fresh VR profile'
 Assert (@($vr.files|Where-Object path -match '(^|/)vrcam.json$').Count -eq 1) 'Fresh VR profile requires the authored VRCAM component catalogue and initial selection'
 Assert (@($vr.files|Where-Object path -eq 'bin/x64/vrport-launcher.ini').Count -eq 1) 'Technical base configs must not be stripped with calibration'
 $player=@($vr.files|Where-Object {$_.scope -eq 'appdata' -and $_.path -eq 'UserSettings.json'})[0]
 Assert ($player.sha256 -eq $payload['red4ext/plugins/cyberpunkvr_stereo/usersettings.json']) 'VR player settings must be the authored native defaults, never @settings'
 $versionHash=[Convert]::ToHexString([Security.Cryptography.SHA256]::HashData([Text.Encoding]::UTF8.GetBytes('tofu-aer-v1-candidate')))
 $mode=@($vr.files|Where-Object {$_.scope -eq 'center-settings' -and $_.path -eq ($versionHash+'/mode.json')})[0]
 Assert ([bool]$mode) 'VR template needs version-keyed initial preset consent'
 $modeState=Read-GameProfileJson (Join-Path $data ('profile-store/objects/'+$mode.sha256))
 Assert ($modeState.recommended -and $modeState.version -ceq 'tofu-aer-v1-candidate') 'BeforeLaunch must see recommended=true in a fresh VR profile'
 $custom=@($vr.files|Where-Object {$_.scope -eq 'center-settings' -and $_.path -eq ($versionHash+'/custom.json')})[0]
 Assert ($custom.sha256 -eq $player.sha256) 'Initial custom must use the same fresh authored defaults, permitting preset reversal'
 # Exercise the production mode decision with a bounded fixture writer. The
 # global live-game/Vortex guard and transaction writer belong to their own tests.
 & {
  . (Join-Path $appRoot 'SettingsProfiles.ps1')
  $roundtripRoot=Join-Path $fixture 'mode-roundtrip'
  $modeConfig=@{data=(Join-Path $roundtripRoot 'data');game=(Join-Path $roundtripRoot 'game');settingsFile=(Join-Path $roundtripRoot 'player/UserSettings.json')}
  $modeReceipt=@{receipt=@{version='tofu-aer-v1-candidate';kind='native'}}
  $paths=Get-SettingsModePaths $modeConfig $modeReceipt
  Write-Fixture $paths.player $authoredSettings
  Write-Fixture $paths.custom ([IO.File]::ReadAllText((Join-Path $data ('profile-store/objects/'+$custom.sha256))))
  Write-Fixture $paths.state ([IO.File]::ReadAllText((Join-Path $data ('profile-store/objects/'+$mode.sha256))))
  function Write-SettingsModeFiles($Config,$State,$Changes){
   foreach($change in $Changes){
    $target=[IO.Path]::GetFullPath($change.path)
    Assert ($target.StartsWith($roundtripRoot+'\',[StringComparison]::OrdinalIgnoreCase)) 'Mode fixture writer escaped its explicit temporary scope'
    Write-Fixture $target $change.text
   }
  }
  Assert (Get-RecommendedSettingsMode $modeConfig $modeReceipt) 'Fresh VR begins in recommended mode'
  Set-RecommendedSettingsMode $modeConfig $modeReceipt $false
  Assert (-not(Get-RecommendedSettingsMode $modeConfig $modeReceipt)) 'Production mode action must allow immediate switch to game settings'
  Assert ([IO.File]::ReadAllText($paths.player) -ceq $authoredSettings) 'Preset reversal restores only the fresh authored baseline'
 }
 Assert (@($vr.files|Where-Object scope -in @('user-state','mods-state','keybindings','usb-state')).Count -eq 0) 'Previous per-profile state must not leak into new VR profile'
 Assert (-not(Get-GameProfileCatalog -StoreConfig $storeConfig).initialized) 'Template creation must not initialize/autosave current game'
 # Public minimal R29 has no legacy ToFu identifier. Exercise the real stock
 # validation/native overlay plan, not a parallel version-selection mock.
 $legacyVersion=$bundle.versions[1]
 $r29Version=$legacyVersion.Clone();$r29Version.id='astra-hd-editor-spatial-20261016-r29'
 $bundle.defaultVersion=$r29Version.id;$bundle.versions=@($bundle.versions[0],$r29Version,$legacyVersion)
 Write-FixtureJson (Join-Path $bundleRoot 'bundle.json') $bundle
 $r29Plan=Get-CyberpunkProfileTemplatePlan -Config $config -StoreConfig $storeConfig -VR
 Assert ($r29Plan.version -ceq $r29Version.id) 'Available native default R29 wins even when legacy ToFu also exists'
 $bundle.versions=@($bundle.versions[0],$r29Version);Write-FixtureJson (Join-Path $bundleRoot 'bundle.json') $bundle
 $r29Plan=Get-CyberpunkProfileTemplatePlan -Config $config -StoreConfig $storeConfig -VR
 Assert ($r29Plan.version -ceq $r29Version.id) 'Public R29-only native bundle builds the actual VR template plan'
 $bundle.defaultVersion='missing-default';$bundle.versions=@($bundle.versions[0],$legacyVersion);Write-FixtureJson (Join-Path $bundleRoot 'bundle.json') $bundle
 Assert ((Get-CyberpunkProfileTemplatePlan -Config $config -StoreConfig $storeConfig -VR).version -ceq 'tofu-aer-v1-candidate') 'Available legacy ToFu fallback is retained'
 $bundle.versions=@($bundle.versions[0]);Write-FixtureJson (Join-Path $bundleRoot 'bundle.json') $bundle
 $null=Assert-Throws {Get-CyberpunkProfileTemplatePlan -Config $config -StoreConfig $storeConfig -VR} 'One available complete native'
 $bundle.defaultVersion=$r29Version.id;$bundle.versions=@($bundle.versions[0],$r29Version,$r29Version);Write-FixtureJson (Join-Path $bundleRoot 'bundle.json') $bundle
 $null=Assert-Throws {Get-CyberpunkProfileTemplatePlan -Config $config -StoreConfig $storeConfig -VR} 'One available complete native'
 $bundle.Remove('defaultVersion');$bundle.versions=@($bundle.versions[0],$legacyVersion);Write-FixtureJson (Join-Path $bundleRoot 'bundle.json') $bundle
 $flatBefore=FixtureHash (Join-Path $data 'profile-store/templates/clean-flat.json')
 Write-Fixture $stockConfigSource 'CORRUPTED FALLBACK'
 Write-Fixture (Join-Path $data ('profile-store/objects/'+$stockConfigHash)) 'CORRUPTED CACHED ORIGINAL'
 $null=Assert-Throws {Ensure-CyberpunkProfileTemplate -Config $config -StoreConfig $storeConfig} 'engine/config/base/general.ini'
 Assert ((FixtureHash (Join-Path $data 'profile-store/templates/clean-flat.json')) -eq $flatBefore) 'Failed validation must retain previous proven template'
 Write-Fixture $stockConfigSource 'stock engine config'
 Write-Fixture (Join-Path $data ('profile-store/objects/'+$stockConfigHash)) 'stock engine config'
 $stock['engine/missing-one.bin']=@{sha1=('a'*40);size=1};$stock['r6/missing-two.bin']=@{sha1=('b'*40);size=1}
 Write-FixtureJson (Join-Path $bundleRoot 'stock-paths.json') $stock
 $missing=Assert-Throws {Ensure-CyberpunkProfileTemplate -Config $config -StoreConfig $storeConfig} 'Missing verified stock'
 Assert (@($missing.Data['MissingStockPaths']).Count -eq 2) 'Preflight must report every missing original, not just first'
 $stock.Remove('engine/missing-one.bin');$stock.Remove('r6/missing-two.bin');Write-FixtureJson (Join-Path $bundleRoot 'stock-paths.json') $stock
 $goodExclusions=$storeConfig.scopes[0].exclusions
 $storeConfig.scopes[0].exclusions=@('archive/pc/content','archive/pc/ep1')
 $null=Assert-Throws {Get-CyberpunkProfileTemplatePlan -Config $config -StoreConfig $storeConfig} 'exact snapshot exclusion'
 $storeConfig.scopes[0].exclusions=$goodExclusions+@('archive/pc/content')
 $null=Assert-Throws {Get-CyberpunkProfileTemplatePlan -Config $config -StoreConfig $storeConfig} 'directory-wide exclusion'
 $storeConfig.scopes[0].exclusions=$goodExclusions
 Assert (-not(Test-GameProfileExcluded (Get-GameProfileContext $storeConfig).scopes.game 'archive/pc/content/collection-addon.archive')) 'Extra archives must remain private profile state'
 $stock['../escape.bin']=@{sha1=('c'*40);size=1};Write-FixtureJson (Join-Path $bundleRoot 'stock-paths.json') $stock
 $null=Assert-Throws {Get-CyberpunkProfileTemplatePlan -Config $config -StoreConfig $storeConfig} 'Invalid relative profile path'
 $stock.Remove('../escape.bin');Write-FixtureJson (Join-Path $bundleRoot 'stock-paths.json') $stock
 $bundle.versions[1].files['red4ext/plugins/HUDitor/huditor.dll']=(FixtureCas 'external contaminated payload')
 Write-FixtureJson (Join-Path $bundleRoot 'bundle.json') $bundle
 $null=Assert-Throws {Ensure-CyberpunkProfileTemplate -Config $config -StoreConfig $storeConfig -VR} 'External component'
 $bundle.versions[1].files.Remove('red4ext/plugins/HUDitor/huditor.dll');Write-FixtureJson (Join-Path $bundleRoot 'bundle.json') $bundle
 $goodExeHash=$bundle.exeSha256;$bundle.exeSha256=('d'*64);Write-FixtureJson (Join-Path $bundleRoot 'bundle.json') $bundle
 $null=Assert-Throws {Get-CyberpunkProfileTemplatePlan -Config $config -StoreConfig $storeConfig} 'Stock executable does not match'
 $bundle.exeSha256=$goodExeHash;Write-FixtureJson (Join-Path $bundleRoot 'bundle.json') $bundle
 $sharedPath=Join-Path $game 'archive/pc/content/base.archive'
 Write-Fixture $sharedPath 'MODDED CONTENT' # same byte length as original; SHA1 must catch it.
 $sharedFailure=Assert-Throws {Ensure-CyberpunkProfileTemplate -Config $config -StoreConfig $storeConfig} 'Unverified shared stock'
 Assert (@($sharedFailure.Data['UnverifiedSharedStockPaths']).Count -eq 1) 'Shared source gaps must name all unverified stock identities'
 Assert ([IO.File]::ReadAllText($sharedPath) -ceq 'MODDED CONTENT') 'Builder must not repair giant shared originals automatically'
 Write-Fixture $sharedPath 'shared content'
 $vrBefore=FixtureHash (Join-Path $data 'profile-store/templates/clean-vr.json')
 Write-Fixture (Join-Path $bundleRoot ('files/'+$payload['red4ext/red4ext.dll'])) 'TAMPERED LOADER'
 $null=Assert-Throws {Ensure-CyberpunkProfileTemplate -Config $config -StoreConfig $storeConfig -VR} 'red4ext/red4ext.dll'
 Assert ((FixtureHash (Join-Path $data 'profile-store/templates/clean-vr.json')) -eq $vrBefore) 'Corrupt native payload must not publish a replacement template'
 Assert ((FixtureInventory $game) -ceq $gameBefore) 'Template builder modified source game'
 Assert ((FixtureInventory $appdata) -ceq $appdataBefore -and (FixtureHash $userState) -ceq $userBefore) 'Template builder modified AppData/user state'
 Assert ((@((FixtureHash $modsState),(FixtureHash $keys),(FixtureHash $usb),(FixtureInventory $settings)) -join '|') -ceq $centerBefore) 'Template builder modified live app preferences'
 'PASS: full private stock, verified bundle/CAS fallback, exact shared files/SHA1, add-on archive preservation, exact VR dependencies, authored settings/custom/preset reversal, empty private data, runtime calibration exclusion, launcher receipt, missing/corrupt/shared/path/foreign/version fail closed, read-only sources.'
}finally{
 if(Test-Path -LiteralPath $fixture){
  $resolved=[IO.Path]::GetFullPath($fixture)
  if(-not $resolved.StartsWith($tempBase+'\cpvr-template-fixture-',[StringComparison]::OrdinalIgnoreCase)){throw 'Unsafe fixture cleanup path'}
  Remove-Item -LiteralPath $resolved -Recurse -Force
 }
}
