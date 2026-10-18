$ErrorActionPreference='Stop'
function Test-CyberpunkProfileMode($Config){return ($null -ne $Config -and $Config.gameProfilesEnabled -is [bool] -and $Config.gameProfilesEnabled)}
function Resolve-ChildPath([string]$Root,[string]$Relative){
    $base=[IO.Path]::GetFullPath($Root).TrimEnd('\'); $path=[IO.Path]::GetFullPath((Join-Path $base $Relative))
    if([IO.Path]::IsPathRooted($Relative) -or -not $path.StartsWith($base+'\',[StringComparison]::OrdinalIgnoreCase)){throw 'Путь выходит за границы каталога.'}
    return $path
}
function Read-Json([string]$Path){if(Test-Path -LiteralPath $Path){return Get-Content -LiteralPath $Path -Raw -Encoding UTF8|ConvertFrom-Json -AsHashtable};return $null}
function Write-Json([string]$Path,$Value){
    [IO.Directory]::CreateDirectory((Split-Path $Path -Parent))|Out-Null
    $temp=$Path+'.'+[guid]::NewGuid().ToString('N')+'.tmp'
    [IO.File]::WriteAllText($temp,($Value|ConvertTo-Json -Depth 35),[Text.UTF8Encoding]::new($false));[IO.File]::Move($temp,$Path,$true)
}
function Assert-ApplyAllowed($State,$Version,[bool]$Opti,[bool]$FG){
    if($State.hold){throw 'Смена сборки доступна после завершения первого запуска.'}
    if($State.gameRunning){throw 'Сначала закрой игру.'}
    if($State.vortexRunning){throw 'Сначала закрой Vortex.'}
    if(-not $Version -or -not $Version.available){throw 'Файлы этой сборки недоступны.'}
    if(($Opti -and -not $Version.allowOpti) -or ($FG -and -not $Version.allowFG)){throw 'Этот компонент не подготовлен для выбранной сборки.'}
    # A closed manager's deployment is not a launch or port-switch dependency.
    # The installer backs up replaced files and breaks hardlinks before writing.
}
function Get-ActiveDeploymentCount($Paths){
    $count=0
    foreach($path in $Paths){
        try{$manifest=Read-Json $path;if(-not $manifest.ContainsKey('files')){$count++;continue};$count+=@($manifest.files).Count}catch{$count++}
    }
    return $count
}
function Get-ManagedModuleRoots($Receipt){
    $roots=@{}
    foreach($p in @($Receipt.paths)){
        $rel=([string]$p).Replace('\','/').ToLowerInvariant()
        if($rel -match '^(bin/x64/plugins/cyber_engine_tweaks/mods/[^/]+/)'){$roots[$Matches[1]]=$true}
        elseif($rel -match '^(red4ext/plugins/[^/]+/)[^/]+\.dll$'){$roots[$Matches[1]]=$true}
    }
    return @($roots.Keys)
}
function Test-ManagedModulePath([string]$Relative,$Roots){
    $rel=$Relative.Replace('\','/').ToLowerInvariant()
    foreach($prefix in $Roots){if($rel.StartsWith($prefix,[StringComparison]::OrdinalIgnoreCase)){return $true}}
    return $false
}
function Get-ModProfileState($Config,$State){
    $saved=Read-Json (Join-Path $Config.data 'mods-state.json')
    $roots=Get-ManagedModuleRoots $State.receipt
    return @{
        enabled=if($saved){[bool]$saved.enabled}else{$false}
        active=@($State.externalFiles|Where-Object {$_ -notmatch '\.(log|json|tmp|cache)$'}).Count
        parked=if($saved){@($saved.files|Where-Object {-not (Test-ManagedModulePath $_.path $roots)}).Count}else{0}
    }
}
function Convert-PortTime($Value){
    if(-not $Value){return $null}
    try{
        if($Value -is [datetime]){return $Value.ToUniversalTime().ToString('o')}
        if($Value -is [datetimeoffset]){return $Value.UtcDateTime.ToString('o')}
        if($Value -match '^\d{8}-\d{6}-\d{3}$'){
            return [datetime]::ParseExact($Value,'yyyyMMdd-HHmmss-fff',[Globalization.CultureInfo]::InvariantCulture,[Globalization.DateTimeStyles]::AssumeLocal).ToUniversalTime().ToString('o')
        }
        return [datetimeoffset]::Parse($Value,[Globalization.CultureInfo]::InvariantCulture).UtcDateTime.ToString('o')
    }catch{return $null}
}
function Get-PortVersionDates($Config,$Bundle,$Receipt){
    $result=@{}
    foreach($v in $Bundle.versions){
        $saved=Convert-PortTime $v.savedUtc
        if(-not $saved -and $v.id -match '^saved-(\d{8})-(\d{6})-[0-9a-f]+$'){
            # Snapshot ids are created from local Get-Date; this is save time, never build time.
            $saved=Convert-PortTime ($Matches[1]+'-'+$Matches[2]+'-000')
        }
        $added=Convert-PortTime $v.addedUtc
        if(-not $added){$added=Convert-PortTime $v.importedUtc}
        $result[$v.id]=@{builtUtc=(Convert-PortTime $v.builtUtc);downloadedUtc=(Convert-PortTime $v.downloadedUtc);installedUtc=$null;addedUtc=$added;savedUtc=$saved}
    }
    $byHash=@{}
    foreach($f in @(Get-ChildItem -LiteralPath (Join-Path $Config.bundle 'downloads') -Filter '*.download.json' -Recurse -File -ErrorAction SilentlyContinue)){
        try{$d=Read-Json $f.FullName;$stamp=Convert-PortTime $d.downloadedUtc
            if($stamp -and $d.sha256 -and (-not $byHash[$d.sha256] -or $stamp -gt $byHash[$d.sha256])){$byHash[$d.sha256]=$stamp}
        }catch{}
    }
    # A shared archive hash proves the original download, not the creation date
    # of every locally modified build that happens to reuse that archive.
    foreach($v in $Bundle.versions){if($v.upstreamOriginal -and $v.archiveSha256 -and $byHash[$v.archiveSha256]){$result[$v.id].downloadedUtc=$byHash[$v.archiveSha256]}}
    $receipts=@($Receipt)
    foreach($f in @(Get-ChildItem -LiteralPath (Join-Path $Config.data 'snapshots') -Filter '*.json' -File -ErrorAction SilentlyContinue)){
        try{$snapshot=Read-Json $f.FullName;$receipts+=,$snapshot.receipt
            if($snapshot.version.id -and $result[$snapshot.version.id]){$result[$snapshot.version.id].savedUtc=Convert-PortTime $snapshot.created}
        }catch{}
    }
    $history=Read-Json (Join-Path $Config.data 'version-install-times.json')
    if($history){foreach($id in $history.Keys){if($result[$id]){$result[$id].installedUtc=Convert-PortTime $history[$id]}}}
    foreach($r in $receipts){if($r -and $r.version -and $result[$r.version]){$stamp=Convert-PortTime $r.time;if($stamp -and $stamp -gt [string]$result[$r.version].installedUtc){$result[$r.version].installedUtc=$stamp}}}
    return $result
}
function Get-SavedPortSourceLabel($Version,$Snapshot){
    if($Version.sourceLabel){return [string]$Version.sourceLabel}
    if($Snapshot -and $Snapshot.receipt.label){return [string]$Snapshot.receipt.label}
    if(([string]$Version.note) -match '^Точная копия файлов перед сменой:\s*(.*?)\.\s*Дополнения'){
        return [string]$Matches[1]
    }
    return ''
}
function Get-SavedPortFamily($Version,$Parent,$SourceLabel){
    if($Version.family){return [string]$Version.family}
    if($Parent -and $Parent.family){return [string]$Parent.family}
    if($SourceLabel -match '(?i)Astra HD|наш AER|Наш VR V1'){return 'Astra HD'}
    if($SourceLabel -match '(?i)^Luke'){return 'Luke Ross'}
    if($SourceLabel -match '^Плоская игра'){return 'Плоская игра'}
    if($SourceLabel -match '(?i)^Ajson'){return 'Ajson44'}
    if($SourceLabel -match '(?i)^iPowerTech'){return 'iPowerTech'}
    if($SourceLabel -match '^Основной порт'){return 'Основной порт'}
    if($SourceLabel -match '(?i)^ToFu|^Tofu Express'){return 'ToFu'}
    if($Version.kind -eq 'own'){return 'Astra HD'}
    if($Version.kind -eq 'luke'){return 'Luke Ross'}
    if($Version.kind -eq 'flat'){return 'Плоская игра'}
    return $null
}
function Save-PortInstallTime($Config,$Receipt){
    if(-not $Receipt){return}
    $stamp=Convert-PortTime $Receipt.time;if(-not $stamp){return}
    $path=Join-Path $Config.data 'version-install-times.json';$history=Read-Json $path;if(-not $history){$history=@{}}
    if($history[$Receipt.version] -ne $stamp){$history[$Receipt.version]=$stamp;Write-Json $path $history}
}
function Find-DownloadedVersion($Bundle,$Download){
    $matches=@($Bundle.versions|Where-Object {$_.archiveSha256 -and $_.archiveSha256 -eq $Download.sha256 -and $_.upstreamOriginal -and -not $_.archived}|Sort-Object id -Descending)
    if($matches.Count){return [string]$matches[0].id}
    return $null
}
function Get-PortState($Config){
    $game=$Config.game; $bundle=Read-Json (Join-Path $Config.bundle 'bundle.json')
    if(-not $bundle){throw 'Не найден каталог сборок bundle.json.'}
    $receipt=Read-Json (Join-Path $game 'CPVR-Portable/installed.json')
    $hold=Read-Json $Config.hold
    $deploy=@();foreach($dir in @('','archive/pc/mod','mods','r6','red4ext','bin/x64')){
        $path=Join-Path $game $dir
        if(Test-Path $path){$deploy+=@(Get-ChildItem -LiteralPath $path -File -Filter '*vortex*deployment*.json' -ErrorAction Stop | ForEach-Object FullName)}
    }
    $components=@();foreach($item in @(
        @('DLSS Super Resolution','bin/x64/nvngx_dlss.dll'),@('DLSS Ray Reconstruction','bin/x64/nvngx_dlssd.dll'),
        @('NVIDIA Frame Generation','bin/x64/nvngx_dlssg.dll'),@('DLSS Neural Rendering','bin/x64/nvngx_dlssnr.dll'),
        @('OptiScaler — ASI','bin/x64/OptiScaler.asi'),
        @('RTX 40 MFG','bin/x64/plugins/RTX40MFG.asi'),@('VR-порт','red4ext/plugins/CyberpunkVR_Stereo/CyberpunkVR_Stereo.dll'))){
        $p=Resolve-ChildPath $game $item[1];$exists=Test-Path -LiteralPath $p -PathType Leaf
        $ver=if($exists){([string][Diagnostics.FileVersionInfo]::GetVersionInfo($p).FileVersion).Replace(',','.')}else{''}
        $components+=@{name=$item[0];path=$item[1];present=$exists;version=$ver;state=$(if($exists){'Файл установлен'}else{'Не установлен'})}
    }
    # The flat package can use dbghelp.dll as its OptiScaler loader; stock dbghelp must never be labelled OptiScaler.
    $loader=Join-Path $game 'bin/x64/dbghelp.dll'
    if(Test-Path $loader){$info=[Diagnostics.FileVersionInfo]::GetVersionInfo($loader);if(($info.ProductName+' '+$info.FileDescription) -match 'OptiScaler'){$components+=@{name='OptiScaler — DLL';path='bin/x64/dbghelp.dll';present=$true;version=$info.FileVersion;state='Файл установлен'}}}
    $versions=@($bundle.versions|Where-Object {-not $_.archived}|ForEach-Object { @{id=$_.id;label=$_.label;sourceLabel=$_.sourceLabel;note=$_.note;family=$_.family;tag=$_.tag;repository=$_.repository;sourceRepoId=$_.sourceRepoId;parentId=$_.parentId;available=[bool]$_.available;allowOpti=[bool]$_.allowOpti;allowFG=[bool]$_.allowFG;handsAvailable=[bool]$_.handsAvailable;kind=$_.kind;installed=($receipt -and $_.id -eq $receipt.version);libraryHashes=@{'nvngx_dlss.dll'=$_.files['bin/x64/nvngx_dlss.dll'];'nvngx_dlssg.dll'=$_.files['bin/x64/nvngx_dlssg.dll'];'nvngx_dlssd.dll'=$_.files['bin/x64/nvngx_dlssd.dll']}} })
    # New-port selectors must propose the effective shared payload, not older bundle defaults.
    # Installed-port selectors still use Get-LibraryCatalog.current in the UI.
    if(Get-Command Get-SharedEnhancementDefinition -ErrorAction SilentlyContinue){
        $shared=Get-SharedEnhancementDefinition $Config
        foreach($file in $shared.files){
            if($file.path -in @('bin/x64/nvngx_dlss.dll','bin/x64/nvngx_dlssd.dll','bin/x64/nvngx_dlssg.dll')){
                $name=[IO.Path]::GetFileName([string]$file.path)
                foreach($version in $versions){$version.libraryHashes[$name]=[string]$file.sha256}
            }
        }
    }
    $byId=@{};foreach($entry in $bundle.versions){$byId[$entry.id]=$entry}
    foreach($version in $versions){
        if($version.id -notlike 'saved-*'){continue}
        $snapshot=Read-Json (Join-Path $Config.data ('snapshots/'+$version.id+'.json'))
        if(-not $version.parentId -and $snapshot -and $snapshot.receipt.version){$version.parentId=[string]$snapshot.receipt.version}
        $version.sourceLabel=Get-SavedPortSourceLabel $version $snapshot
        $parent=if($version.parentId){$byId[$version.parentId]}else{$null}
        if(-not $version.family){$version.family=Get-SavedPortFamily $version $parent $version.sourceLabel}
    }
    $dates=Get-PortVersionDates $Config $bundle $receipt
    foreach($version in $versions){foreach($key in @('builtUtc','downloadedUtc','installedUtc','addedUtc','savedUtc')){$version[$key]=$dates[$version.id][$key]}}
    $vortex=Test-Path -LiteralPath $Config.vortex
    $profileDirs=@(Get-ChildItem -LiteralPath (Join-Path $Config.vortexData 'cyberpunk2077/profiles') -Directory -ErrorAction SilentlyContinue)
    $modDirs=@(Get-ChildItem -LiteralPath (Join-Path $Config.vortexData 'cyberpunk2077/mods') -Directory -ErrorAction SilentlyContinue)
    $owned=@{};if($receipt){foreach($p in $receipt.paths){$owned[$p.Replace('\','/').ToLowerInvariant()]=$true}}
    $managedRoots=Get-ManagedModuleRoots $receipt
    $approved=@{files=@{};runtimeRoots=@();nativeRuntimeRoots=@()}
    $contentManifest=Join-Path $game 'CPVR-Portable/approved-content.json'
    $contentReader=Join-Path $Config.bundle 'ContentPackages.ps1'
    $hasContentReader=Test-Path -LiteralPath $contentReader -PathType Leaf
    if($hasContentReader){. $contentReader}
    if(Test-Path -LiteralPath $contentManifest -PathType Leaf){
        if(-not $hasContentReader){throw 'Нет проверки выбранных пакетов контента.'}
        $approved=Get-ApprovedContentState -Game $game -ManifestPath $contentManifest -ReservedPaths (@($bundle.managedPaths)+@($receipt.paths)) -StockPath (Join-Path $Config.bundle 'stock-paths.json')
    }
    $external=@();foreach($sub in @('archive/pc/mod','r6/scripts','r6/tweaks','r6/input','red4ext/plugins','bin/x64/plugins/cyber_engine_tweaks/mods')){
        $dir=Join-Path $game $sub
        if(Test-Path $dir){foreach($f in Get-ChildItem $dir -File -Recurse){
            $rel=[IO.Path]::GetRelativePath($game,$f.FullName).Replace('\','/')
            $approvedPath=$hasContentReader -and (Test-ApprovedContentPath $approved $rel)
            if($approvedPath -and ($f.Attributes -band [IO.FileAttributes]::ReparsePoint)){throw ('Ссылка в выбранном пакете: '+$rel)}
            if(-not $owned.ContainsKey($rel.ToLowerInvariant()) -and -not (Test-ManagedModulePath $rel $managedRoots) -and -not $approvedPath){$external+=$rel}
        }}
    }
    $result=@{game=$game;receipt=$receipt;versions=$versions;components=$components;hold=[bool]$hold.active;
        gameRunning=[bool](Get-Process Cyberpunk2077 -ErrorAction SilentlyContinue);vortexRunning=[bool](Get-Process Vortex -ErrorAction SilentlyContinue);
        vortexInstalled=$vortex;deploymentCount=(Get-ActiveDeploymentCount $deploy);deploymentManifests=$deploy.Count;deployments=$deploy;vortexProfiles=$profileDirs.Count;vortexStaged=$modDirs.Count;
        externalFiles=$external;exePresent=(Test-Path (Join-Path $game 'bin/x64/Cyberpunk2077.exe'));timestamp=[DateTime]::UtcNow.ToString('o')}
    $result.gameProfiles=if(Get-Command Get-CyberpunkGameProfilesState -ErrorAction SilentlyContinue){Get-CyberpunkGameProfilesState $Config}else{@{enabled=(Test-CyberpunkProfileMode $Config);available=$false;initialized=$false;activeId=$null;profiles=@();recoveryRequired=@();reason='Адаптер профилей игры не загружен.'}}
    return $result
}
function Save-CurrentBuild($Config,$State){
    # Preserve the actual installed binaries before switching, including local development changes.
    $r=$State.receipt;if(-not $r){return $null}
    $manifestPath=Join-Path $Config.bundle 'bundle.json';$bundle=Read-Json $manifestPath
    $source=$bundle.versions|Where-Object id -eq $r.version|Select-Object -First 1
    if(-not $source){throw 'Нельзя сохранить текущий комплект: нет исходной записи в каталоге.'}
    $map=@{};foreach($rel in $r.paths){
        if($rel -eq '@settings'){continue};$p=Resolve-ChildPath $Config.game $rel
        if(-not(Test-Path -LiteralPath $p -PathType Leaf)){throw "Не найден файл текущей сборки: $rel"}
        $hash=(Get-FileHash -LiteralPath $p -Algorithm SHA256).Hash;$obj=Join-Path $Config.bundle "files/$hash"
        if(-not(Test-Path $obj)){Copy-Item -LiteralPath $p -Destination $obj}
        if((Get-FileHash -LiteralPath $obj -Algorithm SHA256).Hash -ne $hash){throw 'Повреждён архив текущей сборки.'}
        $map[$rel]=$hash
    }
    $savedAt=Get-Date
    $id='saved-'+$savedAt.ToString('yyyyMMdd-HHmmss')+'-'+[guid]::NewGuid().ToString('N').Substring(0,6)
    $v=@{id=$id;label=('Сохранённая сборка • '+$savedAt.ToString('dd.MM HH:mm'));sourceLabel=$r.label;family=$source.family;parentId=$source.id;savedUtc=$savedAt.ToUniversalTime().ToString('o');available=$true;kind=$source.kind;allowOpti=$false;allowFG=$false;handsAvailable=[bool]$r.hands;files=$map;noHandsFiles=$map;note=('Точная копия файлов перед сменой: '+$r.label+'. Дополнения уже входят в состав. Настройки сохраняются также штатным установщиком.')}
    $backup=Join-Path $Config.data "catalog-backups/$id.json";Write-Json $backup $bundle
    $bundle.versions=@($bundle.versions)+$v;Write-Json $manifestPath $bundle
    Write-Json (Join-Path $Config.data "snapshots/$id.json") @{receipt=$r;version=$v;created=[DateTime]::UtcNow.ToString('o')}
    return $id
}
