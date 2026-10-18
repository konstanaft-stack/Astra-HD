$ErrorActionPreference='Stop'
$app=Split-Path $PSScriptRoot -Parent
. (Join-Path $app 'EnhancementPackage.ps1')
. (Join-Path $app 'GameLaunch.ps1')
function Put($p,$text){[IO.Directory]::CreateDirectory((Split-Path $p -Parent))|Out-Null;[IO.File]::WriteAllText($p,$text)}
function Check($v,$why){if(-not $v){throw $why};Write-Output ('PASS: '+$why)}
function Get-Process { [CmdletBinding()]param([string[]]$Name); if(@($Name|Where-Object {$_ -in @('Cyberpunk2077','Vortex')}).Count){return};Microsoft.PowerShell.Management\Get-Process @PSBoundParameters }
$f=Join-Path ([IO.Path]::GetTempPath()) ('enhancement-mode-'+[guid]::NewGuid().ToString('N'))
$c=@{game="$f/game";data="$f/data";hold="$f/hold.json";bundle="$f/bundle";imagePackage="$f/package.json"}
Write-Json $c.hold @{active=$false}
Write-Json "$f/bundle/bundle.json" @{versions=@(@{id='flat';kind='flat'},@{id='astra';kind='vr'},@{id='other-vr';kind='vr'})}
Put "$f/game/bin/x64/loader.dll" 'collection-loader'
Put "$f/game/bin/x64/Cyberpunk2077.exe" 'fixture-not-executable'
$flatReceipt=@{version='flat';kind='flat';exeSha256=(Get-FileHash "$f/game/bin/x64/Cyberpunk2077.exe").Hash;critical=@(@{path='bin/x64/loader.dll';sha256=(Get-FileHash "$f/game/bin/x64/loader.dll").Hash})}
Write-Json "$f/game/CPVR-Portable/installed.json" $flatReceipt
$originalReceipt=[IO.File]::ReadAllText("$f/game/CPVR-Portable/installed.json")
$launcherSource=Join-Path (Split-Path $app -Parent) 'tofu-sr-evidence-20261006/scripts/Start-Game.ps1'
[IO.File]::Copy($launcherSource,"$f/game/CPVR-Portable/Start-Game.ps1")
function CheckLaunchReceipt {
    Assert-GameLaunchComposition $c @{game=$c.game;exe="$f/game/bin/x64/Cyberpunk2077.exe";kind='direct'}
    $checkOutput=& (Join-Path $PSHOME 'pwsh.exe') -NoProfile -File "$f/game/CPVR-Portable/Start-Game.ps1" -CheckOnly 2>&1
    if($LASTEXITCODE -ne 0){throw ($checkOutput -join "`n")}
}
Put "$f/game/collection.txt" 'unchanged-collection'
Put "$f/payload/loader.dll" 'current-enhancement-loader'
Put "$f/payload/OptiScaler.ini" "[DlssNr]`nEnabled=false`n[FrameGen]`nEnabled=true"
Put "$f/payload/rtxmfg.json" '{"ratio":3}'
$files=@(foreach($name in @('loader.dll','OptiScaler.ini','rtxmfg.json')){@{path="bin/x64/$name";source="$f/payload/$name";sha256=(Get-FileHash "$f/payload/$name").Hash;mutable=($name -ne 'loader.dll')}})
Write-Json $c.imagePackage @{schema=1;id='current-opti-mfg';label='OptiScaler 10 NR 1.2.1 + MFG 1.5 hotfix';files=$files;requiredFiles=@('bin/x64/loader.dll')}
Set-EnhancementPackage $c $true
CheckLaunchReceipt
Check $true 'enabled flat package passes real composition and Start-Game CheckOnly critical hashes'
$userIni="[DlssNr]`nEnabled=true`nWorkingScale=0.73`n[FrameGen]`nEnabled=false"
$userJson='{"ratio":6,"preset":"C","custom":"keep"}'
Put "$f/game/bin/x64/OptiScaler.ini" $userIni
Put "$f/game/bin/x64/rtxmfg.json" $userJson
Set-EnhancementPackage $c $true
Check ([IO.File]::ReadAllText("$f/game/bin/x64/OptiScaler.ini") -ceq $userIni) 'enabling an already enabled package never resets user NR/FG choices'
$null=Suspend-EnhancementForPort $c 'flat'
Complete-EnhancementPortChange $c 'flat'
Check ((Get-EnhancementPackageState $c).enabled -and [IO.File]::ReadAllText("$f/game/bin/x64/OptiScaler.ini") -ceq $userIni) 'reapplying flat also retains the current package and user choices'
$tx=Suspend-EnhancementForPort $c 'astra'
Check ($tx.previousVersionId -eq 'flat') 'previousVersionId comes from receipt.version'
Check ([IO.File]::ReadAllText("$f/game/CPVR-Portable/installed.json") -ceq $originalReceipt) 'suspension restores the exact original flat receipt'
Check ($tx.stopped -and -not (Get-EnhancementPackageState $c).enabled) 'entering VR suspends the flat package'
Check ([IO.File]::ReadAllText("$f/game/bin/x64/loader.dll") -ceq 'collection-loader') 'suspension restores collection loader before port apply'
Write-Json "$f/game/CPVR-Portable/installed.json" @{version='astra';kind='vr'}
Complete-EnhancementPortChange $c 'astra'
Check (-not (Test-Path "$f/game/bin/x64/OptiScaler.ini")) 'unverified flat package is not installed into VR'
$null=Suspend-EnhancementForPort $c 'other-vr'
Write-Json "$f/game/CPVR-Portable/installed.json" @{version='other-vr';kind='vr'}
Complete-EnhancementPortChange $c 'other-vr'
Check (-not (Get-EnhancementPackageState $c).enabled) 'a second VR port keeps temporary suspension'
$null=Suspend-EnhancementForPort $c 'flat'
Write-Json "$f/game/CPVR-Portable/installed.json" $flatReceipt
Complete-EnhancementPortChange $c 'flat'
CheckLaunchReceipt
Check $true 'flat VR flat passes real composition and Start-Game CheckOnly without stale critical hashes'
Check ((Get-EnhancementPackageState $c).enabled -and [IO.File]::ReadAllText("$f/game/bin/x64/loader.dll") -ceq 'current-enhancement-loader') 'returning flat restores the current configured package'
Check ([IO.File]::ReadAllText("$f/game/bin/x64/OptiScaler.ini") -ceq $userIni -and [IO.File]::ReadAllText("$f/game/bin/x64/rtxmfg.json") -ceq $userJson) 'flat VR flat preserves exact user INI and custom MFG JSON'
Check ([IO.File]::ReadAllText("$f/game/collection.txt") -ceq 'unchanged-collection') 'collection files stay unchanged'
$tx=Suspend-EnhancementForPort $c 'astra'
Restore-EnhancementAfterFailedPortChange $c $tx
Check ((Get-EnhancementPackageState $c).enabled) 'failed apply with restored original files restores the previous enabled state'
$tx=Suspend-EnhancementForPort $c 'astra'
Put "$f/game/bin/x64/loader.dll" 'partially-applied-port'
$refused=$false;try{Restore-EnhancementAfterFailedPortChange $c $tx}catch{$refused=$true}
Check ($refused -and [IO.File]::ReadAllText("$f/game/bin/x64/loader.dll") -ceq 'partially-applied-port') 'failure recovery never overwrites an unresolved partial port apply'
Put "$f/game/bin/x64/loader.dll" 'collection-loader'
Restore-EnhancementAfterFailedPortChange $c $tx
Set-EnhancementPackage $c $false
Check ([IO.File]::ReadAllText("$f/game/CPVR-Portable/installed.json") -ceq $originalReceipt) 'explicit off restores original flat receipt bytes'
$null=Suspend-EnhancementForPort $c 'astra'
Write-Json "$f/game/CPVR-Portable/installed.json" @{version='astra';kind='vr'}
Complete-EnhancementPortChange $c 'astra'
$null=Suspend-EnhancementForPort $c 'flat'
Write-Json "$f/game/CPVR-Portable/installed.json" $flatReceipt
Complete-EnhancementPortChange $c 'flat'
Check (-not (Get-EnhancementPackageState $c).enabled) 'explicit user off remains off after VR and return'
Set-EnhancementPackage $c $true
$enabledReceipt=[IO.File]::ReadAllText("$f/game/CPVR-Portable/installed.json")
Write-Json "$f/game/CPVR-Portable/installed.json" @{version='foreign-port';kind='vr';critical=@()}
$foreignReceipt=[IO.File]::ReadAllText("$f/game/CPVR-Portable/installed.json")
$refused=$false;try{Set-EnhancementPackage $c $false}catch{$refused=$true}
Check ($refused -and [IO.File]::ReadAllText("$f/game/CPVR-Portable/installed.json") -ceq $foreignReceipt -and [IO.File]::ReadAllText("$f/game/bin/x64/loader.dll") -ceq 'current-enhancement-loader') 'off refuses a foreign/new port receipt before restoring any files'
Put "$f/game/CPVR-Portable/installed.json" $enabledReceipt
Set-EnhancementPackage $c $false
Set-EnhancementPackage $c $true
$legacy=Read-Json (Get-EnhancementPaths $c).state;$legacy.Remove('portReceipt')
Write-Json (Get-EnhancementPaths $c).state $legacy
Set-EnhancementPackage $c $true
CheckLaunchReceipt
Check ([IO.File]::ReadAllText("$f/game/bin/x64/OptiScaler.ini") -ceq $userIni) 'legacy enabled receipt migration preserves user settings and passes real launch checks'
Set-EnhancementPackage $c $false
CheckLaunchReceipt
Check ((Read-Json "$f/game/CPVR-Portable/installed.json").critical[0].sha256 -eq (Get-FileHash "$f/game/bin/x64/loader.dll").Hash) 'legacy off restores owned critical baseline without changing port identity'
Write-Output ('Fixture: '+$f)
