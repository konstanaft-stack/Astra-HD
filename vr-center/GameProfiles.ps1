$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot 'Core.ps1')
. (Join-Path $PSScriptRoot 'SharedEnhancementPolicy.ps1')
. (Join-Path $PSScriptRoot 'StockIdentityCache.ps1')
foreach($module in @('GameProfileStore.ps1','GameProfileTemplates.ps1','CleanBaseline.ps1')){
    $path=Join-Path $PSScriptRoot $module
    if(Test-Path -LiteralPath $path -PathType Leaf){. $path}
}

function Get-CyberpunkTrustedProfileRoot([string]$Path,[string]$Name){
    if([string]::IsNullOrWhiteSpace($Path) -or -not [IO.Path]::IsPathFullyQualified($Path)){throw ('Не задан абсолютный путь профиля: '+$Name)}
    return [IO.Path]::GetFullPath($Path).TrimEnd('\')
}
function Get-CyberpunkProfileStoreConfig($Config){
    # An effective MO2 settings target is not the storage root declaration.
    if($Config.profileStoreConfig){$Config=$Config.profileStoreConfig}
    $game=Get-CyberpunkTrustedProfileRoot ([string]$Config.game) 'game'
    $data=Get-CyberpunkTrustedProfileRoot ([string]$Config.data) 'data'
    $bundle=Get-CyberpunkTrustedProfileRoot ([string]$Config.bundle) 'bundle'
    $stock=Read-Json (Join-Path $bundle 'stock-paths.json')
    $capturedStock=Read-Json (Join-Path $data 'profile-store/baseline-stock.json')
    if($capturedStock){$stock=$capturedStock}
    if($stock -isnot [Collections.IDictionary]){throw 'Нет проверенного списка stock-paths.json для границ профиля.'}
    $shared=@{};foreach($key in $stock.Keys){
        $relative=([string]$key).Replace('\','/').ToLowerInvariant()
        if($relative -match '^archive/pc/(content|ep1)/'){
            Assert-GameProfileRelativePath $relative
            if($shared.ContainsKey($relative)){throw 'Повтор пути общего оригинала в stock-paths.json.'}
            $shared[$relative]=$true
        }
    }
    if(-not $shared.Count){throw 'В stock-paths.json не найдены точные общие оригиналы content/ep1.'}
    if($Config.ContainsKey('appDataPath')){$appdata=Get-CyberpunkTrustedProfileRoot ([string]$Config.appDataPath) 'appDataPath'}
    elseif($Config.ContainsKey('settingsFile')){$settings=Get-CyberpunkTrustedProfileRoot ([string]$Config.settingsFile) 'settingsFile';$appdata=Split-Path $settings -Parent}
    else{$local=[Environment]::GetFolderPath('LocalApplicationData');$appdata=Get-CyberpunkTrustedProfileRoot (Join-Path $local 'CD Projekt Red/Cyberpunk 2077') 'AppData Cyberpunk'}
    if($Config.ContainsKey('settingsFile')){
        $settings=Get-CyberpunkTrustedProfileRoot ([string]$Config.settingsFile) 'settingsFile'
        if(-not $settings.StartsWith($appdata+'\',[StringComparison]::OrdinalIgnoreCase)){throw 'settingsFile находится вне зарегистрированного AppData Cyberpunk.'}
    }
    if($Config.ContainsKey('userStateFile')){$userState=Get-CyberpunkTrustedProfileRoot ([string]$Config.userStateFile) 'userStateFile'}
    else{$homeRoot=[Environment]::GetFolderPath('UserProfile');$userState=Get-CyberpunkTrustedProfileRoot (Join-Path $homeRoot 'Saved Games/CD Projekt Red/Cyberpunk 2077/user.gls') 'user.gls'}
    $savedGames=if($Config.ContainsKey('savedGamesPath')){Get-CyberpunkTrustedProfileRoot ([string]$Config.savedGamesPath) 'savedGamesPath'}else{Split-Path $userState -Parent}
    if($userState -ine (Join-Path $savedGames 'user.gls')){throw 'userStateFile должен быть user.gls внутри папки сохранений этого профиля.'}
    # Port metadata is only a lazy display hint. A truncated optional receipt
    # must not hide the independent profile catalog or its recovery journals.
    $receipt=$null;try{$receipt=Read-Json (Join-Path $game 'CPVR-Portable/installed.json')}catch{}
    $scopes=@(
        @{id='game';root=$game;kind='directory';requiredFiles=@('bin/x64/cyberpunk2077.exe');exclusions=@($shared.Keys|Sort-Object)+@('CPVR-Portable-Backups','CPVR-Candidate-Deltas')+@(Get-SharedEnhancementPaths $Config)},
        @{id='appdata';root=$appdata;kind='directory';requiredFiles=@();exclusions=@()},
        @{id='user-state';root=$savedGames;kind='directory';requiredFiles=@();exclusions=@()},
        @{id='center-settings';root=(Join-Path $data 'game-settings');kind='directory';requiredFiles=@();exclusions=@('transactions','*/transactions')},
        @{id='mods-state';root=(Join-Path $data 'mods-state.json');kind='file';requiredFiles=@();exclusions=@()},
        @{id='keybindings';root=(Join-Path $data 'keybindings.json');kind='file';requiredFiles=@();exclusions=@()},
        @{id='usb-state';root=(Join-Path $data 'usb-recovery.json');kind='file';requiredFiles=@();exclusions=@()},
        @{id='launch-source';root=(Join-Path $data 'launch-source.json');kind='file';requiredFiles=@();exclusions=@()},
        @{id='collection';root=(Join-Path $data 'collection');kind='directory';requiredFiles=@();exclusions=@(Get-SharedEnhancementPaths $Config|ForEach-Object {'overwrite/'+$_})},
        @{id='collection-cache';root=(Join-Path $data 'collection-cache');kind='directory';requiredFiles=@();exclusions=@()}
    )
    foreach($scope in $scopes){
        if(Test-Path -LiteralPath $scope.root){$item=Get-Item -LiteralPath $scope.root -Force;if(($scope.kind -eq 'file') -eq [bool]$item.PSIsContainer){throw ('Неправильный тип пути профиля: '+$scope.id)}}
    }
    $store=@{data=$data;scopes=$scopes;initialName='Основной';initialVR=($receipt -and $receipt.kind -eq 'native');legacyFileScopes=@{'user-state'='user.gls'}}
    $null=Get-GameProfileContext $store
    return $store
}
function Get-CyberpunkGameProfilesState($Config){
    $state=@{enabled=(Test-CyberpunkProfileMode $Config);available=$false;initialized=$false;activeId=$null;profiles=@();recoveryRequired=@()}
    if(-not(Test-CyberpunkProfileMode $Config)){$state.reason='Полные профили игры не включены в этой конфигурации.';return $state}
    try{
        if(-not(Get-Command Get-GameProfileCatalog -ErrorAction SilentlyContinue)){throw 'Ядро профилей игры не найдено.'}
        $catalog=Get-GameProfileCatalog -StoreConfig (Get-CyberpunkProfileStoreConfig $Config)
        $state.available=$true;$state.initialized=[bool]$catalog.initialized;$state.activeId=$catalog.activeId
        $state.profiles=@($catalog.profiles);$state.recoveryRequired=@($catalog.recoveryRequired)
        return $state
    }catch{$state.reason=$_.Exception.Message;return $state}
}
function Assert-CyberpunkProfileMutationAllowed($Config){
    if((Read-Json $Config.hold).active){throw 'Сначала заверши первый запуск игры. Состав игры пока не меняем.'}
    $running=@(Get-Process -Name @('Cyberpunk2077','Vortex','ModOrganizer','ModOrganizer2','Wabbajack','Wabbajack.CLI') -ErrorAction SilentlyContinue)
    if($running.Count){throw ('Для применения закрой Cyberpunk, Vortex, Mod Organizer и Wabbajack: '+(@($running|ForEach-Object ProcessName|Sort-Object -Unique) -join ', '))}
}
function Assert-CyberpunkProfileSharedBase($Config,$StoreConfig){
    # These originals are shared rather than copied with every profile. Never
    # mix a saved executable/configuration with changed common game archives.
    $context=Get-GameProfileContext $StoreConfig
    $stock=Read-Json (Join-Path $Config.bundle 'stock-paths.json')
    $capturedStock=Read-GameProfileJson (Get-GameProfileStorePath $context 'baseline-stock.json')
    if($capturedStock){$stock=$capturedStock}
    $failed=[Collections.Generic.List[string]]::new()
    foreach($key in $stock.Keys){
        $relative=ConvertTo-CyberpunkTemplatePath ([string]$key)
        if($relative -notmatch '^archive/pc/(content|ep1)/'){continue}
        if($relative -notin $context.scopes.game.exclusions){throw 'Missing exact shared stock exclusion.'}
        $entry=$stock[$key]
        if($entry.sha1 -notmatch '^[a-fA-F0-9]{40}$' -or ($entry.size -isnot [long] -and $entry.size -isnot [int])){throw 'Invalid shared stock identity.'}
        $identity=Test-VerifiedStockIdentity (Join-Path $context.scopes.game.root $relative) ([string]$entry.sha1) ([long]$entry.size) (Join-Path $Config.data 'stock-identity-cache')
        if(-not $identity.Verified){$failed.Add($relative)}
    }
    if($failed.Count){throw ('Unverified shared stock: '+(@($failed.ToArray()|Select-Object -First 8) -join ', '))}
}
function Invoke-CyberpunkGameProfileAction($Config,$Request){
    if(-not(Test-CyberpunkProfileMode $Config)){throw 'Полные профили игры не включены.'}
    Assert-CyberpunkProfileMutationAllowed $Config
    $store=Get-CyberpunkProfileStoreConfig $Config
    switch([string]$Request.action){
        'CreateGameProfile' {
            $name=[string]$Request.name
            if([string]::IsNullOrWhiteSpace($name) -or $name.Length -gt 128 -or $name -match '[\x00-\x1f]'){throw 'Укажи корректное имя нового профиля.'}
            $context=Get-GameProfileContext $store;Assert-GameProfileNoRecovery $context
            if(-not(Get-CyberpunkCapturedBaseline $Config $store)){throw 'Сначала зафиксируй чистую основу после первого запуска игры до меню.'}
            if(-not(Get-Command Ensure-CyberpunkProfileTemplate -ErrorAction SilentlyContinue)){throw 'Подготовщик чистой основы не найден.'}
            $template=Ensure-CyberpunkProfileTemplate -Config $Config -StoreConfig $store -VR:([bool]$Request.vr)
            Assert-CyberpunkProfileSharedBase $Config $store
            Assert-CyberpunkProfileMutationAllowed $Config
            $null=New-GameProfile -StoreConfig $store -Name $name -TemplateId ([string]$template) -VR:([bool]$Request.vr)
        }
        'DuplicateGameProfile' {
            $source=Read-Json (Join-Path $Config.data 'launch-source.json')
            if($source.kind -in @('mo2','vortex') -and $source.collection.storageMode -ne 'shared-archives'){throw 'Сначала переведи коллекцию на общие архивы: старый импорт содержит полную копию модов.'}
            $null=Copy-GameProfile -StoreConfig $store -Name ([string]$Request.name)
        }
        'SwitchGameProfile' {
            Assert-CyberpunkProfileSharedBase $Config $store
            Assert-CyberpunkProfileMutationAllowed $Config
            $null=Activate-GameProfile -StoreConfig $store -Id ([string]$Request.profileId)
        }
        'DeleteGameProfile' {$null=Remove-GameProfile -StoreConfig $store -Id ([string]$Request.profileId)}
        'RepairGameProfiles' {
            $pending=@((Get-GameProfileCatalog -StoreConfig $store).recoveryRequired)
            if($pending.Count -and -not(Get-Command Repair-GameProfileTransaction -ErrorAction SilentlyContinue)){throw 'Восстановление журнала ещё не доступно в ядре профилей.'}
            foreach($id in $pending){Assert-CyberpunkProfileMutationAllowed $Config;$null=Repair-GameProfileTransaction -StoreConfig $store -Id ([string]$id)}
        }
        default {throw 'Неизвестное действие профиля игры.'}
    }
    return Get-CyberpunkGameProfilesState $Config
}
