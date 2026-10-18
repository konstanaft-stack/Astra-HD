param([string]$Release='3.1.2')
$ErrorActionPreference='Stop';$app=Split-Path $PSScriptRoot -Parent
$package=Join-Path $app ('releases/'+$Release)
$pwsh=(Get-Command pwsh -ErrorAction Stop).Source
$fixture=Join-Path $app ('audit/packaged-enhancement-'+[guid]::NewGuid().ToString('N'))
function Put($p,$text){[IO.Directory]::CreateDirectory((Split-Path $p -Parent))|Out-Null;[IO.File]::WriteAllText($p,$text)}
function Json($p,$v){Put $p ($v|ConvertTo-Json -Depth 20)}
function Check($v,$why){if(-not $v){throw $why};Write-Output ('PASS: '+$why)}
$c=@{game="$fixture/game";data="$fixture/data";bundle="$fixture/bundle";hold="$fixture/hold.json";appDataPath="$fixture/appdata";vortex="$fixture/no-vortex.exe";vortexData="$fixture/vortex";gameProfilesEnabled=$false;pwsh=$pwsh;imagePackage="$fixture/recipe.json"}
Put "$($c.game)/bin/x64/Cyberpunk2077.exe" 'fixture';Put "$($c.game)/bin/x64/dbghelp.dll" 'original'
Json "$($c.bundle)/bundle.json" @{schemaVersion=1;versions=@(@{id='flat';kind='flat';label='Плоская игра';available=$true;files=@{}});modules=@{};managedPaths=@()}
Json $c.hold @{active=$false};Json "$fixture/config.json" $c
Put "$fixture/payload/dbghelp.dll" 'package'
Put "$fixture/payload/OptiScaler.ini" "[Menu]`nShortcutKey=0x2D`n[DlssNr]`nEnabled=false`n[FrameGen]`nEnabled=true"
$files=@(foreach($name in @('dbghelp.dll','OptiScaler.ini')){@{path=('bin/x64/'+$name);source="$fixture/payload/$name";sha256=(Get-FileHash "$fixture/payload/$name").Hash;mutable=($name -like '*.ini')}})
Json $c.imagePackage @{schema=1;id='fixture-flat';files=$files;requiredFiles=@('bin/x64/dbghelp.dll','bin/x64/OptiScaler.ini')}
function Request($body){
 Json "$fixture/request.json" $body
 & $pwsh -NoProfile -File "$package/Backend.ps1" -Request "$fixture/request.json" -Result "$fixture/result.json" -ConfigPath "$fixture/config.json" *> "$fixture/backend.log"
 $answer=Get-Content -Raw -LiteralPath "$fixture/result.json"|ConvertFrom-Json -AsHashtable
 if(-not $answer.ok){throw $answer.error};return $answer
}
$answer=Request @{action='SetEnhancementPackage';enabled=$true}
Check ($answer.state.enhancementPackage.enabled -and $answer.state.enhancementPackage.healthy) 'packaged on endpoint returns connected healthy package to UI'
$answer=Request @{action='Status'}
Check $answer.state.enhancementPackage.enabled 'packaged state survives backend restart'
Json "$fixture/request.json" @{action='ApplyConfiguration';version='luke-test'}
& $pwsh -NoProfile -File "$package/Backend.ps1" -Request "$fixture/request.json" -Result "$fixture/blocked.json" -ConfigPath "$fixture/config.json" *> "$fixture/blocked.log"
$blocked=Get-Content -Raw -LiteralPath "$fixture/blocked.json"|ConvertFrom-Json -AsHashtable
Check (-not $blocked.ok -and $blocked.error -match 'отключи готовый пакет' -and [IO.File]::ReadAllText("$($c.game)/bin/x64/dbghelp.dll") -ceq 'package') 'VR apply is rejected before mutating the enabled flat package'
$answer=Request @{action='SetEnhancementPackage';enabled=$false}
Check (-not $answer.state.enhancementPackage.enabled -and [IO.File]::ReadAllText("$($c.game)/bin/x64/dbghelp.dll") -ceq 'original') 'packaged off endpoint restores the original and returns off state to UI'
Write-Output ('PASS: packaged enhancement evidence '+$fixture)
