$ErrorActionPreference='Stop'
$app=Split-Path $PSScriptRoot -Parent
. "$app/GameLaunch.ps1"
function Check($v,$m){if(-not $v){throw "FAIL $m"};"PASS $m"}
function Put($p,$s){[IO.Directory]::CreateDirectory((Split-Path $p -Parent))|Out-Null;[IO.File]::WriteAllText($p,$s)}
function Assert-GameWritable($c){if($script:active){throw 'active game'}}
$root=Join-Path ([IO.Path]::GetTempPath()) ('nr-startup-'+[guid]::NewGuid().ToString('N'));$c=@{game="$root/game";data="$root/data";imagePackage="$root/spec.json"};$ini="$root/game/bin/x64/OptiScaler.ini";$text="[Menu]`r`nShortcutKey=45`r`nFGShortcutKey=123`r`n[DlssNr]`r`nEnabled = true ; user`r`nToggleKey=220`r`nStyle=1`r`nWorkingScale=1.05`r`n[Other]`r`nEnabled=true`r`n";$expected=$text.Replace('Enabled = true ; user','Enabled = false ; user');$script:active=$false
try{
 Put "$root/game/bin/x64/dbghelp.dll" 'fixture-loader';Put $ini $text
 $files=@(@{path='bin/x64/dbghelp.dll';sha256=(Get-FileHash "$root/game/bin/x64/dbghelp.dll").Hash},@{path='bin/x64/OptiScaler.ini';sha256=(Get-FileHash $ini).Hash;mutable=$true})
 $spec=@{schema=1;scope='game-shared';id='fixture';loaderPath='bin/x64/dbghelp.dll';disabledLoaderPath='bin/x64/dbghelp.dll.disabled';files=$files;nrStartupPolicy='off-until-manual'};Write-Json $c.imagePackage $spec;Write-Json "$root/data/enhancement-common/state.json" @{scope='game-shared';id='fixture';phase='ready'}
 if(Get-Command Invoke-NrStartupPolicy -ErrorAction SilentlyContinue){Invoke-NrStartupPolicy $c}
 Check ([IO.File]::ReadAllText($ini)-ceq$expected) 'policy changes only Enabled true to false'
 $tx=@(Get-ChildItem "$root/data/transactions" -Directory);Check ($tx.Count-eq1-and[IO.File]::ReadAllText((Join-Path $tx[0].FullName '0.bin'))-ceq$text) 'real component transaction preserves exact original rollback'
 Invoke-NrStartupPolicy $c;Check (@(Get-ChildItem "$root/data/transactions" -Directory).Count-eq1) 'already false creates no transaction'
 $unicode=$text+'; пользовательская заметка'+"`r`n";[IO.File]::WriteAllText($ini,$unicode,[Text.UTF8Encoding]::new($true));Invoke-NrStartupPolicy $c
 $expectedBytes=[byte[]](@([byte]239,[byte]187,[byte]191)+@([Text.Encoding]::UTF8.GetBytes($unicode.Replace('Enabled = true ; user','Enabled = false ; user'))))
 Check ([Convert]::ToBase64String([IO.File]::ReadAllBytes($ini))-ceq[Convert]::ToBase64String($expectedBytes)) 'UTF8 BOM Unicode comments and every unrelated byte preserved'
 foreach($bad in @('[DlssNr]`nEnabled=maybe','[DlssNr]`nEnabled=true`nEnabled=false','[DlssNr]`nEnabled=true`n[DlssNr]`nEnabled=false')){Put $ini $bad.Replace('`n',"`n");$before=(Get-FileHash $ini).Hash;$rejected=$false;try{Invoke-NrStartupPolicy $c}catch{$rejected=$true};Check ($rejected-and(Get-FileHash $ini).Hash-eq$before) 'malformed/duplicate policy fails closed without writing'}
 foreach($mode in 'no-policy','legacy','unavailable','off'){
  Put $ini $text;$s=$spec.Clone();if($mode-eq'no-policy'){$s.Remove('nrStartupPolicy')};if($mode-eq'legacy'){$s.scope='port'};Write-Json $c.imagePackage $s
  if($mode-eq'unavailable'){[IO.File]::Move("$root/data/enhancement-common/state.json","$root/data/state.saved")};if($mode-eq'off'){[IO.File]::Move("$root/game/bin/x64/dbghelp.dll","$root/game/bin/x64/dbghelp.dll.disabled")}
  Invoke-NrStartupPolicy $c;Check ([IO.File]::ReadAllText($ini)-ceq$text) "$mode unchanged"
  if($mode-eq'unavailable'){[IO.File]::Move("$root/data/state.saved","$root/data/enhancement-common/state.json")};if($mode-eq'off'){[IO.File]::Move("$root/game/bin/x64/dbghelp.dll.disabled","$root/game/bin/x64/dbghelp.dll")}
 }
 Write-Json $c.imagePackage $spec
 function Get-GameLaunchPlan($c){return @{game=$c.game;kind=$script:kind;arguments=@()}}
 function Assert-GameLaunchComposition($c,$p){}
 function Get-GameLaunchEffectiveConfig($c){return $c.Clone()}
 function Get-GameLaunchProcesses($names){if($script:active){return @(@{Id=1})};return @()}
 function Invoke-RedmodDeployment($c,$p,$l){}
 function Dispatch {Check ([IO.File]::ReadAllText($ini)-ceq$expected) 'startup policy executes before dispatch';throw 'DISPATCH_BOUNDARY'}
 function Start-PlainGameLaunch($p){Dispatch}
 function Start-GameLaunchDispatcher($c,$p,$l){Dispatch}
 function Invoke-VrLauncher($c,$l,[switch]$CheckOnly){if(-not$CheckOnly){Dispatch}}
 foreach($route in 'direct','managed','collection'){
  $script:kind=if($route-eq'collection'){'mo2'}else{'direct'};Put $ini $text;$installed="$root/game/CPVR-Portable/installed.json";if(Test-Path $installed){[IO.File]::Delete($installed)};if($route-eq'managed'){Put $installed '{}'}
  $boundary=$false;try{Invoke-GameProfileLaunch $c "$root/launch.log"}catch{$boundary=$_.Exception.Message-eq'DISPATCH_BOUNDARY'};Check $boundary "$route actual caller reaches guarded dispatch"
 }
 Put $ini $text;$script:active=$true;$rejected=$false;try{Invoke-GameProfileLaunch $c "$root/launch.log"}catch{$rejected=$true};Check ($rejected-and[IO.File]::ReadAllText($ini)-ceq$text) 'active game rejects before policy mutation';$script:active=$false
 'PASS NR startup policy fixture complete; temp roots only'
}finally{if([IO.Path]::GetFullPath($root).StartsWith([IO.Path]::GetFullPath([IO.Path]::GetTempPath()),[StringComparison]::OrdinalIgnoreCase)){Remove-Item -LiteralPath $root -Recurse -Force}}
