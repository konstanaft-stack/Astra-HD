$ErrorActionPreference='Stop'
. (Join-Path (Split-Path $PSScriptRoot -Parent) 'GameProfileStore.ps1')
$fixture=Join-Path ([IO.Path]::GetTempPath()) ('native-inventory-'+[guid]::NewGuid().ToString('N'))
function CompareRows($Expected,$Actual){if($Expected.Count -ne $Actual.Count){throw 'Inventory count changed'};for($i=0;$i -lt $Expected.Count;$i++){foreach($key in @('scope','path','sha256','size')){if($Expected[$i][$key] -cne $Actual[$i][$key]){throw "Inventory row changed: $i/$key"}};if(-not $Actual[$i].ContainsKey('SCOPE')){throw 'Row key comparer changed'}}}
try{
 [IO.Directory]::CreateDirectory("$fixture/game/mods/deep")|Out-Null
 foreach($i in 1..2000){[IO.File]::WriteAllText("$fixture/game/mods/deep/$i.json",'same real hashed bytes')}
 [IO.File]::WriteAllText("$fixture/settings.json",'file scope')
 [IO.File]::SetAttributes("$fixture/game/mods/deep/1.json",[IO.FileAttributes]::Hidden)
 $config=@{data="$fixture/data";scopes=@(@{id='game';kind='directory';root="$fixture/game";requiredFiles=@('mods/deep/1.json');exclusions=@(1..61|ForEach-Object {"archive/pc/content/base$_.archive"})},@{id='settings';kind='file';root="$fixture/settings.json";requiredFiles=@('.');exclusions=@()},@{id='missing';kind='directory';root="$fixture/missing";requiredFiles=@();exclusions=@()})}
 $context=Get-GameProfileContext $config;$manual=@{scopes=@{}}
 foreach($key in $context.scopes.Keys){$scope=$context.scopes[$key].Clone();$scope.Remove('exclusionPolicy');$manual.scopes[$key]=$scope}
 $skip=@{'game/mods/deep/2.json'=$true}
 $timer=[Diagnostics.Stopwatch]::StartNew();$expected=Get-GameProfileInventory $manual $skip;$expectedDirs=Get-GameProfileDirectoryInventory $manual;$timer.Stop();$before=$timer.Elapsed.TotalSeconds
 # The method must exist before this can claim the compiled path was exercised.
 if(-not [VRCenter.ProfilePathChecks].GetMethod('Inventory')){throw 'Native inventory batch is missing'}
 $timer.Restart();$actual=Get-GameProfileInventory $context $skip;$actualDirs=Get-GameProfileDirectoryInventory $context;$timer.Stop()
 CompareRows $expected $actual;CompareRows $expectedDirs $actualDirs
 if($actual.Count -ne 2000 -or @($actual|Where-Object {$_.scope -eq 'game' -and $_.path -eq 'mods/deep/2.json'}).Count){throw 'Skip/file scope/hidden file semantics changed'}
 @{files=$actual.Count;directories=$actualDirs.Count;before=$before;after=$timer.Elapsed.TotalSeconds;ratio=$timer.Elapsed.TotalSeconds/$before}|ConvertTo-Json
 if($timer.Elapsed.TotalSeconds/$before -gt .8){throw 'Native inventory should improve metadata throughput at least twenty percent'}
 foreach($method in @('Get-GameProfileInventory','Get-GameProfileDirectoryInventory')){
  [IO.File]::WriteAllText("$fixture/not-a-directory",'wrong shape');$wrong=@{scopes=@{game=$context.scopes.game.Clone()}};$wrong.scopes.game.root="$fixture/not-a-directory"
  if($method -eq 'Get-GameProfileInventory'){$denied=$false;try{& $method $wrong|Out-Null}catch{$denied=$true};if(-not $denied){throw 'Directory scope file shape accepted'}}
 }
 $held=[IO.File]::Open("$fixture/game/mods/deep/1.json",[IO.FileMode]::Open,[IO.FileAccess]::Read,[IO.FileShare]::None)
 try{foreach($ctx in @($manual,$context)){$denied=$false;try{Get-GameProfileInventory $ctx|Out-Null}catch{$denied=$true};if(-not $denied){throw 'Locked file read error was swallowed'}}}finally{$held.Dispose()}
 [IO.Directory]::CreateDirectory("$fixture/outside")|Out-Null
 New-Item -ItemType Junction -Path "$fixture/game/excluded-link" -Target "$fixture/outside"|Out-Null
 $reparseConfig=@{data=$config.data;scopes=@(@{id='game';kind='directory';root="$fixture/game";requiredFiles=@('mods/deep/1.json');exclusions=@('excluded-link')})}
 $reparseContext=Get-GameProfileContext $reparseConfig
 foreach($method in @('Get-GameProfileInventory','Get-GameProfileDirectoryInventory')){$denied=$false;try{& $method $reparseContext|Out-Null}catch{$denied=$true};if(-not $denied){throw 'Excluded reparse entry accepted'}}
 [IO.Directory]::Delete("$fixture/game/excluded-link")
 [IO.Directory]::CreateDirectory("$fixture/culture/I")|Out-Null;[IO.File]::WriteAllText("$fixture/culture/I/file",'culture bytes')
 $upperFile=@{scopes=@{settings=$context.scopes.settings.Clone()}};$upperFile.scopes.settings.kind='FILE'
 $upperRows=Get-GameProfileInventory $upperFile
 if($upperRows.Count -ne 1 -or $upperRows[0].path -cne '.'){throw 'Case-insensitive file scope kind changed'}
 $upperDirectory=@{scopes=@{game=$context.scopes.game.Clone()}};$upperDirectory.scopes.game.kind='DIRECTORY';$upperDirectory.scopes.game.root="$fixture/culture"
 $upperDirs=Get-GameProfileDirectoryInventory $upperDirectory
 if($upperDirs.Count -ne 1 -or $upperDirs[0].path -cne 'I'){throw 'Case-insensitive directory scope kind changed'}
 $originalCulture=[Globalization.CultureInfo]::CurrentCulture
 try{foreach($creationCulture in @('en-US','tr-TR')){foreach($matchingCulture in @('en-US','tr-TR')){
  [Globalization.CultureInfo]::CurrentCulture=[Globalization.CultureInfo]::GetCultureInfo($creationCulture)
  $cultureContext=Get-GameProfileContext @{data="$fixture/culture-data";scopes=@(@{id='game';kind='directory';root="$fixture/culture";requiredFiles=@('required');exclusions=@('i')})}
  $cultureManual=@{scopes=@{game=$cultureContext.scopes.game.Clone()}};$cultureManual.scopes.game.Remove('exclusionPolicy')
  [Globalization.CultureInfo]::CurrentCulture=[Globalization.CultureInfo]::GetCultureInfo($matchingCulture)
  $left=Get-GameProfileInventory $cultureManual;$right=Get-GameProfileInventory $cultureContext;CompareRows $left $right
  $left=Get-GameProfileDirectoryInventory $cultureManual;$right=Get-GameProfileDirectoryInventory $cultureContext;CompareRows $left $right
 }}}finally{[Globalization.CultureInfo]::CurrentCulture=$originalCulture}
 'PASS: native/manual count/path/hash/size/row comparer, skip, file scope, hidden files, missing roots and root shape.'
}finally{if([IO.Path]::GetFullPath($fixture).StartsWith([IO.Path]::GetTempPath(),[StringComparison]::OrdinalIgnoreCase)){Remove-Item -LiteralPath $fixture -Recurse -Force}}
