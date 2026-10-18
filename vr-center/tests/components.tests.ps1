$ErrorActionPreference='Stop'
. "$PSScriptRoot/../Components.ps1"
function Assert($condition,$message){if(-not $condition){throw $message}}
$root=Join-Path $PSScriptRoot ('fixture-'+[guid]::NewGuid().ToString('N'))
$c=@{game="$root/game";data="$root/data";bundle="$root/bundle";hold="$root/hold.json";vortex="$root/no.exe";vortexData="$root/vortex"}
New-Item -ItemType Directory -Force "$root/game/bin/x64","$root/game/CPVR-Portable","$root/game/archive/pc/mod","$root/bundle/files","$root/data"|Out-Null
Write-Json $c.hold @{active=$false}
[IO.File]::WriteAllText("$root/game/bin/x64/nvngx_dlss.dll",'old-library')
[IO.File]::WriteAllText("$root/new.dll",'new-library')
$newHash=(Get-FileHash "$root/new.dll").Hash;Copy-Item "$root/new.dll" "$root/bundle/files/$newHash"
[IO.File]::WriteAllText("$root/game/archive/pc/mod/core.archive",'required-port')
[IO.File]::WriteAllText("$root/game/archive/pc/mod/mod.archive",'optional-mod')
Write-Json "$root/bundle/bundle.json" @{versions=@(@{id='test';files=@{'bin/x64/nvngx_dlss.dll'=$newHash};portPaths=@('red4ext/plugins/cyberpunkvr_stereo/cyberpunkvr_stereo.dll');kind='native'});managedPaths=@()}
Write-Json "$root/game/CPVR-Portable/installed.json" @{version='test';clean=$false;paths=@('bin/x64/nvngx_dlss.dll','archive/pc/mod/core.archive');critical=@(@{path='bin/x64/nvngx_dlss.dll';sha256=(Get-FileHash "$root/game/bin/x64/nvngx_dlss.dll").Hash})}
$r=@{libraries=@{'nvngx_dlss.dll'=$newHash};mods=$false}
Set-LibrariesAndMods $c $r
Assert ((Get-FileHash "$root/game/bin/x64/nvngx_dlss.dll").Hash -eq $newHash) 'Selected library not applied'
Assert (-not(Test-Path "$root/game/archive/pc/mod/mod.archive")) 'Optional mod not disabled'
Assert (Test-Path "$root/game/archive/pc/mod/core.archive") 'Required port dependency removed'
Assert (((Read-Json "$root/game/CPVR-Portable/installed.json").critical[0].sha256) -eq $newHash) 'Launch receipt mismatch'
$r.mods=$true;Set-LibrariesAndMods $c $r
Assert ([IO.File]::ReadAllText("$root/game/archive/pc/mod/mod.archive") -eq 'optional-mod') 'Mod restore failed'
$r.mods=$false;Set-LibrariesAndMods $c $r
[IO.File]::WriteAllText("$root/game/archive/pc/mod/mod.archive",'changed-by-user')
$r.mods=$true
try{Set-LibrariesAndMods $c $r;throw 'Conflict accepted'}catch{Assert ($_.Exception.Message -match 'Конфликт') 'Wrong conflict error'}
Assert ([IO.File]::ReadAllText("$root/game/archive/pc/mod/mod.archive") -eq 'changed-by-user') 'Existing mod was overwritten'
Write-Json $c.hold @{active=$true}
try{Set-LibrariesAndMods $c $r;throw 'Hold accepted'}catch{Assert ($_.Exception.Message -match 'первый запуск') 'Hold not enforced'}
Write-Output 'PASS: library SHA/receipt; mods off/on; required dependency retained; changed-file conflict; first-launch hold.'
# ZIP import is tested against a temporary bundle, never the real game/catalog.
Add-Type -AssemblyName System.IO.Compression.FileSystem
$z=[IO.Compression.ZipFile]::Open("$root/port.zip",'Create')
$e=$z.CreateEntry('red4ext/plugins/CyberpunkVR_Stereo/CyberpunkVR_Stereo.dll');$w=[IO.StreamWriter]::new($e.Open());$w.Write('test-port');$w.Dispose();$z.Dispose()
$msg=Import-LocalPort $c @{archive="$root/port.zip";name='Test fork';versionLabel='1.0';baseId='test'}
$b=Read-Json "$root/bundle/bundle.json"
Assert ($b.versions.Count -eq 2) 'Imported version missing'
Assert ($b.versions[1].family -eq 'Test fork') 'Imported family missing'
Assert (-not(Test-Path "$root/game/red4ext")) 'Import wrote to game'
Write-Output 'PASS: local ZIP adds a fork/version to the catalog without deployment.'
