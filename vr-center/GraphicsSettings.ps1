$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot 'SettingsProfiles.ps1')

function Get-GraphicsLabels {
    return [ordered]@{FrameGeneration='Генерация кадров';DLSSFrameGen='Генерация кадров DLSS';FSR3_FrameGeneration='Генерация кадров FSR 3';XESS_FrameGeneration='Генерация кадров XeSS';DLSS_MultiFrameGeneration='Множитель кадров DLSS';TextureQuality='Текстуры';RyzenSMTUtilization='AMD SMT';HybridUtilization='Ядра процессора Intel';NoSpatialAudio='Пространственный звук';UseXAudio='Звуковой движок XAudio';ResolutionScaling='Масштабирование';DLSS='Режим DLSS';DLSS_BackendPreset='Модель DLSS';DLSS_NewSharpness='Резкость DLSS';RayTracing='Трассировка лучей';RayTracedPathTracing='Path Tracing';PathTracing='Path Tracing';CrowdDensity='Плотность толпы';MaximumFPS_OnOff='Ограничение FPS';MaximumFPS_Value='Предел FPS'}
}
function Get-GraphicsEntries($Document){
    $labels=Get-GraphicsLabels
    foreach($group in @($Document.data)){
        if($group.group_name -notmatch '^/(graphics|video)(/|$)|^/gameplay/performance$|^/utilities$|^/audio/misc$'){continue}
        foreach($option in @($group.options)){if($labels.Contains([string]$option.name)){Write-Output $option}}
    }
}
function Get-GameOptionDefinitions($Config){
    $definitions=@{};$labels=Get-GraphicsLabels
    foreach($relative in @('r6/config/settings/options.json','r6/config/settings/platform/pc/options.json')){
        $path=Join-Path $Config.game $relative
        if(-not(Test-Path -LiteralPath $path -PathType Leaf)){continue}
        $document=Read-Json $path
        foreach($group in @($document.options)){
            foreach($option in @($group.options)){
                if($labels.Contains([string]$option.name)){$definitions[[string]$option.name]=$option}
            }
        }
    }
    return $definitions
}
function Get-GraphicsChoices($Option){
    if($Option.ContainsKey('values') -and $Option['values']){return @($Option['values'])}
    # Game option definitions and shipped presets agree on these dynamic indices.
    # Use the fallback only when the current persisted value/index confirms the order.
    $values=switch([string]$Option.name){
        'DLSS' {@('Auto','DLAA','Quality','Balanced','Performance','Ultra Performance')}
        'ResolutionScaling' {@('Off','DLSS','FSR','XeSS')}
        # Verified saved pairs: bundle 0759CA7F... DLSS/1; 235D2961... Off/0.
        # Other dynamic indices are deliberately not guessed.
        'FrameGeneration' {@('Off','DLSS')}
        default {@()}
    }
    if($values.Count -and $Option.ContainsKey('index') -and [int]$Option.index -ge 0 -and [int]$Option.index -lt $values.Count -and $values[[int]$Option.index] -ceq $Option.value){return $values}
    return @()
}
function Get-GraphicsSettings($Config){
    $configured=if($Config.settingsFile){$Config.settingsFile}elseif($Config.appDataPath){Join-Path $Config.appDataPath 'UserSettings.json'}else{Join-Path ([Environment]::GetFolderPath('LocalApplicationData')) 'CD Projekt Red/Cyberpunk 2077/UserSettings.json'}
    $path=[IO.Path]::GetFullPath([string]$configured)
    if(-not(Test-Path -LiteralPath $path -PathType Leaf)){return @{available=$false;reason='Первый обычный запуск игры создаст настройки. После выхода нажми «Обновить».';options=@()}}
    $doc=Read-Json $path
    if(-not $doc.data){throw 'Неподдерживаемый формат UserSettings.json.'}
    $labels=Get-GraphicsLabels;$byName=@{};$definitions=Get-GameOptionDefinitions $Config
    $entries=@(Get-GraphicsEntries $doc)
    $hasSelector=@($entries|Where-Object name -eq 'FrameGeneration').Count -gt 0
    foreach($option in $entries){
        $name=[string]$option.name;if($byName.ContainsKey($name)){continue}
        if($hasSelector -and $name -in @('DLSSFrameGen','FSR3_FrameGeneration','XESS_FrameGeneration')){continue}
        $choices=@(Get-GraphicsChoices $option)
        $definition=$definitions[$name]
        if(-not $choices.Count -and $definition -and -not $definition.is_dynamic -and $definition.ContainsKey('values')){$choices=@($definition['values'])}
        $editable=$option.type -eq 'bool' -or $choices.Count -gt 0 -or ($option.type -in @('int','float') -and $option.ContainsKey('min_value') -and $option.ContainsKey('max_value'))
        $policy=if($definition){[string]$definition.update_policy}else{[string]$option.update_policy}
        $menuOnly=$definition -and $definition.ContainsKey('in_game') -and -not $definition.in_game
        $byName[$name]=@{name=$name;label=$labels[$name];type=$option.type;value=$option.value;choices=$choices;editable=[bool]$editable;minimum=$option.min_value;maximum=$option.max_value;step=$option.step_value;requiresRestart=($policy -eq 'require_restart');menuOnly=[bool]$menuOnly;invertDisplay=($name -eq 'NoSpatialAudio')}
    }
    return @{available=$true;path=$path;hash=(Get-FileHash -LiteralPath $path).Hash;options=@($labels.Keys|Where-Object {$byName.ContainsKey($_)}|ForEach-Object {$byName[$_]})}
}
function Get-GraphicsPreset([string]$Name){
    $p=@{ResolutionScaling='DLSS';RayTracing=$false;RayTracedPathTracing=$false;PathTracing=$false}
    switch($Name){
        'performance' {$p.DLSS='Performance';$p.TextureQuality='Medium';$p.CrowdDensity='Low'}
        'balanced' {$p.DLSS='Balanced';$p.TextureQuality='High';$p.CrowdDensity='Medium'}
        'quality' {$p.DLSS='Quality';$p.TextureQuality='High';$p.CrowdDensity='High'}
        default {throw 'Неизвестный пресет графики.'}
    }
    return $p
}
function Set-GraphicsSettings($Config,[string]$ExpectedHash,$Values){
    Assert-GameWritable $Config
    $view=Get-GraphicsSettings $Config
    if(-not $view.available){throw $view.reason}
    if($ExpectedHash -notmatch '^[a-fA-F0-9]{64}$' -or $ExpectedHash -ine $view.hash){throw 'Настройки изменились после открытия окна. Нажми «Обновить», чтобы сохранить новые изменения.'}
    if($Values -isnot [Collections.IDictionary]){throw 'Не переданы изменения графики.'}
    $allowed=@{};foreach($row in $view.options){$allowed[$row.name]=$row}
    foreach($name in $Values.Keys){if(-not $allowed.ContainsKey($name) -or -not $allowed[$name].editable){throw ('Недоступный параметр графики: '+$name)}}
    $doc=Read-Json $view.path
    foreach($option in @(Get-GraphicsEntries $doc)){
        $name=[string]$option.name;if(-not $Values.ContainsKey($name)){continue};$value=$Values[$name]
        if($option.type -eq 'bool'){if($value -isnot [bool]){throw ('Ожидается переключатель: '+$name)}}
        elseif($option.type -in @('string_list','name_list','int_list')){
            $choices=@($allowed[$name].choices);$index=[Array]::IndexOf($choices,$value)
            if($index -lt 0){throw ('Недопустимое значение: '+$name)};$option.index=$index
        }elseif($option.type -in @('int','float')){
            if($value -isnot [ValueType] -or $value -is [bool] -or [double]$value -lt [double]$option.min_value -or [double]$value -gt [double]$option.max_value -or [double]::IsNaN([double]$value) -or [double]::IsInfinity([double]$value)){throw ('Значение вне диапазона: '+$name)}
            if($option.type -eq 'int' -and [double]$value -ne [Math]::Truncate([double]$value)){throw ('Ожидается целое число: '+$name)}
        }else{throw ('Неподдерживаемый параметр: '+$name)}
        $option.value=$value
    }
    if($Values.ContainsKey('FrameGeneration')){
        $mode=[string]$Values['FrameGeneration']
        $legacyModes=@{DLSSFrameGen='DLSS';FSR3_FrameGeneration='FSR3';XESS_FrameGeneration='XeSS'}
        foreach($option in @(Get-GraphicsEntries $doc)){
            if($legacyModes.ContainsKey([string]$option.name) -and $option.type -eq 'bool'){$option.value=($mode -ceq $legacyModes[[string]$option.name])}
        }
    }
    $receipt=Read-Json (Join-Path $Config.game 'CPVR-Portable/installed.json')
    $state=@{receipt=$(if($receipt){$receipt}else{@{version='flat';kind='flat'}})}
    $paths=Get-SettingsModePaths $Config $state;$text=$doc|ConvertTo-Json -Depth 50
    $changes=@(@{path=$paths.player;text=$text},@{path=$paths.custom;text=$text},@{path=$paths.state;text=(@{version=$paths.version;recommended=$false}|ConvertTo-Json)})
    if($state.receipt.kind -eq 'native' -and (Test-Path -LiteralPath $paths.ini)){
        $ini=[IO.File]::ReadAllText($paths.ini)
        $ini=if($ini -match '(?m)^first_launch\s*='){[regex]::Replace($ini,'(?m)^first_launch\s*=[^\r\n]*','first_launch=0')}else{$ini+"`r`nfirst_launch=0`r`n"}
        $changes+=@{path=$paths.ini;text=$ini}
    }
    if((Get-FileHash -LiteralPath $view.path).Hash -ine $ExpectedHash){throw 'Настройки изменились во время подготовки записи.'}
    Write-SettingsModeFiles $Config $state $changes
    return Get-GraphicsSettings $Config
}
