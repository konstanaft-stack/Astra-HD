# A shared package has one physical owner, independent of game-profile snapshots.
function Get-SharedEnhancementDefinition($Config){
    if(-not $Config.imagePackage){return $null}
    $spec=Read-Json ([string]$Config.imagePackage)
    if($spec.scope -ne 'game-shared'){return $null}
    if($spec.schema -ne 1 -or -not $spec.id -or $spec.files -isnot [array] -or
       $spec.loaderPath -cne 'bin/x64/dbghelp.dll' -or $spec.disabledLoaderPath -cne 'bin/x64/dbghelp.dll.disabled'){throw 'Invalid shared enhancement definition.'}
    $seen=@{};foreach($file in $spec.files){
        $path=[string]$file.path
        if(-not $path.StartsWith('bin/x64/',[StringComparison]::OrdinalIgnoreCase) -or $path -match '\\|:|(^|/)\.{1,2}(/|$)|[?*]' -or
           $path -ieq 'bin/x64/nvngx_dlssg.dll' -or $path -ieq $spec.disabledLoaderPath -or $seen.ContainsKey($path) -or $file.sha256 -notmatch '^[A-Fa-f0-9]{64}$'){throw 'Invalid shared enhancement path/hash.'}
        if($file.source -and -not [IO.Path]::IsPathRooted([string]$file.source)){
            $definitionRoot=[IO.Path]::GetDirectoryName([IO.Path]::GetFullPath([string]$Config.imagePackage))
            $file.source=Resolve-ChildPath $definitionRoot ([string]$file.source)
        }
        $null=Resolve-ChildPath $Config.game $path;$seen[$path]=$file
    }
    if(-not $seen.ContainsKey($spec.loaderPath) -or $seen[$spec.loaderPath].mutable){throw 'Shared loader must be immutable.'}
    foreach($path in $spec.runtimeMutablePaths){
        if($path -notin @('bin/x64/RTXMFG-Universal.json','bin/x64/RTXMFG-UI.ini') -or $seen.ContainsKey($path)){throw 'Invalid optional shared runtime setting.'}
        $seen[$path]=$true
    }
    foreach($path in $spec.requiredFiles){if(-not $seen.ContainsKey($path)){throw 'Shared required file absent.'}}
    return $spec
}
function Get-SharedEnhancementPaths($Config){
    $spec=Get-SharedEnhancementDefinition $Config;if(-not $spec){return @()}
    return @(@($spec.files|ForEach-Object path)+@($spec.disabledLoaderPath)+@($spec.runtimeMutablePaths))
}
function Get-SharedEnhancementState($Config,$Spec,[switch]$VerifyFiles){
    $record=Read-Json (Join-Path $Config.data 'enhancement-common/state.json')
    $result=@{scope='game-shared';available=$true;installed=$false;enabled=$false;compatible=$true;healthy=$true;label='NR + MFG';sourceLabel=$Spec.label;packageId=$Spec.id;suspended=$false;status='NR + MFG выключен';reason=''}
    if(-not $record){$result.status='Общий комплект NR + MFG ещё не установлен';return $result}
    try{
        if($record.scope -ne 'game-shared' -or $record.id -cne $Spec.id -or $record.phase -cne 'ready'){throw 'Shared enhancement installation incomplete or definition changed.'}
        $on=Test-Path -LiteralPath (Resolve-ChildPath $Config.game $Spec.loaderPath) -PathType Leaf
        $off=Test-Path -LiteralPath (Resolve-ChildPath $Config.game $Spec.disabledLoaderPath) -PathType Leaf
        if($on -eq $off){throw 'Exactly one shared loader name must exist.'}
        foreach($file in $Spec.files){
            $relative=if($file.path -eq $Spec.loaderPath -and $off){$Spec.disabledLoaderPath}else{$file.path}
            $path=Resolve-ChildPath $Config.game $relative
            if(-not(Test-Path -LiteralPath $path -PathType Leaf) -or (($VerifyFiles -or $file.path -eq $Spec.loaderPath) -and -not $file.mutable -and (Get-FileHash -LiteralPath $path).Hash -ine $file.sha256)){throw ('Shared enhancement file differs: '+$relative)}
        }
        $result.installed=$true;$result.enabled=$on;$result.status=if($on){'NR + MFG включён'}else{'NR + MFG выключен'}
    }catch{$result.healthy=$false;$result.reason=$_.Exception.Message;$result.status=$result.reason}
    return $result
}
function Set-SharedEnhancementEnabled($Config,[bool]$Enabled){
    Assert-GameWritable $Config;$spec=Get-SharedEnhancementDefinition $Config
    $state=Get-SharedEnhancementState $Config $spec -VerifyFiles
    if(-not $state.healthy -or -not(Test-Path -LiteralPath (Join-Path $Config.data 'enhancement-common/state.json'))){throw 'Install/repair the shared package explicitly before toggling.'}
    if($state.enabled -eq $Enabled){return}
    $from=Resolve-ChildPath $Config.game $(if($Enabled){$spec.disabledLoaderPath}else{$spec.loaderPath})
    $to=Resolve-ChildPath $Config.game $(if($Enabled){$spec.loaderPath}else{$spec.disabledLoaderPath})
    # One same-directory rename; never copy payload, rewrite INI or restore port receipts.
    [IO.File]::Move($from,$to,$false)
}
function Install-SharedEnhancementPackage($Config){
    Assert-GameWritable $Config;$spec=Get-SharedEnhancementDefinition $Config;if(-not $spec){throw 'No shared package configured.'}
    $root=Join-Path $Config.data 'enhancement-common';$state=Join-Path $root 'state.json'
    if(Test-Path -LiteralPath $state){throw 'Shared package already has an installation record; use toggle or explicit repair.'}
    if(Test-Path -LiteralPath (Resolve-ChildPath $Config.game $spec.disabledLoaderPath)){throw 'Unowned disabled loader exists.'}
    $fallbackLoaderSha256='';$priorLoader=Resolve-ChildPath $Config.game $spec.loaderPath
    if(Test-Path -LiteralPath $priorLoader -PathType Leaf){
        $signature=Get-AuthenticodeSignature -LiteralPath $priorLoader
        if($signature.Status -eq 'Valid' -and $signature.SignerCertificate.Subject -match '(^|,\s*)O=Microsoft Corporation(,|$)'){$fallbackLoaderSha256=(Get-FileHash -LiteralPath $priorLoader).Hash}
    }
    $changes=@();foreach($file in $spec.files){
        if(-not(Test-Path -LiteralPath $file.source -PathType Leaf) -or (Get-FileHash -LiteralPath $file.source).Hash -ine $file.sha256){throw ('Corrupt shared source: '+$file.path)}
        $target=Resolve-ChildPath $Config.game $file.path
        # Existing user-menu settings always win over package defaults.
        if($file.mutable -and (Test-Path -LiteralPath $target -PathType Leaf)){continue}
        $changes+=@{path=$file.path;source=$file.source;sha256=$file.sha256}
    }
    $receiptPath=Join-Path $Config.game 'CPVR-Portable/installed.json';$receipt=$null
    if(Test-Path -LiteralPath $receiptPath){$receipt=[IO.File]::ReadAllText($receiptPath)|ConvertFrom-Json -AsHashtable -DateKind String -ErrorAction Stop}
    if($receipt){
        $owned=@(Get-SharedEnhancementPaths $Config);$critical=Get-EnhancementCriticalMap $receipt
        foreach($path in $owned){$critical.Remove($path)}
        $receipt.critical=@(foreach($path in $critical.Keys){@{path=$path;sha256=$critical[$path]}})
        $receipt.paths=@($receipt.paths|Where-Object {$_ -notin $owned})
        [IO.Directory]::CreateDirectory($root)|Out-Null
        $prepared=Join-Path $root 'detached-port-receipt.json';Write-Json $prepared $receipt
        $changes+=@{path='CPVR-Portable/installed.json';source=$prepared;sha256=(Get-FileHash $prepared).Hash}
    }
    # Transaction retains exact replaced Microsoft/port dbghelp and every prior payload.
    $backup=Invoke-ComponentTransaction $Config $changes
    try{Write-Json $state @{schema=1;scope='game-shared';id=$spec.id;phase='ready';backup=$backup;fallbackLoaderSha256=$fallbackLoaderSha256;files=$spec.files;installedUtc=[datetime]::UtcNow.ToString('o')}}catch{
        $failure=$_.Exception.Message
        try{
            $plan=Read-Json (Join-Path $backup 'plan.json')
            $rollback=@(foreach($entry in $plan){@{path=$entry.path;source=$(if($entry.existed){$entry.saved}else{$null});sha256=$(if($entry.existed){(Get-FileHash -LiteralPath $entry.saved).Hash}else{''})}})
            $null=Invoke-ComponentTransaction $Config $rollback
        }catch{throw ($failure+'; shared install rollback failed; exact backup: '+$backup+'; '+$_.Exception.Message)}
        throw ($failure+'; shared payload and port receipt restored; backup: '+$backup)
    }
}
