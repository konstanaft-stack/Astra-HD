. "$PSScriptRoot/Core.ps1"
function Get-UsbRecoveryPaths($Config){
    $helper=Join-Path $Config.bundle 'tools/VD-USB'
    @{helper=$helper;watch=(Join-Path $helper 'Watch-UsbRecovery.ps1');preference=(Join-Path $Config.data 'usb-recovery.json');receipt=(Join-Path $Config.data 'usb-session.json');runs=(Join-Path $Config.data 'usb-recovery')}
}
function Get-UsbHelperIdentity([int]$HelperPid){
    $process=Get-Process -Id $HelperPid -ErrorAction SilentlyContinue
    if(-not $process){return $null}
    try{
        # Watch uses GetProcessTimes through Process.StartTime (100 ns). CIM's
        # CreationDate drops the last digit; never use it as the binding token.
        $started=$process.get_StartTime().ToUniversalTime()
        $rows=@(Get-CimInstance Win32_Process -Filter ('ProcessId='+$HelperPid) -Property ProcessId,CreationDate,Name,ExecutablePath)
        if($rows.Count -ne 1 -or -not $rows[0].CreationDate -or $process.get_HasExited()){return $null}
        $name=[IO.Path]::GetFileNameWithoutExtension($rows[0].Name)
        $cimTicks=([datetime]$rows[0].CreationDate).ToUniversalTime().Ticks
        # Check CIM metadata belongs to the same process at CIM precision, while
        # retaining the exact native token for Watch and all later PID checks.
        if($process.ProcessName -ne $name -or ($started.Ticks-($started.Ticks%10)) -ne ($cimTicks-($cimTicks%10))){return $null}
        return @{pid=$HelperPid;start=$started.ToString('o');name=$name;path=$rows[0].ExecutablePath}
    }finally{$process.Dispose()}
}
function Read-UsbRecoveryHelper($Config){
    $p=Get-UsbRecoveryPaths $Config;$r=Read-Json $p.receipt
    if(-not $r){return $null}
    if($r.owner -ne 'VRPorts.UsbGameSession.v1' -or [int]$r.pid -le 0 -or -not $r.start){throw 'Не удалось проверить USB-монитор.'}
    $actual=Get-UsbHelperIdentity ([int]$r.pid)
    if(-not $actual -or $actual.name -notin @('powershell','pwsh') -or ([datetime]$actual.start).ToUniversalTime().Ticks -ne ([datetime]$r.start).ToUniversalTime().Ticks){return $null}
    $root=[IO.Path]::GetFullPath($r.logRoot)
    if(-not $root.StartsWith([IO.Path]::GetFullPath($p.runs).TrimEnd('\')+'\',[StringComparison]::OrdinalIgnoreCase)){throw 'USB-журнал вне каталога VR-центра.'}
    @{receipt=$r;stop=(Join-Path $root 'STOP');status=(Read-Json (Join-Path $root 'status.json'))}
}
function Get-UsbRecoveryState($Config){
    $p=Get-UsbRecoveryPaths $Config;$saved=Read-Json $p.preference;$helper=$null;$errorText=''
    try{$helper=Read-UsbRecoveryHelper $Config}catch{$errorText=$_.Exception.Message}
    $running=[bool]($helper -and $helper.status -and $helper.status.status -notin @('FAILED','GAME_EXITED','STOPPED_BY_USER','STOPPED_BEFORE_RESET') -and -not(Test-Path -LiteralPath $helper.stop))
    @{enabled=[bool]$saved.enabled;running=$running;available=(Test-Path -LiteralPath $p.watch);error=$errorText}
}
function Get-UsbRecoveryGame($Config){
    $games=@(Get-Process Cyberpunk2077 -ErrorAction SilentlyContinue)
    try{
        if($games.Count -gt 1){throw 'Обнаружено несколько процессов Cyberpunk.'}
        if(-not $games.Count){return $null}
        $identity=Get-UsbHelperIdentity $games[0].Id
        $expected=[IO.Path]::GetFullPath((Join-Path $Config.game 'bin/x64/Cyberpunk2077.exe'))
        if(-not $identity -or $identity.name -ne 'Cyberpunk2077' -or -not $identity.path -or [IO.Path]::GetFullPath($identity.path) -ne $expected){throw 'Не удалось проверить процесс игры для USB-монитора.'}
        $identity
    }finally{foreach($game in $games){$game.Dispose()}}
}
function Start-UsbRecoveryForGame($Config,$Game){
    if(-not $Game){throw 'USB-монитор запускается только вместе с игрой.'}
    $actual=Get-UsbRecoveryGame $Config
    if(-not $actual -or $actual.pid -ne $Game.pid -or $actual.start -ne $Game.start){throw 'Процесс игры уже изменился.'}
    $live=Read-UsbRecoveryHelper $Config
    if($live){
        if($live.receipt.gamePid -eq $Game.pid -and ([datetime]$live.receipt.gameStart).ToUniversalTime().Ticks -eq ([datetime]$Game.start).ToUniversalTime().Ticks -and $live.status -and $live.status.status -notin @('FAILED','GAME_EXITED','STOPPED_BY_USER','STOPPED_BEFORE_RESET') -and -not(Test-Path -LiteralPath $live.stop)){return}
        throw 'Предыдущий USB-монитор ещё завершает работу.'
    }
    $p=Get-UsbRecoveryPaths $Config
    foreach($file in @('Watch-UsbRecovery.ps1','Usb-Recovery.Sessions.ps1','Usb-Recovery.Policy.ps1')){if(-not(Test-Path -LiteralPath (Join-Path $p.helper $file))){throw ('Нет файла USB-помощника: '+$file)}}
    [void][IO.Directory]::CreateDirectory($p.runs)
    $run=Join-Path $p.runs ([guid]::NewGuid().ToString('N'))
    $exe=Join-Path $env:SystemRoot 'System32/WindowsPowerShell/v1.0/powershell.exe'
    # User-authorized policy only for this process; global policy unchanged.
    $child=Start-Process -FilePath $exe -Verb RunAs -WindowStyle Hidden -PassThru -ArgumentList @('-NoProfile','-ExecutionPolicy','Bypass','-WindowStyle','Hidden','-File',('"'+$p.watch+'"'),'-GamePid',[string]$Game.pid,'-ExpectedGameName','Cyberpunk2077','-ExpectedGameStart',$Game.start,'-LogDirectory',('"'+$run+'"'),'-StopFile',('"'+(Join-Path $run 'STOP')+'"'),'-AutoRecover')
    $timer=[Diagnostics.Stopwatch]::StartNew()
    try{
        $identity=Get-UsbHelperIdentity $child.Id
        if(-not $identity){throw 'USB-монитор завершился при запуске.'}
        Write-Json $p.receipt @{owner='VRPorts.UsbGameSession.v1';pid=$child.Id;start=$identity.start;gamePid=$Game.pid;gameStart=$Game.start;logRoot=$run}
        while($timer.Elapsed.TotalSeconds -lt 12){
            $status=Read-Json (Join-Path $run 'status.json')
            if($status -and $status.status -eq 'FAILED'){throw ('USB: '+$status.error)}
            if($child.HasExited){throw ('USB-монитор завершился: '+$child.ExitCode)}
            if($status -and $status.game_started_at -and ([datetime]$status.game_started_at).ToUniversalTime().Ticks -eq ([datetime]$Game.start).ToUniversalTime().Ticks){return}
            Start-Sleep -Milliseconds 200
        }
        if(Test-Path -LiteralPath $run){[IO.File]::WriteAllText((Join-Path $run 'STOP'),'Startup timed out')}
        throw 'USB-монитор не подтвердил подключение к игре.'
    }finally{$child.Dispose()}
}
function Stop-UsbRecoveryHelper($Config){
    $helper=Read-UsbRecoveryHelper $Config
    if(-not $helper){return}
    [IO.File]::WriteAllText($helper.stop,'Stopped from VR-center')
    $timer=[Diagnostics.Stopwatch]::StartNew()
    do{Start-Sleep -Milliseconds 200;$live=Read-UsbRecoveryHelper $Config}while($live -and $timer.Elapsed.TotalSeconds -lt 8)
    if($live){throw 'Остановка USB-монитора запрошена, но ещё не подтверждена.'}
}
function Set-UsbRecoveryEnabled($Config,[bool]$Enabled){
    $mutex=[Threading.Mutex]::new($false,'Local.VRPorts.UsbRecovery');$held=$false
    try{
        try{$held=$mutex.WaitOne(0)}catch [Threading.AbandonedMutexException]{$held=$true}
        if(-not $held){throw 'USB-помощник уже переключается.'}
        if($Enabled){$game=Get-UsbRecoveryGame $Config;if($game){Start-UsbRecoveryForGame $Config $game}}else{Stop-UsbRecoveryHelper $Config}
        Write-Json (Get-UsbRecoveryPaths $Config).preference @{enabled=$Enabled}
    }finally{if($held){$mutex.ReleaseMutex()};$mutex.Dispose()}
}
