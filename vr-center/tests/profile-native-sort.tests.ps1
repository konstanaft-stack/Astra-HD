$ErrorActionPreference='Stop'
. (Join-Path (Split-Path $PSScriptRoot -Parent) 'GameProfileStore.ps1')
$original=[Globalization.CultureInfo]::CurrentCulture
try{
 foreach($culture in @('en-US','ru-RU','tr-TR')){
  [Globalization.CultureInfo]::CurrentCulture=[Globalization.CultureInfo]::GetCultureInfo($culture)
  $rows=@();$ordinal=0
  foreach($scope in @('game','Game','appdata','я','I','i','ı','İ')){foreach($path in @('same','Same','a1','a10','a2','é',"e$([char]0x301)",'е','ё','I','i','ı','İ','a-b','a_b','a/b')){$rows+=@{scope=$scope;path=$path;ordinal=$ordinal};$ordinal++}}
  $rows+=@(1..40|ForEach-Object {@{scope='ties';path='same';ordinal=$ordinal};$ordinal++})
  $expected=@($rows|Sort-Object scope,path);$actual=Sort-GameProfileRows $rows
  if(($expected.ordinal -join ',') -cne ($actual.ordinal -join ',')){throw "Native sort sequence changed $culture"}
  $unsupported=@(@{scope=$null;path='x';ordinal=1},@{scope='game';path=$null;ordinal=2},@{scope=12;path='x';ordinal=3},[pscustomobject]@{scope='game';path='x';ordinal=4})
  $expected=@($unsupported|Sort-Object scope,path);$actual=Sort-GameProfileRows $unsupported
  if(($expected.ordinal -join ',') -cne ($actual.ordinal -join ',')){throw 'Unsupported row fallback changed'}
 }
 foreach($count in @(8000,34000)){
  $rows=@(1..$count|ForEach-Object {@{scope='game';path="mods/$_/a/b";ordinal=$_}})
  $timer=[Diagnostics.Stopwatch]::StartNew();$expected=@($rows|Sort-Object scope,path);$timer.Stop();$before=$timer.Elapsed.TotalSeconds
  $timer.Restart();$actual=Sort-GameProfileRows $rows;$timer.Stop()
  if(($expected.ordinal -join ',') -cne ($actual.ordinal -join ',')){throw 'Benchmark sequence changed'}
  @{rows=$count;before=$before;after=$timer.Elapsed.TotalSeconds;ratio=$timer.Elapsed.TotalSeconds/$before}|ConvertTo-Json -Compress
  if($timer.Elapsed.TotalSeconds/$before -gt .5){throw 'Native sort must improve at least twofold'}
 }
 'PASS: default unstable tie sequence, Unicode/culture string ordering, record identity and unsupported field fallback.'
}finally{[Globalization.CultureInfo]::CurrentCulture=$original}
