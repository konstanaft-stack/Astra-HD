param([Parameter(Mandatory)][string]$Request,[Parameter(Mandatory)][string]$Result,[string]$ConfigPath)
$ErrorActionPreference='Stop'
. "$PSScriptRoot/Core.ps1"
. "$PSScriptRoot/Components.ps1"
. "$PSScriptRoot/Updates.ps1"
. "$PSScriptRoot/LibraryActions.ps1"
. "$PSScriptRoot/KeyBindings.ps1"
. "$PSScriptRoot/SettingsProfiles.ps1"
. "$PSScriptRoot/GraphicsSettings.ps1"
. "$PSScriptRoot/UsbRecovery.ps1"
. "$PSScriptRoot/LauncherProcess.ps1"
. "$PSScriptRoot/GameProfiles.ps1"
. "$PSScriptRoot/GameLaunch.ps1"
. "$PSScriptRoot/TestSaves.ps1"
$lock=$null;$held=$false
try{
    $config=Read-Json $(if($ConfigPath){$ConfigPath}else{Join-Path $PSScriptRoot 'config.json'});$req=Read-Json $Request
    $profileAction=$req.action -in @('CreateGameProfile','DuplicateGameProfile','SwitchGameProfile','DeleteGameProfile','RepairGameProfiles','CaptureCleanBaseline')
    $collectionAction=$req.action -in @('InspectCollection','ImportGameCollection','RefreshGameCollection','UseDirectCollection','ReadGraphicsSettings','ApplyGraphicsSettings','SetEnhancementPackage','SetTestSaves','SetTestSaveSelection','ImportTestSaveSet','SetRedmodEnabled')
    # Recovery must reach the journal even if a partial deployment breaks port inspection.
    $inspectionConfig=$config
    if(-not $profileAction -and -not $collectionAction){
        try{$inspectionConfig=Get-GameLaunchEffectiveConfig $config}catch{if($req.action -ne 'Status'){throw}}
    }
    $state=if($profileAction -or $collectionAction){@{}}else{Get-PortState $inspectionConfig};$output='';$snapshot=$null;$focusVersionId=$null
    if(Test-CyberpunkProfileMode $config){
        if($req.action -eq 'SetMods'){throw 'Состав модов принадлежит выбранному профилю игры. Используй создание и переключение профилей.'}
        if($req.action -in @('Apply','ApplyConfiguration','SetRecommendedSettings','SaveKeys','SetUsbRecovery','Launch')){Assert-GameWritable $config}
    }
    if($req.action -ne 'Status' -and -not $profileAction){Save-PortInstallTime $config $state.receipt}
    $log=Join-Path $config.data ('logs/'+(Get-Date -Format 'yyyyMMdd-HHmmss-fff')+'-'+$req.action+'.log')
    if($req.action -ne 'Status'){[IO.Directory]::CreateDirectory((Split-Path $log -Parent))|Out-Null}
    switch($req.action){
        'SetEnhancementPackage' {
            $lock=[Threading.Mutex]::new($false,'Local.VRPorts.Apply');try{$held=$lock.WaitOne(0)}catch [Threading.AbandonedMutexException]{$held=$true}
            if(-not $held){throw 'Применение уже выполняется.'}
            Set-EnhancementPackage $config ([bool]$req.enabled)
            $state=Get-PortState $config
            $output=if(Get-SharedEnhancementDefinition $config){if($req.enabled){'NR + MFG включён. Общие файлы и твои настройки сохранены.'}else{'NR + MFG выключен переименованием загрузчика. Общие файлы и твои настройки сохранены.'}}elseif($req.enabled){'NR + MFG подключён. Твои настройки сохранены.'}else{'NR + MFG отключён; прежние файлы восстановлены, твои настройки сохранены.'}
        }
        'Status' {}
        'SetRedmodEnabled' {
            $lock=[Threading.Mutex]::new($false,'Local.VRPorts.Apply');try{$held=$lock.WaitOne(0)}catch [Threading.AbandonedMutexException]{$held=$true}
            if(-not $held){throw 'Другая операция ещё выполняется.'}
            Set-RedmodEnabled $config ([bool]$req.enabled)
            $state=Get-PortState $config
            $output=if($req.enabled){'REDmod включён; штатный инструмент установлен.'}else{'REDmod выключен для текущего профиля.'}
        }
        {$_ -in @('SetTestSaves','SetTestSaveSelection','ImportTestSaveSet')} {
            $lock=[Threading.Mutex]::new($false,'Local.VRPorts.Apply');try{$held=$lock.WaitOne(0)}catch [Threading.AbandonedMutexException]{$held=$true}
            if(-not $held){throw 'Другая операция с профилем ещё выполняется.'}
            if($req.action -eq 'ImportTestSaveSet'){
                $null=Import-TestSaveSet $config ([string]$req.path) ([string]$req.label)
                $output='Сохранения добавлены в общую библиотеку. Исходная папка сохранена.'
            }elseif($req.action -eq 'SetTestSaveSelection'){
                $null=Set-TestSaveSelection $config @($req.ids)
                $output='Состав тестовых сохранений выбран.'
            }else{
                $null=Set-TestSavesEnabled $config ([bool]$req.enabled)
                $output=if($req.enabled){'Тестовые сохранения подключены к текущему профилю. В игре выбери «Загрузить игру».'}else{'Тестовые сохранения отключены. Изменённые рабочие копии сохранены в профиле.'}
            }
            $state=Get-PortState $config
        }
        'CaptureCleanBaseline' {
            $lock=[Threading.Mutex]::new($false,'Local.VRPorts.Apply');try{$held=$lock.WaitOne(0)}catch [Threading.AbandonedMutexException]{$held=$true}
            if(-not $held){throw 'Применение уже выполняется.'}
            $baseline=Save-CyberpunkCleanBaseline $config -FirstLaunchConfirmed:([bool]$req.firstLaunchConfirmed)
            $config.gameProfilesEnabled=$true
            Write-Json $(if($ConfigPath){$ConfigPath}else{Join-Path $PSScriptRoot 'config.json'}) $config
            $state=Get-PortState $config
            $output='Чистая основа сохранена. Новые профили будут создаваться из неё.'
        }
        {$_ -in @('ReadGraphicsSettings','ApplyGraphicsSettings')} {
            $effective=$config
            if((Get-GameLaunchSource $config).kind -in @('mo2','vortex') -or (Test-Path -LiteralPath (Join-Path $config.game 'bin/x64/Cyberpunk2077.exe'))){$effective=Get-GameLaunchEffectiveConfig $config}
            if($req.action -eq 'ApplyGraphicsSettings'){
                $lock=[Threading.Mutex]::new($false,'Local.VRPorts.Apply');try{$held=$lock.WaitOne(0)}catch [Threading.AbandonedMutexException]{$held=$true}
                if(-not $held){throw 'Применение уже выполняется.'}
                $graphics=Set-GraphicsSettings $effective ([string]$req.hash) $req.values
                $output='Настройки графики сохранены.'
            }else{$graphics=Get-GraphicsSettings $effective}
            $state=Get-PortState $config;$state.graphics=$graphics
            $state.graphicsPresets=@{performance=(Get-GraphicsPreset 'performance');balanced=(Get-GraphicsPreset 'balanced');quality=(Get-GraphicsPreset 'quality')}
        }
        'InspectCollection' {
            $state=Get-PortState $config
            $state.collectionCatalog=if($req.provider -eq 'vortex'){Get-VortexCollectionCatalog $config}else{Get-Mo2CollectionCatalog ([string]$req.root)}
        }
        {$_ -in @('ImportGameCollection','RefreshGameCollection','UseDirectCollection')} {
            Assert-CyberpunkProfileMutationAllowed $config
            $lock=[Threading.Mutex]::new($false,'Local.VRPorts.Apply');try{$held=$lock.WaitOne(0)}catch [Threading.AbandonedMutexException]{$held=$true}
            if(-not $held){throw 'Применение уже выполняется.'}
            if($req.action -eq 'UseDirectCollection'){
                $oldSource=Get-GameLaunchSource $config
                Restore-VortexCollectionPark $config $oldSource
                Write-Json (Join-Path $config.data 'launch-source.json') @{schema=1;kind='direct'}
                $output='Виртуальная коллекция отключена. Файлы установленной игры сохранены.'
            }elseif($req.action -eq 'RefreshGameCollection'){
                $source=Get-GameLaunchSource $config
                if($source.kind -eq 'vortex'){$null=Import-VortexCollection $config $source.origin.profileId -Arguments @($source.arguments) -Refresh}
                else{$null=Import-Mo2Collection $config $source.origin.root $source.profile -Arguments @($source.arguments) -Refresh}
                $output='Список коллекции обновлён; текущие настройки и overwrite сохранены.'
            }else{
                $arguments=if($req.redmod){@('--launcher-skip','-modded')}else{@('--launcher-skip')}
                if($req.provider -eq 'vortex'){$null=Import-VortexCollection $config ([string]$req.profile) -Arguments $arguments}
                else{$null=Import-Mo2Collection $config ([string]$req.root) ([string]$req.profile) -Arguments $arguments}
                $output='Коллекция подключена. Игра запускается автономно из VR-центра.'
            }
            $state=Get-PortState $config
        }
        {$_ -in @('CreateGameProfile','DuplicateGameProfile','SwitchGameProfile','DeleteGameProfile','RepairGameProfiles')} {
            $lock=[Threading.Mutex]::new($false,'Local.VRPorts.Apply');try{$held=$lock.WaitOne(0)}catch [Threading.AbandonedMutexException]{$held=$true}
            if(-not $held){throw 'Применение уже выполняется.'}
            $profiles=Invoke-CyberpunkGameProfileAction $config $req
            if($req.action -in @('CreateGameProfile','SwitchGameProfile','RepairGameProfiles')){$null=Sync-GameLaunchRendererOverride $config}
            $state=Get-PortState $config;$state.gameProfiles=$profiles
            $output=switch($req.action){'CreateGameProfile' {'Новый профиль создан; предыдущий сохранён.'};'DuplicateGameProfile' {'Копия профиля создана и активирована. Общие архивы модов не копировались.'};'SwitchGameProfile' {'Профиль игры переключён.'};'DeleteGameProfile' {'Неактивный профиль удалён.'};'RepairGameProfiles' {'Журналы профилей проверены и восстановлены.'}}
        }
        'SetUsbRecovery' {
            Set-UsbRecoveryEnabled $config ([bool]$req.enabled)
        }
        'SetRecommendedSettings' {
            if($req.version -ne $state.receipt.version){throw 'Сначала установи выбранный порт.'}
            Assert-GameWritable $config
            $lock=[Threading.Mutex]::new($false,'Local.VRPorts.Apply');try{$held=$lock.WaitOne(0)}catch [Threading.AbandonedMutexException]{$held=$true}
            if(-not $held){throw 'Применение уже выполняется.'}
            $settingsConfig=if(Test-CyberpunkProfileMode $config){Get-GameLaunchEffectiveConfig $config}else{$config}
            Set-RecommendedSettingsMode $settingsConfig $state ([bool]$req.recommended)
            $output=if($req.recommended){'Применён пресет порта.'}else{'Используются твои настройки.'}
        }
        'SetMods' {
            Assert-GameWritable $config
            $lock=[Threading.Mutex]::new($false,'Local.VRPorts.Apply');try{$held=$lock.WaitOne(0)}catch [Threading.AbandonedMutexException]{$held=$true}
            if(-not $held){throw 'Применение уже выполняется.'}
            Set-LibrariesAndMods $config @{mods=[bool]$req.mods;libraries=@{}}
            $state=Get-PortState $config
            $output=if($req.mods){'Vortex включён.'}else{'Чистый профиль включён. Обязательные моды VR-порта сохранены.'}
        }
        'RemoveVersions' {$output=Remove-PortVersions $config @($req.ids);$state=Get-PortState $config}
        'SaveKeys' {$output=Save-PortKeys $config $req}
        'CheckUpdates' {$null=Get-PortUpdates $config ([string]$req.repoId);$output='Публичные выпуски GitHub проверены.'}
        'UpdateLibraries' {$libs=Update-NvidiaLibraries $config;$output=($libs|ForEach-Object {$_.file+' '+$_.version}) -join "`n"}
        'DownloadPort' {$download=Download-PortUpdate $config $req;$output='Скачано: '+$download.path}
        'DownloadAndImport' {
            $download=Download-PortUpdate $config $req
            & $config.pwsh -NoProfile -File (Join-Path $config.bundle 'Import-Port.ps1') -ZipPath $download.path *> $log
            $output=Get-Content $log -Raw;if($LASTEXITCODE -ne 0){throw $output}
            $state=Get-PortState $config
            $output+=' Выпуск доступен в списке версий; предыдущие версии сохранены.'
            $focusVersionId=Find-DownloadedVersion (Read-Json (Join-Path $config.bundle 'bundle.json')) $download
        }

        'ImportDownloaded' {
            $download=Read-Json (Join-Path $config.data 'updates/last-download.json')
            if($req.repoId -and $download.repoId -ne $req.repoId){throw 'Сначала скачай выпуск выбранного порта.'}
            if(-not $download -or -not(Test-Path $download.path) -or (Get-FileHash $download.path).Hash -ne $download.sha256){throw 'Нет проверенной загрузки для импорта.'}
            & $config.pwsh -NoProfile -File (Join-Path $config.bundle 'Import-Port.ps1') -ZipPath $download.path *> $log
            $output=Get-Content $log -Raw;if($LASTEXITCODE -ne 0){throw $output};$state=Get-PortState $config
            $focusVersionId=Find-DownloadedVersion (Read-Json (Join-Path $config.bundle 'bundle.json')) $download
        }
        'Import' { $output=Import-LocalPort $config $req; $state=Get-PortState $config }
        'ApplyConfiguration' {
            Assert-GameWritable $config
            $lock=[Threading.Mutex]::new($false,'Local.VRPorts.Apply');try{$held=$lock.WaitOne(0)}catch [Threading.AbandonedMutexException]{$held=$true}
            if(-not $held){throw 'Применение уже выполняется.'}
            $v=$state.versions|Where-Object id -eq $req.version|Select-Object -First 1
            $oldConfig=if(Test-CyberpunkProfileMode $config){Get-GameLaunchEffectiveConfig $config}else{$config}
            if(Test-CyberpunkProfileMode $config){$null=Get-GameLaunchInstallConfig $config ([string]$v.kind) ([bool]$req.opti)}
            $needsInstall=($req.version -ne $state.receipt.version -or [bool]$req.opti -ne [bool]$state.receipt.opti -or [bool]$req.fg -ne [bool]$state.receipt.fg -or [bool]$req.hands -ne [bool]$state.receipt.hands)
            if($needsInstall){Assert-ApplyAllowed $state $v ([bool]$req.opti) ([bool]$req.fg)}
            $catalog=Get-LibraryCatalog $config
            foreach($lib in $catalog){$hash=[string]$req.libraries[$lib.file];if($hash -and $hash -ne $lib.current){if(-not @($lib.choices|Where-Object hash -eq $hash).Count){throw 'Неизвестная библиотека.'};$obj=Join-Path $config.bundle "files/$hash";if((Get-FileHash $obj).Hash -ne $hash){throw 'Библиотека повреждена.'}}}
            # A full build snapshot is required only when replacing the port.
            # Component changes already preserve their exact files transactionally.
            if($needsInstall){$snapshot=Save-CurrentBuild $oldConfig $state}
            if($needsInstall){
                $package=$config.bundle
                if($req.version -eq $state.receipt.version){
                    # Use exact current core files with the original profile identity and its module logic.
                    $b=Read-Json (Join-Path $config.bundle 'bundle.json');$snap=@($b.versions|Where-Object id -eq $snapshot)[0];$base=@($b.versions|Where-Object id -eq $req.version)[0]
                    $modulePaths=@{};foreach($module in $b.modules.Values){foreach($key in $module.Keys){$modulePaths[$key]=$true}}
                    foreach($key in $snap.files.Keys){if(-not $modulePaths.ContainsKey($key)){$base.files[$key]=$snap.files[$key];if($base.noHandsFiles){$base.noHandsFiles[$key]=$snap.files[$key]}}}
                    $package=Join-Path $config.data ('packages/'+[guid]::NewGuid().ToString('N'));[IO.Directory]::CreateDirectory($package)|Out-Null
                    Copy-Item (Join-Path $config.bundle '*.ps1') $package;Copy-Item (Join-Path $config.bundle 'stock-paths.json') $package
                    Write-Json (Join-Path $package 'bundle.json') $b
                    New-Item -ItemType Junction -Path (Join-Path $package 'files') -Target (Join-Path $config.bundle 'files')|Out-Null
                }
                $installRequest=Join-Path $config.data ('requests/install-'+[guid]::NewGuid().ToString('N')+'.json');$install=@{};foreach($key in $req.Keys){$install[$key]=$req[$key]};$install.clean=[bool]$state.receipt.clean
                if(Test-CyberpunkProfileMode $config){$install.clean=$false}
                Write-Json $installRequest $install
                & $config.pwsh -NoProfile -File "$PSScriptRoot/Install-Selected.ps1" -ConfigPath $ConfigPath -Request $installRequest -BundlePath $package -CheckOnly *> $log
                if($LASTEXITCODE -ne 0){throw (Get-Content $log -Raw)}
                $enhancementTransaction=Suspend-EnhancementForPort $config ([string]$req.version)
                try{
                    & $config.pwsh -NoProfile -File "$PSScriptRoot/Install-Selected.ps1" -ConfigPath $ConfigPath -Request $installRequest -BundlePath $package *>> $log
                    if($LASTEXITCODE -ne 0){throw (Get-Content $log -Raw)}
                }catch{
                    $failure=$_.Exception.Message
                    try{Restore-EnhancementAfterFailedPortChange $config $enhancementTransaction}catch{$failure+='; '+$_.Exception.Message}
                    throw $failure
                }
            }
            if($needsInstall -and (Test-CyberpunkProfileMode $config)){$null=Sync-GameLaunchRendererOverride $config}
            $currentConfig=if(Test-CyberpunkProfileMode $config){Get-GameLaunchEffectiveConfig $config}else{$config}
            Set-LibrariesAndMods $currentConfig $req
            if($needsInstall){Complete-EnhancementPortChange $config ([string]$req.version)}
            $state=Get-PortState $currentConfig;$output='Изменения применены. Игра не запускалась.'
        }
        'Verify' {
            $script=Join-Path $config.game 'CPVR-Portable/Start-Game.ps1'
            if(-not(Test-Path $script)){throw 'В игре отсутствует установленный запускатель.'}
            & $config.pwsh -NoProfile -File $script -CheckOnly *> $log
            $output=Get-Content $log -Raw;if($LASTEXITCODE -ne 0){throw $output}
        }
        'CheckPackage' {
            & $config.pwsh -NoProfile -File "$PSScriptRoot/Install-Selected.ps1" -ConfigPath $ConfigPath -Request $Request -CheckOnly *> $log
            $output=Get-Content $log -Raw;if($LASTEXITCODE -ne 0){throw $output}
        }
        'Apply' {
            $lock=[Threading.Mutex]::new($false,'Local.VRPorts.Apply');try{$held=$lock.WaitOne(0)}catch [Threading.AbandonedMutexException]{$held=$true}
            if(-not $held){throw 'Установка уже выполняется.'}
            $v=$state.versions|Where-Object id -eq $req.version|Select-Object -First 1
            Assert-ApplyAllowed $state $v ([bool]$req.opti) ([bool]$req.fg)
            $applyRequest=$Request
            if(Test-CyberpunkProfileMode $config){
                $install=@{};foreach($key in $req.Keys){$install[$key]=$req[$key]};$install.clean=$false
                $applyRequest=Join-Path $config.data ('requests/install-'+[guid]::NewGuid().ToString('N')+'.json');Write-Json $applyRequest $install
            }
            # Verify before preservation or deployment. Existing installer validates payload hashes.
            & $config.pwsh -NoProfile -File "$PSScriptRoot/Install-Selected.ps1" -ConfigPath $ConfigPath -Request $applyRequest -CheckOnly *> $log
            if($LASTEXITCODE -ne 0){throw (Get-Content $log -Raw)}
            $oldConfig=if(Test-CyberpunkProfileMode $config){Get-GameLaunchEffectiveConfig $config}else{$config}
            $snapshot=Save-CurrentBuild $oldConfig $state
            $enhancementTransaction=Suspend-EnhancementForPort $config ([string]$req.version)
            try{
                & $config.pwsh -NoProfile -File "$PSScriptRoot/Install-Selected.ps1" -ConfigPath $ConfigPath -Request $applyRequest *>> $log
                $output=Get-Content $log -Raw;if($LASTEXITCODE -ne 0){throw $output}
            }catch{
                $failure=$_.Exception.Message
                try{Restore-EnhancementAfterFailedPortChange $config $enhancementTransaction}catch{$failure+='; '+$_.Exception.Message}
                throw $failure
            }
            if(Test-CyberpunkProfileMode $config){$null=Sync-GameLaunchRendererOverride $config}
            Complete-EnhancementPortChange $config ([string]$req.version)
            $state=Get-PortState $config
        }
        'Launch' {
            if($state.gameRunning -or $state.vortexRunning){throw 'Перед запуском закрой игру и Vortex.'}
            $lock=[Threading.Mutex]::new($false,'Local.VRPorts.Apply');try{$held=$lock.WaitOne(0)}catch [Threading.AbandonedMutexException]{$held=$true}
            if(-not $held){throw 'Применение уже выполняется.'}
            if(-not(Test-CyberpunkProfileMode $config) -and (Get-GameLaunchSource $config).kind -eq 'direct' -and $state.receipt){
                Ensure-LaunchModProfile $config $state
                Set-RecommendedSettingsMode $config $state (Get-RecommendedSettingsMode $config $state) -BeforeLaunch
            }elseif((Test-CyberpunkProfileMode $config) -and $state.receipt.kind -eq 'native'){
                $settingsConfig=Get-GameLaunchEffectiveConfig $config
                $modePaths=Get-SettingsModePaths $settingsConfig $state
                if(Test-Path -LiteralPath $modePaths.state -PathType Leaf){
                    $mode=Read-Json $modePaths.state
                    if([string]$mode.version -cne $modePaths.version -or $mode.recommended -isnot [bool]){
                        throw 'Не удалось проверить сохранённый выбор настроек этой сборки.'
                    }
                    Set-RecommendedSettingsMode $settingsConfig $state $mode.recommended -BeforeLaunch
                }
                Sync-NativeRequiredIni $settingsConfig $state
            }
            # Only an explicit click invokes this action. Never installs or switches a selected profile.
            if(-not (Get-UsbRecoveryState $config).enabled){
                $launch=Invoke-GameProfileLaunch $config $log -WatchStartup
                $output=$launch.output
                break
            }
            $launch=Invoke-GameProfileLaunch $config $log
            $output=$launch.output
            $usbConfig=if(Test-CyberpunkProfileMode $config){Get-GameLaunchEffectiveConfig $config}else{$config}
            $started=Get-UsbRecoveryGame $usbConfig
            if(-not $started){throw 'Игра завершилась сразу после запуска.'}
            $reported=[regex]::Match($output,'PID\s+(\d+)')
            if(-not $reported.Success -or [int]$reported.Groups[1].Value -ne $started.pid){throw 'Не удалось сопоставить PID запущенной игры.'}
            if($launch.game -and ($launch.game.pid -ne $started.pid -or $launch.game.start -cne $started.start)){throw 'PID/time запущенной игры изменились перед USB tracking.'}
            if((Get-UsbRecoveryState $config).enabled){Start-UsbRecoveryForGame $config $started}
            $gameProcess=Get-Process -Id $started.pid -ErrorAction Stop
            try{if($gameProcess.WaitForExit(30000)){throw ('Игра завершилась во время запуска. Код: '+$gameProcess.ExitCode)}}finally{$gameProcess.Dispose()}
        }
        'ReleaseHold' {
            if($state.gameRunning){throw 'Сначала закрой игру после первого запуска.'}
            $h=Read-Json $config.hold;$h.active=$false;$h.game_files_frozen=$false;$h.user_confirmation_utc=[DateTime]::UtcNow.ToString('o');$h.confirmation_source='VR Ports explicit first-launch-finished button'
            Write-Json $config.hold $h;$state=Get-PortState $config;$output='Завершение первого запуска подтверждено пользователем.'
        }
        default {throw 'Неизвестное действие.'}
    }
    $currentConfig=$config
    try{$currentConfig=Get-GameLaunchEffectiveConfig $config}catch{if($req.action -notin @('Status','ReadGraphicsSettings','InspectCollection')){throw}}
    if($req.action -in @('Apply','ApplyConfiguration')){
        if(Initialize-FirstPortSettings $currentConfig $state){$output+=' Применён исходный пресет выбранного порта.'}
    }
    $state.collection=Get-GameLaunchSource $config
    $state.testSaves=Get-TestSaveState $config
    $state.redmod=Get-RedmodState $config
    $state.enhancementPackage=Get-EnhancementPackageState $config
    $state.mo2Running=[bool](Get-Process ModOrganizer,ModOrganizer2 -ErrorAction SilentlyContinue)
    if($req.action -in @('ImportGameCollection','RefreshGameCollection') -and $state.collection.cleanupWarning){$output+="`n"+$state.collection.cleanupWarning}
    $state.cleanBaseline=Get-CyberpunkBaselineSummary $config
    $state.libraries=Get-LibraryCatalog $currentConfig
    $profile=Get-ModProfileState $config $state
    $state.modsEnabled=$profile.enabled
    $state.activeExtraMods=$profile.active
    $state.parkedMods=$profile.parked
    $state.recommendedSettings=Get-RecommendedSettingsMode $config $state
    $state.usbRecovery=Get-UsbRecoveryState $config
    foreach($version in $state.versions){
        $version.recommendedSettings=Get-RecommendedSettingsMode $config @{receipt=@{version=$version.id;kind=$version.kind}}
    }
    $state.portUpdates=Get-CachedPortUpdates $config
    $state.keyBindings=Read-Json (Join-Path $config.data 'keybindings.json')
    $state.optiMenuKey=Read-OptiMenuKey (Join-Path $currentConfig.game 'bin/x64/OptiScaler.ini')
    $state.nrEnabled=$false
    $optiIni=Join-Path $currentConfig.game 'bin/x64/OptiScaler.ini'
    if(Test-Path $optiIni){$nrSection=[regex]::Match([IO.File]::ReadAllText($optiIni),'(?ms)^\[DlssNr\][^\r\n]*\r?\n.*?(?=^\[|\z)');$state.nrEnabled=[regex]::IsMatch($nrSection.Value,'(?im)^Enabled\s*=\s*true\s*$')}
    $state.nrEnabled=$state.nrEnabled -and [bool]$state.receipt.opti
    $state.lastPortDownload=Read-Json (Join-Path $config.data 'updates/last-download.json')
    $state.lastDownloadedVersionId=if($state.lastPortDownload){Find-DownloadedVersion (Read-Json (Join-Path $config.bundle 'bundle.json')) $state.lastPortDownload}else{$null}
    if($req.action -ne 'Status'){Save-PortInstallTime $config $state.receipt}
    Write-Json $Result @{ok=$true;action=$req.action;state=$state;output=$output;log=$log;snapshot=$snapshot;focusVersionId=$focusVersionId}
}catch{
    $answer=@{ok=$false;action=$req.action;error=$_.Exception.Message;log=$log}
    if($config -and (Test-CyberpunkProfileMode $config)){
        try{$answer.gameProfiles=Get-CyberpunkGameProfilesState $config}catch{$answer.gameProfiles=@{enabled=(Test-CyberpunkProfileMode $config);available=$false;initialized=$false;activeId=$null;profiles=@();recoveryRequired=@();reason=$_.Exception.Message}}
    }
    Write-Json $Result $answer;exit 1
}
finally{if($held){$lock.ReleaseMutex()};if($lock){$lock.Dispose()}}
