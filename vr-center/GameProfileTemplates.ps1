$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot 'GameProfileStore.ps1')
. (Join-Path $PSScriptRoot 'SharedEnhancementPolicy.ps1')

function ConvertTo-CyberpunkTemplatePath([string]$Path){
    $relative=$Path.Replace('\','/').ToLowerInvariant()
    Assert-GameProfileRelativePath $relative
    return $relative
}

function Get-CyberpunkTemplateSource([string]$Path,[string]$Sha1,[Nullable[long]]$Size,[string]$Sha256,[switch]$StockIdentityOnly){
    Assert-GameProfilePlainPath $Path
    if(-not(Test-Path -LiteralPath $Path -PathType Leaf)){return $null}
    # SHA1 stock identity and SHA256 object identity describe the same locked bytes.
    $stream=[IO.File]::Open($Path,[IO.FileMode]::Open,[IO.FileAccess]::Read,[IO.FileShare]::Read)
    try{
        if($null -ne $Size -and $stream.Length -ne $Size){return $null}
        if($Sha1){
            $actualSha1=[Convert]::ToHexString([Security.Cryptography.SHA1]::HashData($stream))
            if($actualSha1 -ine $Sha1){return $null}
            if($StockIdentityOnly){return @{source=$Path;sha1=$actualSha1;size=[long]$stream.Length}}
            $stream.Position=0
        }
        $actualSha256=[Convert]::ToHexString([Security.Cryptography.SHA256]::HashData($stream))
        if($Sha256 -and $actualSha256 -ine $Sha256){return $null}
        return @{source=$Path;sha256=$actualSha256;size=[long]$stream.Length}
    }finally{$stream.Dispose()}
}

function Get-CyberpunkProfileTemplatePlan {
    param($Config,$StoreConfig,[switch]$VR)
    $context=Get-GameProfileContext $StoreConfig
    foreach($id in @('game','appdata','user-state','center-settings','mods-state','keybindings','usb-state')){
        if(-not $context.scopes.ContainsKey($id)){throw ('Missing clean template scope: '+$id)}
    }
    foreach($id in @('game','appdata','user-state','center-settings')){
        if($context.scopes[$id].kind -cne 'directory'){throw ('Clean template requires directory scope: '+$id)}
    }
    foreach($id in @('mods-state','keybindings','usb-state')){
        if($context.scopes[$id].kind -cne 'file'){throw ('Clean template requires file scope: '+$id)}
    }
    if(-not [IO.Path]::IsPathFullyQualified([string]$Config.game) -or -not [IO.Path]::IsPathFullyQualified([string]$Config.bundle)){
        throw 'Template game and bundle paths must be absolute.'
    }
    $game=[IO.Path]::GetFullPath([string]$Config.game).TrimEnd('\')
    $bundleRoot=[IO.Path]::GetFullPath([string]$Config.bundle).TrimEnd('\')
    if($game -ine $context.scopes.game.root){throw 'Template source game must match the trusted game scope.'}
    Assert-GameProfilePlainPath $game;Assert-GameProfilePlainPath $bundleRoot
    if('bin/x64/cyberpunk2077.exe' -notin $context.scopes.game.requiredFiles){throw 'Game scope must require bin/x64/Cyberpunk2077.exe.'}
    $bundle=Read-GameProfileJson (Join-Path $bundleRoot 'bundle.json')
    $stock=Read-GameProfileJson (Join-Path $bundleRoot 'stock-paths.json')
    $baselineStock=Read-GameProfileJson (Get-GameProfileStorePath $context 'baseline-stock.json')
    if($baselineStock){$stock=$baselineStock}
    if($bundle.schemaVersion -ne 1 -or $bundle.exeSha256 -notmatch '^[0-9a-fA-F]{64}$' -or $stock -isnot [Collections.IDictionary]){
        throw 'Invalid clean template source manifests.'
    }
    $flats=@($bundle.versions|Where-Object id -CEQ 'flat')
    if($flats.Count -ne 1 -or -not $flats[0].available -or $flats[0].kind -cne 'flat' -or $flats[0].files -isnot [Collections.IDictionary]){
        throw 'Bundle must contain one available flat stock fallback.'
    }
    $flat=$flats[0];$flatFiles=@{}
    $commonPaths=@(Get-SharedEnhancementPaths $Config)
    foreach($key in $flat.files.Keys){
        if($key -ceq '@settings'){continue}
        $path=ConvertTo-CyberpunkTemplatePath ([string]$key)
        if($path -in $commonPaths){continue}
        $hash=[string]$flat.files[$key]
        if($flatFiles.ContainsKey($path) -or $hash -notmatch '^[0-9a-fA-F]{64}$'){throw ('Invalid flat file identity: '+$path)}
        $flatFiles[$path]=$hash.ToUpperInvariant()
    }
    $private=@{};$shared=@{};$seenStock=@{}
    foreach($key in $stock.Keys){
        $path=ConvertTo-CyberpunkTemplatePath ([string]$key)
        if($path -in $commonPaths){continue}
        $expected=$stock[$key]
        if($seenStock.ContainsKey($path)){throw ('Duplicate stock path: '+$path)}
        $seenStock[$path]=$true
        if($expected.sha1 -notmatch '^[0-9a-fA-F]{40}$' -or ($expected.size -isnot [int] -and $expected.size -isnot [long]) -or [long]$expected.size -lt 0){
            throw ('Invalid stock identity: '+$path)
        }
        if($path -match '^archive/pc/(content|ep1)/'){$shared[$path]=$expected;continue}
        if($private.ContainsKey($path)){throw ('Duplicate stock path: '+$path)}
        $null=Get-GameProfileTargetPath $context 'game' $path
        $private[$path]=$expected
    }
    if(-not $private.ContainsKey('bin/x64/cyberpunk2077.exe')){throw 'Stock manifest omits mandatory bin/x64/Cyberpunk2077.exe.'}
    # Share only the exact stock identities (real bundle: 59 archives + 2 EP1
    # auxiliary files). Add-on archives in either directory remain private state.
    foreach($path in $shared.Keys){
        if($path -notin $context.scopes.game.exclusions){throw ('Shared stock file requires an exact snapshot exclusion: '+$path)}
    }
    foreach($directory in @('archive/pc/content','archive/pc/ep1')){
        $extra=$directory+'/.cpvr-private-extra-'+[guid]::NewGuid().ToString('N')+'.archive'
        if(Test-GameProfileExcluded $context.scopes.game $extra){throw ('Shared directory-wide exclusion would lose private add-on files: '+$directory)}
    }
    $badShared=[Collections.Generic.List[string]]::new();$sharedDiagnostics=[Collections.Generic.List[object]]::new()
    foreach($path in @($shared.Keys|Sort-Object)){
        $identity=$null;$expected=$shared[$path];$sourceError=''
        try{$identity=Get-CyberpunkTemplateSource (Join-Path $game $path) ([string]$expected.sha1) ([long]$expected.size) '' -StockIdentityOnly}catch{$sourceError=$_.Exception.Message}
        if(-not $identity){$badShared.Add($path);$sharedDiagnostics.Add(@{path=$path;sourceError=$sourceError})}
    }
    if($badShared.Count){
        $failure=[InvalidOperationException]::new('Unverified shared stock ('+$badShared.Count+'): '+(@($badShared.ToArray()|Select-Object -First 12) -join ', '))
        $failure.Data['UnverifiedSharedStockPaths']=$badShared.ToArray()
        $failure.Data['SharedStockDiagnostics']=$sharedDiagnostics.ToArray()
        throw $failure
    }
    foreach($path in $flatFiles.Keys){if(-not $private.ContainsKey($path)){throw ('Flat fallback contains non-stock file: '+$path)}}

    # A proven clean base remains useful after collections modify additional
    # stock paths not among the portable flat fallback's 105 files.
    $cachedStock=@{}
    $cached=Read-GameProfileJson (Get-GameProfileStorePath $context 'templates/clean-flat.json')
    if($cached){
        if($cached.schema -ne 1 -or $cached.id -cne 'clean-flat' -or $cached.vr -or $cached.files -isnot [array]){throw 'Invalid existing clean-flat stock cache manifest.'}
        foreach($file in $cached.files){
            if($file.scope -cne 'game'){continue}
            $path=ConvertTo-CyberpunkTemplatePath ([string]$file.path)
            if(-not $private.ContainsKey($path)){continue}
            if($cachedStock.ContainsKey($path) -or $file.sha256 -notmatch '^[0-9a-fA-F]{64}$'){throw ('Invalid cached stock identity: '+$path)}
            $cachedStock[$path]=([string]$file.sha256).ToUpperInvariant()
        }
    }

    $plan=@{};$missing=[Collections.Generic.List[string]]::new();$diagnostics=[Collections.Generic.List[object]]::new()
    foreach($path in @($private.Keys|Sort-Object)){
        $expected=$private[$path];$entry=$null;$attempts=[Collections.Generic.List[string]]::new()
        $source=Join-Path $game $path
        try{$entry=Get-CyberpunkTemplateSource $source ([string]$expected.sha1) ([long]$expected.size) ''}catch{$attempts.Add($_.Exception.Message)}
        if(-not $entry -and $flatFiles.ContainsKey($path)){
            $fallback=Join-Path $bundleRoot ('files/'+$flatFiles[$path])
            try{$entry=Get-CyberpunkTemplateSource $fallback ([string]$expected.sha1) ([long]$expected.size) $flatFiles[$path]}catch{$attempts.Add($_.Exception.Message)}
        }
        if(-not $entry -and $cachedStock.ContainsKey($path)){
            $fallback=Get-GameProfileStorePath $context ('objects/'+$cachedStock[$path])
            try{$entry=Get-CyberpunkTemplateSource $fallback ([string]$expected.sha1) ([long]$expected.size) $cachedStock[$path]}catch{$attempts.Add($_.Exception.Message)}
        }
        if(-not $entry){$missing.Add($path);$diagnostics.Add(@{path=$path;attemptErrors=@($attempts.ToArray())});continue}
        $entry.scope='game';$entry.path=$path;$plan[$path]=$entry
    }
    if($missing.Count){
        $failure=[InvalidOperationException]::new('Missing verified stock ('+$missing.Count+'): '+(@($missing.ToArray()|Select-Object -First 12) -join ', '))
        $failure.Data['MissingStockPaths']=$missing.ToArray();$failure.Data['StockDiagnostics']=$diagnostics.ToArray()
        throw $failure
    }
    if($plan['bin/x64/cyberpunk2077.exe'].sha256 -ine $bundle.exeSha256){throw 'Stock executable does not match bundle.exeSha256.'}

    $version=$flat;$selected=@{}
    foreach($path in $flatFiles.Keys){$selected[$path]=$plan[$path].sha256}
    if($VR){
        $versions=@($bundle.versions|Where-Object {$_.id -ceq [string]$bundle.defaultVersion})
        if($versions.Count -eq 0 -or ($versions.Count -eq 1 -and (-not $versions[0].available -or $versions[0].kind -cne 'native'))){
            $versions=@($bundle.versions|Where-Object id -CEQ 'tofu-aer-v1-candidate')
        }
        if($versions.Count -ne 1 -or -not $versions[0].available -or $versions[0].kind -cne 'native' -or $versions[0].files -isnot [Collections.IDictionary]){
            throw 'One available complete native Astra HD manifest is required.'
        }
        $version=$versions[0];$overlay=@{}
        foreach($key in $version.files.Keys){
            if($key -ceq '@settings'){continue}
            $path=ConvertTo-CyberpunkTemplatePath ([string]$key)
            if($path -in $commonPaths){continue}
            if($path -match '(^|/)(HUDitor[^/]*|HandsPort[^/]*|CyberpunkHandsPort[^/]*|CPVR_HandsTracking|CPVR_Hands[^/]*)(/|\.|$)'){
                throw ('External component in clean native manifest: '+$path)
            }
            # Keep personal head/controller calibration fresh. vrcam.json instead
            # declares the authored camera components and initial selection: the
            # Lua/native bridge cannot enable a stereo view without that catalogue.
            if($path -eq 'bin/x64/vrport.ini'){continue}
            $hash=[string]$version.files[$key]
            if($overlay.ContainsKey($path) -or $hash -notmatch '^[0-9a-fA-F]{64}$'){throw ('Invalid native payload identity: '+$path)}
            $null=Get-GameProfileTargetPath $context 'game' $path
            $entry=Get-CyberpunkTemplateSource (Join-Path $bundleRoot ('files/'+$hash.ToUpperInvariant())) '' $null $hash
            if(-not $entry){throw ('Missing or corrupt exact native payload: '+$path)}
            $entry.scope='game';$entry.path=$path;$overlay[$path]=$entry
        }
        foreach($mandatory in @('red4ext/red4ext.dll','red4ext/plugins/cyberpunkvr_stereo/cyberpunkvr_stereo.dll','red4ext/plugins/cyberpunkvr_stereo/usersettings.json')){
            if(-not $overlay.ContainsKey($mandatory)){throw ('Native manifest omits mandatory file: '+$mandatory)}
        }
        foreach($path in $overlay.Keys){$plan[$path]=$overlay[$path];$selected[$path]=$overlay[$path].sha256}
    }
    $launcher=Get-CyberpunkTemplateSource (Join-Path $bundleRoot 'Start-Game.ps1') '' $null ''
    if(-not $launcher){throw 'Missing bundle Start-Game.ps1 launcher.'}
    $launcher.scope='game';$launcher.path='CPVR-Portable/Start-Game.ps1';$null=Get-GameProfileTargetPath $context $launcher.scope $launcher.path
    $plan[$launcher.path]=$launcher
    $critical=@($selected.Keys|Where-Object {
        $_ -ieq 'archive/pc/mod/cyberpunkvrport.archive' -or
        $_ -match '^bin/x64/(dxgi\.dll|RealFG64\.dll|dbghelp\.dll|version\.dll|winmm\.dll|OptiScaler\.asi|nvngx_dlss\.dll|nvngx_dlssg\.dll|nvngx_dlssnr\.dll)$' -or
        $_ -match '^(red4ext/|bin/x64/(plugins|CPVR_HandsTracking|CheekyFoveatedDLSS)/|CPVR-Portable/NvidiaMenu/).+\.(dll|asi|reds|lua|dxil)$'
    }|Sort-Object|ForEach-Object {@{path=$_;sha256=$selected[$_]}})
    $receipt=@{
        version=[string]$version.id;label=[string]$version.label;kind=[string]$version.kind
        hands=$false;clean=$true;preference=[string]$version.id;opti=$false;fg=$false
        exeSha256=([string]$bundle.exeSha256).ToUpperInvariant();paths=@($selected.Keys|Sort-Object)
        critical=$critical;time=[datetime]::UtcNow.ToString('yyyyMMdd-HHmmss-fff')
    }
    $generated=[Collections.Generic.List[object]]::new()
    if($context.scopes.ContainsKey('launch-source')){
        if($context.scopes['launch-source'].kind -ne 'file'){throw 'Launch source must be a file scope.'}
        $generated.Add(@{scope='launch-source';path='.';text=(@{schema=1;kind='direct'}|ConvertTo-Json)})
    }
    $generated.Add(@{scope='game';path='CPVR-Portable/installed.json';text=($receipt|ConvertTo-Json -Depth 10)})
    if($VR){
        $authored=$plan['red4ext/plugins/cyberpunkvr_stereo/usersettings.json']
        # Use authored native defaults, never retained portable @settings.
        $player=@{scope='appdata';path='UserSettings.json';source=$authored.source;sha256=$authored.sha256;size=$authored.size}
        $plan['appdata/UserSettings.json']=$player
        $key=[Convert]::ToHexString([Security.Cryptography.SHA256]::HashData([Text.Encoding]::UTF8.GetBytes([string]$version.id)))
        $plan['center-settings/'+$key+'/custom.json']=@{scope='center-settings';path=($key+'/custom.json');source=$authored.source;sha256=$authored.sha256;size=$authored.size}
        $generated.Add(@{scope='center-settings';path=($key+'/mode.json');text=(@{version=[string]$version.id;recommended=$true}|ConvertTo-Json)})
    }
    $declared=@{}
    foreach($file in @($plan.Values)+@($generated.ToArray())){
        $null=Get-GameProfileTargetPath $context $file.scope $file.path
        $declared[$file.scope+'/'+$file.path]=$true
    }
    foreach($scope in $context.scopes.Values){foreach($path in $scope.requiredFiles){
        if(-not $declared.ContainsKey($scope.id+'/'+$path)){throw ('Clean template omits scope required file: '+$scope.id+'/'+$path)}
    }}
    return @{
        id=$(if($VR){'clean-vr'}else{'clean-flat'});vr=[bool]$VR;version=[string]$version.id
        privateStockCount=$private.Count;sharedStockCount=$shared.Count
        sourceFiles=@($plan.Values|Sort-Object scope,path);generatedFiles=@($generated.ToArray())
        scopes=@($context.scopes.Values|Sort-Object id|ForEach-Object {@{id=$_.id;kind=$_.kind}})
    }
}

function Ensure-CyberpunkProfileTemplate {
    param($Config,$StoreConfig,[switch]$VR)
    if(Get-Command Get-CyberpunkCapturedBaseline -ErrorAction SilentlyContinue){
        $captured=Get-CyberpunkCapturedBaseline $Config $StoreConfig
        if($captured -and -not $VR){return 'clean-flat'}
    }
    # Complete preflight first. No live files, profile catalog, or template are
    # modified when an original or exact native payload cannot be established.
    $plan=Get-CyberpunkProfileTemplatePlan -Config $Config -StoreConfig $StoreConfig -VR:$VR
    $context=Get-GameProfileContext $StoreConfig
    if($captured -and $VR){
        $baseline=Read-GameProfileJson (Get-GameProfileStorePath $context 'templates/clean-flat.json')
        # A VR add-on changes game files; first-menu user state still comes from
        # the exact clean capture. Port graphics presets remain an explicit action.
        $plan.sourceFiles=@($plan.sourceFiles|Where-Object scope -notin @('appdata','user-state','center-settings'))
        foreach($file in @($baseline.files|Where-Object scope -in @('appdata','user-state'))){
            $plan.sourceFiles+=@{scope=$file.scope;path=$file.path;sha256=$file.sha256;size=$file.size;source=(Get-GameProfileStorePath $context ('objects/'+$file.sha256))}
        }
        $plan.generatedFiles=@($plan.generatedFiles|Where-Object scope -ne 'center-settings')
    }
    $stage=Get-GameProfileStorePath $context ('staging/template-'+[guid]::NewGuid().ToString('N'))
    $files=[Collections.Generic.List[object]]::new()
    try{
        [IO.Directory]::CreateDirectory($stage)|Out-Null
        foreach($source in $plan.sourceFiles){
            $object=Import-GameProfileObject -StoreConfig $StoreConfig -SourcePath $source.source -ExpectedSha256 $source.sha256
            $files.Add(@{scope=$source.scope;path=$source.path;sha256=$object.sha256;size=[long]$object.size})
        }
        $index=0
        foreach($generated in $plan.generatedFiles){
            $source=Join-Path $stage (($index++).ToString()+'.json')
            [IO.File]::WriteAllText($source,$generated.text,[Text.UTF8Encoding]::new($false))
            $hash=Get-GameProfileFileHash $source
            $object=Import-GameProfileObject -StoreConfig $StoreConfig -SourcePath $source -ExpectedSha256 $hash
            $files.Add(@{scope=$generated.scope;path=$generated.path;sha256=$object.sha256;size=[long]$object.size})
        }
        $now=[datetime]::UtcNow.ToString('o')
        $manifest=@{schema=1;id=$plan.id;name=$plan.id;templateId=$plan.id;vr=$plan.vr;createdUtc=$now;savedUtc=$now;scopes=$plan.scopes;files=@($files.ToArray())}
        if($captured -and $VR){$manifest.directories=@($baseline.directories)}
        $null=Import-GameProfileTemplate -StoreConfig $StoreConfig -Id $plan.id -Manifest $manifest
        return [string]$plan.id
    }finally{
        if(Test-Path -LiteralPath $stage){
            $resolved=[IO.Path]::GetFullPath($stage)
            if(-not $resolved.StartsWith($context.store+'\staging\template-',[StringComparison]::OrdinalIgnoreCase)){throw 'Unsafe template staging cleanup path'}
            Assert-GameProfilePlainPath $resolved
            Remove-Item -LiteralPath $resolved -Recurse -Force
        }
    }
}
