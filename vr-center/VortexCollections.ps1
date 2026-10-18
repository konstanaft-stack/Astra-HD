$ErrorActionPreference='Stop'

function Get-VortexCollectionCatalog($Config,[switch]$IncludeFiles){
    if($Config.vortexCatalogFile){$catalog=Read-Json ([string]$Config.vortexCatalogFile)}
    else{
        $data=Get-GameLaunchPlainPath ([string]$Config.vortexData)
        $db=Join-Path $data 'state.v2'
        if(-not(Test-Path -LiteralPath $db -PathType Container)){throw 'Не найдена база профилей Vortex.'}
        if(Get-Process Vortex -ErrorAction SilentlyContinue){throw 'Закрой Vortex, чтобы прочитать сохранённые профили. Для запуска подключённой коллекции он не потребуется.'}
        $cache=Join-Path $Config.data 'vortex-catalog-cache'
        [IO.Directory]::CreateDirectory($cache)|Out-Null
        $inventory=@(Get-ChildItem -LiteralPath $db -File|Where-Object Name -ne 'LOCK'|Sort-Object Name|ForEach-Object {$_.Name+':'+$_.Length+':'+$_.LastWriteTimeUtc.Ticks})
        $deploymentPath=Join-Path $Config.game 'vortex.deployment.json'
        $deploymentStamp=if(Test-Path -LiteralPath $deploymentPath){(Get-FileHash -LiteralPath $deploymentPath).Hash}else{'none'}
        $stamp=($inventory -join '|')+'|deployment:'+$deploymentStamp
        $stored=Read-Json (Join-Path $cache 'stamp.json')
        $output=Join-Path $cache 'catalog.json'
        if($stored.stamp -ceq $stamp -and (Test-Path -LiteralPath $output)){$catalog=Read-Json $output}
        else{
            $snapshot=Join-Path $cache ('snapshot-'+[guid]::NewGuid().ToString('N'))
            [IO.Directory]::CreateDirectory($snapshot)|Out-Null
            foreach($file in Get-ChildItem -LiteralPath $db -File){if($file.Attributes -band [IO.FileAttributes]::ReparsePoint){throw 'Неожиданная ссылка в базе Vortex.'};[IO.File]::Copy($file.FullName,(Join-Path $snapshot $file.Name),$false)}
            $reader=Join-Path $PSScriptRoot 'integrations/vortex/read-catalog.js'
            $exe=Get-GameLaunchPlainPath ([string]$Config.vortex)
            $psi=[Diagnostics.ProcessStartInfo]::new($exe);$psi.UseShellExecute=$false;$psi.CreateNoWindow=$true;$psi.WindowStyle='Hidden'
            $psi.Environment['ELECTRON_RUN_AS_NODE']='1';$psi.RedirectStandardOutput=$true;$psi.RedirectStandardError=$true
            foreach($arg in @($reader,'--db',$snapshot,'--data',$data,'--game',[string]$Config.game,'--output',$output)){$psi.ArgumentList.Add($arg)}
            $process=[Diagnostics.Process]::Start($psi)
            try{$stdout=$process.StandardOutput.ReadToEndAsync();$stderr=$process.StandardError.ReadToEndAsync();$process.WaitForExit();$null=$stdout.GetAwaiter().GetResult();$errorText=$stderr.GetAwaiter().GetResult();if($process.ExitCode -ne 0){throw ('Не удалось прочитать профили Vortex: '+$errorText)}}finally{$process.Dispose()}
            $after=(@(Get-ChildItem -LiteralPath $db -File|Where-Object Name -ne 'LOCK'|Sort-Object Name|ForEach-Object {$_.Name+':'+$_.Length+':'+$_.LastWriteTimeUtc.Ticks}) -join '|')+'|deployment:'+$(if(Test-Path -LiteralPath $deploymentPath){(Get-FileHash -LiteralPath $deploymentPath).Hash}else{'none'})
            if($after -cne $stamp){throw 'Состав Vortex изменился во время чтения. Прочитай список ещё раз.'}
            $catalog=Read-Json $output
            Write-Json (Join-Path $cache 'stamp.json') @{stamp=$stamp}
            # Only the enumerated, newly-created copy is removed; original state is never opened/written.
            $cacheFull=[IO.Path]::GetFullPath($cache);$snapshotFull=[IO.Path]::GetFullPath($snapshot)
            if([IO.Path]::GetDirectoryName($snapshotFull) -ine $cacheFull -or [IO.Path]::GetFileName($snapshotFull) -notmatch '^snapshot-[a-f0-9]{32}$'){throw 'Неверный путь временной копии.'}
            foreach($f in Get-ChildItem -LiteralPath $snapshotFull -File){[IO.File]::Delete($f.FullName)}
            [IO.Directory]::Delete($snapshotFull,$false)
        }
    }
    if($catalog.schema -ne 1 -or $catalog.profiles -isnot [array]){throw 'Неверный каталог профилей Vortex.'}
    if($IncludeFiles){return $catalog}
    return @{provider='vortex';root=$catalog.root;activeProfileId=$catalog.activeProfileId;profiles=@(foreach($p in $catalog.profiles){@{id=$p.id;name=$p.name;enabledCount=$p.enabledCount;active=$p.active;available=$p.available;reason=$p.reason;requiresRedmod=[bool]@($p.files|Where-Object relative -Match '^mods[/\\][^/\\]+[/\\]info.json$').Count}})}
}

