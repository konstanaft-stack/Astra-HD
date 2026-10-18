$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot 'CyberpunkCollection.ps1')

function Get-StandaloneSharedArchiveLinks($Config,$Plan){
    $Config=Get-GameLaunchCollectionConfig $Config $Plan
    if(-not $Config.mo2.sharedModsRoot){return @()}
    $shared=Get-GameLaunchPlainPath ([string]$Config.mo2.sharedModsRoot)
    $inventory=Read-Json (Join-Path $Config.mo2.root 'shared-archives.json')
    if($inventory.schema -ne 1 -or $inventory.mods -isnot [Collections.IDictionary]){throw 'Нет списка общих архивов коллекции. Обнови её подключение.'}
    $seen=@{};$links=[Collections.Generic.List[object]]::new()
    # Preserve the same highest-first winner order as private files. Only archive
    # resources are linked from the common library; runtime configs stay private.
    foreach($root in @(Get-GameLaunchModRoots $Config $Plan -IncludeShared)){
        $isShared=$root.StartsWith($shared+'\',[StringComparison]::OrdinalIgnoreCase)
        if($isShared){
            $name=[IO.Path]::GetFileName($root)
            if(-not $inventory.mods.ContainsKey($name)){throw ('Мод не записан в общей библиотеке: '+$name)}
            if(@(Get-ChildItem -LiteralPath $root -Recurse -File -Filter '*.archive').Count -ne @($inventory.mods[$name]).Count){throw ('Состав общего мода изменён. Обнови подключение коллекции: '+$name)}
            $files=@(foreach($entry in $inventory.mods[$name]){
                if(-not ([string]$entry.path).EndsWith('.archive',[StringComparison]::OrdinalIgnoreCase)){throw 'Неверная ссылка на общий архив.'}
                $path=Get-GameLaunchPlainPath (Resolve-ChildPath $root ([string]$entry.path))
                if(-not(Test-Path -LiteralPath $path -PathType Leaf) -or (Get-Item -LiteralPath $path).Length -ne $entry.size){throw ('Общий архив отсутствует или изменён: '+$name+'/'+$entry.path)}
                Get-Item -LiteralPath $path
            })
        }else{$files=@(Get-ChildItem -LiteralPath $root -Recurse -File -Filter '*.archive')}
        foreach($file in $files){
            if($file.Attributes -band [IO.FileAttributes]::ReparsePoint){throw 'Ссылка вместо общего архива мода.'}
            $relative=[IO.Path]::GetRelativePath($root,$file.FullName)
            if($seen.ContainsKey($relative)){continue};$seen[$relative]=$true
            if($isShared){
                $links.Add(@{source=(Get-GameLaunchPlainPath $file.FullName);destination=(Join-Path $Plan.game $relative);createTarget=$false})
            }
        }
    }
    return @($links.ToArray())
}

function New-StandaloneVfsLaunchPlan($Config,$Plan,[string]$Log){
    $Config=Get-GameLaunchCollectionConfig $Config $Plan
    if($Plan.kind -notin @('mo2','vortex')){throw 'Virtual launch requires an imported collection.'}
    $sharedFiles=@(Get-StandaloneSharedArchiveLinks $Config $Plan)
    if($Plan.kind -eq 'vortex'){
        $sharedRoot=Get-GameLaunchPlainPath ([string]$Config.mo2.sharedModsRoot)
        $privateRoot=Get-GameLaunchPlainPath ([string]$Config.mo2.modsRoot)
        foreach($file in $sharedFiles){
            $relative=[IO.Path]::GetRelativePath($sharedRoot,$file.source)
            [IO.Directory]::CreateDirectory((Split-Path (Resolve-ChildPath $privateRoot $relative) -Parent))|Out-Null
        }
    }
    $preparation=Prepare-CyberpunkCollection $Config $Plan
    $runtime=Get-GameLaunchPlainPath $(if($Config.vfs.runtime){$Config.vfs.runtime}else{Join-Path $PSScriptRoot 'components/usvfs'})
    if(-not(Test-Path -LiteralPath (Join-Path $runtime 'usvfs_x64.dll') -PathType Leaf)){throw 'Компонент usvfs не установлен в VR-центре.'}
    $overwrite=Get-GameLaunchPlainPath $(if($Config.mo2.overwriteRoot){$Config.mo2.overwriteRoot}else{Join-Path $Config.mo2.root 'overwrite'})
    [IO.Directory]::CreateDirectory($overwrite)|Out-Null
    $roots=@(Get-GameLaunchModRoots $Config $Plan)
    $maps=[Collections.Generic.List[object]]::new()
    # MO2 modlist is highest-first. usvfs replaces mappings, so link lowest first.
    for($i=$roots.Count-1;$i -ge 0;$i--){$maps.Add(@{source=$roots[$i];destination=$Plan.game;createTarget=($roots[$i] -ieq $overwrite)})}
    $files=$sharedFiles
    # Cyberpunk basic_games: CET and RED4ext entry points need forced library loads
    # when their proxy DLLs exist only in the virtual filesystem.
    $forced=@()
    foreach($name in @('version.dll','winmm.dll')){
        $relative='bin/x64/'+$name
        if(Resolve-GameLaunchEffectiveFile $Config $Plan $relative -Roots $roots){$forced+=Join-Path $Plan.game $relative}
    }
    if($Plan.source.settingsRelative){
        $destination=if($Config.settingsFile){$Config.settingsFile}elseif($Config.appDataPath){Join-Path $Config.appDataPath 'UserSettings.json'}else{Join-Path ([Environment]::GetFolderPath('LocalApplicationData')) 'CD Projekt Red/Cyberpunk 2077/UserSettings.json'}
        $files+=@{source=$Plan.settingsFile;destination=(Get-GameLaunchPlainPath $destination);createTarget=$false}
    }
    # Physical port files already participate in usvfs's normal base-game fallthrough.
    # A redundant self-link beneath a merged ancestor can fail with ERROR_PATH_NOT_FOUND
    # (the ancestor points at another mod which has no such VR subdirectory).
    # Only override physical files actually shadowed by the collection. Index once,
    # instead of probing every selected path against every mod on each launch.
    $shadowed=@{}
    if($Plan.selectedFiles.Count){foreach($root in $roots){
        foreach($path in [IO.Directory]::EnumerateFiles($root,'*',[IO.SearchOption]::AllDirectories)){
            $relative=[IO.Path]::GetRelativePath($root,$path).Replace('\','/')
            if($Plan.selectedFiles.ContainsKey($relative)){$shadowed[$relative]=$true}
        }
    }}
    foreach($relative in @($Plan.selectedFiles.Keys)){
        $destination=Join-Path $Plan.game $relative
        if(-not $shadowed.ContainsKey($relative) -and [IO.Path]::GetFullPath($Plan.selectedFiles[$relative]) -ieq [IO.Path]::GetFullPath($destination)){continue}
        $files+=@{source=$Plan.selectedFiles[$relative];destination=$destination;createTarget=$false}
    }
    return @{runtime=$runtime;exe=$Plan.exe;arguments=@($Plan.arguments);workingDirectory=$Plan.workingDirectory;directories=@($maps.ToArray());files=$files;forcedLibraries=$forced;timeoutSeconds=0;logPath=[IO.Path]::GetFullPath($Log);redmods=$preparation.redmods}
}

function Get-StandaloneVfsProcessInfo($Config,[string]$HostPath,[string]$RequestPath,[string]$WorkingDirectory){
    $psi=[Diagnostics.ProcessStartInfo]::new();$psi.UseShellExecute=$false;$psi.CreateNoWindow=$true
    $psi.WorkingDirectory=$WorkingDirectory
    if([IO.Path]::GetExtension($HostPath) -ieq '.dll'){
        $psi.FileName=Get-GameLaunchPlainPath ([string]$Config.vfs.dotnet);$psi.ArgumentList.Add($HostPath)
    }else{$psi.FileName=$HostPath}
    $psi.ArgumentList.Add($RequestPath)
    return $psi
}

function Start-StandaloneVfsLaunch($Config,$Plan,[string]$Log){
    $hostPath=Get-GameLaunchPlainPath $(if($Config.vfs.host){$Config.vfs.host}else{Join-Path $PSScriptRoot 'helpers/vfs/VRPorts.VfsHost.exe'})
    if(-not(Test-Path -LiteralPath $hostPath -PathType Leaf)){throw 'Не найден самостоятельный запускатель коллекций VR-центра.'}
    $request=New-StandaloneVfsLaunchPlan $Config $Plan ($Log+'.usvfs.log')
    Invoke-RedmodDeployment $Config $Plan ($Log+'.redmod') $request $hostPath
    $requestPath=Join-Path $Config.data ('launches/'+[guid]::NewGuid().ToString('N')+'.json')
    Write-Json $requestPath $request
    $psi=Get-StandaloneVfsProcessInfo $Config $hostPath $requestPath $Plan.workingDirectory
    $process=[Diagnostics.Process]::Start($psi)
    if(-not $process){throw 'Не удалось запустить виртуальную коллекцию.'}
    try{return @{dispatcherPid=$process.Id;request=$requestPath;output='Коллекция запущена через usvfs.'}}finally{$process.Dispose()}
}
