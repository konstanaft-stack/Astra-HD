$ErrorActionPreference='Stop'
. "$PSScriptRoot/../GameLaunch.ps1"
function Check($ok,$why){if(-not $ok){throw $why}}
function Reject([scriptblock]$work,$why){$failed=$false;try{& $work|Out-Null}catch{$failed=$true};Check $failed $why}
function Put($p,$text){[IO.Directory]::CreateDirectory((Split-Path $p -Parent))|Out-Null;[IO.File]::WriteAllText($p,$text)}
$fixture=Join-Path $PSScriptRoot ('fixture-renderer-override-'+[guid]::NewGuid().ToString('N'))
try{
 $c=@{game="$fixture/game";data="$fixture/data";mo2=@{root="$fixture/mo2";game="$fixture/game";rendererOverrideMod='VR Center - renderer';executables=@{Cyberpunk2077='bin/x64/Cyberpunk2077.exe'}}}
 Put "$($c.game)/bin/x64/Cyberpunk2077.exe" 'never-execute-fixture'
 Put "$($c.game)/bin/x64/dxgi.dll" 'original-Luke-renderer'
 Put "$($c.mo2.root)/ModOrganizer.exe" 'never-execute-fixture'
 foreach($p in @('VR','Flat')){Put "$($c.mo2.root)/profiles/$p/modlist.txt" "+VR Center - renderer`n+ReShade`n+Ultra Plus`n+[DLSS] Dirt Begone";Put "$($c.mo2.root)/profiles/$p/UserSettings.json" '{}'}
 foreach($m in @('VR Center - renderer','ReShade','Ultra Plus','[DLSS] Dirt Begone')){Put "$($c.mo2.root)/mods/$m/keep.txt" ($m+'-assets')}
 Put "$($c.mo2.root)/mods/ReShade/bin/x64/dxgi.dll" 'ReShade-loader'
 Put "$($c.mo2.root)/mods/Ultra Plus/bin/x64/RealVR.ini" 'unrelated-keep'
 Write-Json "$($c.data)/launch-source.json" @{schema=1;kind='mo2';profiles=@{vr='VR';flat='Flat'};executable='Cyberpunk2077';settingsRelative='UserSettings.json'}
 $hash=(Get-FileHash "$($c.game)/bin/x64/dxgi.dll").Hash
 Write-Json "$($c.game)/CPVR-Portable/installed.json" @{kind='luke';critical=@(@{path='bin/x64/dxgi.dll';sha256=$hash})}
 $owned="$($c.mo2.root)/mods/VR Center - renderer/bin/x64/dxgi.dll"
 $result=Sync-GameLaunchRendererOverride $c
 Check ((Get-FileHash $owned).Hash -eq $hash -and $result.changed) 'Luke original physical DXGI copied into owned VR override'
 Check ([IO.File]::ReadAllText("$($c.mo2.root)/mods/ReShade/bin/x64/dxgi.dll") -ceq 'ReShade-loader') 'ReShade asset loader is unchanged'
 Check ([IO.File]::ReadAllText("$($c.mo2.root)/mods/Ultra Plus/bin/x64/RealVR.ini") -ceq 'unrelated-keep') 'Ultra Plus/RealVR settings are untouched'
 Check (-not (Sync-GameLaunchRendererOverride $c).changed) 'same verified override is a no-op'
 Write-Json "$($c.game)/CPVR-Portable/installed.json" @{kind='flat';critical=@()}
 Check (-not (Sync-GameLaunchRendererOverride $c).changed -and (Get-FileHash $owned).Hash -eq $hash) 'flat mode never modifies VR override'
 Write-Json "$($c.game)/CPVR-Portable/installed.json" @{kind='native';critical=@()}
 $removed=Sync-GameLaunchRendererOverride $c
 Check (-not(Test-Path $owned) -and $removed.changed -and (Test-Path "$($removed.backup)/before.dll")) 'native without DXGI removes only owned override with durable backup'
 Check ([IO.File]::ReadAllText("$($c.mo2.root)/mods/ReShade/bin/x64/dxgi.dll") -ceq 'ReShade-loader') 'native removal keeps ReShade winner intact'
 Put $owned 'unowned-foreign'
 Reject {Sync-GameLaunchRendererOverride $c} 'foreign replacement is not removed'
 Check ([IO.File]::ReadAllText($owned) -ceq 'unowned-foreign') 'foreign bytes stay untouched'
 Remove-Item -LiteralPath $owned
 Write-Json "$($c.game)/CPVR-Portable/installed.json" @{kind='luke';critical=@(@{path='bin/x64/dxgi.dll';sha256=$hash})}
 Put "$($c.mo2.root)/overwrite/bin/x64/dxgi.dll" 'higher-priority-foreign'
 Reject {Sync-GameLaunchRendererOverride $c} 'higher overwrite DXGI rejects before sync'
 Remove-Item -LiteralPath "$($c.mo2.root)/overwrite/bin/x64/dxgi.dll"
 Put "$($c.mo2.root)/profiles/VR/modlist.txt" "+ReShade`n+VR Center - renderer`n+Ultra Plus"
 Reject {Sync-GameLaunchRendererOverride $c} 'owned mod must be first enabled priority'
 'PASS: Luke→native owned override, flat no-op, byte preservation, backups, foreign replacement, overwrite and priority guards. No live/MO2/game activity.'
}finally{if(Test-Path $fixture){Remove-Item -LiteralPath $fixture -Recurse -Force}}
