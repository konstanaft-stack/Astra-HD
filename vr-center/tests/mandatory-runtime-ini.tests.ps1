#requires -Version 7.5
param([string]$CodeRoot,[string]$Installer,[string]$Output)
$ErrorActionPreference='Stop'
$allowed=[IO.Path]::GetFullPath($PSScriptRoot).TrimEnd('\')+'\';$root=[IO.Path]::GetFullPath($Output)
if(-not $root.StartsWith($allowed,[StringComparison]::OrdinalIgnoreCase)-or(Test-Path $root)){throw 'Fresh owned audit fixture required'}
[void][IO.Directory]::CreateDirectory($root)
function Put($p,$s){[void][IO.Directory]::CreateDirectory((Split-Path $p -Parent));[IO.File]::WriteAllText($p,$s,[Text.UTF8Encoding]::new($false))}
foreach($entry in @(@{p=(Join-Path $CodeRoot 'Core.ps1');names=@('Read-Json','Write-Json')},@{p=(Join-Path $CodeRoot 'SettingsProfiles.ps1')},@{p=(Join-Path $CodeRoot 'NativeRequiredIni.ps1');optional=$true})){
 if($entry.optional-and-not(Test-Path $entry.p)){continue}
 $tok=$null;$err=$null;$ast=[Management.Automation.Language.Parser]::ParseFile($entry.p,[ref]$tok,[ref]$err);if($err.Count){throw 'Parse'}
 foreach($fn in $ast.FindAll({param($n)$n-is[Management.Automation.Language.FunctionDefinitionAst]},$true)){if(-not $entry.names-or$fn.Name-in$entry.names){. ([scriptblock]::Create($fn.Extent.Text))}}
}
function Assert-GameWritable($c){foreach($p in @($c.game,$c.data,$c.bundle,$c.settingsFile)){if(-not [IO.Path]::GetFullPath($p).StartsWith($allowed,[StringComparison]::OrdinalIgnoreCase)){throw 'Outside fixture'}}}
$checks=[Collections.Generic.List[string]]::new()
function Check($v,$why){if(-not$v){throw ('ASSERT '+$why)};$checks.Add($why);'PASS '+$why}
$nl=[string][char]13+[char]10;$lf=[string][char]10;$renderer='red4ext/plugins/cyberpunkvr_stereo/cyberpunkvr_stereo.dll'
$oldIni=(@('; user comment','first_launch=0','xr_hud_to_second_eye=0 ; preserve comment','ppd=27','xr_aer_v2=0','xr_menu_stereo=1','xr_head_offset_z=0.1234','xr_framegen_overlay_x=0.75','')-join$nl)
$expected=$oldIni.Replace('xr_hud_to_second_eye=0','xr_hud_to_second_eye=1')
$player='{"data":[{"group_name":"/graphics/presets","options":[{"name":"Quality","value":"UserChoice"},{"name":"FrameGeneration","value":"Off"}]},{"group_name":"/controls/fpp_camera","options":[{"name":"FOV","value":83}]}]}'
$required=@{schema=1;values=@{xr_hud_to_second_eye=1}}
$c=@{game="$root/game";data="$root/data";bundle="$root/bundle";settingsFile="$root/player/UserSettings.json"}
Put "$($c.game)/bin/x64/Cyberpunk2077.exe" 'fixture-never-execute';Put $c.settingsFile $player
function Cas($s){$h=[Convert]::ToHexString([Security.Cryptography.SHA256]::HashData([Text.Encoding]::UTF8.GetBytes($s)));Put "$($c.bundle)/files/$h" $s;return $h}
$ih=Cas $oldIni;$sh=Cas $player;$rh=Cas 'parent';$ch=Cas 'child'
$meta=@{exeSha256=(Get-FileHash "$($c.game)/bin/x64/Cyberpunk2077.exe").Hash;managedPaths=@('bin/x64/vrport.ini',$renderer);modules=@{};versions=@(@{id='flat';kind='flat';available=$false;files=@{'@settings'=$sh}},@{id='r28';kind='native';available=$true;files=@{'@settings'=$sh;'bin/x64/vrport.ini'=$ih;$renderer=$rh}},@{id='r29';kind='native';parentId='r28';available=$true;requiredIni=$required;files=@{'@settings'=$sh;'bin/x64/vrport.ini'=$ih;$renderer=$ch}},@{id='legacy';kind='native';available=$true;files=@{'@settings'=$sh;'bin/x64/vrport.ini'=$ih;$renderer=$rh}})}
Write-Json "$($c.bundle)/bundle.json" $meta
$raw=[IO.File]::ReadAllText($Installer)
if(-not$raw.Contains('Local\IRD.SingleBuildTestDeploy.v1')){throw 'Gate contract changed'}
Put "$($c.bundle)/Install.ps1" ($raw.Replace('Local\IRD.SingleBuildTestDeploy.v1','Local\Fixture.RequiredIni.'+[guid]::NewGuid().ToString('N')))
$helper=Join-Path $CodeRoot 'NativeRequiredIni.ps1';if(Test-Path $helper){[IO.File]::Copy($helper,"$($c.bundle)/NativeRequiredIni.ps1")}
Put "$($c.bundle)/Clean-Profile.ps1" 'function Get-CleanProfilePlan {param($Game,$StateDir,$Bundle,$Selected,$Clean,$BackupRelative);@{ExtraRemovals=@();Restores=@();State=@{}}}'
Put "$($c.bundle)/Start-Game.ps1" "throw 'Fixture never launches game'"
Put "$root/worker.ps1" @'
param($Bundle,$Game,$Settings,$Version)
$ErrorActionPreference='Stop'
function Get-Process {param($Name,$Id);@()}
& (Join-Path $Bundle 'Install.ps1') -Version $Version -GamePath $Game -SettingsFile $Settings -PreviousSettingsFile $Settings -Hands $false
exit $LASTEXITCODE
'@
$ini="$($c.game)/bin/x64/vrport.ini";$receipt="$($c.game)/CPVR-Portable/installed.json"
function InstallCase($v='r29'){& (Join-Path $PSHOME 'pwsh.exe') -NoProfile -File "$root/worker.ps1" -Bundle $c.bundle -Game $c.game -Settings $c.settingsFile -Version $v *> "$root/install-last.log";return $LASTEXITCODE}
function Receipt($v,$bytes){Put "$($c.game)/$renderer" $bytes;Write-Json $receipt @{version=$v;kind='native';preference=$v;paths=@('bin/x64/vrport.ini',$renderer);critical=@();opti=$false;fg=$false;hands=$false}}
function ClearPreferences(){if(Test-Path "$($c.game)/CPVR-Portable/preferences"){Remove-Item -LiteralPath "$($c.game)/CPVR-Portable/preferences" -Recurse -Force}}
Check ((InstallCase)-eq0) 'standalone fresh Apply succeeds from registered-style HUD0 seed'
Check ([IO.File]::ReadAllText($ini)-ceq$expected) 'fresh Apply requires HUD1 even when registered seed has HUD0'
Check ([IO.File]::ReadAllText($c.settingsFile)-ceq$player) 'fresh Apply preserves graphics/control JSON'
ClearPreferences;Put $ini $oldIni;Receipt r28 'parent'
Check ((InstallCase)-eq0) 'parent update succeeds'
Check ([IO.File]::ReadAllText($ini)-ceq$expected) 'parent inherited HUD0 changes only required flag'
Check ((Read-Json $receipt).settingsInheritedFrom-ceq'r28') 'parent consent lineage retained'
ClearPreferences;Put $ini $oldIni;Receipt r28 'parent';$child=$oldIni.Replace('ppd=27','ppd=33')
Put "$($c.game)/CPVR-Portable/preferences/r29/bin/x64/vrport.ini" $child
Check ((InstallCase)-eq0) 'child preference update succeeds'
Check ([IO.File]::ReadAllText($ini)-ceq$child.Replace('xr_hud_to_second_eye=0','xr_hud_to_second_eye=1')) 'child preference PPD33/comments retained with HUD1'
Put $ini $oldIni;Receipt r29 'child'
Check ((InstallCase)-eq0) 'same R29 Apply succeeds'
Check ([IO.File]::ReadAllText($ini)-ceq$expected) 'same R29 cannot restore required HUD0'
ClearPreferences;Put $ini $oldIni;Receipt legacy 'parent'
Check ((InstallCase legacy)-eq0) 'legacy native Apply succeeds'
Check ([IO.File]::ReadAllText($ini)-ceq$oldIni) 'legacy metadata-absent INI byte-identical'
$state=@{receipt=@{version='r29';kind='native'}};$paths=Get-SettingsModePaths $c $state
Put $ini $oldIni;Put $paths.custom $player;Write-Json $paths.state @{version='r29';recommended=$false}
Set-RecommendedSettingsMode $c $state $false
Check ([IO.File]::ReadAllText($ini)-ceq$expected) 'Custom reconciles required flag'
Check ([IO.File]::ReadAllText($c.settingsFile)-ceq$player-and[IO.File]::ReadAllText($paths.custom)-ceq$player) 'Custom JSON and backup byte-identical'
Put $ini $oldIni;Set-RecommendedSettingsMode $c $state $false -BeforeLaunch
Check ([IO.File]::ReadAllText($ini)-ceq$expected) 'saved R29 BeforeLaunch repairs restored HUD0'
$time=[IO.File]::GetLastWriteTimeUtc($ini);$count=@(Get-ChildItem (Join-Path $paths.root 'transactions') -Directory).Count
Set-RecommendedSettingsMode $c $state $false -BeforeLaunch
Check ([IO.File]::GetLastWriteTimeUtc($ini)-eq$time-and@(Get-ChildItem (Join-Path $paths.root 'transactions') -Directory).Count-eq$count) 'idempotent launch performs no write/transaction'
[IO.File]::Delete($paths.state);Put $ini $oldIni
$backend=[IO.File]::ReadAllText((Join-Path $CodeRoot 'Backend.ps1'))
$start=$backend.IndexOf("}elseif((Test-CyberpunkProfileMode "+'$config'+") -and "+'$state'+".receipt.kind -eq 'native'){")
$end=$backend.IndexOf('            # Only an explicit click',$start)
if($start -lt 0 -or $end -lt 0){throw 'Native launch branch changed'}
$branch=$backend.Substring($start,$end-$start);$branch=$branch.Substring($branch.IndexOf('{')+1);$branch=$branch.Substring(0,$branch.LastIndexOf('}'))
function Get-GameLaunchEffectiveConfig($Config){$Config}
$config=$c
& ([scriptblock]::Create($branch))
Check ([IO.File]::ReadAllText($ini)-ceq$expected-and-not(Test-Path $paths.state)) 'mode-absent restored R29 does not invent consent'
Check ([IO.File]::ReadAllText($c.settingsFile)-ceq$player) 'restore sync preserves graphics/control JSON'
$bad=@{schema=1;values=@{xr_hud_to_second_eye=1;ppd=20}}
foreach($policy in @($bad,@{schema=2;values=@{xr_hud_to_second_eye=1}},@{schema=1;values=@{xr_hud_to_second_eye=0}},@{schema=1;values=@{xr_hud_to_second_eye=1};other=1})){
 $rejected=$false;try{Update-NativeRequiredIniText $oldIni @{kind='native';requiredIni=$policy}|Out-Null}catch{$rejected=$true};Check $rejected 'unknown key/value/schema rejected'
}
$rejected=$false;try{Update-NativeRequiredIniText ($oldIni+'xr_hud_to_second_eye=1'+$nl) $meta.versions[2]|Out-Null}catch{$rejected=$true};Check $rejected 'duplicate key rejected'
Check ((Update-NativeRequiredIniText $oldIni @{kind='native'})-ceq$oldIni) 'metadata-absent native unchanged'
Check ((Update-NativeRequiredIniText $oldIni @{kind='flat'})-ceq$oldIni) 'flat unchanged'
Check ((Update-NativeRequiredIniText $oldIni.Replace($nl,$lf) $meta.versions[2])-ceq$expected.Replace($nl,$lf)) 'LF/comments retained except one flag digit'
foreach($value in @($true,1.0)){ $rejected=$false;try{Update-NativeRequiredIniText $oldIni @{kind='native';requiredIni=@{schema=1;values=@{xr_hud_to_second_eye=$value}}}|Out-Null}catch{$rejected=$true};Check $rejected 'bool/float mandatory value rejected' }
$bom=[string][char]0xFEFF;Put $ini ($bom+$oldIni);Sync-NativeRequiredIni $c $state
Check ([Convert]::ToHexString([IO.File]::ReadAllBytes($ini))-ceq[Convert]::ToHexString([Text.Encoding]::UTF8.GetBytes($bom+$expected))) 'UTF8 BOM and all unrelated bytes retained in real settings transaction'
$missing=$oldIni.Replace('xr_hud_to_second_eye=0 ; preserve comment'+$nl,'')
Check ((Update-NativeRequiredIniText $missing $meta.versions[2])-ceq($missing+'xr_hud_to_second_eye=1'+$nl)) 'missing key appends with existing newline'
function GamePins(){@(Get-ChildItem $c.game -Recurse -File|ForEach-Object{[pscustomobject]@{path=$_.FullName;sha=(Get-FileHash $_.FullName).Hash}})|ConvertTo-Json -Compress}
$meta.versions[2].requiredIni=$bad;Write-Json "$($c.bundle)/bundle.json" $meta;$before=GamePins
Check ((InstallCase)-ne0) 'invalid metadata fails closed'
Check ((GamePins)-ceq$before) 'invalid metadata causes zero fixture game/preference/receipt changes'
$meta.versions[2].requiredIni=$required;Write-Json "$($c.bundle)/bundle.json" $meta;[IO.File]::Delete("$($c.bundle)/NativeRequiredIni.ps1")
Check ((InstallCase)-ne0) 'missing helper fails closed'
Check ((GamePins)-ceq$before) 'missing helper causes zero fixture game changes'
Put "$root/RESULT.json" (@{status='AUDIT_FIXTURE_PASS';checks=$checks.Count;names=@($checks);actualGameOrConfigTouched=$false;liveGameQueries=$false;sharedBuildMutexUsed=$false}|ConvertTo-Json -Depth 5)
'PASS TOTAL '+$checks.Count
