$ErrorActionPreference='Stop'

function Get-CyberpunkBaselineSummary($Config){
    $path=Join-Path $Config.data 'profile-store/clean-baseline.json'
    $receipt=Read-GameProfileJson $path
    return @{captured=[bool]($receipt -and $receipt.source -eq 'fresh-first-launch');createdUtc=$receipt.createdUtc;storedBytes=$receipt.storedBytes}
}

function Save-CyberpunkCleanBaseline {
    param($Config,[switch]$FirstLaunchConfirmed)
    if(-not $FirstLaunchConfirmed){throw 'Сначала подтверди первый запуск чистой игры до меню и выход.'}
    $running=@(Get-Process -Name @('Cyberpunk2077','Vortex','ModOrganizer','ModOrganizer2','Wabbajack','Wabbajack.CLI') -ErrorAction SilentlyContinue)
    if($running.Count){throw 'Перед снимком закрой игру и программы установки модов.'}
    $store=Get-CyberpunkProfileStoreConfig $Config;$context=Get-GameProfileContext $store
    $receiptPath=Get-GameProfileStorePath $context 'clean-baseline.json'
    if(Test-Path -LiteralPath $receiptPath){throw 'Чистая основа уже зафиксирована. Автоматическая перезапись запрещена.'}
    $settings=Join-Path $context.scopes.appdata.root 'UserSettings.json'
    $userSettings=Read-GameProfileJson $settings
    if(-not $userSettings.data){throw 'Первый запуск ещё не создал UserSettings.json.'}
    $stock=Read-GameProfileJson (Join-Path $Config.bundle 'stock-paths.json')
    $stockMap=@{};foreach($key in $stock.Keys){$stockMap[(ConvertTo-CyberpunkTemplatePath $key)]=$stock[$key]}
    $observedStock=@{};$sharedCount=0;$privateCount=0
    # Validate what Steam actually installed. Optional DLC/tools absent from this
    # installation do not become mandatory because an older inventory had them.
    foreach($file in Get-ChildItem -LiteralPath $Config.game -Recurse -Force){
        if($file.Attributes -band [IO.FileAttributes]::ReparsePoint){throw 'В чистой игре найдена файловая ссылка.'}
        if($file.PSIsContainer){continue}
        $relative=ConvertTo-CyberpunkTemplatePath ([IO.Path]::GetRelativePath($Config.game,$file.FullName))
        $expected=$stockMap[$relative]
        if(-not $expected){throw ('В основе найден нештатный файл: '+$relative)}
        if(-not(Get-CyberpunkTemplateSource $file.FullName $expected.sha1 ([long]$expected.size) '' -StockIdentityOnly)){throw ('Оригинал изменён: '+$relative)}
        $observedStock[$relative]=$expected
        if($relative -match '^archive/pc/(content|ep1)/'){$sharedCount++}else{$privateCount++}
    }
    if(-not $observedStock.ContainsKey('bin/x64/cyberpunk2077.exe') -or -not $sharedCount){throw 'Чистая установка неполная: нет игры или общих архивов.'}
    $inventory=Get-GameProfileInventory $context
    $sourceFiles=@($inventory|Where-Object scope -in @('game','appdata','user-state'))
    $allDirectories=Get-GameProfileDirectoryInventory $context
    $sourceDirectories=@($allDirectories|Where-Object scope -in @('game','appdata','user-state'))
    $progress=@($sourceFiles|Where-Object {$_.scope -eq 'user-state' -and $_.path -match '^(ManualSave|AutoSave|QuickSave|PointOfNoReturnSave)-'})
    if($progress.Count){throw 'В чистой основе уже есть игровой прогресс. Снимок не создан.'}
    $lock=Lock-GameProfileStore $context
    try{
        Assert-GameProfileNoRecovery $context
        if(Test-Path -LiteralPath $receiptPath){throw 'Чистая основа уже зафиксирована.'}
        foreach($entry in $sourceFiles){Save-GameProfileObject $context $entry}
        Assert-GameProfileInventory $inventory (Get-GameProfileInventory $context)
        if(@(Get-Process -Name @('Cyberpunk2077','Vortex','ModOrganizer','ModOrganizer2','Wabbajack','Wabbajack.CLI') -ErrorAction SilentlyContinue).Count){throw 'Во время снимка запущена игра или менеджер. Чистая основа не зафиксирована.'}
        $now=[datetime]::UtcNow.ToString('o')
        Assert-GameProfileDirectories $allDirectories (Get-GameProfileDirectoryInventory $context)
        $manifest=@{schema=1;id='clean-flat';name='Чистая основа';templateId='clean-flat';vr=$false;createdUtc=$now;savedUtc=$now;scopes=@($context.scopes.Values|ForEach-Object {@{id=$_.id;kind=$_.kind}});files=$sourceFiles;directories=$sourceDirectories}
        $manifest=ConvertTo-GameProfileManifest $context $manifest -RequireFiles
        Write-GameProfileJson (Get-GameProfileStorePath $context 'templates/clean-flat.json') $manifest
        Write-GameProfileJson (Get-GameProfileStorePath $context 'baseline-stock.json') $observedStock
        $receipt=@{schema=1;source='fresh-first-launch';templateId='clean-flat';createdUtc=$now;game=$Config.game;privateFileCount=$privateCount;sharedFileCount=$sharedCount;userDataFiles=@($sourceFiles|Where-Object scope -in @('appdata','user-state')).Count;storedBytes=[long](($sourceFiles|Measure-Object size -Sum).Sum);templateHash=(Get-GameProfileFileHash (Get-GameProfileStorePath $context 'templates/clean-flat.json'))}
        Write-GameProfileJson $receiptPath $receipt
        return $receipt
    }finally{$lock.Dispose()}
}

function Get-CyberpunkCapturedBaseline($Config,$StoreConfig){
    $context=Get-GameProfileContext $StoreConfig
    $receipt=Read-GameProfileJson (Get-GameProfileStorePath $context 'clean-baseline.json')
    if(-not $receipt){return $null}
    $path=Get-GameProfileStorePath $context 'templates/clean-flat.json'
    if($receipt.schema -ne 1 -or $receipt.source -cne 'fresh-first-launch' -or $receipt.game -ine $Config.game -or (Get-GameProfileFileHash $path) -ine $receipt.templateHash){throw 'Чистая основа не соответствует сохранённому снимку.'}
    $null=ConvertTo-GameProfileManifest $context (Read-GameProfileJson $path) -RequireFiles
    return $receipt
}
