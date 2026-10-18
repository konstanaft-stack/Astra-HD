$ErrorActionPreference='Stop'
$app=Split-Path $PSScriptRoot -Parent
. (Join-Path $app 'GameProfileStore.ps1')
$fixture=Join-Path $app ('audit/profile-speed-'+[guid]::NewGuid().ToString('N'))
$root=Join-Path $fixture 'game/bin/x64/plugins/example/settings'
[IO.Directory]::CreateDirectory($root)|Out-Null
foreach($i in 1..500){[IO.File]::WriteAllText((Join-Path $root "$i.json"),'{"setting":1}')}
$config=@{data=(Join-Path $fixture 'data');scopes=@(@{id='game';root=(Join-Path $fixture 'game');kind='directory';requiredFiles=@('bin/x64/plugins/example/settings/1.json');exclusions=@()})}
$context=Get-GameProfileContext $config
$timer=[Diagnostics.Stopwatch]::StartNew();$inventory=Get-GameProfileInventory $context;$timer.Stop()
$result=@{files=$inventory.Count;seconds=$timer.Elapsed.TotalSeconds;sha256=$inventory[0].sha256}
$result|ConvertTo-Json|Set-Content -LiteralPath (Join-Path $fixture 'result.json')
Write-Output ($result|ConvertTo-Json -Compress)
if($inventory.Count -ne 500 -or $timer.Elapsed.TotalSeconds -gt 8){throw 'Inventory path validation is too slow for collections: 500 tiny files must take under 8 seconds.'}
Write-Output ('PASS: inventory stays bounded while hashing every file; evidence '+$fixture)
