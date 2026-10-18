. "$PSScriptRoot/Components.ps1"
. "$PSScriptRoot/SharedEnhancementPolicy.ps1"

function Get-EnhancementPaths($Config){
    if(Get-SharedEnhancementDefinition $Config){$root=Join-Path $Config.data 'enhancement-common';return @{root=$root;state=(Join-Path $root 'state.json');mode=(Join-Path $root 'port-preference.json');preset=(Join-Path $root 'saved.ini');ini=(Join-Path $Config.game 'bin/x64/OptiScaler.ini')}}
    $root=Join-Path $Config.data 'game-settings/enhancement'
    return @{root=$root;state=(Join-Path $root 'state.json');mode=(Join-Path $root 'port-preference.json');preset=(Join-Path $root 'saved.ini');ini=(Join-Path $Config.game 'bin/x64/OptiScaler.ini')}
}
function Get-EnhancementSavedSettingPath($Paths,[string]$Relative){
    if($Relative -ieq 'bin/x64/OptiScaler.ini'){return $Paths.preset}
    return Resolve-ChildPath $Paths.root ('saved-settings/'+$Relative)
}
function Test-EnhancementCompatiblePortReceipt($Receipt){
    if(-not $Receipt -or $Receipt.kind -eq 'flat'){return $true}
    # Only the separately packaged Astra diagnostic candidate has been opted
    # into the shared NR/MFG overlay. Other VR ports keep their own binaries.
    return $Receipt.kind -eq 'native' -and ($Receipt.version -in @('astra-hd-editor-projection-20261016','astra-hd-editor-uiheap-20261016') -or $Receipt.version -match '^astra-hd-editor-spatial-20261016(?:-r[0-9]+)?$')
}
function Get-EnhancementDefinition($Config){
    if(-not $Config.imagePackage){return $null}
    $spec=Read-Json ([string]$Config.imagePackage)
    if($spec.schema -ne 1 -or -not $spec.id -or $spec.files -isnot [array]){throw 'Повреждено описание NR + MFG.'}
    $seen=@{}
    foreach($file in $spec.files){
        if(-not ([string]$file.path).StartsWith('bin/x64/',[StringComparison]::OrdinalIgnoreCase)){throw 'Файл NR + MFG указан вне bin/x64.'}
        $null=Resolve-ChildPath $Config.game $file.path
        if($seen.ContainsKey($file.path) -or $file.sha256 -notmatch '^[a-fA-F0-9]{64}$'){throw 'Повтор или неверная контрольная сумма файла NR + MFG.'}
        $seen[$file.path]=$true
    }
    foreach($path in $spec.requiredFiles){if(-not $seen.ContainsKey($path)){throw ('В комплекте нет обязательного файла: '+$path)}}
    return $spec
}
function Get-EnhancementPackageState($Config){
    $shared=Get-SharedEnhancementDefinition $Config;if($shared){return Get-SharedEnhancementState $Config $shared}
    $paths=Get-EnhancementPaths $Config;$record=Read-Json $paths.state
    $port=Read-Json (Join-Path $Config.game 'CPVR-Portable/installed.json')
    $compatible=Test-EnhancementCompatiblePortReceipt $port
    $preference=Read-Json $paths.mode
    $result=@{available=$false;enabled=([bool]$record.enabled -or $record.phase -eq 'prepared');compatible=$compatible;healthy=$true;label='NR + MFG';sourceLabel='';packageId=[string]$record.id;suspended=[bool]$preference.suspended;status='Управление NR + MFG в центре выключено';reason=''}
    try{
        $definition=Get-EnhancementDefinition $Config;$result.available=[bool]$definition
        if($definition){
            $result.sourceLabel=if($definition.label){[string]$definition.label}else{[string]$definition.id}
            if($result.enabled -and $record.id -ne $definition.id){$result.sourceLabel=[string]$record.id}
            if(-not $result.enabled){$result.packageId=[string]$definition.id}
        }
    }catch{$result.reason=$_.Exception.Message}
    if(-not $compatible){$result.reason='Для этого VR-порта пока нет проверенного пресета.'}
    elseif(-not $result.available -and -not $result.reason){$result.reason='NR + MFG ещё не добавлен в библиотеку.'}
    if($result.enabled){$result.available=$true}
    if($record.phase -eq 'prepared'){$result.healthy=$false;$result.reason='Настройка NR + MFG была прервана. Выключи «NR + MFG», чтобы вернуть прежние файлы.'}
    if($result.enabled){$result.status='NR + MFG подключён через центр'}
    elseif($preference.suspended){$result.status='Плоский NR + MFG временно отключён для VR; вернётся при выборе плоской игры.'}
    # The receipt tracks our toggle, not the user's current DLLs or in-game feature state.
    # External managers may replace the complete setup without asking this launcher.
    if(-not $result.healthy){$result.status=$result.reason}
    return $result
}
function Set-EnhancementIniValue([string]$Text,[string]$Section,[string]$Key,[string]$Value){
    $match=[regex]::Match($Text,'(?ms)^\['+[regex]::Escape($Section)+'\][^\r\n]*\r?\n.*?(?=^\[|\z)')
    if(-not $match.Success){throw ('В OptiScaler.ini нет секции '+$Section)}
    $pattern='(?im)^('+[regex]::Escape($Key)+'\s*=\s*)[^\r\n]*'
    if(-not [regex]::IsMatch($match.Value,$pattern)){throw ('В OptiScaler.ini нет параметра '+$Section+'/'+$Key)}
    $body=[regex]::Replace($match.Value,$pattern,('${1}'+$Value))
    return $Text.Substring(0,$match.Index)+$body+$Text.Substring($match.Index+$match.Length)
}
function Clear-EnhancementTransactionPayload($Config,[string]$Backup){
    $base=[IO.Path]::GetFullPath((Join-Path $Config.data 'transactions'))
    $root=[IO.Path]::GetFullPath($Backup)
    if([IO.Path]::GetDirectoryName($root) -ine $base -or [IO.Path]::GetFileName($root) -notmatch '^[a-f0-9]{32}$'){throw 'Неверный каталог временного резерва.'}
    if(-not(Test-Path -LiteralPath (Join-Path $root 'complete.json'))){return}
    foreach($entry in @(Read-Json (Join-Path $root 'plan.json'))){
        $saved=[IO.Path]::GetFullPath([string]$entry.saved)
        if([IO.Path]::GetDirectoryName($saved) -ine $root -or [IO.Path]::GetFileName($saved) -notmatch '^\d+\.bin$'){throw 'Неверный файл временного резерва.'}
        if(Test-Path -LiteralPath $saved){[IO.File]::Delete($saved)}
    }
}
function Get-EnhancementCriticalMap($Receipt){
    $map=@{}
    if($Receipt.critical -is [Collections.IDictionary]){foreach($key in $Receipt.critical.Keys){$map[[string]$key]=[string]$Receipt.critical[$key]}}
    else{foreach($entry in @($Receipt.critical)){
        if(-not $entry.path -or $map.ContainsKey([string]$entry.path)){throw 'Неверный critical в квитанции порта.'}
        $map[[string]$entry.path]=[string]$entry.sha256
    }}
    return $map
}
function New-EnhancementPortReceiptPlan($Config,$Record,$Changes,[switch]$Legacy){
    $paths=Get-EnhancementPaths $Config;$live=Join-Path $Config.game 'CPVR-Portable/installed.json'
    if(-not(Test-Path -LiteralPath $live)){return $null}
    $receipt=Read-Json $live
    if(-not(Test-EnhancementCompatiblePortReceipt $receipt)){throw 'Для выбранного VR-порта нет подтверждённого стыка NR + MFG; файлы не изменены.'}
    $capturedHash=(Get-FileHash -LiteralPath $live).Hash
    $baseline=Read-Json $live;$baseCritical=Get-EnhancementCriticalMap $baseline
    $critical=Get-EnhancementCriticalMap $receipt
    $owned=@{};foreach($file in $Record.files){$owned[[string]$file.path]=$file}
    if($Legacy){
        # Older enabled records lack a receipt backup. Derive only the owned
        # critical entries from verified before-files; never replace port identity.
        foreach($prior in $Record.before){
            if(-not $critical.ContainsKey([string]$prior.path)){continue}
            $expected=$owned[[string]$prior.path]
            if($critical[$prior.path] -ine $prior.sha256 -and $critical[$prior.path] -ine $expected.sha256){throw ('Чужой critical файл в квитанции: '+$prior.path)}
            if($prior.existed){$baseCritical[$prior.path]=[string]$prior.sha256}else{$baseCritical.Remove([string]$prior.path)}
        }
        $baseline.critical=@(foreach($key in $baseCritical.Keys|Sort-Object){@{path=$key;sha256=$baseCritical[$key]}})
    }
    $saved=Join-Path $paths.root 'before-port-receipt.json'
    if($Legacy){Write-Json $saved $baseline}else{[IO.File]::Copy($live,$saved,$true)}
    foreach($change in $Changes){
        if(-not $critical.ContainsKey([string]$change.path)){continue}
        if($owned[[string]$change.path].mutable){$critical.Remove([string]$change.path)}
        else{$critical[[string]$change.path]=[string]$change.sha256}
    }
    $receipt.critical=@(foreach($key in $critical.Keys|Sort-Object){@{path=$key;sha256=$critical[$key]}})
    $prepared=Join-Path $paths.root 'enabled-port-receipt.json';Write-Json $prepared $receipt
    $Record.portReceipt=@{saved='before-port-receipt.json';sha256=(Get-FileHash $saved).Hash;appliedSha256=(Get-FileHash $prepared).Hash;capturedSha256=$capturedHash;version=[string]$receipt.version}
    return @{path='CPVR-Portable/installed.json';source=$prepared;sha256=$Record.portReceipt.appliedSha256}
}
function Get-EnhancementPortReceiptRestoreChange($Config,$Record){
    $paths=Get-EnhancementPaths $Config;$live=Join-Path $Config.game 'CPVR-Portable/installed.json'
    if(-not $Record.portReceipt -and (Test-Path -LiteralPath $live)){
        $changes=@($Record.files|ForEach-Object {@{path=$_.path;sha256=$_.sha256}})
        $null=New-EnhancementPortReceiptPlan $Config $Record $changes -Legacy
        # The legacy receipt has not been patched yet; Off only returns its
        # validated baseline together with the original binaries.
        $Record.portReceipt.appliedSha256=$Record.portReceipt.capturedSha256
    }
    if(-not $Record.portReceipt){return $null}
    $binding=$Record.portReceipt;$hash=Get-EnhancementFileFingerprint $live
    $allowed=@($binding.appliedSha256)
    if($Record.phase -eq 'prepared'){$allowed+=@($binding.capturedSha256,$binding.sha256)}
    if($hash -notin $allowed){throw 'Квитанция порта изменилась после включения NR + MFG; автоматический откат остановлен.'}
    $saved=Resolve-ChildPath $paths.root $binding.saved
    if(-not(Test-Path -LiteralPath $saved) -or (Get-FileHash $saved).Hash -ine $binding.sha256){throw 'Повреждён резерв квитанции NR + MFG.'}
    return @{path='CPVR-Portable/installed.json';source=$saved;sha256=$binding.sha256}
}
function Set-EnhancementPackage($Config,[bool]$Enabled){
    if(Get-SharedEnhancementDefinition $Config){Set-SharedEnhancementEnabled $Config $Enabled;return}
    Assert-GameWritable $Config
    $paths=Get-EnhancementPaths $Config;$old=Read-Json $paths.state
    $status=Get-EnhancementPackageState $Config
    if(-not $Enabled){
        if($old -and ($old.enabled -or $old.phase -eq 'prepared')){Restore-EnhancementPackageFiles $Config $old}
        Write-Json $paths.mode @{schema=1;flatEnabled=$false;suspended=$false;packageId=[string]$old.id}
        return
    }
    if(-not $status.compatible){throw $status.reason}
    if($old.phase -eq 'prepared'){Restore-EnhancementPackageFiles $Config $old;$old=Read-Json $paths.state}
    elseif($old.enabled){
        if(-not $status.healthy){throw $status.reason}
        if($old.portReceipt -and (Get-EnhancementFileFingerprint (Join-Path $Config.game 'CPVR-Portable/installed.json')) -ine $old.portReceipt.appliedSha256){throw 'Квитанция порта изменилась; повторное включение NR + MFG не заменяет её автоматически.'}
        if(-not $old.portReceipt){
            $receiptChange=New-EnhancementPortReceiptPlan $Config $old @($old.files|ForEach-Object {@{path=$_.path;sha256=$_.sha256}}) -Legacy
            if($receiptChange){$backup=Invoke-ComponentTransaction $Config @($receiptChange);Write-Json $paths.state $old;Clear-EnhancementTransactionPayload $Config $backup}
        }
        return
    }
    $spec=Get-EnhancementDefinition $Config
    if(-not $spec){throw 'NR + MFG ещё не добавлен в библиотеку.'}
    if($Enabled){
        # Verify ALL sources first, including the complete Streamline subfolder.
        foreach($file in $spec.files){if(-not(Test-Path -LiteralPath $file.source -PathType Leaf) -or (Get-FileHash -LiteralPath $file.source).Hash -ine $file.sha256){throw ('NR + MFG: не найден или повреждён исходный файл: '+$file.path)}}
        [IO.Directory]::CreateDirectory($paths.root)|Out-Null
        $before=@();$changes=@()
        foreach($file in $spec.files){
            $target=Resolve-ChildPath $Config.game $file.path
            $savedName='before-'+$before.Count+'.bin';$saved=Join-Path $paths.root $savedName
            $existed=Test-Path -LiteralPath $target -PathType Leaf
            if($existed){[IO.File]::Copy($target,$saved,$true)}
            $before+=@{path=$file.path;existed=$existed;saved=$savedName;sha256=$(if($existed){(Get-FileHash -LiteralPath $saved).Hash}else{''})}
            $savedSetting=Get-EnhancementSavedSettingPath $paths $file.path
            $source=if($file.mutable -and (Test-Path -LiteralPath $savedSetting)){$savedSetting}else{$file.source}
            $changes+=@{path=$file.path;source=$source;sha256=(Get-FileHash -LiteralPath $source).Hash}
        }
        $record=@{schema=1;id=$spec.id;enabled=$false;phase='prepared';before=$before;files=$spec.files}
    }
    $receiptChange=New-EnhancementPortReceiptPlan $Config $record $changes
    if($receiptChange){$changes+=@($receiptChange)}
    Write-Json $paths.state $record
    try{$backup=Invoke-ComponentTransaction $Config $changes}catch{
        $reason=$_.Exception.Message
        try{Restore-EnhancementPackageFiles $Config $record}catch{throw ($reason+'; резерв сохранён. После выхода из игры выключи «NR + MFG» для восстановления: '+$_.Exception.Message)}
        throw $reason
    }
    $record.enabled=$Enabled;$record.phase='ready';$record.updatedUtc=[datetime]::UtcNow.ToString('o')
    Write-Json $paths.state $record
    Clear-EnhancementTransactionPayload $Config $backup
    Write-Json $paths.mode @{schema=1;flatEnabled=$true;suspended=$false;packageId=[string]$spec.id}
}
function Get-EnhancementPortKind($Config,[string]$VersionId){
    $bundle=Read-Json (Join-Path $Config.bundle 'bundle.json')
    $version=@($bundle.versions|Where-Object id -eq $VersionId)
    if($version.Count -ne 1){throw 'Неизвестная версия порта для переключения NR + MFG.'}
    return [string]$version[0].kind
}
function Get-EnhancementFileFingerprint([string]$Path){
    if(Test-Path -LiteralPath $Path -PathType Leaf){return (Get-FileHash -LiteralPath $Path).Hash}
    return ''
}
function Suspend-EnhancementForPort($Config,[string]$VersionId){
    if(Get-SharedEnhancementDefinition $Config){return @{schema=1;stopped=$false;shared=$true}}
    Assert-GameWritable $Config
    $kind=Get-EnhancementPortKind $Config $VersionId
    $paths=Get-EnhancementPaths $Config;$record=Read-Json $paths.state
    if($record.phase -eq 'prepared'){throw 'Сначала восстанови прерванную операцию NR + MFG.'}
    $receiptPath=Join-Path $Config.game 'CPVR-Portable/installed.json';$receipt=Read-Json $receiptPath
    $transaction=@{schema=1;stopped=$false;wasEnabled=[bool]$record.enabled;previousVersionId=[string]$receipt.version;receiptHash=(Get-EnhancementFileFingerprint $receiptPath);packageId=[string]$record.id;expectedFiles=@()}
    if(-not $record.enabled){return $transaction}
    # Off saves the user's mutable files before returning the collection's originals.
    Set-EnhancementPackage $Config $false
    $transaction.stopped=$true
    $transaction.receiptHash=Get-EnhancementFileFingerprint $receiptPath
    $transaction.expectedFiles=@(foreach($file in $record.files){@{path=$file.path;hash=(Get-EnhancementFileFingerprint (Resolve-ChildPath $Config.game $file.path))}})
    Write-Json $paths.mode @{schema=1;flatEnabled=$true;suspended=$true;packageId=[string]$record.id}
    return $transaction
}
function Complete-EnhancementPortChange($Config,[string]$VersionId){
    if(Get-SharedEnhancementDefinition $Config){return}
    $kind=Get-EnhancementPortKind $Config $VersionId
    if(-not(Test-EnhancementCompatiblePortReceipt @{kind=$kind;version=$VersionId})){return}
    $paths=Get-EnhancementPaths $Config;$preference=Read-Json $paths.mode
    if(-not $preference.suspended -or -not $preference.flatEnabled){return}
    $spec=Get-EnhancementDefinition $Config
    if(-not $spec -or $spec.id -ne $preference.packageId){throw 'Описание NR + MFG изменилось во время VR. Включи выбранный комплект отдельно.'}
    Set-EnhancementPackage $Config $true
}
function Restore-EnhancementAfterFailedPortChange($Config,$Transaction){
    if(Get-SharedEnhancementDefinition $Config){return}
    if(-not $Transaction -or -not $Transaction.stopped){return}
    Assert-GameWritable $Config
    if($Transaction.schema -ne 1 -or $Transaction.expectedFiles -isnot [array]){throw 'Неизвестное состояние возврата NR + MFG.'}
    $receiptPath=Join-Path $Config.game 'CPVR-Portable/installed.json'
    if((Get-EnhancementFileFingerprint $receiptPath) -ne $Transaction.receiptHash){throw 'Порт не восстановлен после ошибки. NR + MFG пока остаётся отключённым.'}
    foreach($file in $Transaction.expectedFiles){
        if((Get-EnhancementFileFingerprint (Resolve-ChildPath $Config.game $file.path)) -ne $file.hash){throw ('Не восстановлен файл после ошибки смены порта: '+$file.path)}
    }
    $spec=Get-EnhancementDefinition $Config
    if(-not $spec -or $spec.id -ne $Transaction.packageId){throw 'Описание NR + MFG изменилось; автоматический возврат остановлен.'}
    Set-EnhancementPackage $Config $true
}
function Restore-EnhancementPackageFiles($Config,$Record){
    Assert-GameWritable $Config
    $paths=Get-EnhancementPaths $Config
    if($Record.schema -ne 1 -or $Record.before -isnot [array] -or $Record.files -isnot [array]){throw 'Нет проверенного резерва NR + MFG.'}
    # Check ownership BEFORE saving settings or changing any game file. A newer
    # port's receipt must never be replaced by this flat package's old backup.
    $receiptChange=Get-EnhancementPortReceiptRestoreChange $Config $Record
    foreach($file in @($Record.files|Where-Object mutable)){
        $target=Resolve-ChildPath $Config.game $file.path
        if(Test-Path -LiteralPath $target -PathType Leaf){
            $savedSetting=Get-EnhancementSavedSettingPath $paths $file.path
            [void][IO.Directory]::CreateDirectory((Split-Path $savedSetting -Parent))
            [IO.File]::Copy($target,$savedSetting,$true)
        }
    }
    # Preserve an externally changed binary before restoring the original. This
    # also lets Off recover after a partial operation or a missing package recipe.
    foreach($file in $Record.files){
        $target=Resolve-ChildPath $Config.game $file.path
        if(-not $file.mutable -and (Test-Path -LiteralPath $target -PathType Leaf)){
            $hash=(Get-FileHash -LiteralPath $target).Hash
            $prior=@($Record.before|Where-Object path -eq $file.path)
            if($hash -ine $file.sha256 -and -not @($prior|Where-Object sha256 -eq $hash).Count){
                $saved=Join-Path $paths.root ('retained/'+$hash+'.bin');[IO.Directory]::CreateDirectory((Split-Path $saved -Parent))|Out-Null
                [IO.File]::Copy($target,$saved,$true)
                if((Get-FileHash -LiteralPath $saved).Hash -ine $hash){throw 'Не удалось сохранить изменённый файл NR + MFG.'}
            }
        }
    }
    $changes=@(foreach($file in $Record.before){
        @{path=$file.path;source=$(if($file.existed){Resolve-ChildPath $paths.root $file.saved}else{$null});sha256=$file.sha256}
    })
    if($receiptChange){$changes+=@($receiptChange)}
    $Record.phase='prepared';Write-Json $paths.state $Record
    $backup=Invoke-ComponentTransaction $Config $changes
    foreach($file in $Record.before){
        $target=Resolve-ChildPath $Config.game $file.path
        if($file.existed){if(-not(Test-Path -LiteralPath $target) -or (Get-FileHash -LiteralPath $target).Hash -ine $file.sha256){throw 'Прежний файл пакета не восстановлен.'}}
        elseif(Test-Path -LiteralPath $target){throw 'Файл NR + MFG остался после отключения.'}
    }
    $Record.enabled=$false;$Record.phase='ready';Write-Json $paths.state $Record
    Clear-EnhancementTransactionPayload $Config $backup
}
function Reset-EnhancementStartup($Config){
    $paths=Get-EnhancementPaths $Config;$record=Read-Json $paths.state
    if($record.phase -eq 'prepared'){throw 'Настройка NR + MFG была прервана. Выключи «NR + MFG» для восстановления прежних файлов.'}
    if(-not $record.enabled){return}
    if($record.phase -ne 'ready'){throw 'Установка NR + MFG не завершена.'}
    Assert-GameWritable $Config
    $status=Get-EnhancementPackageState $Config
    if(-not $status.compatible){throw 'Настройки NR + MFG несовместимы с выбранным VR-портом.'}
    if(-not $status.healthy){throw $status.reason}
    # Legacy callers use this as a pre-launch check. Defaults belong to the
    # initial recipe; never overwrite choices saved by the user on launch.
}
