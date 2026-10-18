$ErrorActionPreference='Stop'
$app=Split-Path $PSScriptRoot -Parent
$module=Join-Path $app 'GameLaunch.ps1'
if(-not(Test-Path $module)){throw 'RED: per-profile launch route is missing'}
. $module
function Check($ok,$why){if(-not $ok){throw $why}}
function Reject([scriptblock]$work,$why){$failed=$false;try{& $work|Out-Null}catch{$failed=$true};Check $failed $why}
function Put($p,$t){[IO.Directory]::CreateDirectory((Split-Path $p -Parent))|Out-Null;[IO.File]::WriteAllText($p,$t)}
$fixture=Join-Path $PSScriptRoot ('fixture-launch-'+[guid]::NewGuid().ToString('N'))
$config=@{game="$fixture/game";data="$fixture/data";gameProfilesEnabled=$true;mo2=@{root="$fixture/mo2";game="$fixture/game";executables=@{Cyberpunk2077='bin/x64/Cyberpunk2077.exe'}}}
try{
 Put "$($config.game)/bin/x64/Cyberpunk2077.exe" 'dummy-exe'
 Put "$($config.game)/CPVR-Portable/Start-Game.ps1" 'dummy-launcher'
 Put "$($config.mo2.root)/profiles/Ultraviolence 3.5/modlist.txt" '+dummy'
 Put "$($config.mo2.root)/profiles/Ultraviolence 3.5/UserSettings.json" '{}'
 function Get-GameLaunchProcesses {param($Name);@()}
 $direct=Get-GameLaunchPlan $config
 Check ($direct.kind -eq 'direct' -and $direct.game -eq [IO.Path]::GetFullPath($config.game)) 'missing source resolves direct without forcing MO2'
 Put "$($config.data)/launch-source.json" '{"schema":1,"kind":"mo2","profile":"Ultraviolence 3.5","executable":"Cyberpunk2077","settingsRelative":"UserSettings.json"}'
 $mo=Get-GameLaunchPlan $config
 Put "$($config.mo2.root)/mods/dummy/keep.txt" 'dummy'
 Assert-GameLaunchComposition $config $mo
 Put "$($config.game)/red4ext/plugins/cyberpunkvr_stereo/cyberpunkvr_stereo.dll" 'unselected-native'
 Reject {Assert-GameLaunchComposition $config $mo} 'unmanaged flat collection rejects a known VR renderer'
 Remove-Item -LiteralPath "$($config.game)/red4ext/plugins/cyberpunkvr_stereo/cyberpunkvr_stereo.dll"
 Put "$($config.game)/bin/x64/dxgi.dll" 'Luke-loader'
 $dxgi=(Get-FileHash "$($config.game)/bin/x64/dxgi.dll").Hash
 $receipt=@{kind='luke';version='luke-fixture';critical=@(@{path='bin/x64/dxgi.dll';sha256=$dxgi})}
 Write-Json "$($config.game)/CPVR-Portable/installed.json" $receipt
 Put "$($config.mo2.root)/mods/dummy/keep.txt" 'dummy'
 $config.mo2.profileEvidence=@{'Ultraviolence 3.5'=@{modlistSha256=(Get-FileHash "$($mo.profileRoot)/modlist.txt").Hash;shadowDxgiChecked=$true;files=@(@{path="$($config.game)/bin/x64/dxgi.dll";sha256=$dxgi});version='luke-fixture'}}
 Assert-GameLaunchComposition $config $mo
 $config.bundle="$fixture/bundle"
 $otherRenderer='red4ext/plugins/cyberpunkvr_next/native.dll'
 Write-Json "$($config.bundle)/bundle.json" @{versions=@(@{kind='native';files=@{$otherRenderer=('0'*64);'red4ext/plugins/ordinary/ordinary.dll'=('1'*64)}},@{kind='luke';rendererPath='bin/x64/dxgi.dll';files=@{}})}
 Put "$($config.game)/red4ext/plugins/ordinary/ordinary.dll" 'ordinary-plugin'
 Assert-GameLaunchComposition $config $mo
 Put "$($config.game)/$otherRenderer" 'another-bundled-native-renderer'
 Reject {Assert-GameLaunchComposition $config $direct} 'known renderer from bundle versions rejects alongside valid Luke'
 Remove-Item -LiteralPath "$($config.game)/$otherRenderer"
 $nativeRelative='red4ext/plugins/cyberpunkvr_stereo/cyberpunkvr_stereo.dll'
 Put "$($config.game)/$nativeRelative" 'additional-Astra'
 Reject {Assert-GameLaunchComposition $config $direct} 'valid direct Luke rejects an additional physical Astra renderer'
 Reject {Assert-GameLaunchComposition $config $mo} 'valid MO2 Luke rejects an additional physical Astra renderer'
 Remove-Item -LiteralPath "$($config.game)/$nativeRelative"
 $ownRelative='red4ext/plugins/cpvrown/cpvrown.dll'
 Put "$($config.mo2.root)/overwrite/$ownRelative" 'additional-own-renderer'
 Reject {Assert-GameLaunchComposition $config $mo} 'valid MO2 Luke rejects an additional renderer from overwrite'
 Remove-Item -LiteralPath "$($config.mo2.root)/overwrite/$ownRelative"
 Put "$($config.mo2.root)/mods/dummy/r6/scripts/ordinary.reds" 'ordinary collection script'
 Put "$($config.mo2.root)/mods/dummy/bin/x64/ReShade.ini" 'ordinary non-renderer configuration'
 Assert-GameLaunchComposition $config $mo
 $receipt.kind='flat';Write-Json "$($config.game)/CPVR-Portable/installed.json" $receipt
 Put "$($config.mo2.root)/mods/dummy/$nativeRelative" 'additional-Astra'
 Reject {Assert-GameLaunchComposition $config $mo} 'flat rejects active-mod Astra renderer'
 Remove-Item -LiteralPath "$($config.mo2.root)/mods/dummy/$nativeRelative"
 $receipt.kind='native'
 Put "$($config.game)/$nativeRelative" 'native-correct'
 $receipt.critical+=@{path=$nativeRelative;sha256=(Get-FileHash "$($config.game)/$nativeRelative").Hash}
 Write-Json "$($config.game)/CPVR-Portable/installed.json" $receipt
 Put "$($config.mo2.root)/mods/dummy/$nativeRelative" 'native-shadow'
 Reject {Assert-GameLaunchComposition $config $mo} 'every native critical path checks effective MO2 winner'
 Remove-Item -LiteralPath "$($config.mo2.root)/mods/dummy/$nativeRelative"
 Put "$($config.game)/bin/x64/dxgi.dll" 'damaged-loader'
 Reject {Assert-GameLaunchComposition $config $mo} 'array receipt critical SHA is checked before dispatch'
 Put "$($config.game)/bin/x64/dxgi.dll" 'Luke-loader'
 Put "$($config.mo2.root)/mods/dummy/bin/x64/dxgi.dll" 'ReShade-shadow'
 Reject {Assert-GameLaunchComposition $config $mo} 'new enabled-mod DXGI shadow is detected without modlist change'
 Remove-Item -LiteralPath "$($config.mo2.root)/mods/dummy/bin/x64/dxgi.dll"
 Put "$($config.mo2.root)/mods/highest/bin/x64/dxgi.dll" 'Luke-loader'
 Put "$($mo.profileRoot)/modlist.txt" "+highest`n+dummy"
 Check ((Resolve-GameLaunchEffectiveFile $config $mo 'bin/x64/dxgi.dll') -eq [IO.Path]::GetFullPath("$($config.mo2.root)/mods/highest/bin/x64/dxgi.dll")) 'first enabled modlist row wins in v2.5.2'
 Assert-GameLaunchComposition $config $mo
 Put "$($config.mo2.root)/overwrite/bin/x64/dxgi.dll" 'ReShade-shadow'
 Reject {Assert-GameLaunchComposition $config $mo} 'overwrite has higher priority than enabled mods'
 Remove-Item -LiteralPath "$($config.mo2.root)/overwrite/bin/x64/dxgi.dll"
 $previousNativeHash=$receipt.critical[1].sha256
 Put "$($config.game)/$nativeRelative" 'new-port-native'
 $receipt.version='new-port';$receipt.critical[1].sha256=(Get-FileHash "$($config.game)/$nativeRelative").Hash
 Write-Json "$($config.game)/CPVR-Portable/installed.json" $receipt
 $config.mo2.profileEvidence['Ultraviolence 3.5'].files+=@{path="$($config.game)/$nativeRelative";sha256=$previousNativeHash}
 Assert-GameLaunchComposition $config $mo
 Check ($mo.engine -ceq 'usvfs' -and -not $mo.launcher -and $mo.arguments.Count -eq 0) 'collection uses standalone usvfs without ModOrganizer executable'
 Check ($mo.settingsFile -eq [IO.Path]::GetFullPath("$($config.mo2.root)/profiles/Ultraviolence 3.5/UserSettings.json")) 'effective settings belong to selected MO2 profile'
 function Get-GameLaunchProcesses {param($Name);if('ModOrganizer' -in $Name){@(@{pid=88;exe='otherMO2';startedUtc=[datetime]::UtcNow.ToString('o')})}}
 Reject {Get-GameLaunchPlan $config} 'existing primary MO2 cannot silently ignore selected profile'
 function Get-GameLaunchProcesses {param($Name);@()}
 $old=@{pid=12;exe=$mo.exe;startedUtc='2026-10-01T00:00:00Z'}
 Reject {Select-NewGameLaunchIdentity @($old) @($old) $mo.exe ([datetime]::Parse('2026-10-11T00:00:00Z'))} 'existing game PID cannot be adopted'
 $new=@{pid=13;exe=$mo.exe;startedUtc='2026-10-11T00:00:01Z'}
 Check ((Select-NewGameLaunchIdentity @($new) @($old) $mo.exe ([datetime]::Parse('2026-10-11T00:00:00Z'))).pid -eq 13) 'new PID requires expected executable and fresh start'
 Reject {Select-NewGameLaunchIdentity @($new,@{pid=14;exe=$mo.exe;startedUtc=$new.startedUtc}) @() $mo.exe ([datetime]::Parse('2026-10-11T00:00:00Z'))} 'ambiguous new game is rejected'
 $script:dispatched=$false;$script:dispatches=0
 function Invoke-VrLauncher {param($Config,$Log,[switch]$CheckOnly);if(-not $CheckOnly){throw 'Fixture forbids actual direct launch'};@{exitCode=0;output='Fixture CheckOnly'}}
 function Start-GameLaunchDispatcher {param($Config,$Plan,$Log);$script:dispatched=$true;$script:dispatches++;@{dispatcherPid=77}}
 function Get-GameLaunchProcesses {param($Name);if('Cyberpunk2077' -in $Name -and $script:dispatched){@(@{pid=13;exe=$mo.exe;start=[datetime]::UtcNow.ToString('o');startedUtc=[datetime]::UtcNow.ToString('o')})}}
 # The user may replace NR/FG using another manager, leaving the center receipt stale.
 $externalIni="[DlssNr]`nEnabled=true`n[FrameGen]`nEnabled=false`nUserSetting=keep"
 Put "$($config.game)/bin/x64/OptiScaler.ini" $externalIni
 Put "$($config.game)/bin/x64/dbghelp.dll" 'externally-installed-loader'
 Write-Json "$($config.data)/game-settings/enhancement/state.json" @{schema=1;enabled=$true;phase='ready';files=@(@{path='bin/x64/dbghelp.dll';sha256=('0'*64);mutable=$false},@{path='bin/x64/OptiScaler/streamline/sl.interposer.dll';sha256=('1'*64);mutable=$false})}
 $launched=Invoke-GameProfileLaunch $config "$fixture/launch.log"
 Check ($launched.game.pid -eq 13 -and $script:dispatches -eq 1 -and $launched.route -eq 'mo2') 'dispatch returns actual game identity rather than MO2 PID'
 Check ([IO.File]::ReadAllText("$($config.game)/bin/x64/OptiScaler.ini") -ceq $externalIni) 'launch preserves external NR/FG configuration without applying center defaults'
 Check ([IO.File]::ReadAllText("$($config.game)/bin/x64/dbghelp.dll") -ceq 'externally-installed-loader') 'launch does not replace the user loader'
 function Get-GameLaunchProcesses {param($Name);if('Cyberpunk2077' -in $Name){@($old)}}
 Reject {Invoke-GameProfileLaunch $config "$fixture/launch.log"} 'existing game rejects dispatch'
 Check ($script:dispatches -eq 1) 'existing PID rejection occurs before launching a process'
 function Get-GameLaunchProcesses {param($Name);@()}
 Put "$($config.mo2.root)/profiles/Flat/modlist.txt" '+dummy'
 Put "$($config.mo2.root)/profiles/Flat/UserSettings.json" '{"flat":true}'
 Put "$($config.data)/launch-source.json" '{"schema":1,"kind":"mo2","profiles":{"flat":"Flat","vr":"Ultraviolence 3.5"},"executable":"Cyberpunk2077","settingsRelative":"UserSettings.json"}'
 Check ((Get-GameLaunchPlan $config).source.profile -ceq 'Ultraviolence 3.5') 'installed Luke selects VR profile independent of UI draft'
 $receipt.kind='flat';$receipt.opti=$false;Write-Json "$($config.game)/CPVR-Portable/installed.json" $receipt
 Check ((Get-GameLaunchPlan $config).source.profile -ceq 'Flat') 'after install new flat receipt selects flat profile'
 Check ((Get-GameLaunchEffectiveConfig $config).settingsFile -eq [IO.Path]::GetFullPath("$($config.mo2.root)/profiles/Flat/UserSettings.json")) 'explicit settings recompute target from new receipt'
 $receipt.opti=$true;Write-Json "$($config.game)/CPVR-Portable/installed.json" $receipt
 Reject {Get-GameLaunchPlan $config} 'missing flatWithOpti never falls back to authored flat or VR'
 $receipt.kind='unrecognized';Write-Json "$($config.game)/CPVR-Portable/installed.json" $receipt
 Reject {Get-GameLaunchPlan $config} 'unknown receipt mode fails closed'
 Put "$($config.data)/launch-source.json" '{"schema":1,"kind":"mo2","profile":"../escape","executable":"Cyberpunk2077"}'
 Reject {Get-GameLaunchPlan $config} 'profile path traversal is rejected'
 Put "$($config.data)/launch-source.json" '{"schema":1,"kind":"unknown"}'
 Reject {Get-GameLaunchPlan $config} 'unknown source does not silently fall back'
 'PASS: additional physical/overwrite/active-mod/bundle native renderers rejected for Luke/flat; selected native and ordinary mods allowed; route/PID/critical/priority/settings contracts retained.'
}finally{if(Test-Path $fixture){$cleanup=(Get-Item -LiteralPath $fixture).FullName;if([IO.Path]::GetDirectoryName($cleanup) -ine [IO.Path]::GetFullPath($PSScriptRoot) -or [IO.Path]::GetFileName($cleanup) -notmatch '^fixture-launch-[a-f0-9]{32}$'){throw 'Fixture cleanup escaped tests directory'};Remove-Item -LiteralPath $cleanup -Recurse -Force}}
