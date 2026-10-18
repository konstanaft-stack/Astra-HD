param([Parameter(Mandatory)][string]$BundleRoot)
$ErrorActionPreference='Stop'
. "$PSScriptRoot/../Core.ps1"

function Assert($ok,[string]$message){if(-not $ok){throw $message}}
function Hash([string]$path){(Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash}
function Safe-Join([string]$root,[string]$relative){
    if([IO.Path]::IsPathRooted($relative)){throw 'Absolute path'}
    $base=[IO.Path]::GetFullPath($root).TrimEnd('\')
    $path=[IO.Path]::GetFullPath((Join-Path $base $relative))
    if(-not $path.StartsWith($base+'\',[StringComparison]::OrdinalIgnoreCase)){throw 'Escaped path'}
    return $path
}
function Get-BackupStorageKey([string]$relative){'objects/'+[Convert]::ToHexString([Security.Cryptography.SHA256]::HashData([Text.Encoding]::UTF8.GetBytes($relative)))+'.bak'}
function Is-GenericModPath([string]$relative){$true}
. (Join-Path $BundleRoot 'Clean-Profile.ps1')

$root=Join-Path ([IO.Path]::GetTempPath()) ('cpvr-content-'+[guid]::NewGuid().ToString('N'))
$game=Join-Path $root 'game';$bundle=Join-Path $root 'bundle';$data=Join-Path $root 'data'
[IO.Directory]::CreateDirectory($game)|Out-Null
[IO.Directory]::CreateDirectory($bundle)|Out-Null
[IO.Directory]::CreateDirectory($data)|Out-Null
function Add-File([string]$relative,[string]$text){
    $path=Safe-Join $game $relative
    [IO.Directory]::CreateDirectory((Split-Path $path -Parent))|Out-Null
    [IO.File]::WriteAllText($path,$text,[Text.UTF8Encoding]::new($false))
    return (Hash $path)
}
$approved='archive/pc/mod/selected-car.archive'
$disabled='archive/pc/mod/disabled-car.archive'
$unrelated='archive/pc/mod/unrelated.archive'
$hud='bin/x64/plugins/cyber_engine_tweaks/mods/unrelated_hud/init.lua'
$port='red4ext/plugins/cyberpunkvr_stereo/cyberpunkvr_stereo.dll'
$approvedHash=Add-File $approved 'selected car'
$disabledHash=Add-File $disabled 'disabled car'
$null=Add-File $unrelated 'unrelated'
$null=Add-File $hud 'unrelated HUD'
$null=Add-File $port 'port owned'
$cet='bin/x64/plugins/cyber_engine_tweaks/mods/selected_grant/init.lua'
$cetData='bin/x64/plugins/cyber_engine_tweaks/mods/selected_grant/db.sqlite3'
$cetCode='bin/x64/plugins/cyber_engine_tweaks/mods/selected_grant/unlisted.lua'
$cetHash=Add-File $cet 'selected grant'
$null=Add-File $cetData 'generated database'
$null=Add-File $cetCode 'unlisted code'
$audiInput='r6/input/audi_vs_mmi_focus.xml'
$audioDll='red4ext/plugins/AudioXL/AudioXL.dll'
$audioData=@('red4ext/plugins/AudioXL/AudioXL.ini','red4ext/plugins/AudioXL/settings.json','red4ext/plugins/AudioXL/session.log','red4ext/plugins/AudioXL/options.toml','red4ext/plugins/AudioXL/cache.sqlite3')
$audioCode=@('red4ext/plugins/AudioXL/injected.dll','red4ext/plugins/AudioXL/injected.reds','red4ext/plugins/AudioXL/injected.lua')
$disabledDll='red4ext/plugins/DisabledNative/DisabledNative.dll'
$disabledIni='red4ext/plugins/DisabledNative/DisabledNative.ini'
$audiInputHash=Add-File $audiInput '<input />'
$audioDllHash=Add-File $audioDll 'AudioXL binary'
foreach($path in $audioData){$null=Add-File $path 'generated AudioXL data'}
foreach($path in $audioCode){$null=Add-File $path 'unlisted AudioXL code'}
$disabledDllHash=Add-File $disabledDll 'disabled native plugin'
$null=Add-File $disabledIni 'disabled native settings'
$manifest=Join-Path $game 'CPVR-Portable/approved-content.json'
function Save-Manifest($packages){Write-Json $manifest @{schema=1;packages=@($packages)}}
$enabledPackage=@{id='selected-car';enabled=$true;files=@(@{path=$approved;sha256=$approvedHash})}
$disabledPackage=@{id='disabled-car';enabled=$false;files=@(@{path=$disabled;sha256=$disabledHash})}
$cetPackage=@{id='selected-grant';enabled=$true;files=@(@{path=$cet;sha256=$cetHash})}
$audiPackage=@{id='audi-vehicle-services';enabled=$true;files=@(@{path=$audiInput;sha256=$audiInputHash})}
$audioPackage=@{id='audioxl';enabled=$true;files=@(@{path=$audioDll;sha256=$audioDllHash})}
$disabledNativePackage=@{id='disabled-native';enabled=$false;files=@(@{path=$disabledDll;sha256=$disabledDllHash})}
$selectedPackages=@($enabledPackage,$disabledPackage,$cetPackage,$audiPackage,$audioPackage,$disabledNativePackage)
Save-Manifest $selectedPackages

$b=@{managedPaths=@($port);versions=@(@{id='flat';files=@{}},@{id='selected';label='Selected';available=$true;kind='native';files=@{$port=(Hash (Safe-Join $game $port))}})}
Write-Json (Join-Path $bundle 'bundle.json') $b
[IO.File]::WriteAllText((Join-Path $bundle 'stock-paths.json'),'{}',[Text.UTF8Encoding]::new($false))
Copy-Item -LiteralPath (Join-Path $BundleRoot 'ContentPackages.ps1') -Destination (Join-Path $bundle 'ContentPackages.ps1')
$c=@{game=$game;bundle=$bundle;data=$data;hold=(Join-Path $data 'hold.json');vortex=(Join-Path $root 'vortex.exe');vortexData=(Join-Path $root 'vortex')}
Write-Json (Join-Path $game 'CPVR-Portable/installed.json') @{version='selected';kind='native';paths=@($port)}
$state=Get-PortState $c
Assert ($approved -notin $state.externalFiles) 'Enabled exact file classified as external'
Assert ($cet -notin $state.externalFiles -and $cetData -notin $state.externalFiles) 'Selected CET runtime classified as external'
foreach($path in @($audiInput,$audioDll)+$audioData){Assert ($path -notin $state.externalFiles) "Selected vehicle support classified as external: $path"}
foreach($path in @($disabled,$unrelated,$hud,$cetCode,$disabledDll,$disabledIni)+$audioCode){Assert ($path -in $state.externalFiles) "Unapproved path hidden: $path"}

$plan=Get-CleanProfilePlan -Game $game -StateDir (Join-Path $game 'CPVR-Portable') -Bundle $b -Selected @{$port=(Hash (Safe-Join $game $port))} -Clean $true -BackupRelative 'CPVR-Portable-Backups/test'
Assert ($approved -notin $plan.ExtraRemovals) 'Selected content removed during clean switch'
Assert ($cet -notin $plan.ExtraRemovals -and $cetData -notin $plan.ExtraRemovals) 'Selected CET runtime removed during clean switch'
foreach($path in @($audiInput,$audioDll)+$audioData){Assert ($path -notin $plan.ExtraRemovals) "Selected vehicle support removed: $path"}
foreach($path in @($disabled,$unrelated,$hud,$cetCode,$disabledDll,$disabledIni)+$audioCode){Assert ($path -in $plan.ExtraRemovals) "Unapproved path survived clean switch: $path"}

# A second clean-profile plan represents a port switch: content remains independent of the selected renderer.
$second=Get-CleanProfilePlan -Game $game -StateDir (Join-Path $game 'CPVR-Portable') -Bundle $b -Selected @{} -Clean $true -BackupRelative 'CPVR-Portable-Backups/second'
Assert ($approved -notin $second.ExtraRemovals) 'Selected content removed on renderer switch'
foreach($path in @($audiInput,$audioDll)+$audioData){Assert ($path -notin $second.ExtraRemovals) "Selected vehicle support removed on switch: $path"}

Write-Json (Join-Path $bundle 'stock-paths.json') @{'archive/pc/mod/stock.archive'=@{}}
foreach($case in @(
    @{name='traversal';path='archive/pc/mod/../../bin/x64/Cyberpunk2077.exe'},
    @{name='absolute';path='C:/Windows/System32/evil.dll'},
    @{name='port collision';path=$port},
    @{name='stock collision';path='archive/pc/mod/stock.archive'},
    @{name='RCF storage not approved';path='r6/storages/RedscriptConfigFramework/AudioXL.card.json'},
    @{name='other storage namespace';path='r6/storages/OtherFramework/file.json'}
)){
    Save-Manifest @(@{id=$case.name;enabled=$true;files=@(@{path=$case.path;sha256=$approvedHash})})
    $rejected=$false
    try{$null=Get-PortState $c}catch{$rejected=$true}
    Assert $rejected "Invalid manifest accepted: $($case.name)"
}

Save-Manifest @(@{id='wrong-hash';enabled=$true;files=@(@{path=$approved;sha256=('0'*64)})})
$rejected=$false
try{$null=Get-PortState $c}catch{$rejected=$true}
Assert $rejected 'Hash mismatch accepted as approved content'

Save-Manifest @(@{id='missing';enabled=$true;files=@(@{path='r6/scripts/missing.reds';sha256=$approvedHash})})
$rejected=$false
try{$null=Get-PortState $c}catch{$rejected=$true}
Assert $rejected 'Missing selected file silently treated as external'

Save-Manifest $selectedPackages
. "$PSScriptRoot/../Components.ps1"
Ensure-LaunchModProfile $c (Get-PortState $c)
Assert (Test-Path -LiteralPath (Safe-Join $game $approved)) 'Clean launch parked selected car'
Assert (Test-Path -LiteralPath (Safe-Join $game $cetData)) 'Clean launch parked selected CET data'
foreach($path in @($audiInput,$audioDll)+$audioData){Assert (Test-Path -LiteralPath (Safe-Join $game $path)) "Clean launch parked selected vehicle support: $path"}
Assert (-not(Test-Path -LiteralPath (Safe-Join $game $unrelated))) 'Clean launch retained unrelated mod'
Assert (-not(Test-Path -LiteralPath (Safe-Join $game $cetCode))) 'Clean launch retained unlisted executable code'
foreach($path in $audioCode){Assert (-not(Test-Path -LiteralPath (Safe-Join $game $path))) "Clean launch retained injected native plugin code: $path"}
Assert (-not(Test-Path -LiteralPath (Safe-Join $game $disabledIni))) 'Clean launch retained settings of disabled native plugin'

'PASS: exact approved content, disabled and unrelated cleanup, port switch, path and collision rejection, clean launch.'
