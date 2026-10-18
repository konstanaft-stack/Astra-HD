$ErrorActionPreference='Stop'
. "$PSScriptRoot/../Core.ps1"
function Assert($ok,$message){if(-not $ok){throw $message}}
$version=@{id='test';available=$true;allowOpti=$true;allowFG=$false}
$state=@{hold=$true;gameRunning=$false;vortexRunning=$false;deploymentCount=0}
try { Assert-ApplyAllowed $state $version $false $false; throw 'HOLD not enforced' } catch { Assert ($_.Exception.Message -match 'первого запуска') 'hold failure' }
$state.hold=$false;$state.gameRunning=$true
try { Assert-ApplyAllowed $state $version $false $false; throw 'LIVE not enforced' } catch { Assert ($_.Exception.Message -match 'игру') 'live failure' }
$state.gameRunning=$false;$state.vortexRunning=$true
try { Assert-ApplyAllowed $state $version $false $false; throw 'VORTEX not enforced' } catch { Assert ($_.Exception.Message -match 'Vortex') 'vortex failure' }
$state.vortexRunning=$false
try { Assert-ApplyAllowed $state $version $false $true; throw 'FG not enforced' } catch { Assert ($_.Exception.Message -match 'подготовлен') 'fg failure' }
$state.deploymentCount=1
Assert-ApplyAllowed $state $version $true $false # A closed manager is not required for port replacement.
$state.deploymentCount=0
Assert-ApplyAllowed $state $version $true $false
Assert ((Resolve-ChildPath 'C:\game' 'bin/x64/file.dll') -eq 'C:\game\bin\x64\file.dll') 'safe path'
foreach($path in @('../outside','C:\other','bin/../../oops')){
 try { $null=Resolve-ChildPath 'C:\game' $path; throw 'ESCAPE accepted' } catch { Assert ($_.Exception.Message -match 'границы') 'path escape' }
}
Write-Output 'PASS: hold, live game, Vortex, unsupported FG, deployment, valid plan, path containment.'
$fixture=Join-Path $PSScriptRoot ('fixture-'+[guid]::NewGuid().ToString('N'))
[IO.Directory]::CreateDirectory($fixture)|Out-Null
Write-Json "$fixture/empty.json" @{files=@()}
Write-Json "$fixture/active.json" @{files=@(@{relPath='test.dll'})}
Write-Json "$fixture/unknown.json" @{unrecognized=$true}
Assert ((Get-ActiveDeploymentCount @("$fixture/empty.json")) -eq 0) 'Purge with empty manifest should unblock'
Assert ((Get-ActiveDeploymentCount @("$fixture/active.json")) -eq 1) 'Deployment count remains available for status'
Assert ((Get-ActiveDeploymentCount @("$fixture/unknown.json")) -eq 1) 'Unknown schema must not be considered clean'
Write-Output 'PASS: deployment inventory remains available independently of port switching.'
