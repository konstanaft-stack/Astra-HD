param([string]$Request,[switch]$CheckOnly,[string]$BundlePath,[string]$ConfigPath)
$ErrorActionPreference='Stop'
. "$PSScriptRoot/Core.ps1"
. "$PSScriptRoot/GameLaunch.ps1"
. "$PSScriptRoot/PortPayloadPolicy.ps1"
try{
    $c=Read-Json $(if($ConfigPath){$ConfigPath}else{Join-Path $PSScriptRoot 'config.json'});$r=Read-Json $Request
    $package=if($BundlePath){$BundlePath}else{$c.bundle}
    $b=Read-Json (Join-Path $package 'bundle.json');$v=$b.versions|Where-Object id -eq $r.version|Select-Object -First 1
    $old=$c;$target=$c;$settingsArguments=@{}
    if(Test-CyberpunkProfileMode $c){
        $old=Get-GameLaunchEffectiveConfig $c
        $target=Get-GameLaunchInstallConfig $c ([string]$v.kind) ([bool]$r.opti)
        $settingsArguments=@{SettingsFile=$target.settingsFile;PreviousSettingsFile=$old.settingsFile}
    }
    if((Get-GameLaunchSource $c).kind -in @('mo2','vortex') -or (Get-SharedEnhancementDefinition $c)){
        $settings=if(Test-Path -LiteralPath $target.settingsFile -PathType Leaf){$target.settingsFile}else{$old.settingsFile}
        $package=New-CollectionSafePortPackage $c $package ([string]$r.version) $settings
    }
    if(-not $CheckOnly){Assert-ApplyAllowed (Get-PortState $old) $v ([bool]$r.opti) ([bool]$r.fg)}
    & (Join-Path $package 'Install.ps1') -Version ([string]$r.version) -OptiScaler ([bool]$r.opti) -FrameGeneration ([bool]$r.fg) -Hands ([bool]$r.hands) -CleanProfile ([bool]$r.clean) -GamePath $target.game -CheckOnly:$CheckOnly @settingsArguments
    exit $LASTEXITCODE
}catch{Write-Error $_;exit 1}
