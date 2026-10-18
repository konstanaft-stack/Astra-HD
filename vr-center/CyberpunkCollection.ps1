$ErrorActionPreference='Stop'
# Adapted from ModOrganizer2/modorganizer-basic_games game_cyberpunk2077.py,
# f1207d6398fbb4adda281f2bd5c563105391baaf, Copyright 2020 Holt59, MIT.
# See licenses/basic-games-MIT.txt. Only preparation is used; no MO2/PyQt runtime.

function Get-CyberpunkCollectionOrder($Roots,[string]$Kind,[bool]$Reverse){
    $owned=@{};$groups=[Collections.Generic.List[object]]::new()
    # Roots are highest first; select each effective filename before ordering groups.
    foreach($root in $Roots){
        $group=[Collections.Generic.List[string]]::new()
        $files=if($Kind -eq 'archive'){@(Get-ChildItem -LiteralPath (Join-Path $root 'archive/pc/mod') -Filter '*.archive' -File -ErrorAction SilentlyContinue|Sort-Object Name)}else{@(Get-ChildItem -LiteralPath (Join-Path $root 'mods') -Directory -ErrorAction SilentlyContinue|Sort-Object Name)}
        foreach($file in $files){if(-not $owned.ContainsKey($file.Name)){$owned[$file.Name]=$true;$group.Add($file.Name)}}
        $groups.Add(@($group.ToArray()))
    }
    $result=[Collections.Generic.List[string]]::new()
    if($Reverse){foreach($group in $groups){foreach($name in $group){$result.Add($name)}}}
    else{for($i=$groups.Count-1;$i -ge 0;$i--){foreach($name in $groups[$i]){$result.Add($name)}}}
    return @($result.ToArray())
}

