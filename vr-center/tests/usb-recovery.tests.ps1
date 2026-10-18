$ErrorActionPreference='Stop'
. "$PSScriptRoot/../UsbRecovery.ps1"
function Assert($ok,$message){if(-not $ok){throw $message}}
$root=Join-Path ([IO.Path]::GetTempPath()) ('vr-usb-session-test-'+[guid]::NewGuid().ToString('N'))
$c=@{data="$root/data";bundle="$root/bundle";game="$root/game"}
$p=Get-UsbRecoveryPaths $c
[void][IO.Directory]::CreateDirectory($p.helper)
foreach($file in @('Watch-UsbRecovery.ps1','Usb-Recovery.Sessions.ps1','Usb-Recovery.Policy.ps1')){[IO.File]::WriteAllText((Join-Path $p.helper $file),'# fixture')}
$script:starts=0;$script:game=$null;$script:monitorAlive=$true
$stamp=[DateTime]::UtcNow.ToString('o')
function Get-UsbRecoveryGame($Config){$script:game}
function Start-Process {$script:starts++;throw 'UAC cancelled'}
function Get-UsbHelperIdentity([int]$HelperPid){if($script:monitorAlive -and -not(Test-Path (Join-Path $p.runs 'fixture/STOP'))){@{start=$stamp;name='powershell'}}}
Assert (-not (Get-UsbRecoveryState $c).enabled) 'Default must be off'
Set-UsbRecoveryEnabled $c $true
Assert (Get-UsbRecoveryState $c).enabled 'Choice not saved'
Assert ($script:starts -eq 0) 'Helper started outside a game'
Set-UsbRecoveryEnabled $c $false
$script:game=@{pid=789;start=$stamp}
try{Set-UsbRecoveryEnabled $c $true;throw 'Expected UAC cancellation'}catch{Assert ($_.Exception.Message -eq 'UAC cancelled') 'Wrong startup error'}
Assert (-not (Get-UsbRecoveryState $c).enabled) 'Cancelled start persisted enabled'
$run=Join-Path $p.runs 'fixture'
Write-Json $p.receipt @{owner='VRPorts.UsbGameSession.v1';pid=123;start=$stamp;gamePid=789;gameStart=$stamp;logRoot=$run}
Write-Json (Join-Path $run 'status.json') @{status='HEALTHY';game_pid=789;game_started_at=$stamp}
Set-UsbRecoveryEnabled $c $true
Assert ($script:starts -eq 1) 'Duplicate helper started for same game'
Assert (Get-UsbRecoveryState $c).running 'Live helper not recognized'
$script:monitorAlive=$false;$script:game=$null
Assert (-not (Get-UsbRecoveryState $c).running) 'Dead helper displayed as running'
Assert (Get-UsbRecoveryState $c).enabled 'Game exit forgot checkbox choice'
Assert ($script:starts -eq 1) 'Game exit restarted helper'
Set-UsbRecoveryEnabled $c $false
Assert (-not(Test-Path (Join-Path $run 'STOP'))) 'Stale PID was signalled'
$script:monitorAlive=$true
Set-UsbRecoveryEnabled $c $false
Assert (Test-Path (Join-Path $run 'STOP')) 'Live helper did not receive STOP'
'PASS: no helper outside game; choice survives game exit; no automatic rearm; UAC cancellation; same-game reuse; stale PID; explicit STOP. No network operations.'
