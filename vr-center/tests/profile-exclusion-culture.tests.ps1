$ErrorActionPreference='Stop'
. (Join-Path (Split-Path $PSScriptRoot -Parent) 'GameProfileStore.ps1')
$original=[Globalization.CultureInfo]::CurrentCulture
try{
 foreach($creationCulture in @('en-US','tr-TR')){
  [Globalization.CultureInfo]::CurrentCulture=[Globalization.CultureInfo]::GetCultureInfo($creationCulture)
  $patterns=@('dir/I','I*/cache','a`b','I/**','base/**')
  $scope=@{exclusions=$patterns};Initialize-GameProfileExclusions $scope
  foreach($matchCulture in @('en-US','tr-TR')){
   [Globalization.CultureInfo]::CurrentCulture=[Globalization.CultureInfo]::GetCultureInfo($matchCulture)
   foreach($path in @('I','i','ı','İ','I/a','i/a','ı/a','dir/i','dir/ı/a','ıx/cache/a','ix/cache/a','ab','a`b','ab/child','base','BASE/a','other')){
    $expected=$false;foreach($pattern in $patterns){if($path -like $pattern -or $path -like ($pattern.TrimEnd('/')+'/*') -or ($pattern.EndsWith('/**') -and $path -ieq $pattern.Substring(0,$pattern.Length-3))){$expected=$true;break}}
    if((Test-GameProfileExcluded $scope $path) -ne $expected){throw "Policy mismatch create=$creationCulture match=$matchCulture path=$path"}
   }
  }
 }
 'PASS: original wildcard semantics preserved across Turkish/English creation and matching cultures, descendants, escapes, and wildcards.'
}finally{[Globalization.CultureInfo]::CurrentCulture=$original}
