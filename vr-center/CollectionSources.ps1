$ErrorActionPreference='Stop'

function Read-Mo2CollectionIni([string]$Path){
    $values=@{};$section=''
    foreach($line in [IO.File]::ReadAllLines($Path)){
        if($line -match '^\s*\[(.+)\]\s*$'){$section=$Matches[1];continue}
        if($line -match '^\s*([^;#=]+?)=(.*)$'){$key=[Uri]::UnescapeDataString($Matches[1].Trim());$values[$section+'/'+$key]=$Matches[2].Trim()}
    }
    return $values
}
function Resolve-Mo2CollectionPath($Values,[string]$Key,[string]$Default,[string]$Base){
    $value=$Default
    foreach($section in @('General','Settings')){if($Values.ContainsKey($section+'/'+$Key)){$value=[string]$Values[$section+'/'+$Key]}}
    if($value -match '^@ByteArray\((.*)\)$'){$value=$Matches[1]}
    $value=$value.Trim('"').Replace('\\','\').Replace('%BASE_DIR%',$Base)
    if($value.Contains('%')){throw ('Неподдерживаемая переменная пути MO2: '+$Key)}
    return Get-GameLaunchPlainPath ([IO.Path]::GetFullPath($value,$Base))
}
function Get-Mo2CollectionCatalog([string]$Root){
    $rootPath=Get-GameLaunchPlainPath $Root
    $ini=Join-Path $rootPath 'ModOrganizer.ini'
    if(-not(Test-Path -LiteralPath $ini -PathType Leaf)){throw 'В выбранной папке нет ModOrganizer.ini коллекции.'}
    $values=Read-Mo2CollectionIni $ini
    if($values['General/gameName'] -cne 'Cyberpunk 2077'){throw 'Выбранная коллекция относится не к Cyberpunk 2077.'}
    $base=Resolve-Mo2CollectionPath $values 'base_directory' $rootPath $rootPath
    $mods=Resolve-Mo2CollectionPath $values 'mod_directory' 'mods' $base
    $profiles=Resolve-Mo2CollectionPath $values 'profiles_directory' 'profiles' $base
    $overwrite=Resolve-Mo2CollectionPath $values 'overwrite_directory' 'overwrite' $base
    $names=@(Get-ChildItem -LiteralPath $profiles -Directory -ErrorAction Stop|Where-Object {Test-Path -LiteralPath (Join-Path $_.FullName 'modlist.txt') -PathType Leaf}|Sort-Object Name|ForEach-Object Name)
    $options=@{}
    foreach($option in @{enforce_archive_load_order='enforceArchiveOrder';reverse_archive_load_order='reverseArchiveOrder';reverse_redmod_load_order='reverseRedmodOrder'}.GetEnumerator()){
        $value=$values['Plugins/Cyberpunk 2077 Support Plugin\'+$option.Key]
        $options[$option.Value]=([string]$value -ieq 'true')
    }
    return @{root=$rootPath;modsRoot=$mods;profilesRoot=$profiles;overwriteRoot=$overwrite;profiles=$names;cyberpunk=$options}
}
function Test-CollectionSharedArchive([string]$Path){return $Path.EndsWith('.archive',[StringComparison]::OrdinalIgnoreCase) -or $Path.EndsWith('.archive.mohidden',[StringComparison]::OrdinalIgnoreCase)}
function Copy-Mo2CollectionDirectory([string]$Source,[string]$Target,[switch]$ShareArchives){
    $null=Get-GameLaunchPlainPath $Source;$null=Get-GameLaunchPlainPath $Target
    [IO.Directory]::CreateDirectory($Target)|Out-Null
    if(-not(Test-Path -LiteralPath $Source -PathType Container)){return}
    if(@(Get-ChildItem -LiteralPath $Source -Recurse -Force -Attributes ReparsePoint).Count){throw 'В данных импортируемого профиля есть ссылки; требуется обычная папка.'}
    if($ShareArchives){
        foreach($item in Get-ChildItem -LiteralPath $Source -Recurse -Force){
            $destination=Join-Path $Target ([IO.Path]::GetRelativePath($Source,$item.FullName))
            if($item.PSIsContainer){[IO.Directory]::CreateDirectory($destination)|Out-Null}
            elseif(-not(Test-CollectionSharedArchive $item.Name)){
                [IO.Directory]::CreateDirectory((Split-Path $destination -Parent))|Out-Null
                [IO.File]::Copy($item.FullName,$destination,$true)
            }
        }
    }else{foreach($item in Get-ChildItem -LiteralPath $Source -Force){Copy-Item -LiteralPath $item.FullName -Destination $Target -Recurse -Force}}
}
function Get-CollectionCopyInventory([string]$Root){
    $null=Get-GameLaunchPlainPath $Root
    $files=[Collections.Generic.List[object]]::new()
    foreach($entry in Get-ChildItem -LiteralPath $Root -Recurse -Force){
        if($entry.Attributes -band [IO.FileAttributes]::ReparsePoint){throw 'Ссылка в служебной копии коллекции.'}
        if(-not $entry.PSIsContainer){$files.Add(@{path=[IO.Path]::GetRelativePath($Root,$entry.FullName);size=[long]$entry.Length;sha256=(Get-FileHash -LiteralPath $entry.FullName).Hash})}
    }
    return ,@($files.ToArray())
}
function Remove-VerifiedCollectionCopy([string]$Data,[string]$Root,$Expected){
    $dataRoot=Get-GameLaunchPlainPath $Data;$copyRoot=Get-GameLaunchPlainPath $Root
    if([IO.Path]::GetDirectoryName($copyRoot) -ine $dataRoot -or [IO.Path]::GetFileName($copyRoot) -notmatch '^collection-before-[a-f0-9]{32}$'){throw 'Служебная копия вне разрешённого каталога.'}
    $actual=Get-CollectionCopyInventory $copyRoot
    $known=@{};foreach($entry in $Expected){$known[$entry.path]=$entry}
    if($actual.Count -ne $known.Count){throw 'Состав служебной копии изменён; она сохранена.'}
    foreach($entry in $actual){$prior=$known[$entry.path];if(-not $prior -or $prior.sha256 -ine $entry.sha256 -or $prior.size -ne $entry.size){throw 'Файл служебной копии изменён; она сохранена.'}}
    # Only verified, enumerated files are removed. Never recursively delete a
    # computed directory: newly appearing files keep their directory intact.
    foreach($entry in $actual){
        $file=Get-GameLaunchPlainPath (Join-Path $copyRoot $entry.path)
        if(-not $file.StartsWith($copyRoot+'\',[StringComparison]::OrdinalIgnoreCase)){throw 'Файл вне служебной копии.'}
        if((Get-FileHash -LiteralPath $file).Hash -ine $entry.sha256){throw 'Файл изменился во время очистки; оставшиеся данные сохранены.'}
        [IO.File]::Delete($file)
    }
    foreach($directory in @(Get-ChildItem -LiteralPath $copyRoot -Recurse -Directory -Force|Sort-Object @{Expression={$_.FullName.Length};Descending=$true})){
        $null=Get-GameLaunchPlainPath $directory.FullName
        [IO.Directory]::Delete($directory.FullName,$false)
    }
    [IO.Directory]::Delete($copyRoot,$false)
}
function Import-Mo2Collection($Config,[string]$Root,[string]$Profile,[string[]]$Arguments=@('--launcher-skip'),[switch]$Refresh){
    $catalog=Get-Mo2CollectionCatalog $Root
    if($Profile -notin $catalog.profiles -or $Profile -match '[\\/:\x00-\x1f]' -or $Profile -in @('.','..')){throw 'Выбранный профиль коллекции не найден.'}
    foreach($argument in $Arguments){if($argument -notin @('--launcher-skip','-modded','-skipStartScreen')){throw 'Неподдерживаемый аргумент запуска Cyberpunk.'}}
    $data=Get-GameLaunchPlainPath ([string]$Config.data);$target=Join-Path $data 'collection'
    $stage=Join-Path $data ('collection-import-'+[guid]::NewGuid().ToString('N'))
    $prior=Join-Path $data ('collection-before-'+[guid]::NewGuid().ToString('N'))
    $sourcePath=Join-Path $data 'launch-source.json';$oldSource=if(Test-Path -LiteralPath $sourcePath){[IO.File]::ReadAllText($sourcePath)}else{$null}
    $previous=Get-GameLaunchSource $Config
    if($Refresh -and ($previous.kind -ne 'mo2' -or $previous.origin.root -ine $catalog.root -or $previous.profile -cne $Profile)){throw 'Обновлять можно только текущую импортированную коллекцию.'}
    $profileSource=Join-Path $catalog.profilesRoot $Profile
    $sourcePlan=@{profileRoot=$profileSource;collection=@{root=$catalog.root;modsRoot=$catalog.modsRoot;overwriteRoot=$catalog.overwriteRoot}}
    $modRoots=@(Get-GameLaunchModRoots $Config $sourcePlan | Where-Object {$_ -ine $catalog.overwriteRoot})
    # The shared library is available to every copied profile, including options
    # initially disabled in the list. Prepare their small writable sidecars too.
    $libraryRoots=@(Get-ChildItem -LiteralPath $catalog.modsRoot -Directory | Where-Object {-not $_.Name.EndsWith('_separator',[StringComparison]::OrdinalIgnoreCase)} | ForEach-Object {Get-GameLaunchPlainPath $_.FullName})
    $overwriteSource=if($Refresh){$previous.collection.overwriteRoot}else{$catalog.overwriteRoot}
    [long]$bytes=0
    foreach($copyRoot in @($profileSource,$overwriteSource)+$libraryRoots){
        $null=Get-GameLaunchPlainPath $copyRoot
        if($data -ieq $copyRoot -or $data.StartsWith($copyRoot.TrimEnd('\')+'\',[StringComparison]::OrdinalIgnoreCase)){throw 'Хранилище центра не должно находиться внутри импортируемых модов.'}
        if(Test-Path -LiteralPath $copyRoot){foreach($file in Get-ChildItem -LiteralPath $copyRoot -Recurse -Force){
            if($file.Attributes -band [IO.FileAttributes]::ReparsePoint){throw 'В коллекции обнаружена ссылка вместо обычного файла.'}
            if(-not $file.PSIsContainer -and -not($copyRoot -in $libraryRoots -and (Test-CollectionSharedArchive $file.Name))){$bytes+=$file.Length}
        }}
    }
    $drive=[IO.DriveInfo]::new([IO.Path]::GetPathRoot($data))
    if($drive.AvailableFreeSpace -lt $bytes+16MB){throw ('Недостаточно места для независимого состава коллекции: нужно '+[math]::Ceiling($bytes/1GB)+' ГиБ.')}
    Copy-Mo2CollectionDirectory $profileSource (Join-Path $stage ('profiles/'+$Profile))
    $sharedInventory=@{schema=1;mods=@{}}
    foreach($modRoot in $libraryRoots){
        $modName=[IO.Path]::GetFileName($modRoot)
        Copy-Mo2CollectionDirectory $modRoot (Join-Path $stage ('mods/'+$modName)) -ShareArchives
        $sharedInventory.mods[$modName]=@(Get-ChildItem -LiteralPath $modRoot -Recurse -File -Filter '*.archive' | ForEach-Object {@{path=[IO.Path]::GetRelativePath($modRoot,$_.FullName);size=[long]$_.Length}})
    }
    Write-Json (Join-Path $stage 'shared-archives.json') $sharedInventory
    if($Refresh){
        Copy-Mo2CollectionDirectory $previous.collection.overwriteRoot (Join-Path $stage 'overwrite')
        $settings=Join-Path $target ('profiles/'+$Profile+'/UserSettings.json')
        if(Test-Path -LiteralPath $settings){[IO.File]::Copy($settings,(Join-Path $stage ('profiles/'+$Profile+'/UserSettings.json')),$true)}
    }else{Copy-Mo2CollectionDirectory $catalog.overwriteRoot (Join-Path $stage 'overwrite')}
    $source=@{schema=1;kind='mo2';profile=$Profile;executable='Cyberpunk2077';arguments=@($Arguments);importedUtc=[datetime]::UtcNow.ToString('o');origin=@{root=$catalog.root;profile=$Profile;modsRoot=$catalog.modsRoot};cyberpunk=$catalog.cyberpunk;collection=@{root=$target;game=[IO.Path]::GetFullPath($Config.game);modsRoot=(Join-Path $target 'mods');profilesRoot=(Join-Path $target 'profiles');overwriteRoot=(Join-Path $target 'overwrite');executables=@{Cyberpunk2077='bin/x64/Cyberpunk2077.exe'}}}
    if($previous.physicalPark){$source.physicalPark=$previous.physicalPark}
    $source.collection.sharedModsRoot=$catalog.modsRoot
    $source.collection.storageMode='shared-archives'
    if(Test-Path -LiteralPath (Join-Path $stage ('profiles/'+$Profile+'/UserSettings.json'))){$source.settingsRelative='UserSettings.json'}
    $previousFiles=if(Test-Path -LiteralPath $target){Get-CollectionCopyInventory $target}else{@()}
    $movedOld=$false;$movedNew=$false
    try{
        if(Test-Path -LiteralPath $target){$null=Get-GameLaunchPlainPath $target;[IO.Directory]::Move($target,$prior);$movedOld=$true}
        [IO.Directory]::Move($stage,$target);$movedNew=$true
        Write-Json $sourcePath $source
        $plan=Get-GameLaunchPlan $Config -InspectOnly
        $null=Get-GameLaunchModRoots $Config $plan
        $loaderPlan=@{};foreach($key in $plan.Keys){$loaderPlan[$key]=$plan[$key]};$loaderPlan.selectedFiles=@{}
        $loaders=@{}
        foreach($relative in @('bin/x64/dxgi.dll','bin/x64/version.dll','bin/x64/winmm.dll')){
            $winner=Resolve-GameLaunchEffectiveFile $Config $loaderPlan $relative
            # Fingerprint the collection, not the currently selected renderer; otherwise
            # refreshing in VR would prevent returning to the authored flat loader.
            if($winner -and $winner -ine $plan.selectedFiles[$relative]){$loaders[$relative]=(Get-FileHash -LiteralPath $winner).Hash}
        }
        $source.collection.profileEvidence=@{}
        $source.collection.profileEvidence[$Profile]=@{modlistSha256=(Get-FileHash (Join-Path $plan.profileRoot 'modlist.txt')).Hash;loaderHashes=$loaders;shadowDxgiChecked=$true;files=@()}
        Write-Json $sourcePath $source
    }catch{
        if($movedNew){[IO.Directory]::Move($target,$stage)}
        if($movedOld){[IO.Directory]::Move($prior,$target)}
        if($null -ne $oldSource){[IO.File]::WriteAllText($sourcePath,$oldSource,[Text.UTF8Encoding]::new($false))}elseif(Test-Path -LiteralPath $sourcePath){[IO.File]::Delete($sourcePath)}
        throw
    }
    if($movedOld){
        $receiptPath=Join-Path $data ('collection-recovery/'+[IO.Path]::GetFileName($prior)+'.json')
        $record=@{schema=1;root=$prior;activeRoot=$target;files=$previousFiles;state='retained';utc=[datetime]::UtcNow.ToString('o')}
        Write-Json $receiptPath $record
        try{Remove-VerifiedCollectionCopy $data $prior $previousFiles;$record.state='cleaned'}catch{
            $record.reason=$_.Exception.Message;$source.cleanupWarning='Коллекция подключена; предыдущая служебная копия сохранена: '+$prior
            Write-Json $sourcePath $source
        }
        Write-Json $receiptPath $record
    }
    return $source
}
