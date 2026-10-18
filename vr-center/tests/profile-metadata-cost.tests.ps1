param([Parameter(Mandatory)][string]$Baseline,[int]$FileCount=2000,[switch]$MeasureOnly)
$ErrorActionPreference='Stop'
$app=Split-Path $PSScriptRoot -Parent
$fixture=Join-Path ([IO.Path]::GetTempPath()) ('profile-metadata-'+[guid]::NewGuid().ToString('N'))
[IO.Directory]::CreateDirectory("$fixture/game/bin/x64/plugins/example/settings/deep")|Out-Null
foreach($i in 1..$FileCount){[IO.File]::WriteAllText("$fixture/game/bin/x64/plugins/example/settings/deep/$i.json",'same bytes')}
$patterns=@(1..61|ForEach-Object {"archive/pc/content/base$_.archive"})+@('cache/**','*/transactions','mods/[ab]*.tmp','literal')
$config=@{data="$fixture/data";scopes=@(@{id='game';root="$fixture/game";kind='directory';requiredFiles=@('bin/x64/plugins/example/settings/deep/1.json');exclusions=$patterns})}
function Measure-Source($Source){
 . $Source
 $context=Get-GameProfileContext $config
 foreach($scope in @(@{exclusions=$patterns},$context.scopes.game)){
 foreach($path in @('archive/pc/content/BASE1.archive','archive/pc/content/base1.archive/child','archive/pc/content/base100.archive','cache','cache/a/b','a/transactions/file','mods/a.tmp','mods/z.tmp','literal/a','Literal','literal2','bin/x64/plugins/example/settings/deep/1.json')){
  $expected=$false;foreach($pattern in $patterns){if($path -like $pattern -or $path -like ($pattern.TrimEnd('/')+'/*') -or ($pattern.EndsWith('/**') -and $path -ieq $pattern.Substring(0,$pattern.Length-3))){$expected=$true;break}}
  if((Test-GameProfileExcluded $scope $path) -ne $expected){throw "Exclusion semantics changed: $path"}
 }
 }
 $timer=[Diagnostics.Stopwatch]::StartNew();$inventory=Get-GameProfileInventory $context;$directories=Get-GameProfileDirectoryInventory $context;$timer.Stop()
 if($inventory.Count -ne $FileCount -or @($inventory|Where-Object sha256 -ne $inventory[0].sha256).Count){throw 'Inventory lost files or hashes'}
 # Wildcard exclusions protect all directories by existing policy; exercise the
 # separate directory traversal with exact stock paths only as well.
 $exactConfig=@{data=$config.data;scopes=@(@{id='game';root=$config.scopes[0].root;kind='directory';requiredFiles=$config.scopes[0].requiredFiles;exclusions=@($patterns[0..60])})}
 $exactContext=Get-GameProfileContext $exactConfig
 $exactDirectories=Get-GameProfileDirectoryInventory $exactContext
 if($exactDirectories.Count -ne 6){throw ('Directory inventory lost nested directories: '+$exactDirectories.Count)}
 return @{seconds=$timer.Elapsed.TotalSeconds;files=$inventory.Count;directories=$directories.Count;hash=$inventory[0].sha256}
}
try{
 $before=Measure-Source $Baseline;$after=Measure-Source (Join-Path $app 'GameProfileStore.ps1')
 $result=@{before=$before;after=$after;ratio=$after.seconds/$before.seconds};$result|ConvertTo-Json -Depth 5
 if(-not $MeasureOnly -and $result.ratio -gt .8){throw 'Realistic inventory must improve at least 20 percent against frozen baseline'}
}finally{if([IO.Path]::GetFullPath($fixture).StartsWith([IO.Path]::GetTempPath(),[StringComparison]::OrdinalIgnoreCase)){Remove-Item -LiteralPath $fixture -Recurse -Force}}
