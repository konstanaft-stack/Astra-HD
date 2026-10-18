$ErrorActionPreference='Stop'
. "$PSScriptRoot/../Core.ps1"
. "$PSScriptRoot/../CyberpunkCollection.ps1"
function Check($value,$message){if(-not $value){throw $message}}
function Get-GameLaunchCollectionConfig($Config,$Plan){return $Config}
function Get-GameLaunchModRoots($Config,$Plan){return @($Config.mo2.overwriteRoot,$Config.high,$Config.low)}
function Get-GameLaunchPlainPath($Path){return [IO.Path]::GetFullPath($Path)}
function Put($Path,$Text){[IO.Directory]::CreateDirectory((Split-Path $Path -Parent))|Out-Null;[IO.File]::WriteAllText($Path,$Text)}
$fixture=Join-Path ([IO.Path]::GetTempPath()) ('cet-view-'+[guid]::NewGuid().ToString('N'))
$prefix='bin/x64/plugins/cyber_engine_tweaks/mods'
$config=@{data="$fixture/data";game="$fixture/game";high="$fixture/high";low="$fixture/low";mo2=@{overwriteRoot="$fixture/overwrite"}}
$plan=@{game=$config.game;kind='mo2'}
try{
    Put "$($config.low)/$prefix/Test/init.lua" 'base-init'
    Put "$($config.low)/$prefix/Test/modules/child.lua" 'base-child'
    Put "$($config.high)/$prefix/Test/modules/child.lua" 'high-child'
    Put "$($config.low)/$prefix/Test/config.json" 'default-config'
    Put "$($config.mo2.overwriteRoot)/$prefix/Test/config.json" 'user-config'
    Put "$($config.mo2.overwriteRoot)/$prefix/Test/db.sqlite3" 'user-database'
    Put "$($config.low)/$prefix/Test/ignored.lua.mohidden" 'disabled'
    Put "$($config.low)/$prefix/Disabled/init.lua.mohidden" 'disabled-init'
    Put "$($config.low)/$prefix/Hidden.mohidden/init.lua" 'hidden-directory'
    Put "$($config.low)/$prefix/Library/helper.lua" 'library-only'
    $first=Prepare-CetRuntimeView $config $plan
    $out="$($config.mo2.overwriteRoot)/$prefix"
    Check ([IO.File]::ReadAllText("$out/Test/init.lua") -ceq 'base-init') 'relative entrypoint materialized'
    Check ([IO.File]::ReadAllText("$out/Test/modules/child.lua") -ceq 'high-child') 'highest source wins'
    Check ([IO.File]::ReadAllText("$out/Test/config.json") -ceq 'user-config') 'existing user configuration preserved'
    Check ([IO.File]::ReadAllText("$out/Test/db.sqlite3") -ceq 'user-database') 'runtime database preserved'
    Check (-not(Test-Path "$out/Test/ignored.lua.mohidden") -and -not(Test-Path "$out/Disabled")) 'hidden payload does not activate a mod'
    Check (-not(Test-Path "$out/Library")) 'resource-only directory is not promoted to a mod'
    Check (-not(Test-Path "$out/Hidden.mohidden")) 'hidden directories stay disabled'
    $hash=(Get-FileHash "$out/Test/config.json").Hash
    $again=Prepare-CetRuntimeView $config $plan
    Check ($again.copied -eq 0 -and $again.updated -eq 0) 'unchanged preparation does not rewrite payload'
    Put "$($config.high)/$prefix/Test/modules/child.lua" 'high-child-v2'
    $updated=Prepare-CetRuntimeView $config $plan
    Check ([IO.File]::ReadAllText("$out/Test/modules/child.lua") -ceq 'high-child-v2') 'owned payload follows source updates'
    Check ((Get-FileHash "$out/Test/config.json").Hash -ceq $hash) 'source update does not reset settings'
    Put "$out/Test/modules/child.lua" 'user-script-edit'
    Put "$($config.high)/$prefix/Test/modules/child.lua" 'high-child-v3'
    $custom=Prepare-CetRuntimeView $config $plan
    Check ([IO.File]::ReadAllText("$out/Test/modules/child.lua") -ceq 'user-script-edit') 'custom overwrite wins over generated copy'
    $config.high="$fixture/absent-high";$config.low="$fixture/absent-low"
    $removed=Prepare-CetRuntimeView $config $plan
    Check (-not(Test-Path "$out/Test/init.lua")) 'disabled source removes unchanged managed init'
    Check ([IO.File]::ReadAllText("$out/Test/config.json") -ceq 'user-config') 'disable preserves user configuration'
    Check ([IO.File]::ReadAllText("$out/Test/modules/child.lua") -ceq 'user-script-edit') 'disable preserves foreign changed file'
    Put "$($config.game)/$prefix/Port/init.lua" 'physical-port'
    Put "$fixture/selected/init.lua" 'selected-port'
    $plan.selectedFiles=@{"$prefix/Port/init.lua"="$fixture/selected/init.lua"}
    $selected=Prepare-CetRuntimeView $config $plan
    Check ([IO.File]::ReadAllText("$out/Port/init.lua") -ceq 'selected-port') 'explicit port mapping wins over base-game payload'
    'PASS: complete relative CET module view, source priority, hidden filtering, persistent user settings, owned refresh and disable cleanup.'
}finally{
    $full=[IO.Path]::GetFullPath($fixture);$temp=[IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd('\')+'\'
    if(-not $full.StartsWith($temp,[StringComparison]::OrdinalIgnoreCase) -or [IO.Path]::GetFileName($full) -notmatch '^cet-view-[a-f0-9]{32}$'){throw 'Unexpected fixture path'}
    if(Test-Path -LiteralPath $full){Remove-Item -LiteralPath $full -Recurse -Force}
}
