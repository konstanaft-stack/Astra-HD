$ErrorActionPreference='Stop'
. (Join-Path (Split-Path $PSScriptRoot -Parent) 'GameProfileStore.ps1')
function LegacyTarget($Context,[string]$ScopeId,[string]$Relative){$scope=$Context.scopes[$ScopeId];if(-not $scope){throw 'Unknown profile scope.'};Assert-GameProfileRelativePath $Relative $scope.kind;if(Test-GameProfileExcluded $scope $Relative){throw 'Profile path is excluded by scope policy.'};$path=if($scope.kind -eq 'file'){$scope.root}else{[IO.Path]::GetFullPath((Join-Path $scope.root $Relative))};if($scope.kind -eq 'directory' -and -not $path.StartsWith($scope.root+'\',[StringComparison]::OrdinalIgnoreCase)){throw 'Profile path escaped its scope.'};Assert-GameProfilePlainPath $path;return $path}
function LegacyStore($Context,[string]$Relative){Assert-GameProfileRelativePath $Relative;$path=[IO.Path]::GetFullPath((Join-Path $Context.store $Relative));if(-not $path.StartsWith($Context.store+'\',[StringComparison]::OrdinalIgnoreCase)){throw 'Profile store path escaped its root.'};Assert-GameProfilePlainPath $path;return $path}
function LegacyDirectory($Context,$Entry){$scope=$Context.scopes[$Entry.scope];if(-not $scope -or $scope.kind -ne 'directory'){throw 'Invalid directory profile scope.'};$path=if($Entry.path -ceq '.'){$scope.root}else{LegacyTarget $Context $Entry.scope $Entry.path};Assert-GameProfilePlainPath $path;return $path}
function CompareCall([scriptblock]$Old,[scriptblock]$New){$a=$null;$b=$null;$ae=$null;$be=$null;try{$a=&$Old}catch{$ae=$_.Exception.GetBaseException().Message};try{$b=&$New}catch{$be=$_.Exception.GetBaseException().Message};if($a -cne $b -or $ae -cne $be){throw "Path/result/error changed old=[$a][$ae] new=[$b][$be]"}}
$fixture=Join-Path ([IO.Path]::GetTempPath()) ('profile-native-paths-'+[guid]::NewGuid().ToString('N'));$originalCulture=[Globalization.CultureInfo]::CurrentCulture
try{
 [IO.Directory]::CreateDirectory("$fixture/game/existing/deep")|Out-Null;[IO.File]::WriteAllText("$fixture/file.json",'file scope')
 $paths=@('existing/deep','missing/file','.','..','','/root','C:relative','a\b','a//b','a*','a?','a.','a ','con','COM1.txt','lpt9.ext','file.json','I','i','ı','İ','a/[b]','a`b','é',"e$([char]0x301)",'base','base/child','other/deep')
 $count=0
 foreach($creationCulture in @('en-US','ru-RU','tr-TR')){foreach($patterns in @(@(),@('I'),@('I/**'),@('a/[b]'),@('a`b'),@('base/**'))){
  [Globalization.CultureInfo]::CurrentCulture=[Globalization.CultureInfo]::GetCultureInfo($creationCulture)
  $context=Get-GameProfileContext @{data="$fixture/data";scopes=@(@{id='game';kind='directory';root="$fixture/game";requiredFiles=@('required');exclusions=$patterns},@{id='settings';kind='file';root="$fixture/file.json";requiredFiles=@('.');exclusions=@()})}
  foreach($matchingCulture in @('en-US','ru-RU','tr-TR')){
   [Globalization.CultureInfo]::CurrentCulture=[Globalization.CultureInfo]::GetCultureInfo($matchingCulture)
   foreach($path in $paths){foreach($scope in @('game','settings','missing')){CompareCall {LegacyTarget $context $scope $path} {Get-GameProfileTargetPath $context $scope $path};CompareCall {LegacyDirectory $context @{scope=$scope;path=$path}} {Get-GameProfileDirectoryPath $context @{scope=$scope;path=$path}}};$count+=6;CompareCall {LegacyStore $context $path} {Get-GameProfileStorePath $context $path};$count++}
  }
 }}
 if(-not [VRCenter.ProfilePathChecks].GetMethod('TargetPath')){throw 'Thin native path method missing'}
 $manual=@{store=$context.store;scopes=@{game=$context.scopes.game.Clone()}};$manual.scopes.game.Remove('exclusionPolicy')
 CompareCall {LegacyTarget $manual 'game' 'existing/deep'} {Get-GameProfileTargetPath $manual 'game' 'existing/deep'}
 CompareCall {LegacyDirectory $manual ([pscustomobject]@{scope='game';path='existing/deep'})} {Get-GameProfileDirectoryPath $manual ([pscustomobject]@{scope='game';path='existing/deep'})}
 [IO.Directory]::CreateDirectory("$fixture/outside")|Out-Null;New-Item -ItemType Junction -Path "$fixture/game/link" -Target "$fixture/outside"|Out-Null
 CompareCall {LegacyTarget $context 'game' 'link/file'} {Get-GameProfileTargetPath $context 'game' 'link/file'}
 CompareCall {LegacyDirectory $context @{scope='game';path='link'}} {Get-GameProfileDirectoryPath $context @{scope='game';path='link'}}
 [IO.Directory]::Delete("$fixture/game/link")
 "PASS: $count path/result/error comparisons, culture fallback, unknown scope, file dot, reserved/traversal names and reparse ancestry."
}finally{[Globalization.CultureInfo]::CurrentCulture=$originalCulture;if([IO.Directory]::Exists($fixture) -and [IO.Path]::GetFullPath($fixture).StartsWith([IO.Path]::GetTempPath(),[StringComparison]::OrdinalIgnoreCase)){Remove-Item -LiteralPath $fixture -Recurse -Force}}
