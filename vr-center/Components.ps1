. "$PSScriptRoot/Core.ps1"
function Get-LibraryCatalog($Config){
    $b=Read-Json (Join-Path $Config.bundle 'bundle.json');$result=@()
    foreach($file in @('nvngx_dlss.dll','nvngx_dlssg.dll','nvngx_dlssd.dll')){
        $path='bin/x64/'+$file;$extra=Read-Json (Join-Path $Config.data 'library-catalog.json');$hashes=@(@($b.versions|ForEach-Object {$_.files[$path]})+@($extra.entries|Where-Object file -eq $file|ForEach-Object hash)|Where-Object {$_}|Sort-Object -Unique)
        $choices=@();foreach($h in $hashes){$obj=Join-Path $Config.bundle "files/$h";if(Test-Path $obj){$version=([string][Diagnostics.FileVersionInfo]::GetVersionInfo($obj).FileVersion).Replace(',','.');$choices+=@{hash=$h;version=$version.TrimEnd('.0');label=$version}}}
        $live=Join-Path $Config.game $path;$current=if(Test-Path $live){(Get-FileHash $live).Hash}else{''}
        $result+=@{file=$file;path=$path;current=$current;choices=$choices}
    }
    return $result
}
function Assert-GameWritable($Config){
    if(Test-CyberpunkProfileMode $Config){
        Assert-CyberpunkProfileMutationAllowed $Config
        Assert-GameProfileNoRecovery (Get-GameProfileContext (Get-CyberpunkProfileStoreConfig $Config))
        return
    }
    if((Read-Json $Config.hold).active){throw 'Сначала заверши первый запуск игры. Состав игры пока не меняем.'}
    if(Get-Process Cyberpunk2077,Vortex -ErrorAction SilentlyContinue){throw 'Для применения закрой Cyberpunk и Vortex.'}
}
function Invoke-ComponentTransaction($Config,$Changes){
    Assert-GameWritable $Config
    $backup=Join-Path $Config.data ('transactions/'+[guid]::NewGuid().ToString('N'));[IO.Directory]::CreateDirectory($backup)|Out-Null
    $plan=@();$seen=@{}
    foreach($c in $Changes){
        $target=Resolve-ChildPath $Config.game $c.path
        if($seen.ContainsKey($target)){throw 'Повтор пути в операции.'};$seen[$target]=$true
        $cursor=$target;while($cursor -and $cursor.StartsWith([IO.Path]::GetFullPath($Config.game),[StringComparison]::OrdinalIgnoreCase)){if((Test-Path $cursor) -and ((Get-Item $cursor -Force).Attributes -band [IO.FileAttributes]::ReparsePoint)){throw 'Связанный путь не поддерживается.'};$cursor=Split-Path $cursor -Parent}
        if($c.source){if(-not(Test-Path $c.source) -or (Get-FileHash $c.source).Hash -ne $c.sha256){throw ('Повреждён исходный файл: '+$c.path)}}
        $exists=Test-Path $target -PathType Leaf;$saved=Join-Path $backup ($plan.Count.ToString()+'.bin')
        if($exists){Copy-Item -LiteralPath $target -Destination $saved}
        $plan+=@{target=$target;saved=$saved;existed=$exists;source=$c.source;sha256=$c.sha256;path=$c.path}
    }
    Write-Json (Join-Path $backup 'plan.json') $plan
    Assert-GameWritable $Config
    $done=@()
    try{foreach($p in $plan){$done+=,$p;if(Test-Path $p.target){[IO.File]::Delete($p.target)};if($p.source){[IO.Directory]::CreateDirectory((Split-Path $p.target -Parent))|Out-Null;[IO.File]::Copy($p.source,$p.target);if((Get-FileHash $p.target).Hash -ne $p.sha256){throw 'Контрольная сумма после записи не совпала.'}}}}
    catch{$reason=$_.Exception.Message;$failed=@();[array]::Reverse($done);foreach($p in $done){try{if(Test-Path $p.target){[IO.File]::Delete($p.target)};if($p.existed){[IO.File]::Copy($p.saved,$p.target)}}catch{$failed+=$p.target}};if($failed.Count){throw ($reason+'; не восстановлено: '+($failed -join ', '))};throw ($reason+'; исходные файлы восстановлены.')}
    Write-Json (Join-Path $backup 'complete.json') @{utc=[DateTime]::UtcNow.ToString('o');count=$plan.Count}
    return $backup
}
function Set-LibrariesAndMods($Config,$Request){
    Assert-GameWritable $Config
    $catalog=Get-LibraryCatalog $Config;$changes=@();$receipt=Read-Json (Join-Path $Config.game 'CPVR-Portable/installed.json')
    $shared=$false;if(Get-Command Get-SharedEnhancementDefinition -ErrorAction SilentlyContinue){$shared=[bool](Get-SharedEnhancementDefinition $Config)}
    if(-not $shared -and $Request.ContainsKey('nr')){
        if($Request.nr -and -not $Request.opti){throw 'Для DLSS 5 нужен включённый OptiScaler.'}
        $ini=Join-Path $Config.game 'bin/x64/OptiScaler.ini'
        if($Request.opti -and (Test-Path $ini)){
            $text=[IO.File]::ReadAllText($ini);$section=[regex]::Match($text,'(?ms)^\[DlssNr\][^\r\n]*\r?\n.*?(?=^\[|\z)')
            if(-not $section.Success -or -not [regex]::IsMatch($section.Value,'(?m)^Enabled\s*=')){throw 'В этом OptiScaler не найден параметр DLSS-NR.'}
            $value=if($Request.nr){'true'}else{'false'};$body=[regex]::Replace($section.Value,'(?m)^(Enabled\s*=\s*)[^\r\n]*',('${1}'+$value))
            $text=$text.Substring(0,$section.Index)+$body+$text.Substring($section.Index+$section.Length)
            $prepared=Join-Path $Config.data ('requests/opti-'+[guid]::NewGuid().ToString('N')+'.ini');[IO.Directory]::CreateDirectory((Split-Path $prepared -Parent))|Out-Null;[IO.File]::WriteAllText($prepared,$text,[Text.UTF8Encoding]::new($false))
            $changes+=@{path='bin/x64/OptiScaler.ini';source=$prepared;sha256=(Get-FileHash $prepared).Hash}
        }
    }
    foreach($lib in $catalog){
        $h=[string]$Request.libraries[$lib.file];if(-not $h -or $h -eq $lib.current){continue}
        if(-not @($lib.choices|Where-Object hash -eq $h).Count){throw 'Библиотека отсутствует в проверенном каталоге.'}
        $changes+=@{path=$lib.path;source=(Join-Path $Config.bundle "files/$h");sha256=$h}
        $entry=@($receipt.critical|Where-Object path -eq $lib.path);if($entry.Count){$entry[0].sha256=$h}else{$receipt.critical+=@{path=$lib.path;sha256=$h}}
    }
    if(-not(Test-CyberpunkProfileMode $Config)){
    $statePath=Join-Path $Config.data 'mods-state.json';$park=Read-Json $statePath
    $managedRoots=Get-ManagedModuleRoots $receipt
    # Older versions parked the port's own regenerated databases/settings.
    # Never restore stale copies of those over current runtime state.
    if($park){$park.files=@($park.files|Where-Object {-not (Test-ManagedModulePath $_.path $managedRoots)})}
    $next=@{enabled=[bool]$Request.mods;files=@()};$state=Get-PortState $Config
    if(-not $Request.mods){
        if($park -and -not $park.enabled){$next.files=@($park.files)}
        $modRoot=Join-Path $Config.data 'mod-files';[IO.Directory]::CreateDirectory($modRoot)|Out-Null
        foreach($relative in $state.externalFiles){
            if($relative -match '\.(log|json|tmp|cache)$'){continue}
            $src=Resolve-ChildPath $Config.game $relative;$h=(Get-FileHash $src).Hash;$obj=Join-Path $modRoot $h
            if(-not(Test-Path $obj)){Copy-Item -LiteralPath $src -Destination $obj};if((Get-FileHash $obj).Hash -ne $h){throw 'Повреждён резерв мода.'}
            $next.files=@($next.files|Where-Object path -ne $relative)+@(@{path=$relative;source=$obj;sha256=$h})
            $changes+=@{path=$relative;source=$null}
        }
    }elseif($park -and -not $park.enabled){
        foreach($f in $park.files){$dst=Resolve-ChildPath $Config.game $f.path;if((Test-Path $dst) -and (Get-FileHash $dst).Hash -ne $f.sha256){throw ('Конфликт при включении модов: '+$f.path)};$changes+=@{path=$f.path;source=$f.source;sha256=$f.sha256}}
    }
    }
    if($changes.Count){
        $receiptFile=Join-Path $Config.data ('requests/receipt-'+[guid]::NewGuid().ToString('N')+'.json');Write-Json $receiptFile $receipt
        $changes+=@{path='CPVR-Portable/installed.json';source=$receiptFile;sha256=(Get-FileHash $receiptFile).Hash}
        $null=Invoke-ComponentTransaction $Config $changes
    }
    if(-not(Test-CyberpunkProfileMode $Config)){Write-Json $statePath $next}
    $keys=Read-Json (Join-Path $Config.data 'keybindings.json')
    if(-not $shared -and $keys -and $Request.opti -and $keys.ports.ContainsKey([string]$Request.version)){
        . "$PSScriptRoot/KeyBindings.ps1"
        Set-OptiMenuKey (Join-Path $Config.game 'bin/x64/OptiScaler.ini') ([int]$keys.ports[$Request.version].optiKey)
    }
}
function Ensure-LaunchModProfile($Config,$State){
    if(Test-CyberpunkProfileMode $Config){return}
    $profile=Get-ModProfileState $Config $State
    # Redeploying external files must never silently change the user's choice.
    # Reapply only the clean-mod operation; do not reinstall the port/settings.
    if(-not $profile.enabled -and $profile.active -gt 0){
        Set-LibrariesAndMods $Config @{mods=$false;libraries=@{}}
    }
}
function Import-LocalPort($Config,$Request){
    $zip=[IO.Path]::GetFullPath([string]$Request.archive)
    if([IO.Path]::GetExtension($zip) -ne '.zip'){throw 'Для импорта нужен ZIP с файлами порта, не установщик EXE.'}
    if(-not $Request.name -or -not $Request.versionLabel){throw 'Укажи название порта и версию.'}
    $b=Read-Json (Join-Path $Config.bundle 'bundle.json');$base=@($b.versions|Where-Object id -eq $Request.baseId)[0]
    if(-not $base -or -not $base.portPaths){throw 'Выбери основу с отделёнными зависимостями.'}
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $archive=[IO.Compression.ZipFile]::OpenRead($zip)
    try{
        $marker='red4ext/plugins/cyberpunkvr_stereo/cyberpunkvr_stereo.dll'
        $found=@($archive.Entries|Where-Object {$_.FullName.Replace('\','/').ToLowerInvariant().EndsWith($marker)})
        if($found.Count -ne 1){throw 'Структура не распознана: нужен ZIP форка ToFu с red4ext/plugins/CyberpunkVR_Stereo/CyberpunkVR_Stereo.dll.'}
        $prefix=$found[0].FullName.Replace('\','/');$prefix=$prefix.Substring(0,$prefix.Length-$marker.Length)
        $map=@{};foreach($k in $base.files.Keys){if($k -notin $base.portPaths){$map[$k]=$base.files[$k]}}
        $portPaths=@();$seen=@{};[long]$size=0
        $temp=Join-Path $Config.data ('imports/'+[guid]::NewGuid().ToString('N'));[IO.Directory]::CreateDirectory($temp)|Out-Null
        foreach($e in $archive.Entries){
            $name=$e.FullName.Replace('\','/');if($name.EndsWith('/')){continue}
            if(-not $name.StartsWith($prefix,[StringComparison]::OrdinalIgnoreCase)){continue};$rel=$name.Substring($prefix.Length).ToLowerInvariant()
            $null=Resolve-ChildPath $Config.game $rel
            if($rel -match '(^|/)(\.\.?)(/|$)' -or $rel -match ':' -or (($e.ExternalAttributes -shr 16) -band 0xF000) -eq 0xA000){throw 'Небезопасный путь ZIP.'}
            if($rel -notmatch '^(bin|red4ext|archive|r6|engine|mods)/'){continue}
            if($rel -match 'cyberpunk2077\.exe$'){throw 'Архив заменяет EXE игры.'}
            if($seen.ContainsKey($rel)){throw 'Повторяющийся путь ZIP.'};$seen[$rel]=$true;$size+=$e.Length;if($size -gt 2GB -or $seen.Count -gt 100000){throw 'Превышен лимит архива.'}
            $tmp=Join-Path $temp ($seen.Count.ToString()+'.bin');$input=$e.Open();$out=[IO.File]::Create($tmp);try{$input.CopyTo($out)}finally{$input.Dispose();$out.Dispose()}
            $h=(Get-FileHash $tmp).Hash;$obj=Join-Path $Config.bundle "files/$h";if(-not(Test-Path $obj)){Copy-Item $tmp $obj};if((Get-FileHash $obj).Hash -ne $h){throw 'Повреждён объект каталога.'};$map[$rel]=$h;$portPaths+=$rel
        }
        $sha=(Get-FileHash $zip).Hash;$id='custom-'+$sha.Substring(0,16).ToLowerInvariant()
        if(@($b.versions|Where-Object id -eq $id).Count){return 'Этот архив уже добавлен.'}
        $v=@{id=$id;label=([string]$Request.name+' — '+[string]$Request.versionLabel);family=[string]$Request.name;note='Импортирован локальный ZIP. Совместимость в игре ещё не проверена.';available=$true;kind='native';allowOpti=$false;allowFG=$false;handsAvailable=$false;files=$map;noHandsFiles=$map;portPaths=$portPaths;sourceArchive=@{path=$zip;sha256=$sha;baseId=$base.id}}
        Write-Json (Join-Path $temp 'bundle-before.json') $b;$b.versions=@($b.versions)+$v;$b.managedPaths=@(@($b.managedPaths)+$portPaths|Sort-Object -Unique);Write-Json (Join-Path $Config.bundle 'bundle.json') $b
        return ('Добавлен '+$v.label+'. В игру ничего не установлено.')
    }finally{$archive.Dispose()}
}
