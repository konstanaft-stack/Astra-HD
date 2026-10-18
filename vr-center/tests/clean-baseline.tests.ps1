$ErrorActionPreference='Stop'
$app=Split-Path $PSScriptRoot -Parent
. (Join-Path $app 'GameProfiles.ps1')
# The fixture owns every filesystem root; a user's unrelated live game is not
# part of this synthetic capture/activation cycle. No process action is taken.
function Get-Process { [CmdletBinding()]param([string[]]$Name); return @() }
function Check($value,$why){if(-not $value){throw $why};Write-Output ('PASS: '+$why)}
function Put($path,$value){[IO.Directory]::CreateDirectory((Split-Path $path -Parent))|Out-Null;[IO.File]::WriteAllText($path,$value)}
$fixture=Join-Path ([IO.Path]::GetTempPath()) ('cpvr-clean-baseline-'+[guid]::NewGuid().ToString('N'))
$c=@{game="$fixture/game";data="$fixture/data";bundle="$fixture/bundle";settingsFile="$fixture/appdata/UserSettings.json";appDataPath="$fixture/appdata";userStateFile="$fixture/saves/user.gls";hold="$fixture/hold.json";gameProfilesEnabled=$true}
Put "$($c.game)/bin/x64/Cyberpunk2077.exe" 'stock-game';Put "$($c.game)/engine/config/base.ini" 'stock-config';Put "$($c.game)/archive/pc/content/base.archive" 'large-shared'
$stock=@{}
foreach($f in Get-ChildItem -LiteralPath $c.game -Recurse -File){$relative=[IO.Path]::GetRelativePath($c.game,$f.FullName).Replace('\','/');$stock[$relative]=@{sha1=(Get-FileHash -LiteralPath $f.FullName -Algorithm SHA1).Hash;size=[long]$f.Length}}
$stock['tools/redmod/bin/redMod.exe']=@{sha1=('A'*40);size=999}
Write-Json "$($c.bundle)/stock-paths.json" $stock
Write-Json $c.settingsFile @{data=@(@{group_name='/graphics/presets';options=@(@{name='FrameGeneration';type='string_list';value='Off';index=0})});firstMenu='fresh'}
Put $c.userStateFile 'fresh-accepted-eula';Put "$($c.appDataPath)/cache/first-run.dat" 'fresh-cache'
[IO.Directory]::CreateDirectory("$($c.appDataPath)/empty-default")|Out-Null
if(-not(Get-Command Save-CyberpunkCleanBaseline -ErrorAction SilentlyContinue)){throw 'RED: fresh first-run baseline capture is missing'}
$before=(Get-FileHash $c.settingsFile).Hash
$capture=Save-CyberpunkCleanBaseline $c -FirstLaunchConfirmed
Check ($capture.templateId -eq 'clean-flat' -and $capture.privateFileCount -eq 2 -and $capture.sharedFileCount -eq 1) 'fresh baseline captures installed game only, without requiring optional REDmod'
Check ((Get-FileHash $c.settingsFile).Hash -eq $before) 'baseline capture does not rewrite fresh game settings'
$receipt=Read-Json "$($c.data)/profile-store/clean-baseline.json"
Check ($receipt.source -eq 'fresh-first-launch' -and $receipt.userDataFiles -eq 3) 'baseline records full fresh AppData and Saved Games including initial user.gls'
Put "$($c.game)/red4ext/plugins/old.dll" 'foreign-port';Put "$fixture/saves/ManualSave-1/sav.dat" 'A-save';Put $c.settingsFile '{"changed":"A"}'
[IO.Directory]::CreateDirectory("$($c.game)/empty-mod-A/nested")|Out-Null
$b=Invoke-CyberpunkGameProfileAction $c @{action='CreateGameProfile';name='Fresh';vr=$false}
Check (-not(Test-Path "$($c.game)/red4ext/plugins/old.dll") -and -not(Test-Path "$fixture/saves/ManualSave-1/sav.dat") -and (Get-FileHash $c.settingsFile).Hash -eq $before -and [IO.File]::ReadAllText($c.userStateFile) -eq 'fresh-accepted-eula') 'new profile restores captured first-menu settings and removes old port/progress'
Check (-not(Test-Path "$($c.game)/red4ext") -and -not(Test-Path "$fixture/saves/ManualSave-1")) 'return to clean base removes empty mod and save directories left by outgoing files'
Check (-not(Test-Path "$($c.game)/empty-mod-A") -and (Test-Path "$($c.appDataPath)/empty-default")) 'clean profile removes an originally empty foreign directory and keeps a captured empty directory'
Check (-not(Test-Path "$($c.game)/CPVR-Portable/installed.json")) 'clean base contains no synthetic VR receipt or launcher'
$null=Invoke-CyberpunkGameProfileAction $c @{action='SwitchGameProfile';profileId='current'}
Check ([IO.File]::ReadAllText("$($c.game)/red4ext/plugins/old.dll") -eq 'foreign-port' -and [IO.File]::ReadAllText("$fixture/saves/ManualSave-1/sav.dat") -eq 'A-save') 'return restores outgoing profile files and save exactly'
Check (Test-Path "$($c.game)/empty-mod-A/nested") 'return to A recreates its exact empty directory tree'
$rejected=$false;try{Save-CyberpunkCleanBaseline $c -FirstLaunchConfirmed|Out-Null}catch{$rejected=$true}
Check $rejected 'established clean baseline cannot be silently recaptured from a modded profile'
$payload=@{}
foreach($relative in @('red4ext/red4ext.dll','red4ext/plugins/cyberpunkvr_stereo/cyberpunkvr_stereo.dll','red4ext/plugins/cyberpunkvr_stereo/usersettings.json')){
 $text=if($relative.EndsWith('usersettings.json')){'{"authored":"port-preset"}'}else{'fixture native bytes'}
 Put "$fixture/payload.tmp" $text;$sha=(Get-FileHash "$fixture/payload.tmp").Hash;Put "$($c.bundle)/files/$sha" $text;$payload[$relative]=$sha
}
$exeHash=(Get-FileHash "$($c.game)/bin/x64/Cyberpunk2077.exe").Hash
Write-Json "$($c.bundle)/bundle.json" @{schemaVersion=1;exeSha256=$exeHash;defaultVersion='astra-hd-editor-spatial-20261016-r29';versions=@(@{id='flat';kind='flat';available=$true;files=@{}},@{id='astra-hd-editor-spatial-20261016-r29';kind='native';available=$true;files=$payload})}
Put "$($c.bundle)/Start-Game.ps1" '# not executed'
$vr=Invoke-CyberpunkGameProfileAction $c @{action='CreateGameProfile';name='Clean VR';vr=$true}
Check ((Get-FileHash $c.settingsFile).Hash -eq $before -and [IO.File]::ReadAllText($c.userStateFile) -ceq 'fresh-accepted-eula' -and [IO.File]::ReadAllText("$($c.appDataPath)/cache/first-run.dat") -ceq 'fresh-cache') 'VR template keeps the same captured first-menu state instead of replacing it with an authored preset'
$nativeReceipt=Read-Json "$($c.game)/CPVR-Portable/installed.json"
Check ($nativeReceipt.version -ceq 'astra-hd-editor-spatial-20261016-r29') 'Fresh VR profile uses the available native default R29 without a legacy ToFu entry'
$catalog=Get-GameProfileCatalog -StoreConfig (Get-CyberpunkProfileStoreConfig $c)
$flatProfile=@($catalog.profiles|Where-Object name -CEQ 'Fresh')[0]
if(-not $flatProfile){throw 'Captured flat profile missing from actual profile catalog'}
$null=Invoke-CyberpunkGameProfileAction $c @{action='SwitchGameProfile';profileId=$flatProfile.id}
Check (-not(Test-Path "$($c.game)/red4ext") -and (Get-FileHash $c.settingsFile).Hash -eq $before -and [IO.File]::ReadAllText($c.userStateFile) -ceq 'fresh-accepted-eula') 'Return from R29 VR to captured flat restores stock and first-menu user state'
Write-Output ('PASS: evidence '+$fixture)
