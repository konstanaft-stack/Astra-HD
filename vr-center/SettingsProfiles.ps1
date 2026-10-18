. "$PSScriptRoot/Components.ps1"
. "$PSScriptRoot/NativeRequiredIni.ps1"
function Get-SettingsModePaths($Config,$State){
    $version=[string]$State.receipt.version
    if(-not $version){throw 'Сначала установи VR-порт.'}
    $sha=[Security.Cryptography.SHA256]::Create()
    try{$key=[Convert]::ToHexString($sha.ComputeHash([Text.Encoding]::UTF8.GetBytes($version)))}finally{$sha.Dispose()}
    $root=Join-Path $Config.data "game-settings/$key"
    $player=if($Config.settingsFile){$Config.settingsFile}else{Join-Path ([Environment]::GetFolderPath('LocalApplicationData')) 'CD Projekt Red/Cyberpunk 2077/UserSettings.json'}
    return @{root=$root;state=(Join-Path $root 'mode.json');custom=(Join-Path $root 'custom.json');player=$player;version=$version;ini=(Join-Path $Config.game 'bin/x64/vrport.ini')}
}
function Get-RecommendedSettingsMode($Config,$State){
    if(-not $State.receipt){return $false}
    $paths=Get-SettingsModePaths $Config $State
    return [bool](Read-Json $paths.state).recommended
}
function Initialize-FirstPortSettings($Config,$State){
    if(-not $State.receipt -or $State.receipt.kind -eq 'flat'){return $false}
    $paths=Get-SettingsModePaths $Config $State
    # Apply the selected author's preset once. A saved choice, including Custom,
    # belongs to the player and survives later applies and launches.
    if(Test-Path -LiteralPath $paths.state){
        # A restored profile can bring back first_launch=1 independently of the
        # saved manager choice. Honor that choice without reapplying a preset.
        Set-RecommendedSettingsMode $Config $State (Get-RecommendedSettingsMode $Config $State) -BeforeLaunch
        return $false
    }
    if(-not(Test-Path -LiteralPath $paths.player)){return $false}
    # The installer records a validated direct native update independently of
    # which INI wins. Validate again before inheriting preset consent; a saved
    # child INI must not silently enable Recommended when its mode is absent.
    $parent=[string]$State.receipt.settingsInheritedFrom
    if($State.receipt.kind -eq 'native' -and $parent -match '^[a-zA-Z0-9_-]+$'){
        $bundle=Read-Json (Join-Path $Config.bundle 'bundle.json')
        $child=@($bundle.versions|Where-Object id -CEQ $paths.version)
        $source=@($bundle.versions|Where-Object id -CEQ $parent)
        if($child.Count -eq 1 -and $source.Count -eq 1 -and $child[0].kind -eq 'native' -and
           $source[0].kind -eq 'native' -and [string]$child[0].parentId -ceq $parent){
            $oldPaths=Get-SettingsModePaths $Config @{receipt=@{version=$parent;kind='native'}}
            if(Test-Path -LiteralPath $oldPaths.state -PathType Leaf){
                $oldMode=Read-Json $oldPaths.state
                if([string]$oldMode.version -cne $parent -or $oldMode.recommended -isnot [bool]){
                    throw 'Не удалось проверить сохранённый выбор настроек предыдущей сборки.'
                }
                $changes=@()
                if(Test-Path -LiteralPath $oldPaths.custom -PathType Leaf){
                    $changes+=@{path=$paths.custom;text=[IO.File]::ReadAllText($oldPaths.custom)}
                }elseif($oldMode.recommended){throw 'Не найдена копия своих настроек предыдущей сборки.'}
                $changes+=@{path=$paths.state;text=(@{version=$paths.version;recommended=$oldMode.recommended;inheritedFrom=$parent}|ConvertTo-Json)}
                Write-SettingsModeFiles $Config $State $changes
                Set-RecommendedSettingsMode $Config $State $oldMode.recommended -BeforeLaunch
                # False means no author preset was applied (Backend's message).
                return $false
            }
        }
    }
    Set-RecommendedSettingsMode $Config $State $true
    return $true
}
function Merge-RecommendedSettings([string]$Player,[string]$Preset){
    $current=$Player|ConvertFrom-Json -AsHashtable -ErrorAction Stop
    $source=$Preset|ConvertFrom-Json -AsHashtable -ErrorAction Stop
    if(-not $current.data -or -not $source.data){throw 'Неправильный формат настроек игры.'}
    foreach($group in $source.data){
        $name=[string]$group.group_name
        if($name -notmatch '^/(graphics|gameplay/performance)(/|$)' -and $name -ne '/video/display'){continue}
        $target=@($current.data|Where-Object group_name -ceq $name)
        if($target.Count -gt 1){throw 'Повтор группы настроек.'}
        if($target.Count -eq 0){continue}
        foreach($option in $group.options){
            if($name -eq '/video/display' -and $option.name -notin @('Resolution','ReflexMode','MaximumFPS_Value')){continue}
            $matches=@($target[0].options|Where-Object name -ceq $option.name)
            if($matches.Count -gt 1){throw 'Повтор настройки.'}
            if($matches.Count -eq 0){continue}
            $matches[0].value=$option.value
            if($option.ContainsKey('index')){$matches[0].index=$option.index}
        }
    }
    return $current|ConvertTo-Json -Depth 50
}
function Get-RecommendedSettingsText($Config,$State){
    $bundle=Read-Json (Join-Path $Config.bundle 'bundle.json')
    $version=@($bundle.versions|Where-Object id -eq $State.receipt.version)
    if($version.Count -ne 1){throw 'Не найден пресет установленного порта.'}
    $v=$version[0];$path='red4ext/plugins/cyberpunkvr_stereo/usersettings.json'
    $hash=if($v.kind -eq 'native' -and $v.files.ContainsKey($path)){$v.files[$path]}else{$v.files['@settings']}
    if($hash -notmatch '^[a-fA-F0-9]{64}$'){throw 'У порта нет проверенного пресета.'}
    $file=Join-Path $Config.bundle "files/$hash"
    if((Get-FileHash -LiteralPath $file).Hash -ne $hash){throw 'Пресет повреждён.'}
    return [IO.File]::ReadAllText($file)
}
function Write-SettingsModeFiles($Config,$State,$Changes){
    Assert-GameWritable $Config
    $paths=Get-SettingsModePaths $Config $State
    $backup=Join-Path $paths.root ('transactions/'+[guid]::NewGuid().ToString('N'))
    [IO.Directory]::CreateDirectory($backup)|Out-Null
    $records=@();$done=@()
    foreach($item in $Changes){
        if($item.path -notin @($paths.player,$paths.ini,$paths.custom,$paths.state)){throw 'Unexpected settings target'}
        $exists=Test-Path -LiteralPath $item.path
        $old=if($exists){[IO.File]::ReadAllBytes($item.path)}else{$null}
        if($exists -and [IO.File]::ReadAllText($item.path) -ceq $item.text){continue}
        $record=@{path=$item.path;existed=$exists;backup=(Join-Path $backup ($records.Count.ToString()+'.bin'));text=$item.text}
        if($exists){[IO.File]::WriteAllBytes($record.backup,$old)}
        $records+=,$record
    }
    Write-Json (Join-Path $backup 'plan.json') @($records|Select-Object path,existed,backup)
    Assert-GameWritable $Config
    try{
        foreach($item in $records){
            [IO.Directory]::CreateDirectory((Split-Path $item.path -Parent))|Out-Null
            $temp=$item.path+'.'+[guid]::NewGuid().ToString('N')+'.tmp'
            [IO.File]::WriteAllText($temp,$item.text,[Text.UTF8Encoding]::new($false))
            [IO.File]::Move($temp,$item.path,$true);$done+=,$item
        }
    }catch{
        $reason=$_.Exception.Message;[array]::Reverse($done)
        foreach($item in $done){if($item.existed){[IO.File]::WriteAllBytes($item.path,[IO.File]::ReadAllBytes($item.backup))}else{[IO.File]::Delete($item.path)}}
        throw $reason
    }
    Write-Json (Join-Path $backup 'complete.json') @{count=$records.Count;utc=[DateTime]::UtcNow.ToString('o')}
}
function Get-RequiredNativeSettingsVersion($Config,$State){
    if($State.receipt.kind -cne 'native'){return $null}
    $bundle=Read-Json (Join-Path $Config.bundle 'bundle.json')
    $versions=@($bundle.versions|Where-Object id -CEQ ([string]$State.receipt.version))
    if($versions.Count -ne 1 -or $versions[0].kind -cne 'native'){throw 'Invalid installed native settings version'}
    $null=Get-NativeRequiredIniPolicy $versions[0]
    return $versions[0]
}
function Sync-NativeRequiredIni($Config,$State){
    $v=Get-RequiredNativeSettingsVersion $Config $State
    if(-not $v -or -not(Get-NativeRequiredIniPolicy $v)){return}
    $paths=Get-SettingsModePaths $Config $State
    if(-not(Test-Path -LiteralPath $paths.ini -PathType Leaf)){throw 'Required native INI missing'}
    $ini=Read-NativeRequiredIniText $paths.ini
    $updated=Update-NativeRequiredIniText $ini $v
    if($updated -cne $ini){Write-SettingsModeFiles $Config $State @(@{path=$paths.ini;text=$updated})}
}
function Set-RecommendedSettingsMode($Config,$State,[bool]$Enabled,[switch]$BeforeLaunch){
    $paths=Get-SettingsModePaths $Config $State
    $nativeVersion=Get-RequiredNativeSettingsVersion $Config $State
    if($nativeVersion -and (Get-NativeRequiredIniPolicy $nativeVersion) -and -not(Test-Path -LiteralPath $paths.ini -PathType Leaf)){throw 'Required native INI missing'}
    $previous=Get-RecommendedSettingsMode $Config $State
    $changes=@()
    # A launch honors preset consent but preserves subsequent in-game changes.
    # Only an explicit preset action applies values or captures a custom backup.
    if($Enabled -and -not $BeforeLaunch){
        $player=[IO.File]::ReadAllText($paths.player)
        if(-not $previous -or -not(Test-Path -LiteralPath $paths.custom)){$changes+=@{path=$paths.custom;text=$player}}
        $merged=Merge-RecommendedSettings $player (Get-RecommendedSettingsText $Config $State)
        $changes+=@{path=$paths.player;text=$merged}
    }elseif($previous -and -not $BeforeLaunch){
        if(-not(Test-Path -LiteralPath $paths.custom)){throw 'Не найдена сохранённая копия твоих настроек; пресет не отключён.'}
        $changes+=@{path=$paths.player;text=[IO.File]::ReadAllText($paths.custom)}
    }
    # The manager owns preset consent. A first-launch flag may not override Custom.
    if($State.receipt.kind -eq 'native' -and (Test-Path -LiteralPath $paths.ini)){
        $ini=if($nativeVersion -and (Get-NativeRequiredIniPolicy $nativeVersion)){Read-NativeRequiredIniText $paths.ini}else{[IO.File]::ReadAllText($paths.ini)}
        $updated=if($ini -match '(?m)^first_launch\s*='){
            [regex]::Replace($ini,'(?m)^first_launch\s*=[^\r\n]*','first_launch=0')
        }else{$ini+"`r`nfirst_launch=0`r`n"}
        if($nativeVersion){$updated=Update-NativeRequiredIniText $updated $nativeVersion}
        if($updated -cne $ini){$changes+=@{path=$paths.ini;text=$updated}}
    }
    if(-not $BeforeLaunch -or -not(Test-Path -LiteralPath $paths.state)){
        $changes+=@{path=$paths.state;text=(@{version=$paths.version;recommended=$Enabled}|ConvertTo-Json)}
    }
    if($changes.Count){Write-SettingsModeFiles $Config $State $changes}
}
