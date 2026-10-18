param([switch]$CheckOnly,[switch]$WatchStartup)
$ErrorActionPreference='Stop'
try {
    $game=Split-Path -Parent $PSScriptRoot
    $bin=Join-Path $game 'bin\x64'
    $receipt=Get-Content -LiteralPath (Join-Path $PSScriptRoot 'installed.json') -Raw -Encoding UTF8|ConvertFrom-Json
    if($receipt.PSObject.Properties['isolation']){
        . (Join-Path $PSScriptRoot 'Isolated-Profile.ps1')
        Validate-IsolatedReference -Reference $receipt.isolation.reference -Game $game|Out-Null
    }
    if((Get-FileHash -LiteralPath (Join-Path $bin 'Cyberpunk2077.exe')).Hash -ne $receipt.exeSha256){throw 'Изменилась версия игры. Комплект подготовлен для Cyberpunk 2.31.'}
    foreach($f in $receipt.critical){if((Get-FileHash -LiteralPath (Join-Path $game $f.path)).Hash -ne $f.sha256){throw ('Изменён файл выбранной сборки: '+$f.path)}}
    $start=[Diagnostics.ProcessStartInfo]::new()
    $start.FileName=Join-Path $bin 'Cyberpunk2077.exe';$start.WorkingDirectory=$bin;$start.UseShellExecute=$false
    $start.EnvironmentVariables['SteamAppId']='1091500';$start.EnvironmentVariables['SteamGameId']='1091500'
    if($receipt.clean -and $receipt.version -in @('luke263','luke263-sr31091','tofu-aer-x2-prepared-20261006')){
        $start.EnvironmentVariables['CHEEKY_OPENXR_LAYER_DISABLE']='1'
        $start.EnvironmentVariables['XRFG_DISABLE_OFXR_BRIDGE']='1'
    }
    $start.EnvironmentVariables['CPVR_HANDS_SESSION']='0';$start.EnvironmentVariables['CPVR_INPUT_DISABLE']='1'
    $start.EnvironmentVariables['IRD_NVIDIA_MENU_DISABLE']='1';$start.EnvironmentVariables.Remove('CPVR_INPUT_PRIVATE_ATTACH')
    $layers=@($start.EnvironmentVariables['XR_ENABLE_API_LAYERS'] -split ';'|Where-Object {$_ -and $_ -notin @('XR_APILAYER_CPVR_input','XR_APILAYER_IRD_nvidia_menu')})
    $paths=@($start.EnvironmentVariables['XR_API_LAYER_PATH'] -split ';'|Where-Object {$_ -and (Split-Path -Leaf $_.TrimEnd('\')) -notin @('CPVR_HandsTracking','Cyberpunk-NVIDIA-Menu','NvidiaMenu')})
    if($receipt.hands){
        $start.EnvironmentVariables['CPVR_HANDS_SESSION']='1';$start.EnvironmentVariables.Remove('CPVR_INPUT_DISABLE')
        $start.EnvironmentVariables['CPVR_INPUT_PRIVATE_ATTACH']='1'
        $layers+='XR_APILAYER_CPVR_input';$paths+=Join-Path $bin 'CPVR_HandsTracking'
    }
    if($receipt.version -eq 'luke15' -and $receipt.fg){
        $start.EnvironmentVariables.Remove('IRD_NVIDIA_MENU_DISABLE')
        $layers+='XR_APILAYER_IRD_nvidia_menu';$paths+=Join-Path $PSScriptRoot 'NvidiaMenu'
    }
    if($layers.Count){$start.EnvironmentVariables['XR_ENABLE_API_LAYERS']=$layers -join ';'}else{$start.EnvironmentVariables.Remove('XR_ENABLE_API_LAYERS')}
    if($paths.Count){$start.EnvironmentVariables['XR_API_LAYER_PATH']=$paths -join ';'}else{$start.EnvironmentVariables.Remove('XR_API_LAYER_PATH')}
    if($receipt.PSObject.Properties['nativeGameFg'] -and $receipt.nativeGameFg.enabled){
        . (Join-Path $PSScriptRoot 'GenerationLaunchOwner.ps1')
        $gameSettingsPath=Join-Path $env:LOCALAPPDATA 'CD Projekt Red\Cyberpunk 2077\UserSettings.json'
        $gameSettings=Get-Content -LiteralPath $gameSettingsPath -Raw -Encoding UTF8|ConvertFrom-Json
        $fgOwner=Set-CpvrGenerationLaunchOwner -StartInfo $start -Settings $gameSettings
        Write-Output ('FG session owner: '+$fgOwner+'; owner changes apply after game restart.')
        $start.EnvironmentVariables['DISABLE_XR_APILAYER_reshade_1']='1'
        $start.EnvironmentVariables['CHEEKY_OPENXR_LAYER_DISABLE']='1'
        $start.EnvironmentVariables['XRFG_DISABLE_OFXR_BRIDGE']='1'
        foreach($name in @('CPVR_PPD_DIAGNOSTIC','CPVR_D3D12_VALIDATION','CPVR_NATIVE_FG_PIXEL_DIR','CPVR_NATIVE_FG_OUTPUT_DIAG','CPVR_AER_PIXEL_CAPTURE_DIR','CPVR_SR_PIXEL_CAPTURE_DIR')){$start.EnvironmentVariables.Remove($name)}
        if($start.EnvironmentVariables['XR_RUNTIME_JSON'] -match 'openxr.simulator'){$start.EnvironmentVariables.Remove('XR_RUNTIME_JSON')}
        $activeLayers=@($start.EnvironmentVariables['XR_ENABLE_API_LAYERS'] -split ';'|Where-Object {$_ -and $_ -notmatch 'reshade'})
        if($activeLayers.Count){$start.EnvironmentVariables['XR_ENABLE_API_LAYERS']=$activeLayers -join ';'}else{$start.EnvironmentVariables.Remove('XR_ENABLE_API_LAYERS')}
    }
    if($CheckOnly){Write-Output ('PASS: '+$receipt.version+'; launch paths and critical hashes verified; game not started.');exit 0}
    if(Get-Process Cyberpunk2077,Vortex -ErrorAction SilentlyContinue | Where-Object {$_.HasExited -ne $true}){throw 'Перед запуском закрой игру и Vortex.'}
    if($receipt.version -in @('luke263-cheeky055','luke263-fullstack-20261006')){
        $ini=Join-Path $bin 'OptiScaler.ini';$text=[IO.File]::ReadAllText($ini)
        $section=[regex]::Match($text,'(?ms)^\[DlssNr\][^\r\n]*\r?\n.*?(?=^\[|\z)')
        if($section.Success){
            $body=[regex]::Replace($section.Value,'(?m)^(Enabled\s*=\s*)[^\r\n]*','${1}false')
            $text=$text.Substring(0,$section.Index)+$body+$text.Substring($section.Index+$section.Length)
            [IO.File]::WriteAllText($ini,$text,[Text.UTF8Encoding]::new($false))
        }
    }
    if($receipt.PSObject.Properties['isolation']){
        . (Join-Path $PSScriptRoot 'Profile-Settings.ps1')
        Start-ProfileGame -StartInfo $start -ProfileSettingsFile $receipt.isolation.settingsFile -ProfileRoot $receipt.isolation.profileRoot
        exit 0
    }
    $process=[Diagnostics.Process]::Start($start)
    if(-not $process){throw 'Не удалось запустить игру.'}
    Write-Output ('Запущено: '+$receipt.label+'; PID '+$process.Id)
    if($WatchStartup -and $process.WaitForExit(30000)){
        $code=[uint32]([long]$process.ExitCode -band 0xffffffffL)
        throw ('Игра завершилась во время запуска. Код: 0x{0:X8}.' -f $code)
    }
    exit 0
}catch{Write-Output $_.Exception.Message;exit 1}
