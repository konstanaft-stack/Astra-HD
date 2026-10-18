param([Parameter(Mandatory)][string]$BundleRoot)
$ErrorActionPreference='Stop'
$app=Split-Path $PSScriptRoot -Parent
. (Join-Path $app 'Core.ps1')
. (Join-Path $app 'PortPayloadPolicy.ps1')
function Check($ok,[string]$why){if(-not $ok){throw $why}}
function Put([string]$path,[string]$text){[IO.Directory]::CreateDirectory((Split-Path $path -Parent))|Out-Null;[IO.File]::WriteAllText($path,$text,[Text.UTF8Encoding]::new($false))}
function Hash([string]$path){return (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash}
$fixtureParent=[IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd('\')
$fixture=Join-Path $fixtureParent ('fixture-collection-port-'+[guid]::NewGuid().ToString('N'))
$fixture=[IO.Path]::GetFullPath($fixture)
$source=Join-Path $fixture 'source'
$game=Join-Path $fixture 'game'
$settings=Join-Path $fixture 'profile/UserSettings.json'
$legacy=$BundleRoot
$pwsh=(Get-Process -Id $PID).Path
$complete=$false
try{
    [IO.Directory]::CreateDirectory((Join-Path $source 'files'))|Out-Null
    # Unmodified installer/helper copies; no real game, DLL or manager is run.
    foreach($name in @('Install.ps1','Clean-Profile.ps1','ContentPackages.ps1','Start-Game.ps1','Profile-Settings.ps1','Isolated-Profile.ps1')){
        [IO.File]::Copy((Join-Path $legacy $name),(Join-Path $source $name),$false)
    }
    Put (Join-Path $source 'stock-paths.json') '{}'
    function Add-Object([string]$text){
        $bytes=[Text.Encoding]::UTF8.GetBytes($text)
        $hash=[Convert]::ToHexString([Security.Cryptography.SHA256]::HashData($bytes))
        [IO.File]::WriteAllBytes((Join-Path $source ('files/'+$hash)),$bytes)
        return $hash
    }
    $native='red4ext/plugins/cyberpunkvr_stereo/cyberpunkvr_stereo.dll'
    $nativeScript='bin/x64/plugins/cyber_engine_tweaks/mods/cyberpunkvrport_stereo/init.lua'
    $archiveXL='red4ext/plugins/archivexl/archivexl.dll'
    $common=@{
        $archiveXL='collection-ArchiveXL-1.27.4'
        'red4ext/plugins/codeware/codeware.dll'='collection-Codeware'
        'bin/x64/plugins/cyber_engine_tweaks.asi'='collection-CET'
        'bin/x64/plugins/cyber_engine_tweaks/bindings.json'='{"collectionBinding":"F6"}'
        'bin/x64/version.dll'='collection-CET-loader'
        'bin/x64/winmm.dll'='collection-RED4ext-loader'
        'bin/x64/global.ini'='collection-loader-config'
        'r6/cache/inputcontexts.xml'='<collection-input-cache/>'
        'r6/scripts/equipmentex/equipmentex.reds'='collection-EquipmentEX'
        'archive/pc/mod/collection-apartments.archive'='collection-apartments'
    }
    $oldSnapshot=@{}
    foreach($path in $common.Keys){
        Put (Join-Path $game $path) $common[$path]
        $oldSnapshot[$path]=Add-Object ('old snapshot replacement for '+$path)
    }
    Put (Join-Path $game 'bin/x64/Cyberpunk2077.exe') 'fixture bytes only; never executed'
    Put (Join-Path $game 'bin/x64/steam_api64.dll') 'same stock API'
    $oldSettings=Add-Object '{"options":[{"name":"DLSS","value":"Off"},{"name":"FrameGeneration","value":"Off","index":0},{"name":"DLSSFrameGen","value":false}]}'
    $nativeFiles=@{};foreach($path in $oldSnapshot.Keys){$nativeFiles[$path]=$oldSnapshot[$path]}
    $nativeFiles[$native]=Add-Object 'exact saved native renderer bytes'
    $nativeFiles[$nativeScript]=Add-Object 'return {} -- connected port script'
    $nativeFiles['bin/x64/vrport.ini']=Add-Object "xr_framegen=1`nxr_native_generated_frames=1`n"
    $nativeFiles['@settings']=$oldSettings
    $lukeFiles=@{
        'bin/x64/dxgi.dll'=(Add-Object 'exact other renderer bytes')
        'bin/x64/RealVR.ini'=(Add-Object "[RVR]`nExpertMode=0`n")
        'bin/x64/realrepo/cp2077/s0.dxbc'=(Add-Object 'connected Luke shader')
        $archiveXL=$oldSnapshot[$archiveXL]
        '@settings'=$oldSettings
    }
    $bundle=@{
        schemaVersion=1;exeSha256=(Hash (Join-Path $game 'bin/x64/Cyberpunk2077.exe'))
        versions=@(
            @{id='flat';label='Flat fixture';kind='flat';available=$true;allowOpti=$true;allowFG=$true;files=@{'bin/x64/steam_api64.dll'=(Add-Object 'same stock API');'@settings'=$oldSettings}},
            @{id='native-fixture';label='Exact native fixture';kind='native';available=$true;allowOpti=$false;allowFG=$false;handsAvailable=$false;files=$nativeFiles},
            @{id='other-fixture';label='Other renderer fixture';kind='luke';available=$true;allowOpti=$false;allowFG=$false;handsAvailable=$false;files=$lukeFiles}
        )
        modules=@{}
        managedPaths=@(@($nativeFiles.Keys)+@($lukeFiles.Keys)|Sort-Object -Unique)
    }
    Write-Json (Join-Path $source 'bundle.json') $bundle
    $originalCatalogHash=Hash (Join-Path $source 'bundle.json')
    $originalInstallerHash=Hash (Join-Path $source 'Install.ps1')
    $commonHashes=@{};foreach($path in $common.Keys){$commonHashes[$path]=Hash (Join-Path $game $path)}
    $config=@{bundle=$source}
    $driver=Join-Path $fixture 'invoke-installer.ps1'
    Put $driver @'
param([string]$Fixture,[string]$Installer,[string]$Game,[string]$Settings,[string]$Version,[switch]$CheckOnly)
$ErrorActionPreference='Stop'
$root=[IO.Path]::GetFullPath($Fixture).TrimEnd('\')+'\'
foreach($path in @($Installer,$Game,$Settings)){
    if(-not([IO.Path]::GetFullPath($path).StartsWith($root,[StringComparison]::OrdinalIgnoreCase))){throw 'Test driver refuses a path outside the disposable fixture.'}
}
# A live Vortex must not make a disposable fixture test depend on desktop state.
# This override exists in this child only; it does not close any real process.
function global:Get-Process { [CmdletBinding()]param([string[]]$Name);return @() }
& $Installer -Version $Version -GamePath $Game -SettingsFile $Settings -PreviousSettingsFile $Settings -CleanProfile $false -Hands $false -OptiScaler $false -FrameGeneration $false -CheckOnly:$CheckOnly
exit $LASTEXITCODE
'@
    $steps=0
    foreach($fg in @($true,$false)){
        $fgText=if($fg){'DLSS'}else{'Off'}
        Put $settings (@{options=@(@{name='DLSS';value='Balanced'},@{name='FrameGeneration';value=$fgText;index=$(if($fg){1}else{0})},@{name='DLSSFrameGen';value=$fg});collection='must survive exactly'}|ConvertTo-Json -Depth 6)
        $settingsHash=Hash $settings
        foreach($version in @('flat','native-fixture','other-fixture','flat')){
            $package=New-CollectionSafePortPackage -Config $config -SourceBundle $source -VersionId $version -SettingsFile $settings
            $package=[IO.Path]::GetFullPath([string]$package)
            Check ($package.StartsWith($fixture+'\',[StringComparison]::OrdinalIgnoreCase)) 'prepared package must remain in fixture'
            $prepared=Read-Json (Join-Path $package 'bundle.json')
            $flat=@($prepared.versions|Where-Object id -eq 'flat')[0]
            Check ($flat.files['@settings'] -eq $settingsHash) 'fallback settings object must contain current profile, not old preset'
            Check (-not $prepared.managedPaths.Contains($archiveXL)) 'ArchiveXL cannot be managed by port package'
            $installer=Join-Path $package 'Install.ps1'
            $output=@(& $pwsh -NoProfile -File $driver -Fixture $fixture -Installer $installer -Game $game -Settings $settings -Version $version -CheckOnly 2>&1)
            Check ($LASTEXITCODE -eq 0) ('CheckOnly failed for '+$version+': '+($output -join "`n"))
            Check ((Hash $settings) -eq $settingsHash) 'CheckOnly changed settings'
            $output=@(& $pwsh -NoProfile -File $driver -Fixture $fixture -Installer $installer -Game $game -Settings $settings -Version $version 2>&1)
            Check ($LASTEXITCODE -eq 0) ('Apply failed for '+$version+': '+($output -join "`n"))
            Check ((Hash $settings) -eq $settingsHash) ('UserSettings bytes changed on '+$version+'; Balanced/FG choice must survive')
            foreach($path in $common.Keys){Check ((Hash (Join-Path $game $path)) -eq $commonHashes[$path]) ('collection file changed: '+$path+' on '+$version)}
            $receipt=Read-Json (Join-Path $game 'CPVR-Portable/installed.json')
            Check ($receipt.version -eq $version) 'receipt must describe selected renderer'
            Check (-not @($receipt.critical|Where-Object path -eq $archiveXL).Count) 'receipt must not give old ArchiveXL selectedFiles priority'
            if($version -eq 'native-fixture'){
                Check ((Hash (Join-Path $game $native)) -eq $nativeFiles[$native]) 'native renderer bytes changed'
                Check ((Hash (Join-Path $game $nativeScript)) -eq $nativeFiles[$nativeScript]) 'connected native script missing'
                Check (-not(Test-Path -LiteralPath (Join-Path $game 'bin/x64/dxgi.dll'))) 'old Luke renderer survived native switch'
            }elseif($version -eq 'other-fixture'){
                Check ((Hash (Join-Path $game 'bin/x64/dxgi.dll')) -eq $lukeFiles['bin/x64/dxgi.dll']) 'other renderer bytes changed'
                Check (-not(Test-Path -LiteralPath (Join-Path $game $native))) 'old native renderer survived other-port switch'
                Check (-not(Test-Path -LiteralPath (Join-Path $game $nativeScript))) 'old native script survived other-port switch'
            }else{
                Check (-not(Test-Path -LiteralPath (Join-Path $game $native))) 'flat retained native renderer'
                Check (-not(Test-Path -LiteralPath (Join-Path $game 'bin/x64/dxgi.dll'))) 'flat retained Luke renderer'
            }
            $steps++
        }
    }
    Check ((Hash (Join-Path $source 'bundle.json')) -eq $originalCatalogHash) 'source catalog mutated'
    Check ((Hash (Join-Path $source 'Install.ps1')) -eq $originalInstallerHash) 'source installer mutated'
    $complete=$true
    Write-Output ('PASS: '+$steps+' CheckOnly+Apply transitions through real copied legacy installer; flat/native/other/flat; collection ArchiveXL/loaders/settings preserved for FG On and Off; no live writes')
}finally{
    if($complete -and (Test-Path -LiteralPath $fixture)){
        $expected=$fixtureParent+'\'
        if(-not $fixture.StartsWith($expected,[StringComparison]::OrdinalIgnoreCase) -or [IO.Path]::GetFileName($fixture) -notlike 'fixture-collection-port-*'){throw 'Refusing cleanup outside owned test fixture'}
        Remove-Item -LiteralPath $fixture -Recurse -Force
    }elseif(Test-Path -LiteralPath $fixture){Write-Output ('FAILED fixture preserved: '+$fixture)}
}
