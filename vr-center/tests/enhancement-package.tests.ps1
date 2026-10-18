$ErrorActionPreference='Stop'
$app=Split-Path $PSScriptRoot -Parent
. (Join-Path $app 'Components.ps1')
if(Test-Path (Join-Path $app 'EnhancementPackage.ps1')){. (Join-Path $app 'EnhancementPackage.ps1')}
function Put($p,$text){[IO.Directory]::CreateDirectory((Split-Path $p -Parent))|Out-Null;[IO.File]::WriteAllText($p,$text)}
function Check($value,$why){if(-not $value){throw $why};Write-Output ('PASS: '+$why)}
$f=Join-Path $app ('audit/image-package-'+[guid]::NewGuid().ToString('N'))
$c=@{game="$f/game";data="$f/data";hold="$f/hold.json";imagePackage="$f/package.json"}
# Process presence is external to this isolated file fixture; do not require the user to quit a real game.
function Get-Process { [CmdletBinding()]param([string[]]$Name); if(@($Name|Where-Object {$_ -in @('Cyberpunk2077','Vortex')}).Count){return};Microsoft.PowerShell.Management\Get-Process @PSBoundParameters }
Write-Json $c.hold @{active=$false}
Put "$f/game/bin/x64/dbghelp.dll" 'original'
Put "$f/payload/dbghelp.dll" 'package-proxy'
Put "$f/payload/sl.dll" 'complete-streamline'
Put "$f/payload/dlssg_sm86.ini" "[DLSSG]`nQuality=default"
Put "$f/payload/OptiScaler.ini" "[DlssNr]`nEnabled=false`nWorkingScale=1.0`n[FrameGen]`nEnabled=true`nFGInput=dlssg`nFGOutput=dlssg"
$entries=@(foreach($pair in @(@('dbghelp.dll','bin/x64/dbghelp.dll'),@('sl.dll','bin/x64/OptiScaler/streamline/sl.interposer.dll'),@('OptiScaler.ini','bin/x64/OptiScaler.ini'))){@{path=$pair[1];source="$f/payload/$($pair[0])";sha256=(Get-FileHash "$f/payload/$($pair[0])").Hash;mutable=($pair[0] -eq 'OptiScaler.ini')}})
$entries+=@{path='bin/x64/OptiScaler/dlssg_sm86/dlssg_sm86.ini';source="$f/payload/dlssg_sm86.ini";sha256=(Get-FileHash "$f/payload/dlssg_sm86.ini").Hash;mutable=$true}
Write-Json $c.imagePackage @{schema=1;id='flat-nr-mfg-test';label='Пакет NR + MFG';files=$entries;requiredFiles=@('bin/x64/dbghelp.dll','bin/x64/OptiScaler.ini','bin/x64/OptiScaler/streamline/sl.interposer.dll')}
Set-EnhancementPackage $c $true
Check ((Get-EnhancementPackageState $c).enabled -and [IO.File]::ReadAllText("$f/game/bin/x64/dbghelp.dll") -ceq 'package-proxy') 'one action installs the complete package and reports actual state'
Put "$f/game/bin/x64/OptiScaler.ini" "[DlssNr]`nEnabled=true`nWorkingScale=0.8`n[FrameGen]`nEnabled=false`nFGInput=dlssg`nFGOutput=dlssg"
Put "$f/game/bin/x64/OptiScaler/dlssg_sm86/dlssg_sm86.ini" "[DLSSG]`nQuality=user-choice"
Check ((Get-EnhancementPackageState $c).healthy) 'editing an auxiliary INI does not report damaged package binaries'
Reset-EnhancementStartup $c
$ini=[IO.File]::ReadAllText("$f/game/bin/x64/OptiScaler.ini")
Check ($ini -match '(?s)\[DlssNr\]\s+Enabled=true' -and $ini -match '(?s)\[FrameGen\]\s+Enabled=false' -and $ini.Contains('WorkingScale=0.8')) 'legacy pre-launch check preserves user NR and FG choices and image tuning'
Set-EnhancementPackage $c $false
Check (-not(Get-EnhancementPackageState $c).enabled -and [IO.File]::ReadAllText("$f/game/bin/x64/dbghelp.dll") -ceq 'original' -and -not(Test-Path "$f/game/bin/x64/OptiScaler/streamline/sl.interposer.dll")) 'off restores the replaced game file and removes only installed package files'
Set-EnhancementPackage $c $true
Check ([IO.File]::ReadAllText("$f/game/bin/x64/OptiScaler.ini").Contains('WorkingScale=0.8')) 'on restores the user preset instead of resetting every parameter'
Check ([IO.File]::ReadAllText("$f/game/bin/x64/OptiScaler/dlssg_sm86/dlssg_sm86.ini") -ceq "[DLSSG]`nQuality=user-choice") 'auxiliary settings survive off-on without being replaced by the main OptiScaler preset'
Set-EnhancementPackage $c $false
[IO.File]::Move($c.imagePackage,$c.imagePackage+'.saved')
Write-Json $c.imagePackage (Read-Json ($c.imagePackage+'.saved'))
Set-EnhancementPackage $c $true
[IO.File]::Delete("$f/game/bin/x64/OptiScaler/streamline/sl.interposer.dll")
$externalState=Get-EnhancementPackageState $c
Check ($externalState.healthy -and -not $externalState.reason) 'external file changes do not produce an unsolicited repair error'
Put "$f/game/bin/x64/dbghelp.dll" 'external-user-version'
Check ((Get-EnhancementPackageState $c).healthy) 'a user-installed loader is not compared against the old package hash'
Set-EnhancementPackage $c $false
Set-EnhancementPackage $c $true
Put "$f/game/bin/x64/dbghelp.dll" 'external-user-version'
[IO.File]::Delete($c.imagePackage)
Set-EnhancementPackage $c $false
Check ([IO.File]::ReadAllText("$f/game/bin/x64/dbghelp.dll") -ceq 'original' -and @(Get-ChildItem "$f/data/game-settings/enhancement/retained" -File).Count -eq 1) 'off works without the external recipe and retains changed user bytes before restoring'
[IO.File]::Move($c.imagePackage+'.saved',$c.imagePackage)
Set-EnhancementPackage $c $true
$statePath=(Get-EnhancementPaths $c).state;$interrupted=Read-Json $statePath;$interrupted.phase='prepared';$interrupted.enabled=$false;Write-Json $statePath $interrupted
$interruptedLaunch=$false;try{Reset-EnhancementStartup $c}catch{$interruptedLaunch=$true}
Check $interruptedLaunch 'interrupted enabling cannot launch a partially applied package'
Set-EnhancementPackage $c $false
Check ([IO.File]::ReadAllText("$f/game/bin/x64/dbghelp.dll") -ceq 'original' -and -not(Get-EnhancementPackageState $c).enabled) 'off repairs an interrupted operation using its saved originals'
[IO.File]::Delete("$f/payload/sl.dll")
$rejected=$false;try{Set-EnhancementPackage $c $true}catch{$rejected=$true}
Check ($rejected -and [IO.File]::ReadAllText("$f/game/bin/x64/dbghelp.dll") -ceq 'original') 'missing Streamline dependency rejects whole install before changing the game'
Check (@(Get-ChildItem "$f/data/transactions" -Filter '*.bin' -File -Recurse).Count -eq 0) 'completed toggles do not accumulate full binary copies in transaction backups'
Write-Output ('PASS: package evidence '+$f)
