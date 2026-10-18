$ErrorActionPreference='Stop'
$app=Split-Path $PSScriptRoot -Parent
. (Join-Path $app 'GameProfileStore.ps1')
function LegacyMap($Context,$Raw,$Files){
 $map=@{}
 foreach($entry in @($Raw)){if(-not $entry){continue};$null=Get-GameProfileDirectoryPath $Context $entry;if(-not(Test-GameProfileProtectedDirectory $Context.scopes[$entry.scope] $entry.path)){$map[$entry.scope+'/'+$entry.path]=@{scope=$entry.scope;path=$entry.path}}}
 foreach($entry in $Files){$scope=$Context.scopes[$entry.scope];if($scope.kind -ne 'directory'){continue};$relative=$entry.path;while($relative -ne '.'){$relative=if($relative.Contains('/')){$relative.Substring(0,$relative.LastIndexOf('/'))}else{'.'};if(-not(Test-GameProfileProtectedDirectory $scope $relative)){$map[$entry.scope+'/'+$relative]=@{scope=$entry.scope;path=$relative}}}}
 return $map
}
$fixture=Join-Path ([IO.Path]::GetTempPath()) ('manifest-map-'+[guid]::NewGuid().ToString('N'))
try{
 $files=@();$raw=@();foreach($i in 1..1000){[IO.Directory]::CreateDirectory("$fixture/game/mods/$i/a/b")|Out-Null;$files+=@{scope='game';path="mods/$i/a/b/file"};$raw+=@{scope='game';path="mods/$i/a/b"}}
 $context=Get-GameProfileContext @{data="$fixture/data";scopes=@(@{id='game';kind='directory';root="$fixture/game";requiredFiles=@('mods/1/a/b/file');exclusions=@(1..61|ForEach-Object {"archive/pc/content/base$_.archive"})})}
 $timer=[Diagnostics.Stopwatch]::StartNew();$old=LegacyMap $context $raw $files;$timer.Stop();$before=$timer.Elapsed.TotalSeconds
 $timer.Restart();$new=Get-GameProfileManifestDirectories $context $raw $files;$timer.Stop()
 if(($old|ConvertTo-Json -Depth 6 -Compress) -eq ''){throw 'Empty legacy map'}
 if($old.Count -ne $new.Count){throw 'Map count changed'}
 foreach($key in $old.Keys){if(-not $new.ContainsKey($key) -or $old[$key].scope -cne $new[$key].scope -or $old[$key].path -cne $new[$key].path){throw "Map changed: $key"}}
 if($new['game/mods']['SCOPE'] -cne 'game' -or $new['game/mods']['PATH'] -cne 'mods'){throw 'Directory entry key comparer changed'}
 $objectRaw=@([pscustomobject]@{scope='game';path='mods/1/a/b'})
 $objectExpected=LegacyMap $context $objectRaw @();$objectActual=Get-GameProfileManifestDirectories $context $objectRaw @()
 if($objectExpected.Count -ne $objectActual.Count -or $objectActual['game/mods/1/a/b'].path -cne 'mods/1/a/b'){throw 'PSCustomObject directory entries changed'}
 @{before=$before;after=$timer.Elapsed.TotalSeconds;ratio=$timer.Elapsed.TotalSeconds/$before;directories=$new.Count}|ConvertTo-Json
 if($timer.Elapsed.TotalSeconds/$before -gt .8){throw 'Compiled map must improve metadata normalization at least twenty percent'}
 $originalCulture=[Globalization.CultureInfo]::CurrentCulture
 try{
  $vectors=@('.','a','A','I','i','ı','dir/I','dir/i','a/b','a/b/c','a/bc','archive/pc/content/base.archive','base','base/a','base/b','ab','a`b','literal','other','other/deep')
  $policies=@(@(),@('I'),@('I/**'),@('a/b'),@('base/**'),@('*/transactions'),@('a/[bc]'),@('a`b'),@('archive/pc/content/base.archive'),@('literal'))
  $caseCount=0
  foreach($creationCulture in @('en-US','tr-TR')){foreach($patterns in $policies){
   [Globalization.CultureInfo]::CurrentCulture=[Globalization.CultureInfo]::GetCultureInfo($creationCulture)
   $scopes=@();foreach($id in @('game','appdata','user-state','center-settings','collection','launch-source')){$scopes+=@{id=$id;kind='directory';root="$fixture/$id";requiredFiles=@('required/file');exclusions=$patterns}}
   $matrixContext=Get-GameProfileContext @{data="$fixture/matrix-data";scopes=$scopes}
   foreach($matchingCulture in @('en-US','tr-TR')){
    [Globalization.CultureInfo]::CurrentCulture=[Globalization.CultureInfo]::GetCultureInfo($matchingCulture)
    $matrixFiles=@();foreach($scope in $scopes){foreach($path in $vectors){$matrixFiles+=@{scope=$scope.id;path=($path+'/file')};$caseCount++}}
    $expected=LegacyMap $matrixContext @() $matrixFiles;$actual=Get-GameProfileManifestDirectories $matrixContext @() $matrixFiles
    if($expected.Count -ne $actual.Count){throw "Matrix map count changed $creationCulture/$matchingCulture [$patterns]"}
    foreach($key in $expected.Keys){if(-not $actual.ContainsKey($key) -or $expected[$key].scope -cne $actual[$key].scope -or $expected[$key].path -cne $actual[$key].path){throw "Matrix map changed: $key"}}
   }
  }}
  if($caseCount -lt 4000){throw 'Insufficient policy conformance cases'}
  Write-Output "PASS: $caseCount directory policy vectors across six scopes, culture changes, exact/wildcard/escaped/empty exclusions."
 }finally{[Globalization.CultureInfo]::CurrentCulture=$originalCulture}
}finally{if([IO.Path]::GetFullPath($fixture).StartsWith([IO.Path]::GetTempPath(),[StringComparison]::OrdinalIgnoreCase)){Remove-Item -LiteralPath $fixture -Recurse -Force}}
