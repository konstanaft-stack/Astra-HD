$ErrorActionPreference='Stop'
$app=Split-Path $PSScriptRoot -Parent
. (Join-Path $app 'TestSaves.ps1')
function Put($p,$text){[void][IO.Directory]::CreateDirectory((Split-Path $p -Parent));[IO.File]::WriteAllText($p,$text)}
function Check($ok,$why){if(-not $ok){throw $why};Write-Output ('PASS: '+$why)}
function Get-Process { [CmdletBinding()]param([string[]]$Name);if('Cyberpunk2077' -in $Name){return};Microsoft.PowerShell.Management\Get-Process @PSBoundParameters }
$f=Join-Path ([IO.Path]::GetTempPath()) ('test-save-fixture-'+[guid]::NewGuid().ToString('N'))
$c=@{data="$f/data";savedGamesPath="$f/saves"}
Put "$f/input/nested/ManualSave-12/sav.dat" 'source-A'
Put "$f/input/nested/ManualSave-12/screenshot.png" 'preview-A'
Put "$f/second/ManualSave-2/sav.dat" 'source-B'
Put "$f/saves/ManualSave-0/sav.dat" 'existing-user-save'
Put "$f/saves/user.gls" 'user-settings'
$a=Import-TestSaveSet $c "$f/input" 'Nexus set A'
$b=Import-TestSaveSet $c "$f/second" 'Nexus set B'
Check ((Get-TestSaveState $c).sets.Count -eq 2) 'imported sets remain in a shared expandable library'
Check (-not (Get-TestSaveState $c).enabled -and (Get-TestSaveState $c).selectedIds.Count -eq 2) 'new profile defaults to Off with all current library sets selected'
Set-TestSaveSelection $c @()
Check ((Get-TestSaveState $c).selectedIds.Count -eq 0) 'explicit empty selection is preserved instead of replaced with all sets'
Set-TestSaveSelection $c @($a.id)
Set-TestSavesEnabled $c $true
$state=Get-TestSaveState $c
Check ($state.enabled -and $state.installedCount -eq 1) 'On installs selected sets into current profile saves'
Check ([IO.File]::ReadAllText("$f/saves/ManualSave-0/sav.dat") -ceq 'existing-user-save') 'existing save slot collisions never overwrite user saves'
$own=@(Get-ChildItem "$f/saves" -Directory|Where-Object {Test-Path (Join-Path $_.FullName '.vr-center-test-save.json')})
Check ($own.Count -eq 1 -and $own[0].Name -ne 'ManualSave-0') 'new test saves receive a free ManualSave number'
Put (Join-Path $own[0].FullName 'sav.dat') 'user-played-A'
[void][IO.Directory]::CreateDirectory((Join-Path $own[0].FullName 'user-empty-directory'))
Set-TestSavesEnabled $c $true
Check ((Get-TestSaveState $c).installedCount -eq 1) 'repeated On is idempotent'
Set-TestSavesEnabled $c $false
Check ((Get-TestSaveState $c).installedCount -eq 0 -and [IO.File]::ReadAllText("$f/saves/user.gls") -ceq 'user-settings') 'Off parks only owned saves and never user.gls'
Set-TestSavesEnabled $c $true
$own=@(Get-ChildItem "$f/saves" -Directory|Where-Object {Test-Path (Join-Path $_.FullName '.vr-center-test-save.json')})
Check ([IO.File]::ReadAllText((Join-Path $own[0].FullName 'sav.dat')) -ceq 'user-played-A') 'Off On restores user changes from profile cache'
Check (Test-Path (Join-Path $own[0].FullName 'user-empty-directory') -PathType Container) 'empty directories created by the user survive Off On'
Set-TestSaveSelection $c @($b.id)
Check ((Get-TestSaveState $c).installedCount -eq 1) 'changing enabled selection safely reconnects only selected sets'
$own=@(Get-ChildItem "$f/saves" -Directory|Where-Object {Test-Path (Join-Path $_.FullName '.vr-center-test-save.json')})
Check ([IO.File]::ReadAllText((Join-Path $own[0].FullName 'sav.dat')) -ceq 'source-B') 'selection uses the chosen save source'
Set-TestSaveSelection $c @($a.id)
$own=@(Get-ChildItem "$f/saves" -Directory|Where-Object {Test-Path (Join-Path $_.FullName '.vr-center-test-save.json')})
Check ([IO.File]::ReadAllText((Join-Path $own[0].FullName 'sav.dat')) -ceq 'user-played-A') 'returning to previous set preserves played save changes'
Set-TestSavesEnabled $c $false
$profileRoot=Join-Path $c.data 'game-settings/test-saves'
[IO.Directory]::Move($profileRoot,"$f/profile-A")
Set-TestSaveSelection $c @($a.id)
Set-TestSavesEnabled $c $true
$own=@(Get-ChildItem "$f/saves" -Directory|Where-Object {Test-Path (Join-Path $_.FullName '.vr-center-test-save.json')})
Check ([IO.File]::ReadAllText((Join-Path $own[0].FullName 'sav.dat')) -ceq 'source-A') 'a new profile gets clean library copies instead of another profile changes'
Set-TestSavesEnabled $c $false
[IO.Directory]::Move($profileRoot,"$f/profile-B")
[IO.Directory]::Move("$f/profile-A",$profileRoot)
Set-TestSavesEnabled $c $true
$own=@(Get-ChildItem "$f/saves" -Directory|Where-Object {Test-Path (Join-Path $_.FullName '.vr-center-test-save.json')})
Check ([IO.File]::ReadAllText((Join-Path $own[0].FullName 'sav.dat')) -ceq 'user-played-A') 'return to original profile restores its own working save'
$realWriteJson=${function:Write-Json}
$script:failTestSaveCommit=$true
function Write-Json($Path,$Value){if($script:failTestSaveCommit -and $Path -eq (Join-Path $profileRoot 'state.json')){$script:failTestSaveCommit=$false;throw 'fixture commit failure'};& $realWriteJson $Path $Value}
$failed=$false;try{Set-TestSaveSelection $c @($b.id)}catch{$failed=$true}
Check ($failed -and (Get-TestSaveState $c).selectedIds[0] -eq $a.id -and (Get-TestSaveState $c).installedCount -eq 1) 'partial selection failure restores original state and active saves'
$own=@(Get-ChildItem "$f/saves" -Directory|Where-Object {Test-Path (Join-Path $_.FullName '.vr-center-test-save.json')})
Check ([IO.File]::ReadAllText((Join-Path $own[0].FullName 'sav.dat')) -ceq 'user-played-A') 'rollback preserves user edited bytes'
Check ([IO.File]::ReadAllText("$f/input/nested/ManualSave-12/sav.dat") -ceq 'source-A' -and [IO.File]::ReadAllText("$f/saves/ManualSave-0/sav.dat") -ceq 'existing-user-save') 'library import sources and unrelated saves remain unchanged'
$beforeStateHash=(Get-FileHash (Join-Path $profileRoot 'state.json')).Hash
function Get-Process { [CmdletBinding()]param([string[]]$Name);if('Cyberpunk2077' -in $Name){return @{Id=123}} }
$rejected=$false;try{Set-TestSavesEnabled $c $false}catch{$rejected=$_.Exception.Message -match 'закрой Cyberpunk'}
Check ($rejected -and (Get-FileHash (Join-Path $profileRoot 'state.json')).Hash -ceq $beforeStateHash) 'an open game rejects mutations before changing profile state'
Write-Output ('Fixture: '+$f)
