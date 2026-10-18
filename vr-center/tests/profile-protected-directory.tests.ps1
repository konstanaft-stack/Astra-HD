param([Parameter(Mandatory)][string]$Baseline)
$ErrorActionPreference='Stop'
$app=Split-Path $PSScriptRoot -Parent
. $Baseline
$legacy=${function:Test-GameProfileProtectedDirectory}
. (Join-Path $app 'GameProfileStore.ps1')
$original=[Globalization.CultureInfo]::CurrentCulture
try{
 foreach($culture in @('en-US','tr-TR')){
  [Globalization.CultureInfo]::CurrentCulture=[Globalization.CultureInfo]::GetCultureInfo($culture)
  foreach($patterns in @(@(),@('archive/pc/content/base.archive'),@('I/path'),@('a/**'),@('a/*/b'),@('a/[bc]'),@('literal'),@('a`b/c'))){
   $scope=@{exclusions=$patterns};Initialize-GameProfileExclusions $scope
   foreach($matchCulture in @('en-US','tr-TR')){
    [Globalization.CultureInfo]::CurrentCulture=[Globalization.CultureInfo]::GetCultureInfo($matchCulture)
    foreach($relative in @('.','archive','ARCHIVE/PC','archive/pc/content','archive/pc/content/base.archive','archive/pc/content/base.archive/child','archive/pc/content2','I','i','ı','a','a/[bc]','a/b','ab','literal','literal/child','other')){
     if((& $legacy @{exclusions=$patterns} $relative) -ne (Test-GameProfileProtectedDirectory $scope $relative)){throw "Protected directory mismatch $culture/$matchCulture [$patterns] [$relative]"}
    }
   }
   [Globalization.CultureInfo]::CurrentCulture=[Globalization.CultureInfo]::GetCultureInfo($culture)
  }
 }
}finally{[Globalization.CultureInfo]::CurrentCulture=$original}
$fixture=Join-Path ([IO.Path]::GetTempPath()) ('profile-dirs-'+[guid]::NewGuid().ToString('N'))
try{
 foreach($i in 1..1000){[IO.Directory]::CreateDirectory("$fixture/game/mods/$i/deep/leaf")|Out-Null}
 $patterns=@(1..61|ForEach-Object {"archive/pc/content/base$_.archive"})
 $config=@{data="$fixture/data";scopes=@(@{id='game';kind='directory';root="$fixture/game";requiredFiles=@('bin/game.exe');exclusions=$patterns})}
 . $Baseline
 $context=Get-GameProfileContext $config
 $beforeTimer=[Diagnostics.Stopwatch]::StartNew();$before=Get-GameProfileDirectoryInventory $context;$beforeTimer.Stop()
 . (Join-Path $app 'GameProfileStore.ps1')
 $context=Get-GameProfileContext $config
 $timer=[Diagnostics.Stopwatch]::StartNew();$inventory=Get-GameProfileDirectoryInventory $context;$timer.Stop()
 if($inventory.Count -ne 3001){throw ('Directory inventory changed: '+$inventory.Count)}
 @{directories=$inventory.Count;beforeSeconds=$beforeTimer.Elapsed.TotalSeconds;seconds=$timer.Elapsed.TotalSeconds;ratio=$timer.Elapsed.TotalSeconds/$beforeTimer.Elapsed.TotalSeconds}|ConvertTo-Json
 if($timer.Elapsed.TotalSeconds -gt 8){throw 'Compiled protection policy must keep 3001 directories under eight seconds'}
 'PASS: protected directory policy matches legacy exact ancestors, wildcards, empty exclusions, cultures and descendants.'
}finally{if([IO.Path]::GetFullPath($fixture).StartsWith([IO.Path]::GetTempPath(),[StringComparison]::OrdinalIgnoreCase)){Remove-Item -LiteralPath $fixture -Recurse -Force}}
