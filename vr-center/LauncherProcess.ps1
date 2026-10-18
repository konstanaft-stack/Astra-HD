function Read-LauncherOutput([string]$Path){
    if(-not(Test-Path -LiteralPath $Path)){return ''}
    $stream=[IO.File]::Open($Path,[IO.FileMode]::Open,[IO.FileAccess]::Read,([IO.FileShare]::ReadWrite -bor [IO.FileShare]::Delete))
    $reader=[IO.StreamReader]::new($stream)
    try{return $reader.ReadToEnd()}finally{$reader.Dispose()}
}
function Invoke-VrLauncher($Config,[string]$Log,[switch]$WatchStartup,[switch]$CheckOnly){
    $script=Join-Path $Config.game 'CPVR-Portable/Start-Game.ps1'
    if(-not $CheckOnly -and $Config.launchArguments.Count){
        $body=[IO.File]::ReadAllText($script)
        $anchor="    `$start.EnvironmentVariables['SteamAppId']='1091500';`$start.EnvironmentVariables['SteamGameId']='1091500'"
        if(([regex]::Matches($body,[regex]::Escape($anchor))).Count -ne 1){throw 'Неизвестный запускатель: аргументы REDmod не применены.'}
        $lines=@(foreach($arg in $Config.launchArguments){
            if($arg -notin @('--launcher-skip','-modded','-skipStartScreen')){throw 'Недопустимый аргумент запуска.'}
            "    `$start.ArgumentList.Add('$arg')"
        })
        $body=$body.Replace($anchor,($lines -join "`n")+"`n"+$anchor)
        $script=Join-Path $Config.game 'CPVR-Portable/Start-Game-center.ps1'
        [IO.File]::WriteAllText($script,$body,[Text.UTF8Encoding]::new($false))
    }
    $arguments=@('-NoProfile','-File',('"'+$script+'"'))
    if($WatchStartup){$arguments+='-WatchStartup'}
    if($CheckOnly){$arguments+='-CheckOnly'}
    $errorLog=$Log+'.stderr'
    # File handles, not the caller's output pipe: the game may keep inherited
    # output open for its whole lifetime. Wait only for the launcher process.
    $launcher=Start-Process -FilePath $Config.pwsh -ArgumentList $arguments -RedirectStandardOutput $Log -RedirectStandardError $errorLog -WindowStyle Hidden -PassThru
    try{
        if(-not $launcher.WaitForExit(45000)){throw 'Запускатель не завершил подготовку за 45 секунд.'}
        $code=$launcher.ExitCode
        $output=Read-LauncherOutput $Log
        $errorText=Read-LauncherOutput $errorLog
        if($code -ne 0){throw ($output+"`n"+$errorText).Trim()}
        return @{exitCode=$code;output=$output;errorOutput=$errorText}
    }finally{$launcher.Dispose()}
}
