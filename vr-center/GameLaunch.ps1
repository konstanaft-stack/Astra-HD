$ErrorActionPreference='Stop'
. "$PSScriptRoot/Core.ps1"
. "$PSScriptRoot/LauncherProcess.ps1"
. "$PSScriptRoot/UsbRecovery.ps1"
. "$PSScriptRoot/StandaloneVfs.ps1"
. "$PSScriptRoot/CollectionSources.ps1"
. "$PSScriptRoot/VortexCollections.ps1"
. "$PSScriptRoot/EnhancementPackage.ps1"
. "$PSScriptRoot/NrStartupPolicy.ps1"
. "$PSScriptRoot/PortPayloadPolicy.ps1"
. "$PSScriptRoot/Redmod.ps1"

function Get-GameLaunchSource($Config){
    $path=Join-Path $Config.data 'launch-source.json'
    $source=Read-Json $path
    if(-not $source){if(Test-Path -LiteralPath $path){throw 'Повреждён источник запуска профиля.'};return @{schema=1;kind='direct'}}
    if($source.schema -ne 1 -or $source.kind -notin @('direct','mo2','vortex')){throw 'Неизвестный источник запуска профиля.'}
    return $source
}
function Get-GameLaunchPlainPath([string]$Path){
    if(-not [IO.Path]::IsPathFullyQualified($Path)){throw 'Источник запуска требует абсолютный доверенный путь.'}
    $full=[IO.Path]::GetFullPath($Path)
    $cursor=$full
    while($cursor){
        if(Test-Path -LiteralPath $cursor){if((Get-Item -LiteralPath $cursor -Force).Attributes -band [IO.FileAttributes]::ReparsePoint){throw 'Источник запуска содержит reparse point.'}}
        $parent=Split-Path $cursor -Parent;if($parent -eq $cursor){break};$cursor=$parent
    }
    return $full
}
function Get-GameLaunchProcesses([string[]]$Name){
    foreach($process in @(Get-Process -Name $Name -ErrorAction SilentlyContinue)){
        try{
            $identity=Get-UsbHelperIdentity $process.Id
            if(-not $identity){throw 'Не удалось подтвердить executable/time существующего процесса.'}
            @{pid=$identity.pid;exe=$identity.path;start=$identity.start;startedUtc=$identity.start}
        }finally{$process.Dispose()}
    }
}
function Get-GameLaunchPlan($Config,[switch]$InspectOnly,[string]$RequestedKind,[bool]$RequestedOpti=$false){
    $source=Get-GameLaunchSource $Config
    if($source.collection){
        $copy=@{};foreach($key in $Config.Keys){$copy[$key]=$Config[$key]};$copy.mo2=$source.collection;$Config=$copy
    }
    $game=Get-GameLaunchPlainPath ([string]$Config.game)
    $defaultSettings=if($Config.settingsFile){$Config.settingsFile}elseif($Config.appDataPath){Join-Path $Config.appDataPath 'UserSettings.json'}else{Join-Path ([Environment]::GetFolderPath('LocalApplicationData')) 'CD Projekt Red/Cyberpunk 2077/UserSettings.json'}
    $plan=@{kind=$source.kind;game=$game;source=$source;arguments=@($source.arguments|Where-Object {$_});settingsFile=$defaultSettings;collection=$Config.mo2}
    foreach($argument in $plan.arguments){if($argument -notin @('--launcher-skip','-modded','-skipStartScreen')){throw 'Неизвестный аргумент запуска игры.'}}
    if($source.kind -in @('mo2','vortex')){
        if(-not $Config.mo2){throw 'Для профиля не настроен доверенный MO2 source.'}
        $game=Get-GameLaunchPlainPath ([string]$Config.mo2.game);$plan.game=$game
        if($source.profiles){
            $receipt=if($RequestedKind){@{kind=$RequestedKind;opti=$RequestedOpti}}else{Read-Json (Join-Path $game 'CPVR-Portable/installed.json')}
            $mode=if(-not $receipt -or $receipt.kind -eq 'flat'){if($receipt.opti){'flatWithOpti'}else{'flat'}}elseif($receipt.kind -in @('native','luke','monolithic','own')){'vr'}else{throw 'Не подтверждён режим установленного порта для MO2.'}
            if(-not $source.profiles[$mode]){throw ('MO2 профиль для режима '+$mode+' ещё не подготовлен.')}
            $source.profile=[string]$source.profiles[$mode];$plan.mode=$mode
        }
        if(-not $Config.mo2){throw 'Для профиля не настроен доверенный MO2 source.'}
        $root=Get-GameLaunchPlainPath ([string]$Config.mo2.root)
        $game=Get-GameLaunchPlainPath ([string]$Config.mo2.game);$plan.game=$game
        $profile=[string]$source.profile
        if([string]::IsNullOrWhiteSpace($profile) -or $profile -in @('.','..') -or $profile -match '[\\/:\x00-\x1f]' -or $profile.TrimEnd(' ','.') -cne $profile){throw 'Неправильное имя MO2 профиля.'}
        $profiles=if($Config.mo2.profilesRoot){Get-GameLaunchPlainPath ([string]$Config.mo2.profilesRoot)}else{Join-Path $root 'profiles'}
        $profileRoot=Get-GameLaunchPlainPath (Join-Path $profiles $profile)
        if(-not(Test-Path -LiteralPath (Join-Path $profileRoot 'modlist.txt') -PathType Leaf)){throw 'Выбранный MO2 профиль отсутствует или ещё не подготовлен.'}
        $executable=[string]$source.executable
        if(-not $Config.mo2.executables -or -not $Config.mo2.executables.ContainsKey($executable)){throw 'MO2 executable не входит в доверенную конфигурацию.'}
        $relative=[string]$Config.mo2.executables[$executable]
        if($relative -notmatch '^bin[/\\]x64[/\\]Cyberpunk2077\.exe$'){throw 'MO2 executable должен указывать на Cyberpunk2077.'}
        $plan.engine='usvfs'
        $plan.arguments=@($source.arguments)
        if(-not $source.arguments){$plan.arguments=@()}
        foreach($argument in $plan.arguments){if($argument -notin @('--launcher-skip','-modded','-skipStartScreen')){throw ('Неподдерживаемый аргумент Cyberpunk: '+$argument)}}
        $plan.workingDirectory=Join-Path $game 'bin/x64';$plan.profileRoot=$profileRoot
        if($source.settingsRelative){
            $relative=[string]$source.settingsRelative
            if([IO.Path]::IsPathRooted($relative) -or $relative -match '(^|[/\\])\.\.?([/\\]|$)' -or $relative -match '[:\x00-\x1f]'){throw 'Неправильный относительный путь MO2 UserSettings.'}
            $plan.settingsFile=Get-GameLaunchPlainPath (Join-Path $profileRoot $relative)
            if(-not $plan.settingsFile.StartsWith($profileRoot.TrimEnd('\')+'\',[StringComparison]::OrdinalIgnoreCase) -or (-not $RequestedKind -and -not(Test-Path -LiteralPath $plan.settingsFile -PathType Leaf))){throw 'Эффективный MO2 UserSettings не найден.'}
        }
        if(-not $InspectOnly -and @(Get-GameLaunchProcesses @('ModOrganizer','ModOrganizer2')).Count){throw 'Закрой MO2 перед запуском: он может изменять состав импортированной коллекции.'}
    }
    $plan.exe=Get-GameLaunchPlainPath (Join-Path $plan.game 'bin/x64/Cyberpunk2077.exe')
    if(-not(Test-Path -LiteralPath $plan.exe -PathType Leaf)){throw 'Не найден executable выбранной игры.'}
    if(-not $plan.workingDirectory){$plan.workingDirectory=Split-Path $plan.exe -Parent}
    $plan.selectedFiles=Get-GameLaunchSelectedFiles $plan $Config
    return $plan
}
function Get-GameLaunchSelectedFiles($Plan,$Config){
    $files=@{}
    if(-not $Plan.source.collection -and $Plan.kind -notin @('mo2','vortex')){return $files}
    $receipt=Read-Json (Join-Path $Plan.game 'CPVR-Portable/installed.json')
    if($receipt.kind -in @('luke','native','own','monolithic')){
        $critical=if($receipt.critical -is [Collections.IDictionary]){@($receipt.critical.Keys|ForEach-Object {@{path=$_;sha256=$receipt.critical[$_]}})}else{@($receipt.critical)}
        foreach($entry in $critical){
            if(-not $entry.path -or $entry.sha256 -notmatch '^[a-fA-F0-9]{64}$'){throw 'Некорректный состав выбранного VR-порта.'}
            $files[[string]$entry.path]=Get-GameLaunchPlainPath (Resolve-ChildPath $Plan.game ([string]$entry.path))
        }
        foreach($relative in @($receipt.paths|Where-Object {$_})){
            if(Test-CollectionPortOwnedPath $relative){$files[$relative]=Get-GameLaunchPlainPath (Resolve-ChildPath $Plan.game $relative)}
        }
    }
    if($Config.data){
        $enhancement=Read-Json (Join-Path $Config.data 'game-settings/enhancement/state.json')
        $common=Get-SharedEnhancementDefinition $Config
        if($common -and -not(Test-Path -LiteralPath (Resolve-ChildPath $Plan.game $common.loaderPath)) -and $Plan.kind -in @('mo2','vortex')){
            $commonRecord=Read-Json (Join-Path $Config.data 'enhancement-common/state.json')
            foreach($root in @(Get-GameLaunchModRoots $Config $Plan)){
                $shadow=Join-Path $root $common.loaderPath
                if(Test-Path -LiteralPath $shadow -PathType Leaf){
                    if($commonRecord.fallbackLoaderSha256 -match '^[A-Fa-f0-9]{64}$' -and (Get-FileHash -LiteralPath $shadow).Hash -ieq $commonRecord.fallbackLoaderSha256){continue}
                    throw 'Disabled shared loader is shadowed by a collection; remove the private loader before launch.'
                }
            }
        }
        if($common -or $enhancement.enabled){foreach($file in @($(if($common){@($common.files)+@($common.runtimeMutablePaths|ForEach-Object {@{path=$_}})}else{$enhancement.files}))){
            if($common -and $file.path -eq $common.loaderPath -and -not(Test-Path -LiteralPath (Resolve-ChildPath $Plan.game $file.path))){continue}
            $path=Get-GameLaunchPlainPath (Resolve-ChildPath $Plan.game ([string]$file.path))
            if(Test-Path -LiteralPath $path -PathType Leaf){$files[[string]$file.path]=$path}
        }}
    }
    return $files
}
function Get-GameLaunchEffectiveConfig($Config){
    $plan=Get-GameLaunchPlan $Config -InspectOnly
    $effective=@{};foreach($key in $Config.Keys){$effective[$key]=$Config[$key]}
    $effective.game=$plan.game
    if($plan.settingsFile){$effective.settingsFile=$plan.settingsFile}
    $effective.profileStoreConfig=if($Config.profileStoreConfig){$Config.profileStoreConfig}else{$Config}
    return $effective
}
function Get-GameLaunchInstallConfig($Config,[string]$Kind,[bool]$Opti){
    if($Kind -notin @('flat','native','luke','monolithic','own')){throw 'Неизвестный requested kind установки.'}
    $plan=Get-GameLaunchPlan $Config -InspectOnly -RequestedKind $Kind -RequestedOpti $Opti
    $effective=@{};foreach($key in $Config.Keys){$effective[$key]=$Config[$key]}
    $effective.game=$plan.game
    $effective.settingsFile=if($plan.settingsFile){Get-GameLaunchPlainPath ([string]$plan.settingsFile)}else{Join-Path ([Environment]::GetFolderPath('LocalApplicationData')) 'CD Projekt Red/Cyberpunk 2077/UserSettings.json'}
    $effective.profileStoreConfig=if($Config.profileStoreConfig){$Config.profileStoreConfig}else{$Config}
    return $effective
}
function Select-NewGameLaunchIdentity($Processes,$Before,[string]$Exe,[datetime]$StartedUtc){
    $known=@{};foreach($p in @($Before)){$known[[string]$p.pid]=$true}
    $matching=@($Processes|Where-Object{$_.exe -ieq $Exe -and -not $known.ContainsKey([string]$_.pid) -and ([datetimeoffset]::Parse($_.startedUtc)).UtcDateTime -ge $StartedUtc.ToUniversalTime()})
    if($matching.Count -ne 1){throw 'Не подтверждён единственный новый PID игры с ожидаемыми executable/time.'}
    return $matching[0]
}
function Get-GameLaunchCollectionConfig($Config,$Plan){
    if(-not $Plan.collection){return $Config}
    $copy=@{};foreach($key in $Config.Keys){$copy[$key]=$Config[$key]};$copy.mo2=$Plan.collection;return $copy
}
function Get-GameLaunchModRoots($Config,$Plan,[switch]$IncludeShared){
    $Config=Get-GameLaunchCollectionConfig $Config $Plan
    $mods=if($Config.mo2.modsRoot){Get-GameLaunchPlainPath ([string]$Config.mo2.modsRoot)}else{Join-Path $Config.mo2.root 'mods'}
    $overwrite=if($Config.mo2.overwriteRoot){Get-GameLaunchPlainPath ([string]$Config.mo2.overwriteRoot)}else{Join-Path $Config.mo2.root 'overwrite'}
    $roots=[Collections.Generic.List[string]]::new()
    if(Test-Path -LiteralPath $overwrite -PathType Container){$roots.Add((Get-GameLaunchPlainPath $overwrite))}
    $seen=@{}
    # v2.5.2 profile.cpp: file order is inverted into priority. Overwrite
    # wins first, then the first enabled modlist row, then physical game.
    foreach($raw in [IO.File]::ReadAllLines((Join-Path $Plan.profileRoot 'modlist.txt'))){
        $line=$raw.Trim();if(-not $line -or $line.StartsWith('#')){continue}
        $enabled=-not $line.StartsWith('-')
        $name=if($line[0] -in @('+','-','*')){$line.Substring(1).Trim()}else{$line}
        if(-not $name -or $seen.ContainsKey($name)){continue};$seen[$name]=$true
        if(-not $enabled -or $line.StartsWith('*') -or $name.EndsWith('_separator',[StringComparison]::OrdinalIgnoreCase)){continue}
        if($name -match '[\\/:\x00-\x1f]' -or $name -in @('.','..') -or $name.TrimEnd(' ','.') -cne $name){throw 'Неправильное имя активного MO2 мода.'}
        $root=Get-GameLaunchPlainPath (Join-Path $mods $name)
        if(-not(Test-Path -LiteralPath $root -PathType Container)){throw ('Отсутствует активный MO2 мод: '+$name)}
        $roots.Add($root)
        if($IncludeShared -and $Config.mo2.sharedModsRoot){
            $sharedRoot=Get-GameLaunchPlainPath (Join-Path ([string]$Config.mo2.sharedModsRoot) $name)
            if(-not(Test-Path -LiteralPath $sharedRoot -PathType Container)){throw ('Отсутствует общая папка мода: '+$name)}
            $roots.Add($sharedRoot)
        }
    }
    return @($roots.ToArray())
}
function Resolve-GameLaunchEffectiveFile($Config,$Plan,[string]$Relative,[string[]]$Roots){
    if([IO.Path]::IsPathRooted($Relative) -or $Relative -match '(^|[/\\])\.\.?([/\\]|$)' -or $Relative -match ':'){throw 'Недопустимый effective file path.'}
    foreach($part in $Relative.Replace('\','/').Split('/')){if(-not $part -or $part.TrimEnd(' ','.') -cne $part -or $part -match '[*?"<>|\x00-\x1f]'){throw 'Недопустимый effective file component.'}}
    if($Plan.selectedFiles -and $Plan.selectedFiles[$Relative]){return $Plan.selectedFiles[$Relative]}
    $modRoots=if($PSBoundParameters.ContainsKey('Roots')){$Roots}else{Get-GameLaunchModRoots $Config $Plan}
    foreach($root in @($modRoots)+@($Plan.game)){
        $candidate=Join-Path $root $Relative
        if(Test-Path -LiteralPath $candidate -PathType Leaf){return Get-GameLaunchPlainPath $candidate}
    }
    return $null
}
function Get-GameLaunchRendererPaths($Config){
    $known=@{
        'red4ext/plugins/cyberpunkvr_stereo/cyberpunkvr_stereo.dll'=$true
        'red4ext/plugins/cpvrown/cpvrown.dll'=$true
    }
    if($Config.bundle){
        $bundle=Read-Json (Join-Path $Config.bundle 'bundle.json')
        foreach($version in @($bundle.versions)){
            foreach($relative in @($version.files.Keys)){
                $normalized=([string]$relative).Replace('\','/').ToLowerInvariant()
                if($normalized -match '^red4ext/plugins/(cyberpunkvr[^/]*|cpvrown)/[^/]+\.dll$'){$known[$normalized]=$true}
            }
            foreach($relative in @($version.rendererPath)+@($version.rendererPaths)){
                if($relative -and $version.kind -in @('native','own')){
                    $normalized=([string]$relative).Replace('\','/').ToLowerInvariant()
                    # Shared loader slots identify the selected renderer by SHA;
                    # their presence alone cannot distinguish Luke/ReShade.
                    if($normalized -notin @('bin/x64/dxgi.dll','bin/x64/version.dll','bin/x64/winmm.dll')){$known[$normalized]=$true}
                }
            }
        }
    }
    return @($known.Keys|Sort-Object)
}
function Get-GameLaunchRendererOverrideContext($Config,$Plan,$Receipt){
    $Config=Get-GameLaunchCollectionConfig $Config $Plan
    if($Plan.kind -notin @('mo2','vortex') -or -not $Config.mo2.rendererOverrideMod -or $Receipt.kind -eq 'flat'){return $null}
    if($Receipt.kind -notin @('luke','native','own','monolithic')){throw 'Не подтверждён VR режим renderer override.'}
    $name=[string]$Config.mo2.rendererOverrideMod
    if(-not $name -or $name -match '[\\/:*?"<>|\x00-\x1f]' -or $name.TrimEnd(' ','.') -cne $name){throw 'Недопустимое имя управляемого renderer override.'}
    $mods=if($Config.mo2.modsRoot){$Config.mo2.modsRoot}else{Join-Path $Config.mo2.root 'mods'}
    $root=Get-GameLaunchPlainPath (Join-Path $mods $name)
    $roots=@(Get-GameLaunchModRoots $Config $Plan)
    $overwrite=Get-GameLaunchPlainPath $(if($Config.mo2.overwriteRoot){$Config.mo2.overwriteRoot}else{Join-Path $Config.mo2.root 'overwrite'})
    if(Test-Path -LiteralPath (Join-Path $overwrite 'bin/x64/dxgi.dll') -PathType Leaf){throw 'MO2 overwrite содержит DXGI выше управляемого renderer override.'}
    $enabled=@($roots|Where-Object{$_ -ine $overwrite})
    if(-not $enabled.Count -or $enabled[0] -ine $root){throw 'Управляемый renderer override должен быть первым активным MO2 модом VR профиля.'}
    $hash=$null
    if($Receipt.critical -is [Collections.IDictionary]){$hash=$Receipt.critical['bin/x64/dxgi.dll']}
    else{
        $entries=@($Receipt.critical|Where-Object{$_.path -ieq 'bin/x64/dxgi.dll'})
        if($entries.Count -gt 1){throw 'Повтор critical DXGI в receipt.'}
        if($entries.Count){$hash=$entries[0].sha256}
    }
    if($hash -and $hash -notmatch '^[a-fA-F0-9]{64}$'){throw 'Неправильный critical DXGI SHA.'}
    $file=Get-GameLaunchPlainPath (Join-Path $root 'bin/x64/dxgi.dll')
    $ownerPath=Get-GameLaunchPlainPath (Join-Path $root '.vr-center-renderer.json')
    $owner=Read-Json $ownerPath
    if(Test-Path -LiteralPath $ownerPath){
        if(-not $owner -or $owner.owner -cne 'VRPorts.RendererOverride.v1' -or $owner.modRoot -ine $root -or $owner.game -ine $Plan.game -or $owner.path -cne 'bin/x64/dxgi.dll' -or $owner.state -notin @('installed','empty')){throw 'Не подтверждено владение renderer override.'}
    }
    $exists=Test-Path -LiteralPath $file -PathType Leaf
    if($exists){
        if(-not $owner -or $owner.state -ne 'installed' -or $owner.sha256 -notmatch '^[a-fA-F0-9]{64}$' -or (Get-FileHash $file).Hash -ine $owner.sha256){throw 'Renderer override содержит посторонний/unowned DXGI.'}
    }elseif($owner -and $owner.state -eq 'installed'){throw 'Управляемый renderer override исчез после последней проверки.'}
    return @{root=$root;file=$file;ownerPath=$ownerPath;owner=$owner;exists=$exists;hash=$hash;game=$Plan.game}
}
function Sync-GameLaunchRendererOverride($Config){
    $plan=Get-GameLaunchPlan $Config -InspectOnly
    if($plan.kind -notin @('mo2','vortex') -or -not $Config.mo2.rendererOverrideMod){return @{changed=$false}}
    $receipt=Read-Json (Join-Path $plan.game 'CPVR-Portable/installed.json')
    $context=Get-GameLaunchRendererOverrideContext $Config $plan $receipt
    if(-not $context){return @{changed=$false}}
    $physical=Get-GameLaunchPlainPath (Join-Path $plan.game 'bin/x64/dxgi.dll')
    if($context.hash -and (-not(Test-Path -LiteralPath $physical -PathType Leaf) -or (Get-FileHash $physical).Hash -ine $context.hash)){throw 'Physical DXGI не соответствует новому installed receipt.'}
    if(($context.hash -and $context.exists -and $context.owner.sha256 -ieq $context.hash) -or (-not $context.hash -and -not $context.exists -and $context.owner)){return @{changed=$false}}
    $data=Get-GameLaunchPlainPath ([string]$Config.data)
    if($data.StartsWith($context.root.TrimEnd('\')+'\',[StringComparison]::OrdinalIgnoreCase) -or $context.root.StartsWith($data.TrimEnd('\')+'\',[StringComparison]::OrdinalIgnoreCase) -or $data -ieq $context.root){throw 'Renderer override и transaction data должны иметь независимые roots.'}
    $backup=Join-Path $data ('transactions/renderer-override-'+[guid]::NewGuid().ToString('N'))
    [void][IO.Directory]::CreateDirectory($backup)
    $beforeOwnerHash=if(Test-Path -LiteralPath $context.ownerPath -PathType Leaf){(Get-FileHash $context.ownerPath).Hash}else{$null}
    if($context.exists){[IO.File]::Copy($context.file,(Join-Path $backup 'before.dll'),$false);if((Get-FileHash (Join-Path $backup 'before.dll')).Hash -ine $context.owner.sha256){throw 'Renderer override изменился при сохранении backup.'}}
    if($beforeOwnerHash){[IO.File]::Copy($context.ownerPath,(Join-Path $backup 'before-owner.json'),$false)}
    $transaction=@{state='prepared';target=$context.file;beforeSha256=$context.owner.sha256;afterSha256=$context.hash;utc=[datetime]::UtcNow.ToString('o')}
    Write-Json (Join-Path $backup 'receipt.json') $transaction
    try{
        # Re-check ownership immediately before replacing or removing its leaf.
        $current=Get-GameLaunchRendererOverrideContext $Config $plan $receipt
        $ownerHash=if(Test-Path -LiteralPath $context.ownerPath -PathType Leaf){(Get-FileHash $context.ownerPath).Hash}else{$null}
        if($current.exists -ne $context.exists -or $ownerHash -cne $beforeOwnerHash){throw 'Владение renderer override изменилось во время sync.'}
        if($context.hash){
            [void][IO.Directory]::CreateDirectory((Split-Path $context.file -Parent))
            $temp=$context.file+'.'+[guid]::NewGuid().ToString('N')+'.new'
            [IO.File]::Copy($physical,$temp,$false)
            if((Get-FileHash $temp).Hash -ine $context.hash -or (Get-FileHash $physical).Hash -ine $context.hash){throw 'Physical DXGI изменился при подготовке override.'}
            $null=Get-GameLaunchRendererOverrideContext $Config $plan $receipt
            [IO.File]::Move($temp,$context.file,$true)
        }elseif($context.exists){[IO.File]::Delete($context.file)}
        $owner=@{owner='VRPorts.RendererOverride.v1';modRoot=$context.root;game=$plan.game;path='bin/x64/dxgi.dll';state=$(if($context.hash){'installed'}else{'empty'});sha256=$context.hash;utc=[datetime]::UtcNow.ToString('o')}
        Write-Json $context.ownerPath $owner
        $null=Get-GameLaunchRendererOverrideContext $Config $plan $receipt
        $transaction.state='committed';Write-Json (Join-Path $backup 'receipt.json') $transaction
        return @{changed=$true;backup=$backup}
    }catch{
        $transaction.state='recoveryRequired';$transaction.error=$_.Exception.Message
        Write-Json (Join-Path $backup 'receipt.json') $transaction
        throw ('Renderer override sync не завершён; backup='+$backup+'; '+$_.Exception.Message)
    }
}
function Assert-GameLaunchComposition($Config,$Plan){
    $shared=Get-SharedEnhancementDefinition $Config
    if($shared){$health=Get-SharedEnhancementState $Config $shared;if(-not $health.installed -or -not $health.healthy){throw ('Shared enhancement is not ready: '+$health.status)}}
    $Config=Get-GameLaunchCollectionConfig $Config $Plan
    $receipt=Read-Json (Join-Path $Plan.game 'CPVR-Portable/installed.json')
    if(-not $receipt){
        if(Test-Path -LiteralPath (Join-Path $Plan.game 'CPVR-Portable/installed.json')){throw 'Повреждён служебный файл установленного порта.'}
        $roots=if($Plan.kind -in @('mo2','vortex')){@(Get-GameLaunchModRoots $Config $Plan)}else{@()}
        foreach($relative in @(Get-GameLaunchRendererPaths $Config)){
            if(Resolve-GameLaunchEffectiveFile $Config $Plan $relative -Roots $roots){throw ('Для плоского запуска обнаружен невыбранный VR renderer: '+$relative)}
        }
        return
    }
    $override=Get-GameLaunchRendererOverrideContext $Config $Plan $receipt
    if($override -and (($override.hash -and (-not $override.exists -or $override.owner.sha256 -ine $override.hash)) -or (-not $override.hash -and $override.exists))){throw 'Renderer override не синхронизирован с установленным портом.'}
    $critical=@{}
    if($receipt.critical -is [Collections.IDictionary]){foreach($key in $receipt.critical.Keys){$critical[$key]=$receipt.critical[$key]}}
    else{foreach($entry in @($receipt.critical)){
        if(-not $entry.path -or $critical.ContainsKey([string]$entry.path)){throw 'Неправильный или повторный critical receipt entry.'}
        $critical[[string]$entry.path]=[string]$entry.sha256
    }}
    if(-not $critical.Count -and $receipt.exeSha256 -notmatch '^[a-fA-F0-9]{64}$'){throw 'Receipt не содержит проверяемой идентичности состава.'}
    if($receipt.exeSha256 -and (Get-FileHash $Plan.exe).Hash -ine $receipt.exeSha256){throw 'Executable не совпадает с применённым профилем.'}
    $roots=@();$modlistHash=$null
    if($Plan.kind -in @('mo2','vortex')){$modlistHash=(Get-FileHash (Join-Path $Plan.profileRoot 'modlist.txt')).Hash;$roots=@(Get-GameLaunchModRoots $Config $Plan)}
    foreach($relative in @(Get-GameLaunchRendererPaths $Config)){
        $physical=Resolve-GameLaunchEffectiveFile $Config $Plan $relative -Roots @()
        $effective=if($Plan.kind -in @('mo2','vortex')){Resolve-GameLaunchEffectiveFile $Config $Plan $relative -Roots $roots}else{$physical}
        if(($physical -or $effective) -and ($receipt.kind -notin @('native','own') -or -not $critical.ContainsKey($relative))){
            throw ('Обнаружен лишний native VR renderer для выбранного порта: '+$relative)
        }
    }
    foreach($relative in @($critical.Keys)){
        if([IO.Path]::IsPathRooted($relative) -or $relative -match '(^|[/\\])\.\.?([/\\]|$)' -or $relative -match ':'){throw 'Неправильный critical path receipt.'}
        $path=if($Plan.kind -in @('mo2','vortex')){Resolve-GameLaunchEffectiveFile $Config $Plan $relative -Roots $roots}else{Get-GameLaunchPlainPath (Join-Path $Plan.game $relative)}
        if($critical[$relative] -notmatch '^[a-fA-F0-9]{64}$' -or -not $path -or -not(Test-Path -LiteralPath $path -PathType Leaf) -or (Get-FileHash -LiteralPath $path).Hash -ine $critical[$relative]){throw ('Не применён critical файл профиля: '+$relative)}
    }
    if($Plan.kind -in @('mo2','vortex')){
        # A physical snapshot does not prove a virtual mod composition. Require
        # trusted per-profile evidence for loaders that may shadow physical DXGI.
        $evidence=$Config.mo2.profileEvidence[$Plan.source.profile]
        if(-not $evidence -or $evidence.modlistSha256 -notmatch '^[a-fA-F0-9]{64}$'){throw 'Нет проверенного MO2 modlist/loader evidence выбранного профиля.'}
        foreach($file in @($evidence.files)){
            if(-not $file.path -or $file.sha256 -notmatch '^[a-fA-F0-9]{64}$'){throw 'Неправильный MO2 loader evidence.'}
            $path=Get-GameLaunchPlainPath ([string]$file.path)
            $managed=$false
            foreach($root in @($roots)+@($Plan.game)){
                $prefix=$root.TrimEnd('\')+'\'
                if($path.StartsWith($prefix,[StringComparison]::OrdinalIgnoreCase) -and $critical.ContainsKey($path.Substring($prefix.Length).Replace('\','/'))){$managed=$true;break}
            }
            # Applied renderer/dependency identity comes from the NEW receipt;
            # historical evidence remains only for external unmanaged files.
            if($managed){continue}
            if(-not(Test-Path -LiteralPath $path -PathType Leaf) -or (Get-FileHash $path).Hash -ine $file.sha256){throw 'MO2 effective loader отличается от проверенного состава (возможен shadow dxgi).'}
        }
        if(-not $evidence.shadowDxgiChecked){throw 'MO2 shadow DXGI ещё не проверен.'}
        foreach($relative in @('bin/x64/dxgi.dll','bin/x64/version.dll','bin/x64/winmm.dll')){
            $winner=Resolve-GameLaunchEffectiveFile $Config $Plan $relative -Roots $roots
            $expected=$critical[$relative]
            if(-not $expected -and $evidence.loaderHashes){$expected=$evidence.loaderHashes[$relative]}
            if($winner -and ($expected -notmatch '^[a-fA-F0-9]{64}$' -or (Get-FileHash $winner).Hash -ine $expected)){throw ('MO2 effective loader не соответствует порту (shadow): '+$relative)}
            if(-not $winner -and $expected){throw ('Отсутствует MO2 effective loader: '+$relative)}
        }
        if((Get-FileHash (Join-Path $Plan.profileRoot 'modlist.txt')).Hash -ine $modlistHash){throw 'MO2 modlist изменился во время проверки effective файлов.'}
    }
}
function Start-GameLaunchDispatcher($Config,$Plan,[string]$Log){
    return Start-StandaloneVfsLaunch $Config $Plan $Log
}
function Start-PlainGameLaunch($Plan){
    $psi=[Diagnostics.ProcessStartInfo]::new();$psi.FileName=$Plan.exe;$psi.WorkingDirectory=$Plan.workingDirectory;$psi.UseShellExecute=$false
    foreach($arg in $Plan.arguments){$psi.ArgumentList.Add($arg)}
    $process=[Diagnostics.Process]::Start($psi)
    if(-not $process){throw 'Не удалось запустить плоскую игру.'}
    try{return @{output=('PID '+$process.Id);dispatcherPid=$process.Id}}finally{$process.Dispose()}
}
function Invoke-GameProfileLaunch($Config,[string]$Log,[switch]$WatchStartup){
    $plan=Get-GameLaunchPlan $Config
    Assert-GameLaunchComposition $Config $plan
    $effective=Get-GameLaunchEffectiveConfig $Config
    $managed=Test-Path -LiteralPath (Join-Path $plan.game 'CPVR-Portable/installed.json') -PathType Leaf
    if($managed){$null=Invoke-VrLauncher $effective ($Log+'.check') -CheckOnly}
    $before=@(Get-GameLaunchProcesses @('Cyberpunk2077'))
    if($before.Count){throw 'Перед запуском закрой уже работающую игру.'}
    Invoke-NrStartupPolicy $Config
    # Launch the files/settings chosen by the user; NR/FG installation is an explicit action.
    $started=[datetime]::UtcNow
    # Re-check immediately before dispatch; no implicit install/reset/switch.
    $plan=Get-GameLaunchPlan $Config
    if($plan.kind -eq 'direct'){
        Invoke-RedmodDeployment $Config $plan ($Log+'.redmod')
        $effective.launchArguments=@($plan.arguments)
        $launch=if($managed){Invoke-VrLauncher $effective $Log}else{Start-PlainGameLaunch $plan}
    }
    else{$launch=Start-GameLaunchDispatcher $Config $plan $Log}
    $deadline=[datetime]::UtcNow.AddSeconds(45);$identity=$null
    do{
        $games=@(Get-GameLaunchProcesses @('Cyberpunk2077'))
        if($games.Count){$identity=Select-NewGameLaunchIdentity $games $before $plan.exe $started;break}
        Start-Sleep -Milliseconds 200
    }while([datetime]::UtcNow -lt $deadline)
    if(-not $identity){throw 'Не появился новый подтверждённый PID Cyberpunk.'}
    Assert-GameLaunchComposition $Config $plan
    if($plan.kind -eq 'direct'){
        $reported=[regex]::Match([string]$launch.output,'PID\s+(\d+)')
        if(-not $reported.Success -or [int]$reported.Groups[1].Value -ne $identity.pid){throw 'Direct launcher PID не совпал с новым процессом игры.'}
    }
    $output=([string]$launch.output+"`nPID "+$identity.pid+' '+$identity.exe).Trim()
    if($WatchStartup){
        $process=Get-Process -Id $identity.pid -ErrorAction Stop
        try{if($process.WaitForExit(30000)){throw ('Игра завершилась во время запуска. Код: '+$process.ExitCode)}}finally{$process.Dispose()}
    }
    return @{exitCode=0;output=$output;game=$identity;route=$plan.kind}
}
