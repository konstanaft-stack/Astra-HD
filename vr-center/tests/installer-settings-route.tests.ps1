$ErrorActionPreference='Stop'
$app=Split-Path $PSScriptRoot -Parent
. "$app/GameLaunch.ps1"
function Check($ok,$why){if(-not $ok){throw $why}}
function Put($p,$text){[IO.Directory]::CreateDirectory((Split-Path $p -Parent))|Out-Null;[IO.File]::WriteAllText($p,$text)}
$fixture=Join-Path $PSScriptRoot ('fixture-install-settings-'+[guid]::NewGuid().ToString('N'))
$pwsh=(Get-Process -Id $PID).Path
try{
 $bundle="$fixture/bundle";$game="$fixture/game"
 $raw=[IO.File]::ReadAllText("$app/installer/Install.ps1")
 # Only the disposable fixture gets a private mutex and mocked process query.
 Put "$bundle/Install.ps1" ($raw.Replace('Local\IRD.SingleBuildTestDeploy.v1','Local\Fixture.SettingsRoute.'+[guid]::NewGuid().ToString('N')))
 Put "$bundle/Clean-Profile.ps1" 'function Get-CleanProfilePlan {param($Game,$StateDir,$Bundle,$Selected,$Clean,$BackupRelative);@{ExtraRemovals=@();Restores=@();State=@{}}}'
 Put "$bundle/Start-Game.ps1" "throw 'Fixture never launches game'"
 Put "$game/bin/x64/Cyberpunk2077.exe" 'fixture-only-never-execute'
 function Cas($text){Put "$fixture/cas-input" $text;$hash=(Get-FileHash "$fixture/cas-input").Hash;Put "$bundle/files/$hash" $text;return $hash}
 $payload=Cas '{"owner":"payload","options":[{"name":"FrameGeneration","value":"Off","index":0},{"name":"DLSSFrameGen","value":false}]}'
 $config=@{game=$game;data="$fixture/data";mo2=@{root="$fixture/mo2";game=$game;executables=@{Cyberpunk2077='bin/x64/Cyberpunk2077.exe'}}}
 foreach($profile in @('VR','Flat','Opti')){Put "$fixture/mo2/profiles/$profile/modlist.txt" '+dummy';Put "$fixture/mo2/profiles/$profile/UserSettings.json" ('{"owner":"'+$profile+'","options":[{"name":"FrameGeneration","value":"DLSS","index":1},{"name":"DLSSFrameGen","value":true}]}')}
 Put "$fixture/mo2/ModOrganizer.exe" 'fixture-mo2'
 Write-Json "$($config.data)/launch-source.json" @{schema=1;kind='mo2';profiles=@{flat='Flat';vr='VR';flatWithOpti='Opti'};executable='Cyberpunk2077';settingsRelative='UserSettings.json'}
 $meta=@{exeSha256=(Get-FileHash "$game/bin/x64/Cyberpunk2077.exe").Hash;managedPaths=@();modules=@{optiFlat=@{};fgFlat=@{}};versions=@(@{id='flat';kind='flat';available=$true;allowOpti=$true;allowFG=$true;files=@{'@settings'=$payload}},@{id='vr';kind='native';available=$true;files=@{'@settings'=$payload}})}
 Write-Json "$bundle/bundle.json" $meta
 Write-Json "$game/CPVR-Portable/installed.json" @{version='vr';kind='native';preference='vr';paths=@();critical=@();opti=$false}
 $old=Get-GameLaunchEffectiveConfig $config
 $before=(Get-FileHash $old.settingsFile).Hash
 $target=Get-GameLaunchInstallConfig $config 'flat' $false
 Check ($target.settingsFile -eq [IO.Path]::GetFullPath("$fixture/mo2/profiles/Flat/UserSettings.json")) 'requested flat resolves target before installed VR receipt changes'
 Put "$game/CPVR-Portable/preferences/flat/_settings" '{"owner":"STALE","options":[{"name":"FrameGeneration","value":"Off","index":0},{"name":"DLSSFrameGen","value":false}]}'
 function Install($settings,$previous,[bool]$opti,[bool]$fg,[bool]$clean=$false){
   $request=@{settings=$settings;previous=$previous;opti=$opti;fg=$fg;clean=$clean}
   Write-Json "$fixture/request.json" $request
   Put "$fixture/run.ps1" @'
param($Fixture)
function Get-Process {param($Name);@()}
$r=Get-Content "$Fixture/request.json" -Raw|ConvertFrom-Json
& "$Fixture/bundle/Install.ps1" -Version flat -GamePath "$Fixture/game" -SettingsFile $r.settings -PreviousSettingsFile $r.previous -OptiScaler ([bool]$r.opti) -FrameGeneration ([bool]$r.fg) -CleanProfile ([bool]$r.clean) -Hands $false
exit $LASTEXITCODE
'@
   & $pwsh -NoProfile -File "$fixture/run.ps1" $fixture *> "$fixture/install.log"
   return $LASTEXITCODE
 }
 Check ((Install $target.settingsFile $old.settingsFile $false $false) -eq 0) ('explicit settings install failed: '+[IO.File]::ReadAllText("$fixture/install.log"))
 Check ((Read-Json $target.settingsFile).owner -eq 'Flat') 'installation uses target current text, not cached other-profile/payload settings'
 Check (-not (Read-Json $target.settingsFile).options[1].value) 'flat FG policy applies to target only'
 Check ((Get-FileHash $old.settingsFile).Hash -eq $before) 'previous VR source stays byte-identical'
 Check ((Get-FileHash "$game/CPVR-Portable/preferences/vr/_settings").Hash -eq $before) 'previous preference saved from actual old MO2 source'
 $next=Get-GameLaunchInstallConfig $config 'flat' $true
 Check ((Install $next.settingsFile $target.settingsFile $true $true) -eq 0) 'flatWithOpti transition applies'
 Check ((Read-Json $next.settingsFile).owner -eq 'Opti' -and (Read-Json $next.settingsFile).options[1].value) 'Opti target remains its own text with explicit flat FG policy'
 Check ((Get-GameLaunchEffectiveConfig $config).settingsFile -eq $next.settingsFile) 'post-install actual receipt recomputes current Opti target'
 $targetBefore=(Get-FileHash $next.settingsFile).Hash
 Check ((Install $next.settingsFile $target.settingsFile $true $true $true) -ne 0) 'explicit settings plus CleanProfile fails before writes'
 Check ((Get-FileHash $next.settingsFile).Hash -eq $targetBefore) 'clean conflict does not mutate explicit target'
 $wrapper="$fixture/center";[IO.Directory]::CreateDirectory($wrapper)|Out-Null
 foreach($module in @('Install-Selected.ps1','GameLaunch.ps1')){Copy-Item -LiteralPath "$app/$module" -Destination "$wrapper/$module"}
 foreach($module in @('UsbRecovery.ps1','LauncherProcess.ps1')){Put "$wrapper/$module" ''}
 Put "$wrapper/Core.ps1" @'
function Read-Json($Path){if(Test-Path $Path){Get-Content $Path -Raw|ConvertFrom-Json -AsHashtable}}
function Test-CyberpunkProfileMode($Config){$Config.gameProfilesEnabled -eq $true}
function Get-PortState($Config){if($Config.settingsFile -notlike '*\VR\UserSettings.json'){throw 'Wrapper did not inspect old actual settings source'};@{hold=$false;gameRunning=$false;vortexRunning=$false}}
function Assert-ApplyAllowed($State,$Version,$Opti,$FG){}
'@
 $config.gameProfilesEnabled=$true;$config.bundle=$bundle
 Write-Json "$wrapper/config.json" $config
 Write-Json "$fixture/wrapper-request.json" @{version='flat';opti=$false;fg=$false;hands=$false;clean=$false}
 Write-Json "$game/CPVR-Portable/installed.json" @{version='vr';kind='native';preference='vr';paths=@();critical=@();opti=$false}
 Put "$fixture/run-wrapper.ps1" @'
param($Fixture)
function Get-Process {param($Name);@()}
& "$Fixture/center/Install-Selected.ps1" -Request "$Fixture/wrapper-request.json"
exit $LASTEXITCODE
'@
 & $pwsh -NoProfile -File "$fixture/run-wrapper.ps1" $fixture *> "$fixture/wrapper.log"
 Check ($LASTEXITCODE -eq 0) ('wrapper transition failed: '+[IO.File]::ReadAllText("$fixture/wrapper.log"))
 Check ((Read-Json "$game/CPVR-Portable/installed.json").settingsFile -eq $target.settingsFile) 'real Install-Selected forwards requested target settings'
 Check ((Get-FileHash $old.settingsFile).Hash -eq $before) 'wrapper leaves old VR settings unchanged'
 Remove-Item -LiteralPath "$game/CPVR-Portable/installed.json"
 Remove-Item -LiteralPath "$game/CPVR-Portable/preferences/flat/_settings"
 Check ((Install $target.settingsFile $old.settingsFile $false $false) -eq 0) 'first explicit install without old receipt succeeds'
 Check ((Get-FileHash "$game/CPVR-Portable/preferences/flat/_settings").Hash -eq $before) 'original-flat backup also captures previous source rather than target'
 'PASS: requested VR→flat→Opti targets, actual old source preservation/preferences, target text, post-install resolver and clean conflict. No game/AppData access.'
}finally{if(Test-Path $fixture){Remove-Item -LiteralPath $fixture -Recurse -Force}}
