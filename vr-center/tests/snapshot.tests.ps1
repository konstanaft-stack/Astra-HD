$ErrorActionPreference='Stop'
. "$PSScriptRoot/../Core.ps1"
$root=Join-Path $PSScriptRoot ('fixture-'+[guid]::NewGuid().ToString('N'))
$config=@{game="$root/game";bundle="$root/bundle";data="$root/data"}
New-Item -ItemType Directory -Force "$root/game/bin/x64","$root/bundle/files" | Out-Null
[IO.File]::WriteAllText("$root/game/bin/x64/changed.dll",'current-development-binary')
Write-Json "$root/bundle/bundle.json" @{versions=@(@{id='candidate';kind='native';files=@{}});managedPaths=@('bin/x64/changed.dll')}
$r=@{version='candidate';label='Test current';hands=$false;paths=@('bin/x64/changed.dll');critical=@()}
$id=Save-CurrentBuild $config @{receipt=$r}
$b=Read-Json "$root/bundle/bundle.json";$v=$b.versions|Where-Object id -eq $id
$hash=$v.files['bin/x64/changed.dll'];$obj=Join-Path "$root/bundle/files" $hash
if((Get-FileHash $obj).Hash -ne $hash){throw 'Snapshot mismatch'}
if([IO.File]::ReadAllText($obj) -ne 'current-development-binary'){throw 'Wrong installed bytes'}
if([IO.File]::ReadAllText("$root/game/bin/x64/changed.dll") -ne 'current-development-binary'){throw 'Source game changed'}
if(-not(Test-Path "$root/data/catalog-backups/$id.json")){throw 'Catalog backup missing'}
Write-Output 'PASS: current changed bytes preserved; restorable catalog entry; manifest backed up; source unchanged.'
