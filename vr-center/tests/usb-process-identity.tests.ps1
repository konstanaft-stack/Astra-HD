$ErrorActionPreference='Stop'
. "$PSScriptRoot/../UsbRecovery.ps1"
function Assert($ok,$message){if(-not $ok){throw $message}}
$script:nativeStamp=[datetime]::Parse('2026-10-10T16:15:46.2198759Z').ToUniversalTime()
$script:cimStamp=[datetime]::Parse('2026-10-10T16:15:46.2198750Z').ToUniversalTime()
$script:dead=$false;$script:disposed=0;$script:nativeName='Cyberpunk2077'
function Get-CimInstance {
    [pscustomobject]@{ProcessId=777;CreationDate=$script:cimStamp;Name='Cyberpunk2077.exe';ExecutablePath='C:\fixture\Cyberpunk2077.exe'}
}
function Get-Process {
    param([int]$Id)
    $p=[pscustomobject]@{Id=$Id;ProcessName=$script:nativeName}
    $p|Add-Member ScriptMethod get_StartTime {$script:nativeStamp}
    $p|Add-Member ScriptMethod get_HasExited {$script:dead}
    $p|Add-Member ScriptMethod Dispose {$script:disposed++}
    $p
}
$result=Get-UsbHelperIdentity 777
Assert ($result.start -ceq $script:nativeStamp.ToString('o')) 'USB binding lost native 100-ns creation time: Watch rejects the same process'
Assert ($script:disposed -eq 1) 'Identity lookup leaked its process handle'
$script:cimStamp=$script:cimStamp.AddSeconds(-1)
Assert ($null -eq (Get-UsbHelperIdentity 777)) 'Reused PID accepted with stale CIM metadata'
$script:cimStamp=$script:nativeStamp.AddTicks(-9)
$script:nativeName='unrelated'
Assert ($null -eq (Get-UsbHelperIdentity 777)) 'Different process name accepted'
$script:nativeName='Cyberpunk2077';$script:dead=$true
Assert ($null -eq (Get-UsbHelperIdentity 777)) 'Exited process accepted'
'PASS: exact native creation time, stale metadata/name/exit rejection, handle disposal.'
