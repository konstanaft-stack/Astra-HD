. "$PSScriptRoot/Core.ps1"

function Get-TestSavePaths($Config){
    $saves=if($Config.savedGamesPath){[string]$Config.savedGamesPath}elseif($Config.userStateFile){Split-Path ([string]$Config.userStateFile) -Parent}else{Join-Path ([Environment]::GetFolderPath('UserProfile')) 'Saved Games/CD Projekt Red/Cyberpunk 2077'}
    $root=Join-Path $Config.data 'game-settings/test-saves';$library=Join-Path $Config.data 'test-save-library'
    return @{root=$root;state=(Join-Path $root 'state.json');cache=(Join-Path $root 'cache');library=$library;catalog=(Join-Path $library 'catalog.json');saves=[IO.Path]::GetFullPath($saves)}
}
function Assert-TestSaveGameClosed {
    if(Get-Process Cyberpunk2077 -ErrorAction SilentlyContinue){throw 'Перед изменением тестовых сохранений закрой Cyberpunk 2077.'}
}
function Assert-TestSavePlainPath([string]$Path){
    $cursor=[IO.Path]::GetFullPath($Path)
    while($cursor){if(Test-Path -LiteralPath $cursor){if((Get-Item -LiteralPath $cursor -Force).Attributes -band [IO.FileAttributes]::ReparsePoint){throw ('Связанные папки сохранений не поддерживаются: '+$cursor)}};$parent=Split-Path $cursor -Parent;if($parent -eq $cursor){break};$cursor=$parent}
}
function Get-TestSaveFiles([string]$Root){
    Assert-TestSavePlainPath $Root
    if(-not(Test-Path -LiteralPath $Root -PathType Container)){throw ('Не найдена папка сохранения: '+$Root)}
    $prefix=[IO.Path]::GetFullPath($Root).TrimEnd('\','/')+[IO.Path]::DirectorySeparatorChar
    $result=@();$pending=[Collections.Generic.Stack[string]]::new();$pending.Push($Root)
    while($pending.Count){foreach($entry in Get-ChildItem -LiteralPath $pending.Pop() -Force){
        Assert-TestSavePlainPath $entry.FullName
        if($entry.PSIsContainer){$pending.Push($entry.FullName)}else{$result+=@{path=$entry.FullName.Substring($prefix.Length).Replace('\','/');source=$entry.FullName;sha256=(Get-FileHash -LiteralPath $entry.FullName).Hash}}
    }}
    return @($result|Sort-Object path)
}
function Copy-TestSaveTree([string]$Source,[string]$Target){
    Assert-TestSavePlainPath $Target
    $files=@(Get-TestSaveFiles $Source)
    [void][IO.Directory]::CreateDirectory($Target)
    $prefix=[IO.Path]::GetFullPath($Source).TrimEnd('\','/')+[IO.Path]::DirectorySeparatorChar
    foreach($dir in Get-ChildItem -LiteralPath $Source -Directory -Recurse -Force){
        Assert-TestSavePlainPath $dir.FullName
        [void][IO.Directory]::CreateDirectory((Resolve-ChildPath $Target $dir.FullName.Substring($prefix.Length)))
    }
    foreach($file in $files){
        $dest=Resolve-ChildPath $Target $file.path;[void][IO.Directory]::CreateDirectory((Split-Path $dest -Parent))
        [IO.File]::Copy($file.source,$dest,$true)
        if((Get-FileHash -LiteralPath $dest).Hash -ine $file.sha256){throw 'Не удалось проверить копию сохранения.'}
    }
}
function Remove-TestSaveWorkspace([string]$Base,[string]$Path){
    $full=[IO.Path]::GetFullPath($Path);$baseFull=[IO.Path]::GetFullPath($Base)
    if([IO.Path]::GetDirectoryName($full) -ine $baseFull -or [IO.Path]::GetFileName($full) -notmatch '^(txn|import)-[a-f0-9]{32}$'){throw 'Неверный временный каталог сохранений.'}
    if(Test-Path -LiteralPath $full){$null=Get-TestSaveFiles $full;Remove-Item -LiteralPath $full -Recurse -Force}
}
function Test-TestSaveOwned([string]$Folder,[string]$Token){
    Assert-TestSavePlainPath $Folder
    try{$marker=Read-Json (Join-Path $Folder '.vr-center-test-save.json')}catch{return $false}
    return [bool]($marker -and $marker.schema -eq 1 -and $marker.token -ceq $Token)
}
function Remove-TestSaveOwned([string]$Folder,[string]$Token){
    if(-not(Test-Path -LiteralPath $Folder)){return}
    if(-not(Test-TestSaveOwned $Folder $Token)){throw ('Папка больше не принадлежит тестовому набору: '+$Folder)}
    $null=Get-TestSaveFiles $Folder
    Remove-Item -LiteralPath ([IO.Path]::GetFullPath($Folder)) -Recurse -Force
}
function Move-TestSaveOwned([string]$Source,[string]$Target,[string]$Token){
    if(-not(Test-TestSaveOwned $Source $Token)){throw 'Нет подтверждения владения сохранением.'}
    if(Test-Path -LiteralPath $Target){throw ('Папка сохранения уже существует: '+$Target)}
    [void][IO.Directory]::CreateDirectory($Target)
    Write-Json (Join-Path $Target '.vr-center-test-save.json') @{schema=1;token=$Token}
    Copy-TestSaveTree $Source $Target
    Remove-TestSaveOwned $Source $Token
}
function Get-TestSaveCatalog($Paths){
    $catalog=Read-Json $Paths.catalog
    if(-not $catalog){return @{schema=1;sets=@()}}
    if($catalog.schema -ne 1 -or $catalog.sets -isnot [array]){throw 'Повреждён каталог тестовых сохранений.'}
    return $catalog
}
function Get-TestSaveRecord($Paths,$Catalog){
    $record=Read-Json $Paths.state
    if(-not $record){return @{schema=1;enabled=$false;selectedIds=@($Catalog.sets|ForEach-Object id);items=@()}}
    if($record.schema -ne 1 -or $record.items -isnot [array]){throw 'Повреждено состояние тестовых сохранений.'}
    return $record
}
function Get-TestSaveState($Config){
    $p=Get-TestSavePaths $Config;$catalog=Get-TestSaveCatalog $p;$r=Get-TestSaveRecord $p $catalog
    $installed=@($r.items|Where-Object {$_.location -eq 'active' -and (Test-TestSaveOwned (Resolve-ChildPath $p.saves $_.slot) $_.token)}).Count
    return @{enabled=[bool]$r.enabled;selectedIds=@($r.selectedIds);sets=@($catalog.sets|ForEach-Object {@{id=$_.id;label=$_.label;saveCount=$_.saves.Count;selected=($_.id -in $r.selectedIds)}});installedCount=$installed;libraryRoot=$p.library}
}
function Import-TestSaveSet($Config,[string]$Path,[string]$Label){
    Assert-TestSaveGameClosed
    $p=Get-TestSavePaths $Config;$source=[IO.Path]::GetFullPath($Path)
    if(-not(Test-Path -LiteralPath $source -PathType Container)){throw 'Для импорта выбери распакованную папку сохранений.'}
    $all=@(Get-TestSaveFiles $source);$folders=@($all|Where-Object {(Split-Path $_.source -Leaf) -eq 'sav.dat'}|ForEach-Object {Split-Path $_.source -Parent}|Sort-Object -Unique)
    if(-not $folders.Count){throw 'В выбранной папке нет sav.dat.'}
    foreach($a in $folders){foreach($b in $folders){if($a -ine $b -and $b.StartsWith($a.TrimEnd('\')+'\',[StringComparison]::OrdinalIgnoreCase)){throw 'Вложенные друг в друга папки сохранений не поддерживаются.'}}}
    [void][IO.Directory]::CreateDirectory($p.library);Assert-TestSavePlainPath $p.library
    $temp=Join-Path $p.library ('import-'+[guid]::NewGuid().ToString('N'))
    [void][IO.Directory]::CreateDirectory($temp);$saves=@();$fingerprint=''
    try{
        foreach($folder in $folders){
            $saveId='save-'+$saves.Count.ToString('D4');$files=@()
            foreach($file in @(Get-TestSaveFiles $folder|Where-Object path -ne '.vr-center-test-save.json')){
                $rel='saves/'+$saveId+'/'+$file.path;$dest=Resolve-ChildPath $temp $rel
                [void][IO.Directory]::CreateDirectory((Split-Path $dest -Parent));[IO.File]::Copy($file.source,$dest)
                if((Get-FileHash $dest).Hash -ine $file.sha256){throw 'Повреждена импортированная копия сохранения.'}
                $files+=@{path=$file.path;sha256=$file.sha256};$fingerprint+=$saveId+'/'+$file.path+':'+$file.sha256+"`n"
            }
            $saves+=@{id=$saveId;folder=('saves/'+$saveId);files=$files}
        }
        $sha=[Security.Cryptography.SHA256]::Create();try{$digest=[BitConverter]::ToString($sha.ComputeHash([Text.Encoding]::UTF8.GetBytes($fingerprint))).Replace('-','').ToLowerInvariant()}finally{$sha.Dispose()}
        $id='set-'+$digest.Substring(0,24);$catalog=Get-TestSaveCatalog $p
        $existing=@($catalog.sets|Where-Object id -eq $id)
        if($existing.Count){return @{id=$id;label=$existing[0].label;saveCount=$existing[0].saves.Count}}
        $entry=@{id=$id;label=$(if($Label){$Label}else{Split-Path $source -Leaf});saves=$saves}
        $target=Resolve-ChildPath $p.library $id
        if(Test-Path -LiteralPath $target){throw 'Каталог набора уже существует без записи в библиотеке.'}
        [IO.Directory]::Move($temp,$target)
        try{$catalog.sets+=@($entry);Write-Json $p.catalog $catalog}catch{
            # Move only our newly created directory back; the input folder is untouched.
            [IO.Directory]::Move($target,$temp);throw
        }
        return @{id=$id;label=$entry.label;saveCount=$entry.saves.Count}
    }finally{Remove-TestSaveWorkspace $p.library $temp}
}
function Set-TestSaveConfiguration($Config,[string[]]$Ids,[bool]$Enabled){
    Assert-TestSaveGameClosed
    $p=Get-TestSavePaths $Config;$catalog=Get-TestSaveCatalog $p;$r=Get-TestSaveRecord $p $catalog
    $Ids=@($Ids|Select-Object -Unique)
    foreach($id in $Ids){if(-not @($catalog.sets|Where-Object id -ceq $id).Count){throw ('Неизвестный набор сохранений: '+$id)}}
    # Validate all clean sources before making any change to active saves.
    if($Enabled){foreach($set in @($catalog.sets|Where-Object id -in $Ids)){foreach($save in $set.saves){foreach($file in $save.files){
        $src=Resolve-ChildPath $p.library ($set.id+'/'+$save.folder+'/'+$file.path);Assert-TestSavePlainPath $src
        if(-not(Test-Path -LiteralPath $src -PathType Leaf) -or (Get-FileHash -LiteralPath $src).Hash -ine $file.sha256){throw 'Изменились исходники тестового сохранения.'}
    }}}}
    Assert-TestSavePlainPath $p.root;Assert-TestSavePlainPath $p.saves
    [void][IO.Directory]::CreateDirectory($p.root)
    $txn=Join-Path $p.root ('txn-'+[guid]::NewGuid().ToString('N'));[void][IO.Directory]::CreateDirectory($txn)
    $stateExisted=Test-Path -LiteralPath $p.state;$active=@();$touched=@{}
    try{
        if($stateExisted){[IO.File]::Copy($p.state,(Join-Path $txn 'state.json'))}
        if(Test-Path -LiteralPath $p.cache){Copy-TestSaveTree $p.cache (Join-Path $txn 'cache')}
        foreach($item in $r.items){
            if($item.slot -notmatch '^ManualSave-\d+$' -or $item.key -notmatch '^set-[a-f0-9]{24}-save-\d{4}$' -or $item.token -notmatch '^[a-f0-9]{32}$'){throw 'Повреждена привязка тестового сохранения.'}
            $target=Resolve-ChildPath $p.saves $item.slot
            if($item.location -eq 'active' -and (Test-TestSaveOwned $target $item.token)){
                $backup='active/'+$active.Count;Copy-TestSaveTree $target (Join-Path $txn $backup)
                $active+=@{slot=$item.slot;token=$item.token;backup=$backup};$touched[$item.slot]=$item.token
            }
        }
        Assert-TestSaveGameClosed
        try{
            foreach($item in $r.items){if($item.location -eq 'active' -and (-not $Enabled -or $item.setId -notin $Ids)){
                $target=Resolve-ChildPath $p.saves $item.slot
                if(Test-TestSaveOwned $target $item.token){Move-TestSaveOwned $target (Resolve-ChildPath $p.cache $item.key) $item.token;$item.location='cache'}else{$item.location='lost'}
            }}
            if($Enabled){foreach($set in @($catalog.sets|Where-Object id -in $Ids)){foreach($save in $set.saves){
                $key=$set.id+'-'+$save.id;$found=@($r.items|Where-Object key -ceq $key)
                if($found.Count -gt 1){throw 'Повтор привязки тестового сохранения.'}
                $item=if($found.Count){$found[0]}else{$new=@{key=$key;setId=$set.id;saveId=$save.id;token=[guid]::NewGuid().ToString('N');slot='';location='new'};$r.items+=@($new);$new}
                if($item.location -eq 'active' -and (Test-TestSaveOwned (Resolve-ChildPath $p.saves $item.slot) $item.token)){continue}
                $number=0;do{$slot='ManualSave-'+$number;$number++}while(Test-Path -LiteralPath (Join-Path $p.saves $slot))
                $item.slot=$slot;$target=Resolve-ChildPath $p.saves $slot;$touched[$slot]=$item.token
                $cached=Resolve-ChildPath $p.cache $key
                if(Test-Path -LiteralPath $cached){Move-TestSaveOwned $cached $target $item.token}else{
                    [void][IO.Directory]::CreateDirectory($target);Write-Json (Join-Path $target '.vr-center-test-save.json') @{schema=1;token=$item.token}
                    Copy-TestSaveTree (Resolve-ChildPath $p.library ($set.id+'/'+$save.folder)) $target
                }
                $item.location='active'
            }}}
            $r.enabled=$Enabled;$r.selectedIds=@($Ids);Write-Json $p.state $r
        }catch{
            $reason=$_.Exception.Message
            try{
                foreach($slot in $touched.Keys){Remove-TestSaveOwned (Resolve-ChildPath $p.saves $slot) $touched[$slot]}
                if(Test-Path -LiteralPath $p.cache){
                    # Cache belongs solely to this profile. Remove only marker-owned entries.
                    foreach($dir in Get-ChildItem -LiteralPath $p.cache -Directory){$m=Read-Json (Join-Path $dir.FullName '.vr-center-test-save.json');if(-not $m.token){throw 'Неизвестная папка в кэше сохранений.'};Remove-TestSaveOwned $dir.FullName $m.token}
                }
                if(Test-Path -LiteralPath (Join-Path $txn 'cache')){Copy-TestSaveTree (Join-Path $txn 'cache') $p.cache}
                foreach($entry in $active){Copy-TestSaveTree (Join-Path $txn $entry.backup) (Resolve-ChildPath $p.saves $entry.slot)}
                if($stateExisted){[IO.File]::Copy((Join-Path $txn 'state.json'),$p.state,$true)}elseif(Test-Path -LiteralPath $p.state){[IO.File]::Delete($p.state)}
            }catch{throw ($reason+'; резерв сохранён в '+$txn+'; ошибка возврата: '+$_.Exception.Message)}
            throw ($reason+'; прежние сохранения и настройки восстановлены.')
        }
    }catch{
        # Keep rollback evidence if restoration failed; otherwise this is safe to discard.
        if($_.Exception.Message -match 'ошибка возврата:'){throw}
        Remove-TestSaveWorkspace $p.root $txn;throw
    }
    Remove-TestSaveWorkspace $p.root $txn
}
function Set-TestSaveSelection($Config,[string[]]$Ids){
    $p=Get-TestSavePaths $Config;$r=Get-TestSaveRecord $p (Get-TestSaveCatalog $p)
    Set-TestSaveConfiguration $Config $Ids ([bool]$r.enabled)
}
function Set-TestSavesEnabled($Config,[bool]$Enabled){
    $p=Get-TestSavePaths $Config;$r=Get-TestSaveRecord $p (Get-TestSaveCatalog $p)
    Set-TestSaveConfiguration $Config @($r.selectedIds) $Enabled
}
