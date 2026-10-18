param([string]$AppRoot=(Split-Path $PSScriptRoot -Parent))
$ErrorActionPreference='Stop'
$fixture=Join-Path ([IO.Path]::GetTempPath()) ('vr-installer-config-'+[guid]::NewGuid().ToString('N'))
[IO.Directory]::CreateDirectory($fixture)|Out-Null
$pwsh=(Get-Process -Id $PID).Path
$tokens=$null;$errors=$null
$ast=[Management.Automation.Language.Parser]::ParseFile((Join-Path $AppRoot 'Backend.ps1'),[ref]$tokens,[ref]$errors)
if($errors.Count){throw 'Backend parse failed'}
$commands=@($ast.FindAll({param($node) $node -is [Management.Automation.Language.CommandAst] -and $node.Extent.Text -match 'Install-Selected\.ps1'},$true)|ForEach-Object {$_.Parent.Extent.Text})
if($commands.Count -ne 5){throw ('Unexpected installer call count: '+$commands.Count)}
$external=Join-Path $fixture 'external/config.json'
[IO.Directory]::CreateDirectory((Split-Path $external -Parent))|Out-Null
[IO.File]::WriteAllText($external,'{"marker":"external-owner","bundle":"separate-bundle"}')
[IO.File]::WriteAllText((Join-Path $fixture 'request.json'),'{}')
[IO.File]::WriteAllText((Join-Path $fixture 'Install-Selected.ps1'),@'
param([string]$Request,[switch]$CheckOnly,[string]$BundlePath,[string]$ConfigPath)
$ErrorActionPreference='Stop'
if(-not $ConfigPath -or -not(Test-Path -LiteralPath $ConfigPath)){Write-Output 'External configuration path was lost at child-process boundary';exit 9}
$c=Get-Content -LiteralPath $ConfigPath -Raw|ConvertFrom-Json
if($c.marker -cne 'external-owner'){Write-Output 'Wrong configuration owner';exit 10}
@{config=$ConfigPath;checkOnly=[bool]$CheckOnly;bundleOverride=$BundlePath}|ConvertTo-Json -Compress|Add-Content -LiteralPath (Join-Path $PSScriptRoot 'seen.jsonl')
exit 0
'@)
$harness=@'
param($ShellPath,$ExternalConfig)
$config=@{pwsh=$ShellPath};$ConfigPath=$ExternalConfig
$Request=Join-Path $PSScriptRoot 'request.json';$installRequest=$Request;$applyRequest=$Request
$package='selected-bundle-override';$log=Join-Path $PSScriptRoot 'install.log'
'@
$index=0
foreach($command in $commands){
    $harness+="`n"+$command+"`n"+'if($LASTEXITCODE -ne 0){throw (''Installer call '+$index+' lost external config: ''+[IO.File]::ReadAllText($log))}'
    $index++
}
[IO.File]::WriteAllText((Join-Path $fixture 'run.ps1'),$harness)
try{
    & $pwsh -NoProfile -File (Join-Path $fixture 'run.ps1') -ShellPath $pwsh -ExternalConfig $external *> (Join-Path $fixture 'run.log')
    if($LASTEXITCODE -ne 0){throw ([IO.File]::ReadAllText((Join-Path $fixture 'run.log')))}
    $seen=@(Get-Content -LiteralPath (Join-Path $fixture 'seen.jsonl')|ForEach-Object {$_|ConvertFrom-Json})
    if($seen.Count -ne 5 -or @($seen|Where-Object {$_.config -cne $external}).Count){throw 'Installer invocations did not retain external config'}
    if(@($seen|Where-Object checkOnly).Count -ne 3){throw 'Preflight/apply call modes changed'}
    if($seen[0].bundleOverride -cne 'selected-bundle-override' -or $seen[1].bundleOverride -cne 'selected-bundle-override'){throw 'Explicit bundle override changed'}
    'PASS: all five real Backend installer command lines forward the external config, including preflight/apply and bundle overrides. No game files accessed.'
}finally{
    $resolved=[IO.Path]::GetFullPath($fixture)
    $temp=[IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd('\')+'\'
    if(-not $resolved.StartsWith($temp,[StringComparison]::OrdinalIgnoreCase) -or [IO.Path]::GetFileName($resolved) -notmatch '^vr-installer-config-[a-f0-9]{32}$'){throw 'Unexpected fixture cleanup scope'}
    Remove-Item -LiteralPath $resolved -Recurse -Force
}
