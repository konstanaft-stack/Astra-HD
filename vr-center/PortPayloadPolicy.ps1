# Derived installation metadata only. The source bundle and render code are never
# modified. A collection supplies shared frameworks and its own non-port mods.
function Test-CollectionPortOwnedPath {
    param([Parameter(Mandatory)][string]$RelativePath)
    if($RelativePath -eq '@settings'){return $false}
    $path=$RelativePath.Replace('\','/')
    if($path -match '(^/|:|[\x00-\x1f]|(^|/)\.{1,2}(/|$)|//|[?*])'){
        throw ('Invalid port payload relative path: '+$RelativePath)
    }

    # Whole known roots are retained together: native renderer/shaders, connected
    # Lua modules, REDscript, archive resources and the port's tweak records.
    if($path -match '^(red4ext/plugins/(cyberpunkvr_stereo|cyberpunkhandsport|cpvrown)/|bin/x64/plugins/cyber_engine_tweaks/mods/(cyberpunkvrport_|cpvr_hands)[^/]+/|r6/scripts/(cyberpunkvrport_|cpvr_hands)[^/]+/|r6/tweaks/(vrport|vrcigarette)/|bin/x64/cpvr_handstracking/)'){return $true}
    if($path -match '^archive/pc/mod/(cyberpunkvrport|vrcigarette|vrport_|cpvrown)[^/]*$'){return $true}
    if($path -match '^r6/input/(cyberpunkvrport_|cpvr_hands)[^/]+\.xml$' -or $path -eq 'engine/config/platform/pc/vrcam_cpu_tweaks.ini'){return $true}
    if($path -match '^bin/x64/(cyberpunkvr_[^/]+\.ini|vrport(?:-launcher)?\.ini)$'){return $true}
    if($path -match '^bin/x64/realrepo/'){return $true}

    # Explicit graphics components/loaders, not an arbitrary bin/x64 wildcard.
    # CET itself, RED4ext and shared mod frameworks are deliberately absent.
    if($path -match '^bin/x64/(dxgi\.dll|d3d11\.dll|d3d12\.dll|dinput8\.dll|dbghelp\.dll|openvr_api\.dll|realvr64\.dll|realvr\.ini|realfg64\.dll|realfg-control\.ini|optiscaler\.(asi|ini)|dxcompiler\.dll|dxil\.dll|d3dcompiler_47\.dll)$'){return $true}
    if($path -match '^bin/x64/(nvngx(?:_[a-z0-9_]+)?\.dll|sl\.[a-z0-9_]+\.dll|cudart64_[0-9]+\.dll|nvtoolsext64_1\.dll|amd_fidelityfx[^/]*\.dll|ffx_[^/]+\.dll|libxess[^/]*\.dll|libxell\.dll)$'){return $true}
    if($path -match '^(bin/x64/(OptiScaler|CheekyFoveatedDLSS)/|CPVR-Portable/(NvidiaMenu|OptiScaler-NR-[^/]+)/)'){return $true}
    if($path -match '^bin/x64/plugins/rtx40mfg\.asi$'){return $true}
    return $false
}

function Get-CollectionSafePortDefinition {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory)]$Bundle,
        [Parameter(Mandatory)][string]$VersionId,
        [string[]]$SharedPaths=@()
    )
    # JSON is the bundle's actual storage contract; deep-clone both hashtable and
    # PSCustomObject callers, including nested/no-hands/module maps.
    $copy=$Bundle|ConvertTo-Json -Depth 100 -Compress|ConvertFrom-Json -AsHashtable
    $selected=@($copy.versions|Where-Object { $_.id -ceq $VersionId })
    if($selected.Count -ne 1){throw ('Unknown or duplicate port version: '+$VersionId)}

    function Select-CollectionPortFiles($Files){
        $result=[ordered]@{}
        if($null -eq $Files){return ,$result}
        if($Files -isnot [Collections.IDictionary]){throw 'Port file manifest must be a path/hash map.'}
        foreach($path in $Files.Keys){
            if($path -in $SharedPaths){continue}
            if(Test-CollectionPortOwnedPath ([string]$path)){
                if([string]$Files[$path] -notmatch '^[a-fA-F0-9]{64}$'){throw ('Invalid port object SHA256: '+$path)}
                $result[[string]$path]=$Files[$path]
            }
        }
        return ,$result
    }

    foreach($version in $copy.versions){
        foreach($map in @($version.files,$version.noHandsFiles)){if($map){foreach($path in @($map.Keys)){if($path -in $SharedPaths){$map.Remove($path)}}}}
        if($version.kind -eq 'flat'){
            # Keep original stock definitions. The adapter must replace this
            # removed fallback with a hash of CURRENT effective UserSettings,
            # before invoking the legacy portable installer.
            if($version.files){$version.files.Remove('@settings')|Out-Null}
        }else{
            $oldCount=if($version.files){$version.files.Count}else{0}
            $version.files=Select-CollectionPortFiles $version.files
            if($version.Contains('noHandsFiles')){$version.noHandsFiles=Select-CollectionPortFiles $version.noHandsFiles}
            if($version.Contains('portPaths')){$version.portPaths=@($version.portPaths|Where-Object {$_ -notin $SharedPaths -and (Test-CollectionPortOwnedPath ([string]$_))})}
            if($version.id -ceq $VersionId){
                if(-not $version.files.Count){throw ('Selected port has no recognized port-owned payload: '+$VersionId)}
                $version.collectionSafePayload=@{schemaVersion=1;sourceVersionId=$VersionId;derivedPackagingOnly=$true;originalFiles=$oldCount;retainedFiles=$version.files.Count;sharedFrameworksFromCollection=$true;requiresCurrentSettingsSnapshot=$true}
            }
        }
    }
    if($copy.modules){foreach($name in @($copy.modules.Keys)){$copy.modules[$name]=Select-CollectionPortFiles $copy.modules[$name]}}
    $copy.managedPaths=@($copy.managedPaths|Where-Object {$_ -notin $SharedPaths -and (Test-CollectionPortOwnedPath ([string]$_))})
    $copy.collectionSafePayload=@{schemaVersion=1;selectedVersionId=$VersionId;derivedPackagingOnly=$true;requiresCurrentSettingsSnapshot=$true}
    return ,$copy
}

