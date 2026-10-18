param([string]$Runtime=(Join-Path (Split-Path $PSScriptRoot -Parent) 'components/usvfs'),[string]$Dotnet=(Get-Command dotnet -ErrorAction Stop).Source)
$ErrorActionPreference='Stop'
$app=Split-Path $PSScriptRoot -Parent
. (Join-Path $app 'GameLaunch.ps1')
# Dotnet comes from the explicit parameter or PATH.
$hostDll=Join-Path $app 'helpers\VfsHost\bin\Release\net8.0\VRPorts.VfsHost.dll'
if(-not(Test-Path -LiteralPath $hostDll)){throw 'Standalone VFS host not built'}
$fixture=Join-Path $app ('audit\vfs-fixture-'+[guid]::NewGuid().ToString('N'))
function Put([string]$Path,[string]$Value){[IO.Directory]::CreateDirectory((Split-Path $Path -Parent))|Out-Null;[IO.File]::WriteAllText($Path,$Value)}
function Assert($Value,[string]$Why){if(-not $Value){throw $Why};Write-Output ('PASS: '+$Why)}
$game=Join-Path $fixture 'game';$low=Join-Path $fixture 'low';$high=Join-Path $fixture 'high';$overwrite=Join-Path $fixture 'overwrite'
Put "$game/conflict.txt" 'base';Put "$game/stock.txt" 'stock'
Put "$low/conflict.txt" 'lower';Put "$low/low-only.txt" 'low'
Put "$high/conflict.txt" 'winner';Put "$high/hidden.txt.mohidden" 'hidden'
Put "$overwrite/existing.txt" 'previous-overwrite'
$report=Join-Path $fixture 'consumer.json'
$consumer=Join-Path $PSScriptRoot 'VfsConsumer/bin/Release/net8.0/VfsConsumer.dll'
if(-not(Test-Path -LiteralPath $consumer)){throw 'Native consumer must be built before this integration test'}
$plan=@{runtime=$Runtime;exe=$dotnet;arguments=@($consumer,$game,$report);workingDirectory=$game;directories=@(@{source=$low;destination=$game;createTarget=$false},@{source=$high;destination=$game;createTarget=$false},@{source=$overwrite;destination=$game;createTarget=$true});files=@();timeoutSeconds=30;logPath="$fixture/usvfs.log"}
# Use real modlist parsing and center launch planning, with no ModOrganizer executable.
Put "$fixture/profiles/Collection/modlist.txt" "+high`n-low-disabled`n+Presentation_separator`n+low"
Put "$fixture/low-disabled/conflict.txt" 'disabled'
Put "$game/bin/x64/Cyberpunk2077.exe" 'unused game placeholder'
Put "$fixture/data/launch-source.json" '{"schema":1,"kind":"mo2","profile":"Collection","executable":"Cyberpunk2077"}'
$config=@{game=$game;data="$fixture/data";vfs=@{runtime=$Runtime;host=$hostDll;dotnet=$dotnet};mo2=@{root=$fixture;game=$game;modsRoot=$fixture;overwriteRoot=$overwrite;executables=@{Cyberpunk2077='bin/x64/Cyberpunk2077.exe'}}}
$gamePlan=Get-GameLaunchPlan $config -InspectOnly
$plan=New-StandaloneVfsLaunchPlan $config $gamePlan "$fixture/usvfs.log"
$plan.exe=$dotnet;$plan.arguments=@($consumer,$game,$report);$plan.workingDirectory=$game;$plan.timeoutSeconds=30
Put "$fixture/plan.json" ($plan|ConvertTo-Json -Depth 6)
$processesBefore=@(Get-Process ModOrganizer,ModOrganizer2 -ErrorAction SilentlyContinue | ForEach-Object Id)
& $dotnet $hostDll "$fixture/plan.json" *> "$fixture/host.log"
$code=$LASTEXITCODE
Get-Content -LiteralPath "$fixture/host.log"
if(Test-Path -LiteralPath $report){Get-Content -LiteralPath $report}
Assert ($code -eq 0) ('hooked consumer exits successfully; evidence '+$fixture)
$actual=Get-Content -LiteralPath $report -Raw|ConvertFrom-Json
Assert ($actual.conflict -ceq 'winner' -and $actual.stock -ceq 'stock' -and $actual.low -ceq 'low' -and $actual.overwrite -ceq 'previous-overwrite') 'native consumer sees priority winners, base files and overwrite'
Assert (-not $actual.hidden) 'mohidden file is not exposed through virtual directory'
Assert ([IO.File]::ReadAllText("$overwrite/generated/new.txt") -ceq 'new-profile-data' -and -not(Test-Path "$game/generated/new.txt")) 'new files are redirected to the selected overwrite'
Assert ([IO.File]::ReadAllText("$game/conflict.txt") -ceq 'base' -and [IO.File]::ReadAllText("$high/conflict.txt") -ceq 'winner') 'original game and mod files remain unchanged'
$processesAfter=@(Get-Process ModOrganizer,ModOrganizer2 -ErrorAction SilentlyContinue | ForEach-Object Id)
Assert (($processesBefore -join ',') -ceq ($processesAfter -join ',')) 'no ModOrganizer process was launched'
$plan.files=@(@{source="$game/conflict.txt";destination="$game/conflict.txt";createTarget=$false})
Put "$fixture/selected-plan.json" ($plan|ConvertTo-Json -Depth 6)
& $dotnet $hostDll "$fixture/selected-plan.json" *> "$fixture/selected-host.log"
Assert ($LASTEXITCODE -eq 0 -and (Get-Content -LiteralPath $report -Raw|ConvertFrom-Json).conflict -ceq 'base') 'selected physical file overrides a virtual mod mapping without copying or replacing sources'
Put "$game/bin/x64/plugins/mods/vr/reload/poses/native.lua" 'native-vr-pose'
Put "$low/bin/x64/plugins/mods/settings-ui/init.lua" 'other mod sharing the ancestor'
$gamePlan.selectedFiles=@{'bin/x64/plugins/mods/vr/reload/poses/native.lua'="$game/bin/x64/plugins/mods/vr/reload/poses/native.lua"}
$plan=New-StandaloneVfsLaunchPlan $config $gamePlan "$fixture/nested-usvfs.log"
$plan.exe=$dotnet;$plan.arguments=@($consumer,$game,$report);$plan.workingDirectory=$game;$plan.timeoutSeconds=30
Put "$fixture/nested-selected-plan.json" ($plan|ConvertTo-Json -Depth 6)
& $dotnet $hostDll "$fixture/nested-selected-plan.json" *> "$fixture/nested-selected-host.log"
Assert ($LASTEXITCODE -eq 0 -and (Get-Content -LiteralPath $report -Raw|ConvertFrom-Json).nestedNative -ceq 'native-vr-pose') 'selected native VR file remains visible beneath an ancestor merged from another mod'
Put "$low/bin/x64/plugins/mods/vr/reload/poses/native.lua" 'collection-shadow'
$plan=New-StandaloneVfsLaunchPlan $config $gamePlan "$fixture/nested-shadow-usvfs.log"
$plan.exe=$dotnet;$plan.arguments=@($consumer,$game,$report);$plan.workingDirectory=$game;$plan.timeoutSeconds=30
Put "$fixture/nested-shadow-plan.json" ($plan|ConvertTo-Json -Depth 6)
& $dotnet $hostDll "$fixture/nested-shadow-plan.json" *> "$fixture/nested-shadow-host.log"
Assert ($LASTEXITCODE -eq 0 -and (Get-Content -LiteralPath $report -Raw|ConvertFrom-Json).nestedNative -ceq 'native-vr-pose') 'selected physical renderer still overrides an actual nested collection conflict'
[pscustomobject]@{status='PASS';evidence=$fixture}|ConvertTo-Json -Compress
