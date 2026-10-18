. "$PSScriptRoot/Core.ps1"

function Get-RedmodState($Config){
    $source=Get-GameLaunchSource $Config
    $exe=Join-Path $Config.game 'tools/redmod/bin/redMod.exe'
    $installed=Test-Path -LiteralPath $exe -PathType Leaf
    $enabled='-modded' -in @($source.arguments)
    $repairAvailable=[bool]($Config.redmodPackage -and (Test-Path -LiteralPath $Config.redmodPackage -PathType Leaf))
    return @{enabled=$enabled;installed=$installed;available=($installed -or $repairAvailable -or $enabled);repairAvailable=$repairAvailable;exe=$exe;status=$(if($enabled -and -not $installed){'REDmod включён, но штатный инструмент отсутствует.'}elseif($enabled){'REDmod включён'}else{'REDmod выключен'})}
}

function Ensure-RedmodInstalled($Config){
    $state=Get-RedmodState $Config
    if($state.installed){return}
    Assert-GameWritable $Config
    if(-not $state.repairAvailable){throw 'Штатный инструмент REDmod не установлен: tools/redmod/bin/redMod.exe. Галочка включает его использование, а не устанавливает DLC.'}
    $package=Read-Json $Config.redmodPackage
    if($package.schema -ne 1 -or -not $package.files.Count){throw 'Повреждён сохранённый комплект REDmod.'}
    $root=Join-Path $Config.game 'tools/redmod';$changes=@();$seen=@{}
    foreach($file in $package.files){
        $target=Resolve-ChildPath $root ([string]$file.path)
        if($seen.ContainsKey($file.path) -or $file.sha256 -notmatch '^[A-Fa-f0-9]{64}$'){throw 'Некорректный состав REDmod.'};$seen[$file.path]=$true
        if(-not(Test-Path -LiteralPath $file.source -PathType Leaf) -or (Get-FileHash -LiteralPath $file.source).Hash -ine $file.sha256){throw ('Не найден сохранённый файл REDmod: '+$file.path)}
        if(Test-Path -LiteralPath $target){
            if(-not(Test-Path -LiteralPath $target -PathType Leaf) -or (Get-FileHash -LiteralPath $target).Hash -ine $file.sha256){throw ('Не заменяю другой файл REDmod: '+$file.path)}
        }else{$changes+=@{path=$file.path;source=$file.source;target=$target;sha256=$file.sha256}}
    }
    if(-not $seen.ContainsKey('bin/redMod.exe')){throw 'В сохранённом комплекте нет redMod.exe.'}
    $done=@()
    try{
        Assert-GameWritable $Config
        foreach($file in $changes){
            [IO.Directory]::CreateDirectory((Split-Path $file.target -Parent))|Out-Null
            [IO.File]::Copy($file.source,$file.target,$false);$done+=$file
        }
        Write-Json (Join-Path $Config.data ('tool-receipts/redmod-'+[guid]::NewGuid().ToString('N')+'.json')) @{schema=1;package=$Config.redmodPackage;game=$Config.game;createdUtc=[datetime]::UtcNow.ToString('o');addedFiles=$done}
    }catch{
        foreach($file in $done){if((Test-Path -LiteralPath $file.target -PathType Leaf) -and (Get-FileHash -LiteralPath $file.target).Hash -ieq $file.sha256){[IO.File]::Delete($file.target)}}
        throw
    }
}

function Set-RedmodEnabled($Config,[bool]$Enabled){
    Assert-GameWritable $Config
    if($Enabled){Ensure-RedmodInstalled $Config}
    $source=Get-GameLaunchSource $Config
    $source.arguments=@($source.arguments|Where-Object {$_ -and $_ -ne '-modded'})
    if($Enabled){$source.arguments+='-modded'}
    Write-Json (Join-Path $Config.data 'launch-source.json') $source
}

function Invoke-RedmodDeployment($Config,$Plan,[string]$Log,$Request=$null,[string]$HostPath=''){
    if('-modded' -notin @($Plan.arguments)){return}
    $mods=if($Request){@($Request.redmods)}else{@(Get-ChildItem -LiteralPath (Join-Path $Plan.game 'mods') -Directory -ErrorAction SilentlyContinue|Where-Object {Test-Path -LiteralPath (Join-Path $_.FullName 'info.json')}|ForEach-Object Name)}
    if(-not $mods.Count){return}
    Ensure-RedmodInstalled $Config
    $exe=Join-Path $Plan.game 'tools/redmod/bin/redMod.exe'
    $orderPath=Join-Path $Config.data 'launches/MO_REDmod_load_order.txt'
    [IO.Directory]::CreateDirectory((Split-Path $orderPath -Parent))|Out-Null
    $order=$mods -join "`n";$previous=if(Test-Path -LiteralPath $orderPath){[IO.File]::ReadAllText($orderPath)}else{''}
    [IO.File]::WriteAllText($orderPath,$order,[Text.UTF8Encoding]::new($false))
    $args=@('deploy','-reportProgress',('-modlist='+$orderPath))
    if($previous -cne $order){$args+='-force'}
    if($Request){
        $utility=$Request.Clone();$utility.exe=$exe;$utility.workingDirectory=Split-Path $exe -Parent
        $utility.forcedLibraries=@();$utility.timeoutSeconds=900;$utility.logPath=$Log+'.usvfs.log';$utility.arguments=$args
        $utilityPath=Join-Path $Config.data ('launches/redmod-'+[guid]::NewGuid().ToString('N')+'.json')
        Write-Json $utilityPath $utility
        $psi=Get-StandaloneVfsProcessInfo $Config $HostPath $utilityPath $utility.workingDirectory
    }else{
        $psi=[Diagnostics.ProcessStartInfo]::new($exe);$psi.UseShellExecute=$false;$psi.CreateNoWindow=$true;$psi.WindowStyle='Hidden';$psi.WorkingDirectory=Split-Path $exe -Parent
        foreach($arg in $args){$psi.ArgumentList.Add($arg)}
    }
    $psi.RedirectStandardOutput=$true;$psi.RedirectStandardError=$true
    $process=[Diagnostics.Process]::Start($psi)
    try{
        $stdout=$process.StandardOutput.ReadToEndAsync();$stderr=$process.StandardError.ReadToEndAsync()
        $process.WaitForExit();$out=$stdout.GetAwaiter().GetResult();$err=$stderr.GetAwaiter().GetResult()
        [IO.File]::WriteAllText($Log+'.result.log',$out+"`n"+$err)
        if($process.ExitCode -ne 0){throw ('REDmod не завершил подготовку модов. Код '+$process.ExitCode+'; журнал '+$Log+'.result.log')}
    }finally{$process.Dispose()}
}