function Prepare-CetRuntimeView($Config,$Plan){
    $Config=Get-GameLaunchCollectionConfig $Config $Plan
    $overwrite=Get-GameLaunchPlainPath $(if($Config.mo2.overwriteRoot){$Config.mo2.overwriteRoot}else{Join-Path $Config.mo2.root 'overwrite'})
    $prefix='bin/x64/plugins/cyber_engine_tweaks/mods'
    $targetRoot=Join-Path $overwrite $prefix
    $identity=[Convert]::ToHexString([Security.Cryptography.SHA256]::HashData([Text.Encoding]::UTF8.GetBytes($overwrite.ToLowerInvariant())))
    $statePath=Join-Path $Config.data ('collection-cache/cet-'+$identity+'.json')
    $before=Read-Json $statePath
    if($before -and ($before.overwrite -ine $overwrite -or $before.schema -ne 1)){throw 'CET runtime ownership mismatch.'}
    $owned=if($before){$before.files}else{@{}}
    $effective=@{};$modules=@{};$next=@{}
    $stats=@{copied=0;updated=0;removed=0;preserved=0;modules=0;bytesCopied=0;manifest=$statePath}
    # CET changes cwd before CRT fopen("init.lua"). usvfs redirects that cwd to
    # the writable directory: it must contain the complete effective module.
    $roots=@(Get-GameLaunchModRoots $Config $Plan)+@($Plan.game)
    foreach($root in $roots){
        if((Get-GameLaunchPlainPath $root) -ieq $overwrite){continue}
        $base=Join-Path $root $prefix
        if(-not(Test-Path -LiteralPath $base -PathType Container)){continue}
        foreach($file in Get-ChildItem -LiteralPath $base -File -Recurse){
            $relative=[IO.Path]::GetRelativePath($base,$file.FullName).Replace('\','/')
            if($relative -match '(^|/)([^/]*\.mohidden|\.git)(/|$)' -or ($file.Attributes -band [IO.FileAttributes]::ReparsePoint)){continue}
            if(-not $effective.ContainsKey($relative)){$effective[$relative]=$file.FullName}
        }
    }
    # Selected native-port scripts have the same precedence as explicit VFS links.
    foreach($relative in @($Plan.selectedFiles.Keys)){
        if($relative -and $relative.StartsWith($prefix+'/',[StringComparison]::OrdinalIgnoreCase)){
            $effective[$relative.Substring($prefix.Length+1)]=$Plan.selectedFiles[$relative]
        }
    }
    foreach($relative in $effective.Keys){if($relative -match '^([^/]+)/init\.lua$'){$modules[$Matches[1]]=$true}}
    $stats.modules=$modules.Count
    function Resolve-CetWritableFile([string]$Relative){
        $path=Resolve-ChildPath $targetRoot $Relative
        $cursor=$path
        while($cursor.Length -ge $overwrite.Length){
            if(Test-Path -LiteralPath $cursor){
                if((Get-Item -LiteralPath $cursor -Force).Attributes -band [IO.FileAttributes]::ReparsePoint){throw ('CET writable path contains a link: '+$cursor)}
            }
            if($cursor -ieq $overwrite){break}
            $cursor=Split-Path $cursor -Parent
        }
        return $path
    }
    foreach($relative in @($effective.Keys)){
        if(-not $modules.ContainsKey($relative.Split('/')[0])){continue}
        $target=Resolve-CetWritableFile $relative
        $exists=Test-Path -LiteralPath $target -PathType Leaf
        if($exists){
            if(-not $owned.ContainsKey($relative)){$stats.preserved++;continue}
            $currentHash=(Get-FileHash -LiteralPath $target).Hash
            # Settings, databases, and edited scripts become user-owned on change.
            if($currentHash -cne $owned[$relative]){$stats.preserved++;continue}
        }
        $source=$effective[$relative];$hash=(Get-FileHash -LiteralPath $source).Hash
        if(-not $exists -or $currentHash -cne $hash){
            [IO.Directory]::CreateDirectory((Split-Path $target -Parent))|Out-Null
            $temp=$target+'.'+[guid]::NewGuid().ToString('N')+'.tmp'
            try{
                [IO.File]::Copy($source,$temp,$false)
                [IO.File]::Move($temp,$target,$true)
            }finally{if(Test-Path -LiteralPath $temp){Remove-Item -LiteralPath $temp -Force}}
            if($exists){$stats.updated++}else{$stats.copied++}
            $stats.bytesCopied+=(Get-Item -LiteralPath $source).Length
        }
        $next[$relative]=$hash
    }
    foreach($relative in $owned.Keys){
        if($effective.ContainsKey($relative) -and $modules.ContainsKey($relative.Split('/')[0])){continue}
        $target=Resolve-CetWritableFile $relative
        if((Test-Path -LiteralPath $target -PathType Leaf) -and (Get-FileHash -LiteralPath $target).Hash -ceq $owned[$relative]){
            # Remove only unchanged generated files; never user data or directories.
            Remove-Item -LiteralPath $target -Force;$stats.removed++
        }
    }
    Write-Json $statePath @{schema=1;overwrite=$overwrite;files=$next}
    return $stats
}

function Prepare-CyberpunkCollection($Config,$Plan){
    $Config=Get-GameLaunchCollectionConfig $Config $Plan
    $overwrite=Get-GameLaunchPlainPath $(if($Config.mo2.overwriteRoot){$Config.mo2.overwriteRoot}else{Join-Path $Config.mo2.root 'overwrite'})
    [IO.Directory]::CreateDirectory($overwrite)|Out-Null
    $roots=@(Get-GameLaunchModRoots $Config $Plan -IncludeShared)+@($Plan.game)
    $options=$Plan.source.cyberpunk
    $archives=@(Get-CyberpunkCollectionOrder $roots 'archive' ([bool]$options.reverseArchiveOrder))
    $redmods=@(Get-CyberpunkCollectionOrder $roots 'redmod' ([bool]$options.reverseRedmodOrder))
    if($options.enforceArchiveOrder){
        $orderPath=Get-GameLaunchPlainPath (Join-Path $overwrite 'archive/pc/mod/modlist.txt')
        [IO.Directory]::CreateDirectory((Split-Path $orderPath -Parent))|Out-Null
        [IO.File]::WriteAllText($orderPath,(@($archives) -join "`n"),[Text.UTF8Encoding]::new($false))
    }
    # Copy mutable base caches to overwrite so the hooked game cannot rewrite stock files.
    $cacheSource=Get-GameLaunchPlainPath (Join-Path $Plan.game 'r6/cache')
    $cacheTarget=Get-GameLaunchPlainPath (Join-Path $overwrite 'r6/cache')
    $identity=[Convert]::ToHexString([Security.Cryptography.SHA256]::HashData([Text.Encoding]::UTF8.GetBytes($overwrite.ToLowerInvariant())))
    $statePath=Join-Path $Config.data ('collection-cache/'+$identity+'.json')
    $before=Read-Json $statePath;$hashes=@{}
    $baseFiles=@(Get-ChildItem -LiteralPath $cacheSource -File -ErrorAction SilentlyContinue)
    foreach($file in $baseFiles){$hashes[$file.Name]=(Get-FileHash -LiteralPath $file.FullName).Hash}
    $changed=$false
    if($before){
        if($before.game -ine $Plan.game -or $before.overwrite -ine $overwrite){throw 'Cache ownership mismatch.'}
        $changed=$before.hashes.Count -ne $hashes.Count
        foreach($name in $hashes.Keys){if($before.hashes[$name] -cne $hashes[$name]){$changed=$true}}
    }
    if($changed -and (Test-Path -LiteralPath $cacheTarget)){
        # Keep the rename on the same volume even when center data is on D: and mods on C:.
        $retired=Get-GameLaunchPlainPath (Join-Path (Split-Path $overwrite -Parent) ('.vr-center-retired-caches/'+[guid]::NewGuid().ToString('N')))
        if(-not $cacheTarget.StartsWith($overwrite.TrimEnd('\')+'\',[StringComparison]::OrdinalIgnoreCase)){throw 'Cache escaped overwrite.'}
        [IO.Directory]::CreateDirectory((Split-Path $retired -Parent))|Out-Null
        [IO.Directory]::Move($cacheTarget,$retired)
    }
    foreach($file in $baseFiles){
        $relative='r6/cache/'+$file.Name
        $effective=Resolve-GameLaunchEffectiveFile $Config $Plan $relative -Roots $roots
        if($effective -ieq $file.FullName){
            $target=Get-GameLaunchPlainPath (Join-Path $overwrite $relative)
            [IO.Directory]::CreateDirectory((Split-Path $target -Parent))|Out-Null
            [IO.File]::Copy($file.FullName,$target,$false)
        }
    }
    Write-Json $statePath @{schema=1;game=$Plan.game;overwrite=$overwrite;hashes=$hashes}
    $cet=Prepare-CetRuntimeView $Config $Plan
    return @{archives=$archives;redmods=$redmods;cacheUpdated=$changed;overwrite=$overwrite;cet=$cet}
}
