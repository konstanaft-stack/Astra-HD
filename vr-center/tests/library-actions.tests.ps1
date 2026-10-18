$ErrorActionPreference='Stop'
. "$PSScriptRoot/../LibraryActions.ps1"
$d=Join-Path $PSScriptRoot ('fixture-'+[guid]::NewGuid().ToString('N'))
$c=@{game="$d/game";bundle="$d/bundle";data="$d/data"}
Write-Json "$d/game/CPVR-Portable/installed.json" @{version='active'}
Write-Json "$d/bundle/bundle.json" @{versions=@(@{id='flat';available=$true},@{id='active';available=$true},@{id='old';available=$true;files=@{'shared.dll'='abc'}})}
foreach($id in @('flat','active')){try{$null=Remove-PortVersions $c @($id);throw 'guard failed'}catch{if($_.Exception.Message -eq 'guard failed'){throw}}}
$null=Remove-PortVersions $c @('old')
$b=Read-Json "$d/bundle/bundle.json"
if(-not $b.versions[2].archived -or $b.versions[2].available){throw 'Old build still visible'}
if($b.versions[2].files['shared.dll'] -ne 'abc'){throw 'Shared dependency definition deleted'}
if((Read-Json "$d/game/CPVR-Portable/installed.json").version -ne 'active'){throw 'Installed game changed'}
if(@(Get-ChildItem "$d/data/library-trash" -File).Count -ne 1){throw 'Recovery entry missing'}
'PASS: active and flat protected; old build removed from visible catalog; recovery and shared files preserved.'