function New-CollectionSafePortPackage {
    param($Config,[string]$SourceBundle,[string]$VersionId,[string]$SettingsFile)
    if(-not(Test-Path -LiteralPath $SettingsFile -PathType Leaf)){throw 'Не найдены текущие настройки профиля игры; старый пресет не будет подставлен.'}
    $settings=[IO.File]::ReadAllBytes($SettingsFile)
    $null=[Text.Encoding]::UTF8.GetString($settings).TrimStart([char]0xFEFF)|ConvertFrom-Json -ErrorAction Stop
    $settingsHash=[Convert]::ToHexString([Security.Cryptography.SHA256]::HashData($settings))
    $bundle=Get-CollectionSafePortDefinition (Read-Json (Join-Path $SourceBundle 'bundle.json')) $VersionId -SharedPaths @(Get-SharedEnhancementPaths $Config)
    $flat=@($bundle.versions|Where-Object id -eq 'flat')[0]
    $flat.files['@settings']=$settingsHash
    $root=Join-Path (Split-Path ([IO.Path]::GetFullPath([string]$Config.bundle)) -Parent) 'VR-Center-Prepared'
    $package=Join-Path $root ([guid]::NewGuid().ToString('N'))
    $objects=Join-Path $package 'files'
    [IO.Directory]::CreateDirectory($objects)|Out-Null
    foreach($script in Get-ChildItem -LiteralPath $SourceBundle -File -Filter '*.ps1'){[IO.File]::Copy($script.FullName,(Join-Path $package $script.Name),$false)}
    [IO.File]::Copy((Join-Path $SourceBundle 'stock-paths.json'),(Join-Path $package 'stock-paths.json'),$false)
    # The old standalone installer changes game FG while switching renderers.
    # Center-managed settings are already chosen per mode; preserve their values.
    $installer=Join-Path $package 'Install.ps1';$text=[IO.File]::ReadAllText($installer)
    $needle="if(`$Kind -eq 'native'){return `$Text}"
    if(([regex]::Matches($text,[regex]::Escape($needle))).Count -ne 1){throw 'Изменился контракт установщика: сохранение настроек требует адаптации.'}
    $text=$text.Replace($needle,"if(`$SettingsFile -or `$Kind -eq 'native'){return `$Text}")
    [IO.File]::WriteAllText($installer,$text,[Text.UTF8Encoding]::new($false))
    # Only referenced objects are linked. Shared mod archives never enter this package.
    $hashes=[Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
    foreach($v in @($flat)+@($bundle.versions|Where-Object id -eq $VersionId)){
        foreach($map in @($v.files,$v.noHandsFiles)){if($map){foreach($hash in $map.Values){[void]$hashes.Add([string]$hash)}}}
    }
    foreach($map in $bundle.modules.Values){foreach($hash in $map.Values){[void]$hashes.Add([string]$hash)}}
    foreach($hash in $hashes){
        if($hash -eq $settingsHash){continue}
        $src=Join-Path $SourceBundle ('files/'+$hash)
        if(-not(Test-Path -LiteralPath $src -PathType Leaf)){throw ('Нет сохранённого объекта порта: '+$hash)}
        New-Item -ItemType HardLink -Path (Join-Path $objects $hash) -Target $src -ErrorAction Stop|Out-Null
    }
    [IO.File]::WriteAllBytes((Join-Path $objects $settingsHash),$settings)
    Write-Json (Join-Path $package 'bundle.json') $bundle
    Write-Json (Join-Path $package 'preparation.json') @{schema=1;sourceBundle=$SourceBundle;version=$VersionId;settingsFile=$SettingsFile;settingsHash=$settingsHash;createdUtc=[datetime]::UtcNow.ToString('o')}
    return $package
}
