$ErrorActionPreference='Stop'
. "$PSScriptRoot/../LauncherProcess.ps1"
$root=Join-Path ([IO.Path]::GetTempPath()) ('vr-launch-lifetime-'+[guid]::NewGuid().ToString('N'))
[void][IO.Directory]::CreateDirectory((Join-Path $root 'game/CPVR-Portable'))
$exe=(Get-Process -Id $PID).Path
$fixture=@'
param([switch]$WatchStartup,[switch]$CheckOnly)
if($CheckOnly){'Fixture CheckOnly';exit 0}
$start=[Diagnostics.ProcessStartInfo]::new()
$start.FileName=(Get-Process -Id $PID).Path
$start.UseShellExecute=$false;$start.CreateNoWindow=$true
$start.ArgumentList.Add('-NoProfile');$start.ArgumentList.Add('-Command');$start.ArgumentList.Add('Start-Sleep -Seconds 10')
$child=[Diagnostics.Process]::Start($start)
Write-Output ('Fixture PID '+$child.Id)
'@
[IO.File]::WriteAllText((Join-Path $root 'game/CPVR-Portable/Start-Game.ps1'),$fixture)
$config=@{game=(Join-Path $root 'game');pwsh=$exe}
$check=Invoke-VrLauncher $config (Join-Path $root 'check.log') -CheckOnly
if($check.output.Trim() -ne 'Fixture CheckOnly'){throw 'CheckOnly contract was not forwarded: a launch happened'}
$clock=[Diagnostics.Stopwatch]::StartNew()
$r=Invoke-VrLauncher $config (Join-Path $root 'launch.log')
$elapsed=$clock.Elapsed.TotalSeconds
$match=[regex]::Match($r.output,'Fixture PID (\d+)')
if(-not $match.Success){throw 'Launcher output was lost'}
$child=Get-Process -Id ([int]$match.Groups[1].Value) -ErrorAction Stop
try{
    if($elapsed -ge 7 -or $child.HasExited){throw 'Launcher waited for the child lifetime; USB attachment would be delayed'}
    if(-not $child.WaitForExit(15000)){throw 'Harmless test child did not finish'}
}finally{$child.Dispose()}
"PASS: launcher returned in $([math]::Round($elapsed,2))s while its 10-second child remained alive; output preserved. No game/USB activity."