function Restore-VortexCollectionPark($Config,$Source){
    if(-not $Source.physicalPark){return}
    $park=Get-GameLaunchPlainPath ([string]$Source.physicalPark.root)
    $expectedRoot=Get-GameLaunchPlainPath (Join-Path $Config.data 'vortex-physical-backups')
    if([IO.Path]::GetDirectoryName($park) -ine $expectedRoot){throw 'Резерв файлов коллекции находится вне хранилища центра.'}
    foreach($entry in @($Source.physicalPark.files)){
        $target=Resolve-ChildPath $Config.game ([string]$entry.path)
        $saved=Resolve-ChildPath $park ([string]$entry.path)
        if(Test-Path -LiteralPath $saved){
            if(Test-Path -LiteralPath $target){throw ('Не перезаписываю изменённый файл при отключении коллекции: '+$entry.path)}
            [IO.Directory]::CreateDirectory((Split-Path $target -Parent))|Out-Null
            [IO.File]::Move($saved,$target)
        }
    }
}

function Import-VortexCollection($Config,[string]$Profile,[string[]]$Arguments=@('--launcher-skip'),[switch]$Refresh){
    foreach($arg in $Arguments){if($arg -notin @('--launcher-skip','-modded','-skipStartScreen')){throw 'Неверный аргумент запуска коллекции.'}}
    if(Get-Process Cyberpunk2077,Vortex -ErrorAction SilentlyContinue){throw 'Закрой игру и Vortex перед подключением коллекции.'}
    $catalog=Get-VortexCollectionCatalog $Config -IncludeFiles
    $selected=@($catalog.profiles|Where-Object id -CEQ $Profile)
    if($selected.Count -ne 1){throw 'Профиль Vortex не найден.'};$selected=$selected[0]
    if(-not $selected.available){throw ('Коллекция не подключена: '+$selected.reason)}
    if($Profile -notmatch '^[a-zA-Z0-9_-]+$'){throw 'Неверный идентификатор профиля Vortex.'}
    $previous=Get-GameLaunchSource $Config
    if($Refresh -and ($previous.kind -ne 'vortex' -or $previous.origin.profileId -cne $Profile)){throw 'Обновлять можно только выбранную коллекцию Vortex.'}
    $data=Get-GameLaunchPlainPath ([string]$Config.data);$target=Join-Path $data 'collection'
    $stage=Join-Path $data ('collection-import-'+[guid]::NewGuid().ToString('N'))
    $old=Join-Path $data ('collection-before-'+[guid]::NewGuid().ToString('N'))
    $shared=Join-Path $data ('shared-collections/vortex-'+[guid]::NewGuid().ToString('N'))
    $private=Join-Path $stage 'mods/Resolved';$common=Join-Path $shared 'mods/Resolved'
    $profileRoot=Join-Path $stage ('profiles/'+$Profile)
    foreach($dir in @($private,$common,$profileRoot,(Join-Path $stage 'overwrite'))){[IO.Directory]::CreateDirectory($dir)|Out-Null}
    $archiveFiles=@();$seen=@{}
    foreach($file in @($selected.files)){
        $rel=([string]$file.relative).Replace('\','/');$source=Get-GameLaunchPlainPath ([string]$file.source)
        $staging=Get-GameLaunchPlainPath ([string]$catalog.stagingPath)
        if(-not $source.StartsWith($staging+'\',[StringComparison]::OrdinalIgnoreCase)){throw 'Файл коллекции находится вне библиотеки Vortex.'}
        if(-not(Test-Path -LiteralPath $source -PathType Leaf) -or (Get-Item -LiteralPath $source).Length -ne $file.size){throw ('Файл Vortex изменился или отсутствует: '+$rel)}
        if($seen.ContainsKey($rel)){throw ('Повтор назначения в коллекции: '+$rel)};$seen[$rel]=$true
        $destination=Resolve-ChildPath $(if($rel.EndsWith('.archive',[StringComparison]::OrdinalIgnoreCase)){$common}else{$private}) $rel
        [IO.Directory]::CreateDirectory((Split-Path $destination -Parent))|Out-Null
        if($rel.EndsWith('.archive',[StringComparison]::OrdinalIgnoreCase)){
            # usvfs file links need the parent in the corresponding merged private tree.
            # Create directories only; the archive payload remains shared.
            [IO.Directory]::CreateDirectory((Split-Path (Resolve-ChildPath $private $rel) -Parent))|Out-Null
            if([IO.Path]::GetPathRoot($source) -ine [IO.Path]::GetPathRoot($destination)){throw 'Общая библиотека архивов Vortex и хранилище центра должны быть на одном диске; копирование больших архивов автоматически не выполняется.'}
            New-Item -ItemType HardLink -Path $destination -Target $source -ErrorAction Stop|Out-Null
            $archiveFiles+=@{path=$rel;size=[long]$file.size}
        }else{[IO.File]::Copy($source,$destination,$false)}
    }
    [IO.File]::WriteAllText((Join-Path $profileRoot 'modlist.txt'),"+Resolved`n",[Text.UTF8Encoding]::new($false))
    Write-Json (Join-Path $stage 'shared-archives.json') @{schema=1;mods=@{Resolved=@($archiveFiles)}}
    # Keep writable runtime overrides on refresh of the same imported profile.
    if($Refresh -and (Test-Path -LiteralPath $previous.collection.overwriteRoot)){Copy-Mo2CollectionDirectory $previous.collection.overwriteRoot (Join-Path $stage 'overwrite')}
    $park=$previous.physicalPark
    if(-not $park){$park=@{root=(Join-Path $data ('vortex-physical-backups/'+[guid]::NewGuid().ToString('N')));files=@()}}
    $source=@{schema=1;kind='vortex';profile=$Profile;profileId=$Profile;profileName=$selected.name;executable='Cyberpunk2077';arguments=@($Arguments);importedUtc=[datetime]::UtcNow.ToString('o');origin=@{provider='vortex';root=$catalog.root;profileId=$Profile;profile=$selected.name;modsRoot=$catalog.stagingPath};cyberpunk=@{};physicalPark=$park;collection=@{root=$target;game=[IO.Path]::GetFullPath($Config.game);modsRoot=(Join-Path $target 'mods');profilesRoot=(Join-Path $target 'profiles');overwriteRoot=(Join-Path $target 'overwrite');sharedModsRoot=(Join-Path $shared 'mods');storageMode='shared-archives';executables=@{Cyberpunk2077='bin/x64/Cyberpunk2077.exe'}}}
    $sourcePath=Join-Path $data 'launch-source.json';$oldSource=if(Test-Path -LiteralPath $sourcePath){[IO.File]::ReadAllText($sourcePath)}else{$null}
    $movedOld=$false;$movedNew=$false;$parkedNow=@()
    try{
        # Suppress only physical Vortex files excluded by the chosen profile. Never touch its staging library.
        $deployment=Read-Json (Join-Path $Config.game 'vortex.deployment.json')
        if($deployment -and $deployment.gameId -eq 'cyberpunk2077' -and [IO.Path]::GetFullPath([string]$deployment.targetPath).TrimEnd('\') -ieq [IO.Path]::GetFullPath([string]$Config.game).TrimEnd('\')){
            foreach($entry in @($deployment.files)){
                $relative=([string]$entry.relPath).Replace('\','/')
                if($seen.ContainsKey($relative)){continue}
                $live=Resolve-ChildPath $Config.game $relative
                if(-not(Test-Path -LiteralPath $live -PathType Leaf)){continue}
                $saved=Resolve-ChildPath ([string]$park.root) $relative
                if(Test-Path -LiteralPath $saved){throw ('Сохранённый файл коллекции уже существует: '+$relative)}
                [IO.Directory]::CreateDirectory((Split-Path $saved -Parent))|Out-Null
                [IO.File]::Move($live,$saved);$parkedNow+=@{path=$relative};$park.files+=@{path=$relative}
            }
        }
        if(Test-Path -LiteralPath $target){[IO.Directory]::Move($target,$old);$movedOld=$true}
        [IO.Directory]::Move($stage,$target);$movedNew=$true
        Write-Json $sourcePath $source
        $plan=Get-GameLaunchPlan $Config -InspectOnly
        $null=Get-GameLaunchModRoots $Config $plan
        $source.collection.profileEvidence=@{}
        $loaders=@{}
        foreach($relative in @('bin/x64/dxgi.dll','bin/x64/version.dll','bin/x64/winmm.dll')){
            $effective=Resolve-GameLaunchEffectiveFile $Config $plan $relative
            if($effective){$loaders[$relative]=(Get-FileHash -LiteralPath $effective).Hash}
        }
        $source.collection.profileEvidence[$Profile]=@{modlistSha256=(Get-FileHash -LiteralPath (Join-Path $plan.profileRoot 'modlist.txt')).Hash;loaderHashes=$loaders;shadowDxgiChecked=$true;files=@()}
        Write-Json $sourcePath $source
        if($movedOld){Write-Json (Join-Path $data ('collection-recovery/'+[IO.Path]::GetFileName($old)+'.json')) @{schema=1;root=$old;state='retained';reason='Предыдущая частная коллекция сохранена для возврата; общие архивы не копировались.'}}
        return $source
    }catch{
        if($movedNew){[IO.Directory]::Move($target,$stage)}
        if($movedOld){[IO.Directory]::Move($old,$target)}
        foreach($entry in $parkedNow){$saved=Resolve-ChildPath $park.root $entry.path;$live=Resolve-ChildPath $Config.game $entry.path;if((Test-Path -LiteralPath $saved) -and -not(Test-Path -LiteralPath $live)){[IO.Directory]::CreateDirectory((Split-Path $live -Parent))|Out-Null;[IO.File]::Move($saved,$live)}}
        if($null -ne $oldSource){[IO.File]::WriteAllText($sourcePath,$oldSource,[Text.UTF8Encoding]::new($false))}elseif(Test-Path -LiteralPath $sourcePath){[IO.File]::Delete($sourcePath)}
        throw
    }
}
