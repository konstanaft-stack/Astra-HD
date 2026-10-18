param([string]$Version='luke263',[bool]$OptiScaler=$false,[bool]$FrameGeneration=$false,[bool]$CleanProfile=$false,[bool]$Hands=$true,[switch]$Uninstall,[switch]$Run,[switch]$CheckOnly,[string]$GamePath,[string]$SettingsFile,[string]$PreviousSettingsFile)
$ErrorActionPreference='Stop'
$ProgressPreference='SilentlyContinue'
. (Join-Path $PSScriptRoot 'NativeRequiredIni.ps1')
$mutex=[Threading.Mutex]::new($false,'Local\IRD.SingleBuildTestDeploy.v1');$locked=$false
$done=[Collections.Generic.List[object]]::new()
$isolation=$null
function Hash([string]$path){(Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash}
function Get-BackupStorageKey([string]$relative){
    $normalized=$relative.Replace('\','/').ToLowerInvariant()
    $sha=[Security.Cryptography.SHA256]::Create()
    try{$key=([BitConverter]::ToString($sha.ComputeHash([Text.Encoding]::UTF8.GetBytes($normalized)))).Replace('-','').ToLowerInvariant()}finally{$sha.Dispose()}
    return 'objects/'+$key+'.bak'
}
function Safe-Join([string]$root,[string]$relative){
    if([IO.Path]::IsPathRooted($relative)){throw 'Absolute managed path is invalid.'}
    $base=[IO.Path]::GetFullPath($root).TrimEnd('\');$path=[IO.Path]::GetFullPath((Join-Path $base $relative))
    if(-not $path.StartsWith($base+'\',[StringComparison]::OrdinalIgnoreCase)){throw 'Path escapes target folder.'}
    $cursor=$path
    while($cursor){
        if((Test-Path -LiteralPath $cursor) -and ((Get-Item -LiteralPath $cursor -Force).Attributes -band [IO.FileAttributes]::ReparsePoint)){throw ('Linked path: '+$cursor)}
        $cursor=Split-Path -Parent $cursor
    }
    return $path
}
function Object-File([string]$hash){
    if($hash -notmatch '^[0-9A-Fa-f]{64}$'){throw 'Invalid object hash.'}
    return Join-Path $PSScriptRoot ('files\'+$hash)
}
function Explicit-SettingsPath([string]$path){
    if(-not [IO.Path]::IsPathFullyQualified($path)){throw 'Explicit settings path must be absolute.'}
    $full=[IO.Path]::GetFullPath($path)
    if([IO.Path]::GetFileName($full) -ine 'UserSettings.json'){throw 'Explicit settings target must be UserSettings.json.'}
    return Safe-Join (Split-Path $full -Parent) ([IO.Path]::GetFileName($full))
}
function Is-Preference([string]$path){
    if($path -like '*candidate-manifest.json'){return $false}
    return ($path -eq '@settings' -or $path -match '^bin/x64/(RealVR\.ini|OptiScaler\.ini|RealFG-Control\.ini|CyberpunkVR[^/]*\.ini|vrport(?:-launcher)?\.ini)$' -or $path -match '^(red4ext/plugins/|bin/x64/plugins/cyber_engine_tweaks/mods/).+\.(ini|json|toml)$')
}
function Enable-LukeExpertMode([string]$Text){
    # Explicit owner policy: every Luke version keeps Expert Mode enabled,
    # including after restoring a previously saved preference file.
    $newline=if($Text.Contains("`r`n")){"`r`n"}else{"`n"}
    $section=[regex]::Match($Text,'(?ims)^\[RVR\][ \t]*\r?\n.*?(?=^\[|\z)')
    if(-not $section.Success){return $Text+$newline+'[RVR]'+$newline+'ExpertMode=1'+$newline}
    $body=$section.Value
    if($body -match '(?im)^[ \t]*ExpertMode\s*='){
        $body=[regex]::Replace($body,'(?im)^[ \t]*ExpertMode\s*=[^\r\n]*','ExpertMode=1')
    }else{
        $header=[regex]::Match($body,'^[^\r\n]*\r?\n')
        $body=$body.Insert($header.Length,'ExpertMode=1'+$newline)
    }
    return $Text.Substring(0,$section.Index)+$body+$Text.Substring($section.Index+$section.Length)
}
function Is-GenericModPath([string]$path){
    if($path -match 'CyberpunkVR|CpvrOwn|CPVR_Hands|RealVR|RealFG|NvidiaMenu|vrport_|vrcigarette|vrcam_|RTX40MFG|OptiScaler|nvngx.dll_dlssnr|nvngx_dlssnr'){return $false}
    return ($path -match '^(red4ext/|bin/x64/plugins/|archive/pc/mod/|mods/|r6/scripts/|r6/tweaks/|r6/input/|engine/tools/)' -or $path -match '^bin/x64/(version\.dll|winmm\.dll|global\.ini)$' -or $path -ieq 'engine/config/base/scripts.ini')
}
function Find-Game {
    $roots=[Collections.Generic.List[string]]::new()
    foreach($drive in @('C','D')){
        foreach($sub in @('Program Files (x86)\Steam','Program Files\Steam','Steam','SteamLibrary','Games\Steam','Games\SteamLibrary')){$roots.Add($drive+':\'+$sub)}
        $roots.Add($drive+':\')
    }
    foreach($reg in @('HKCU:\Software\Valve\Steam','HKLM:\SOFTWARE\WOW6432Node\Valve\Steam')){
        $r=Get-ItemProperty -LiteralPath $reg -ErrorAction SilentlyContinue
        if($r){foreach($field in @('SteamPath','InstallPath')){if($r.PSObject.Properties[$field]){$roots.Add([string]$r.$field)}}}
    }
    foreach($root in @($roots.ToArray())){
        $vdf=Join-Path $root 'steamapps\libraryfolders.vdf'
        if(Test-Path -LiteralPath $vdf){foreach($m in [regex]::Matches([IO.File]::ReadAllText($vdf),'"path"\s+"([^"]+)"')){$roots.Add($m.Groups[1].Value.Replace('\\','\'))}}
    }
    $found=@($roots|Where-Object {$_ -match '^[CD]:[\\/]'}|ForEach-Object {
        $candidate=Join-Path $_ 'steamapps\common\Cyberpunk 2077'
        if(Test-Path -LiteralPath (Join-Path $candidate 'bin\x64\Cyberpunk2077.exe')){[IO.Path]::GetFullPath($candidate)}
    }|Sort-Object -Unique)
    if($found.Count -eq 0){throw 'Cyberpunk не найден в библиотеках Steam на C: и D:.'}
    if($found.Count -eq 1){return $found[0]}
    Add-Type -AssemblyName System.Windows.Forms
    $form=[Windows.Forms.Form]::new();$form.Text='Выбери установленную игру';$form.Width=740;$form.Height=170;$form.StartPosition='CenterScreen'
    $list=[Windows.Forms.ComboBox]::new();$list.SetBounds(15,15,685,28);$list.DropDownStyle='DropDownList';$list.Items.AddRange([object[]]$found);$list.SelectedIndex=0
    $button=[Windows.Forms.Button]::new();$button.Text='Выбрать';$button.SetBounds(580,60,120,30);$button.DialogResult='OK';$form.AcceptButton=$button
    $form.Controls.AddRange(@($list,$button));$result=$form.ShowDialog();$value=$list.SelectedItem;$form.Dispose()
    if($result -ne 'OK'){throw 'Выбор отменён.'};return [string]$value
}
function Native-FG([string]$text,[bool]$enabled){
    $null=$text|ConvertFrom-Json -ErrorAction Stop
    $updates=@{FrameGeneration=@{value=$(if($enabled){'"DLSS"'}else{'"Off"'});index=$(if($enabled){'1'}else{'0'})};DLSSFrameGen=@{value=$(if($enabled){'true'}else{'false'})}}
    foreach($name in $updates.Keys){
        # Match complete leaf objects regardless of property order; quoted braces are not structure.
        $objects=[regex]::Matches($text,'\{(?:[^{}"]|"(?:\\.|[^"\\])*")*\}')
        $matches=@($objects|Where-Object {($_.Value|ConvertFrom-Json).name -ceq $name})
        if($matches.Count -ne 1){throw ('Missing game FG setting '+$name)}
        $m=$matches[0];$body=$m.Value
        foreach($key in $updates[$name].Keys){
            $pattern='("'+$key+'"\s*:\s*)("[^"\r\n]*"|true|false|[0-9]+)'
            if([regex]::Matches($body,$pattern).Count -ne 1){throw ('Invalid game FG field '+$name+'.'+$key)}
            $body=[regex]::Replace($body,$pattern,('${1}'+$updates[$name][$key]))
        }
        $text=$text.Substring(0,$m.Index)+$body+$text.Substring($m.Index+$m.Length)
    }
    $null=$text|ConvertFrom-Json;return $text
}
function Installation-Settings([string]$Text,[string]$Kind,[bool]$EnableFlatFG){
    $null=$Text|ConvertFrom-Json -ErrorAction Stop
    # Native VR ports publish the player's game FG choice themselves.
    # Installing or switching components must not replace that choice with Off.
    if($Kind -eq 'native'){return $Text}
    return Native-FG $Text ($Kind -eq 'flat' -and $EnableFlatFG)
}
try {
    if($PreviousSettingsFile -and -not $SettingsFile){throw 'PreviousSettingsFile requires an explicit SettingsFile target.'}
    if($SettingsFile -and ($CleanProfile -or -not $GamePath)){throw 'Explicit settings target conflicts with clean/automatic isolation.'}
    if($SettingsFile){$SettingsFile=Explicit-SettingsPath $SettingsFile}
    if($PreviousSettingsFile){$PreviousSettingsFile=Explicit-SettingsPath $PreviousSettingsFile}
    try{$locked=$mutex.WaitOne(0)}catch [Threading.AbandonedMutexException]{$locked=$true}
    if(-not $locked){throw 'Сейчас выполняется другая установка или сборка.'}
    if(-not $CheckOnly -and (Get-Process Cyberpunk2077,Vortex -ErrorAction SilentlyContinue | Where-Object {$_.HasExited -ne $true})){throw 'Полностью закрой Cyberpunk и Vortex.'}
    if($Uninstall){$Version='flat';$OptiScaler=$false;$FrameGeneration=$false;$CleanProfile=$false;$Hands=$false;$Run=$false}
    $bundle=Get-Content -LiteralPath (Join-Path $PSScriptRoot 'bundle.json') -Raw -Encoding UTF8|ConvertFrom-Json
    $v=@($bundle.versions|Where-Object id -eq $Version)
    if($v.Count -ne 1 -or -not $v[0].available){throw 'Недоступная версия.'};$v=$v[0]
    $nativeRequiredIni=Get-NativeRequiredIniPolicy $v
    if(($OptiScaler -and -not $v.allowOpti) -or ($FrameGeneration -and -not $v.allowFG)){throw 'Это дополнение не подготовлено для выбранного порта.'}
    $selected=@{};$flat=$bundle.versions|Where-Object id -eq 'flat'
    foreach($p in $flat.files.PSObject.Properties){$selected[$p.Name]=$p.Value}
    $wantHands=$Hands -and [bool]$v.handsAvailable
    $profileFiles=if($v.handsAvailable -and -not $wantHands -and $v.PSObject.Properties['noHandsFiles'] -and $v.noHandsFiles){$v.noHandsFiles}else{$v.files}
    foreach($p in $profileFiles.PSObject.Properties){
        $rel=$p.Name;$keep=$true
        if($CleanProfile -and -not $wantHands -and $v.kind -eq 'luke'){
            $keep=($rel -notmatch '^(red4ext/|archive/pc/mod/|mods/|r6/scripts/|r6/tweaks/|bin/x64/plugins/|engine/tools/|bin/x64/CPVR_HandsTracking/)' -and $rel -notmatch '^bin/x64/(version\.dll|winmm\.dll|global\.ini)$' -and $rel -ine 'engine/config/base/scripts.ini')
            if($rel -in @($v.keepRootLoadersOnClean)){$keep=$true}
        }elseif($CleanProfile -and $v.kind -eq 'native'){
            # The native port is one complete manifest, including its scripts and assets.
            # Clean-Profile removes external mods; it must not strip the selected port.
            $keep=$true
        }
        if($keep){$selected[$rel]=$p.Value}
    }
    $modNames=@();if($OptiScaler){$modNames+= $(if($v.kind -eq 'flat'){'optiFlat'}else{'optiVR'})}
    if($FrameGeneration){$modNames+= $(if($v.kind -eq 'flat'){'fgFlat'}else{'fgVR'})}
    foreach($name in $modNames){foreach($p in $bundle.modules.$name.PSObject.Properties){$selected[$p.Name]=$p.Value}}
    if(-not $GamePath){
        $GamePath=Find-Game
        if($CleanProfile){
            . (Join-Path $PSScriptRoot 'Isolated-Profile.ps1')
            $isolation=Get-IsolatedGameProfile -Bundle $PSScriptRoot -Version $Version -Hands $wantHands -Clean $true -SourceGame $GamePath -CheckOnly:$CheckOnly
            if($CheckOnly -and $isolation.NeedsCreate){Write-Output 'Проверена основа отдельной папки; она будет создана при установке.';exit 0}
            $GamePath=$isolation.Game
        }
    };$GamePath=[IO.Path]::GetFullPath($GamePath).TrimEnd('\')
    $exe=Safe-Join $GamePath 'bin\x64\Cyberpunk2077.exe'
    if((Hash $exe) -ne $bundle.exeSha256){throw 'Версия игры отличается от сохранённой 2.31. Исполняемый файл игры не заменяется.'}
    $stateDir=Safe-Join $GamePath 'CPVR-Portable';$receiptPath=Safe-Join $GamePath 'CPVR-Portable/installed.json'
    $receipt=$null;if(Test-Path -LiteralPath $receiptPath){$receipt=Get-Content -LiteralPath $receiptPath -Raw -Encoding UTF8|ConvertFrom-Json}
    if($Run -and -not $SettingsFile -and -not $CheckOnly -and -not $CleanProfile -and $receipt -and -not $receipt.clean -and $receipt.version -eq $Version -and $receipt.opti -eq $OptiScaler -and $receipt.fg -eq $FrameGeneration -and $receipt.hands -eq $wantHands){
        $mutex.ReleaseMutex();$locked=$false
        & (Join-Path $stateDir 'Start-Game.ps1');exit $LASTEXITCODE
    }
    $settingsBase=if($isolation){$isolation.SettingsRoot}else{$env:LOCALAPPDATA}
    $settingsRoot=Join-Path $settingsBase 'CD Projekt Red\Cyberpunk 2077';$settings=if($SettingsFile){$SettingsFile}else{Safe-Join $settingsRoot 'UserSettings.json'}
    $previousSettings=if($PreviousSettingsFile){$PreviousSettingsFile}else{$settings}
    foreach($rel in $bundle.managedPaths){$null=Safe-Join $GamePath $rel}
    foreach($rel in @('bin/x64/d3d12.dll','bin/x64/d3d11.dll','bin/x64/nvngx.dll')){
        if(-not $CleanProfile -and (Test-Path -LiteralPath (Join-Path $GamePath $rel)) -and $rel -notin $bundle.managedPaths){throw ('Другой графический перехватчик: '+$rel+'. Он не относится к этому комплекту.')}
    }
    $stamp=Get-Date -Format 'yyyyMMdd-HHmmss-fff';$backupRelative='CPVR-Portable-Backups/'+$stamp;$backup=Safe-Join $GamePath $backupRelative
    . (Join-Path $PSScriptRoot 'Clean-Profile.ps1')
    $cleanPlan=Get-CleanProfilePlan -Game $GamePath -StateDir $stateDir -Bundle $bundle -Selected $selected -Clean $CleanProfile -BackupRelative $backupRelative
    $preference=$Version+$(if($CleanProfile){'__clean'}else{''})+$(if($v.handsAvailable -and -not $wantHands){'__nohands'}else{''})
    # Clean/Hands switches change installed modules, not tuning of the same renderer.
    $rendererKey=if($v.kind -eq 'luke'){'bin/x64/dxgi.dll'}elseif($v.kind -eq 'native'){'red4ext/plugins/cyberpunkvr_stereo/cyberpunkvr_stereo.dll'}elseif($v.kind -eq 'own'){'red4ext/plugins/cpvrown/cpvrown.dll'}else{$null}
    $sameRenderer=$false
    if($rendererKey -and $selected.ContainsKey($rendererKey)){
        $rendererFile=Safe-Join $GamePath $rendererKey
        $sameRenderer=(Test-Path -LiteralPath $rendererFile -PathType Leaf) -and (Hash $rendererFile) -eq $selected[$rendererKey]
    }elseif($v.kind -eq 'flat'){$sameRenderer=$receipt -and $receipt.kind -eq 'flat'}
    # Resolve and validate exactly the settings that installation will use, before any writes.
    $prefSettings=Safe-Join $stateDir ('preferences/'+$preference+'/_settings')
    $previousPreference=if($receipt -and $receipt.PSObject.Properties['preference']){$receipt.preference}elseif($receipt){$receipt.version}else{$null}
    # A directly related native update changes code, not the player's tuning.
    # A saved choice for this child still wins. Keep the current INI in place
    # rather than seeding preferences before the installation transaction.
    $nativeParentUpdate=$false;$inheritNativeSettings=$false
    if($receipt -and $receipt.kind -eq 'native' -and $v.kind -eq 'native' -and
       $v.PSObject.Properties['parentId'] -and [string]$v.parentId -ceq [string]$receipt.version){
        $parents=@($bundle.versions|Where-Object {$_.id -ceq [string]$receipt.version -and $_.kind -eq 'native'})
        $nativeParentUpdate=$parents.Count -eq 1
        if($nativeParentUpdate -and $selected.ContainsKey('bin/x64/vrport.ini')){
            $childIniPreference=Safe-Join $stateDir ('preferences/'+$preference+'/bin/x64/vrport.ini')
            $currentNativeIni=Safe-Join $GamePath 'bin/x64/vrport.ini'
            $inheritNativeSettings=(Test-Path -LiteralPath $currentNativeIni -PathType Leaf) -and
                -not(Test-Path -LiteralPath $childIniPreference -PathType Leaf)
        }
    }
    if($nativeRequiredIni){
        if(-not $selected.ContainsKey('bin/x64/vrport.ini')){throw 'Required native INI missing from selected build'}
        $iniSource=Object-File $selected['bin/x64/vrport.ini']
        $iniPreference=Safe-Join $stateDir ('preferences/'+$preference+'/bin/x64/vrport.ini')
        if(Test-Path -LiteralPath $iniPreference -PathType Leaf){$iniSource=$iniPreference}
        elseif($inheritNativeSettings){$iniSource=$currentNativeIni}
        $null=Update-NativeRequiredIniText (Read-NativeRequiredIniText $iniSource) $v
    }
    $useCurrent=$sameRenderer -or ($receipt -and $previousPreference -eq $preference) -or (-not $receipt -and $Version -eq 'flat')
    $sourceSettings=if($SettingsFile){if(Test-Path -LiteralPath $settings -PathType Leaf){$settings}else{Object-File $selected['@settings']}}elseif($useCurrent -and (Test-Path -LiteralPath $settings -PathType Leaf)){$settings}elseif(Test-Path -LiteralPath $prefSettings -PathType Leaf){$prefSettings}elseif($inheritNativeSettings -and (Test-Path -LiteralPath $settings -PathType Leaf)){$settings}else{Object-File $selected['@settings']}
    if($sourceSettings -eq (Object-File $selected['@settings']) -and (Hash $sourceSettings) -ne $selected['@settings']){throw 'Повреждены сохранённые настройки игры.'}
    $desiredSettings=Installation-Settings ([IO.File]::ReadAllText($sourceSettings)) $v.kind $FrameGeneration
    if($CheckOnly){
        foreach($hash in @($selected.Values|Sort-Object -Unique)){if((Hash (Object-File $hash)) -ne $hash){throw ('Повреждён файл комплекта: '+$hash)}}
        Write-Output ('PASS: {0}; Opti={1}; FG={2}; {3} payload entries; settings validated; {4}. No changes.' -f $Version,$OptiScaler,$FrameGeneration,$selected.Count,$GamePath);exit 0
    }
    if(-not $receipt -and (Test-Path -LiteralPath $previousSettings) -and -not(Test-Path -LiteralPath (Join-Path $GamePath 'bin/x64/dxgi.dll')) -and -not(Test-Path -LiteralPath (Join-Path $GamePath 'red4ext/plugins/CyberpunkVR_Stereo/CyberpunkVR_Stereo.dll'))){
        $originalFlat=Safe-Join $stateDir 'preferences/flat/_settings'
        if(-not(Test-Path -LiteralPath $originalFlat)){[void][IO.Directory]::CreateDirectory((Split-Path $originalFlat -Parent));[IO.File]::Copy($previousSettings,$originalFlat,$false)}
    }
    # Save edited preferences per version before moving to another one.
    if($receipt){
        if($receipt.version -notmatch '^[a-zA-Z0-9_-]+$'){throw 'Invalid previous profile identity.'}
        foreach($rel in @($receipt.paths)+@('@settings')){
            if(-not (Is-Preference $rel)){continue}
            $src=if($rel -eq '@settings'){$previousSettings}else{Safe-Join $GamePath $rel}
            $previousPreference=if($receipt.PSObject.Properties['preference']){$receipt.preference}else{$receipt.version}
            if($previousPreference -notmatch '^[a-zA-Z0-9_-]+$'){throw 'Invalid preference identity.'}
            $pref=Safe-Join $stateDir ('preferences/'+$previousPreference+'/'+$rel.Replace('@','_'))
            if(Test-Path -LiteralPath $src -PathType Leaf){[void][IO.Directory]::CreateDirectory((Split-Path $pref -Parent));[IO.File]::Copy($src,$pref,$true)}
        }
    }
    $operations=[Collections.Generic.List[object]]::new()
    if($sameRenderer){
        foreach($rel in @('@settings','bin/x64/realvr.ini','bin/x64/optiscaler.ini')){
            if($SettingsFile -and $rel -eq '@settings'){continue}
            if(-not $selected.ContainsKey($rel)){continue}
            $current=if($rel -eq '@settings'){$settings}else{Safe-Join $GamePath $rel}
            if(Test-Path -LiteralPath $current -PathType Leaf){
                $preserved=Safe-Join $stateDir ('preferences/'+$preference+'/'+$rel.Replace('@','_'))
                [void][IO.Directory]::CreateDirectory((Split-Path $preserved -Parent));[IO.File]::Copy($current,$preserved,$true)
            }
        }
    }
    foreach($rel in @(@($bundle.managedPaths)+@($cleanPlan.ExtraRemovals)|Sort-Object -Unique)){
        $target=Safe-Join $GamePath $rel
        $source=$null;$preparedIni=$null
        if($selected.ContainsKey($rel)){
            $source=Object-File $selected[$rel]
            if(Is-Preference $rel){
                $pref=Safe-Join $stateDir ('preferences/'+$preference+'/'+$rel)
                if(Test-Path -LiteralPath $pref -PathType Leaf){$source=$pref}
                elseif($inheritNativeSettings -and $rel -ieq 'bin/x64/vrport.ini'){$source=$currentNativeIni}
            }
            $sourceHash=if($source -eq (Object-File $selected[$rel])){$selected[$rel]}else{Hash $source}
            if($nativeRequiredIni -and $rel -ieq 'bin/x64/vrport.ini'){
                if((Hash $source) -ne $sourceHash){throw 'Native INI source changed during preparation'}
                $preparedIni=Update-NativeRequiredIniText (Read-NativeRequiredIniText $source) $v
                if((Test-Path -LiteralPath $target -PathType Leaf) -and (Read-NativeRequiredIniText $target) -ceq $preparedIni){continue}
            }elseif($rel -ieq 'bin/x64/RealVR.ini'){
                if((Hash $source) -ne $sourceHash){throw 'RealVR.ini source changed during preparation.'}
                $preparedIni=[IO.File]::ReadAllText($source)
                $cache=if($isolation){Join-Path $isolation.ProfileRoot 'temp'}else{[IO.Path]::GetTempPath().TrimEnd('\')}
                $preparedIni=[regex]::Replace($preparedIni,'(?m)^IntermediateCachePath=[^\r\n]*',([Text.RegularExpressions.MatchEvaluator]{param($m)'IntermediateCachePath='+$cache}))
                if($v.kind -eq 'luke'){$preparedIni=Enable-LukeExpertMode $preparedIni}
                if((Test-Path -LiteralPath $target -PathType Leaf) -and [IO.File]::ReadAllText($target) -ceq $preparedIni){continue}
            }elseif((Test-Path -LiteralPath $target -PathType Leaf) -and (Hash $target) -eq $sourceHash){continue}
        }elseif(-not(Test-Path -LiteralPath $target -PathType Leaf)){continue}
        elseif(-not $CleanProfile -and (Is-GenericModPath $rel)){continue}
        $operation=[pscustomobject]@{target=$target;source=$source;key=$rel}
        if($null -ne $preparedIni){$operation|Add-Member -NotePropertyName text -NotePropertyValue $preparedIni}
        $operations.Add($operation)
    }
    foreach($restore in $cleanPlan.Restores){
        if($selected.ContainsKey($restore.path)){continue}
        $operations.Add([pscustomobject]@{target=(Safe-Join $GamePath $restore.path);source=$restore.source;key=$restore.path})
    }
    $operations.Add([pscustomobject]@{target=$settings;source=$null;key='_settings';text=$desiredSettings})
    $operations.Add([pscustomobject]@{target=(Safe-Join $GamePath 'CPVR-Portable/Start-Game.ps1');source=(Join-Path $PSScriptRoot 'Start-Game.ps1');key='CPVR-Portable/Start-Game.ps1'})
    if($isolation){foreach($helper in @('Isolated-Profile.ps1','Profile-Settings.ps1')){$operations.Add([pscustomobject]@{target=(Safe-Join $stateDir $helper);source=(Join-Path $PSScriptRoot $helper);key=('CPVR-Portable/'+$helper)})}}
    $editorNativeDualArchive='archive/pc/mod/cyberpunkvrport_editor_dual.archive'
    $editorNativeDualRequired=$false
    if($v.kind -eq 'native' -and $v.editorNativeDual){
        if($v.editorNativeDual.schema -ne 1 -or $v.editorNativeDual.required -isnot [bool] -or -not $v.editorNativeDual.required -or
           $v.editorNativeDual.path -cne $editorNativeDualArchive -or $v.editorNativeDual.sha256 -cnotmatch '^[A-F0-9]{64}$' -or
           -not $selected.ContainsKey($editorNativeDualArchive) -or $selected[$editorNativeDualArchive] -cne $v.editorNativeDual.sha256){throw 'Invalid selected NativeDual archive declaration'}
        $editorNativeDualRequired=$true
    }
    $critical=@($selected.Keys|Where-Object {$_ -ieq 'archive/pc/mod/cyberpunkvrport.archive' -or ($editorNativeDualRequired -and $_ -ceq $editorNativeDualArchive) -or $_ -match '^bin/x64/(dxgi\.dll|RealFG64\.dll|dbghelp\.dll|version\.dll|winmm\.dll|OptiScaler\.asi|nvngx_dlss\.dll|nvngx_dlssg\.dll|nvngx_dlssnr\.dll)$' -or $_ -match '^(red4ext/|bin/x64/(plugins|CPVR_HandsTracking|CheekyFoveatedDLSS)/|CPVR-Portable/NvidiaMenu/).+\.(dll|asi|reds|lua|dxil)$'}|Sort-Object|ForEach-Object {@{path=$_;sha256=$selected[$_]}})
    $newReceipt=@{version=$Version;label=$v.label;kind=$v.kind;hands=$wantHands;clean=$CleanProfile;preference=$preference;opti=$OptiScaler;fg=$FrameGeneration;exeSha256=$bundle.exeSha256;paths=@($selected.Keys|Where-Object {$_ -ne '@settings'});critical=$critical;time=$stamp}
    if($nativeParentUpdate){$newReceipt.settingsInheritedFrom=[string]$receipt.version}
    if($SettingsFile){$newReceipt.settingsFile=$settings}
    if($isolation){$newReceipt.isolation=@{profileRoot=$isolation.ProfileRoot;settingsFile=$settings;reference=$isolation.ReferencePath}}
    $operations.Add([pscustomobject]@{target=$receiptPath;source=$null;key='CPVR-Portable/installed.json';text=($newReceipt|ConvertTo-Json -Depth 8)})
    $operations.Add([pscustomobject]@{target=(Safe-Join $stateDir 'clean-state.json');source=$null;key='CPVR-Portable/clean-state.json';text=($cleanPlan.State|ConvertTo-Json -Depth 8)})
    foreach($src in @($operations|Where-Object {$_.source -and $_.source.StartsWith((Join-Path $PSScriptRoot 'files')+'\',[StringComparison]::OrdinalIgnoreCase)}|ForEach-Object source|Sort-Object -Unique)){
        if((Hash $src) -ne (Split-Path -Leaf $src)){throw ('Повреждён файл комплекта: '+(Split-Path -Leaf $src))}
    }
    if(Get-Process Cyberpunk2077,Vortex -ErrorAction SilentlyContinue){throw 'Cyberpunk или Vortex запущен во время подготовки. Файлы игры ещё не изменены.'}
    [void][IO.Directory]::CreateDirectory($backup)
    $backupIndex=@($operations|ForEach-Object {@{key=$_.key;saved=(Get-BackupStorageKey $_.key);target=$_.target;existed=(Test-Path -LiteralPath $_.target -PathType Leaf)}})
    [IO.File]::WriteAllText((Join-Path $backup 'index.json'),(@{schemaVersion=2;files=$backupIndex}|ConvertTo-Json -Depth 6),[Text.UTF8Encoding]::new($false))
    foreach($op in $operations){
        $saved=Safe-Join $backup (Get-BackupStorageKey $op.key);$existed=Test-Path -LiteralPath $op.target -PathType Leaf
        if($existed){[void][IO.Directory]::CreateDirectory((Split-Path $saved -Parent));[IO.File]::Copy($op.target,$saved,$false)}
        $done.Add([pscustomobject]@{target=$op.target;saved=$saved;existed=$existed})
        # Break a possible Vortex hardlink before overwriting its deployed path.
        if($existed){[IO.File]::Delete($op.target)}
        if($op.PSObject.Properties['text']){[void][IO.Directory]::CreateDirectory((Split-Path $op.target -Parent));[IO.File]::WriteAllText($op.target,$op.text,[Text.UTF8Encoding]::new($false))}
        elseif($op.source){[void][IO.Directory]::CreateDirectory((Split-Path $op.target -Parent));[IO.File]::Copy($op.source,$op.target,$false);if((Hash $op.target) -ne (Hash $op.source)){throw 'Copy verification failed.'}}
        elseif($existed){[IO.File]::Delete($op.target)}
    }
    $realIni=Join-Path $GamePath 'bin\x64\RealVR.ini'
    if(Test-Path -LiteralPath $realIni){
        $cache=if($isolation){Join-Path $isolation.ProfileRoot 'temp'}else{[IO.Path]::GetTempPath().TrimEnd('\')}
        [void][IO.Directory]::CreateDirectory($cache)
    }
    if($v.kind -eq 'own'){
        Write-Output ('Готово: '+$v.label+'. NVIDIA SR/MFG и OptiScaler/NR входят в модуль; управление F10/F9/F11 в игре.')
    }else{Write-Output ('Готово: '+$v.label+'; OptiScaler='+$OptiScaler+'; FG='+$FrameGeneration)}
    Write-Output ('Путь: '+$GamePath);Write-Output ('Резерв заменённых файлов: '+$backup)
    # Release the install lock before starting the user's requested game.
    $done.Clear();$mutex.ReleaseMutex();$locked=$false
    if($Run){& (Join-Path $stateDir 'Start-Game.ps1');if($LASTEXITCODE -ne 0){exit $LASTEXITCODE}}
    elseif($CleanProfile -and $isolation){Write-Output 'Игра не запускалась. Отдельный чистый профиль запускается кнопкой «Запустить игру» в главном окне.'}
    elseif($CleanProfile){Write-Output 'Игра не запускалась. Чистый профиль применён к этой папке; можно запускать обычным ярлыком или через Steam.'}
    else{Write-Output 'Игра не запускалась. Теперь можно запускать обычным ярлыком, через Steam или Vortex.'}
    exit 0
}catch{
    $reason=$_.Exception.Message;$errors=@()
    for($i=$done.Count-1;$i -ge 0;$i--){$op=$done[$i];try{if($op.existed){[IO.File]::Copy($op.saved,$op.target,$true)}elseif(Test-Path -LiteralPath $op.target){[IO.File]::Delete($op.target)}}catch{$errors+=$_.Exception.Message}}
    Write-Output ('Ошибка: '+$reason)
    if($errors.Count){Write-Output ('Нужна проверка отката: '+($errors -join '; '))}elseif($done.Count){Write-Output 'Изменённые файлы восстановлены.'}
    exit 1
}finally{if($locked){$mutex.ReleaseMutex()};$mutex.Dispose()}
