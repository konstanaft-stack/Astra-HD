$ErrorActionPreference='Stop'
. "$PSScriptRoot/../SettingsProfiles.ps1"
function Assert($test,$why){if(-not $test){throw $why}}
$root=Join-Path $PSScriptRoot ('fixture-settings-'+[guid]::NewGuid().ToString('N'))
$c=@{game="$root/game";data="$root/data";bundle="$root/bundle";hold="$root/hold.json";settingsFile="$root/player/UserSettings.json"}
# Process state is simulated only for this disposable fixture. Production guards
# remain intact and no live game, manager, config or AppData path is used.
function Get-Process {
    param([string[]]$Name,[int]$Id)
    if($Config.game -cne $c.game){throw 'Process simulation called outside its settings fixture.'}
    return @()
}
try{
New-Item -ItemType Directory -Force "$root/game/bin/x64","$root/bundle/files","$root/player","$root/data"|Out-Null
Write-Json $c.hold @{active=$false}
$state=@{receipt=@{version='native-test';kind='native'}}
$custom='{"data":[{"group_name":"/graphics/presets","options":[{"name":"DLSS","value":"Balanced","index":3},{"name":"FrameGeneration","value":"DLSS","index":1},{"name":"Sharpness","value":0.15}]},{"group_name":"/audio/volume","options":[{"name":"MasterVolume","value":73}]}]}'
$preset=$custom.Replace('Balanced','Quality').Replace('"value":"DLSS","index":1','"value":"Off","index":0').Replace('"value":0.15','"value":0.25').Replace('"value":73','"value":10')
[IO.File]::WriteAllText($c.settingsFile,$custom)
[IO.File]::WriteAllText("$root/preset.json",$preset)
$hash=(Get-FileHash "$root/preset.json").Hash
Copy-Item "$root/preset.json" "$root/bundle/files/$hash"
Write-Json "$root/bundle/bundle.json" @{versions=@(@{id='native-test';kind='native';files=@{'red4ext/plugins/cyberpunkvr_stereo/usersettings.json'=$hash}})}
[IO.File]::WriteAllText("$root/game/bin/x64/vrport.ini","first_launch=1`nother=23`n")
Assert (-not (Get-RecommendedSettingsMode $c $state)) 'Custom must be default'
Set-RecommendedSettingsMode $c $state $false -BeforeLaunch
Assert ([IO.File]::ReadAllText($c.settingsFile) -ceq $custom) 'Custom launch changed saved settings'
Assert ([IO.File]::ReadAllText("$root/game/bin/x64/vrport.ini") -ceq "first_launch=0`nother=23`n") 'First-launch preset not disabled narrowly'
Set-RecommendedSettingsMode $c $state $true
$recommended=Read-Json $c.settingsFile
Assert ($recommended.data[0].options[0].value -eq 'Quality') 'Preset not applied'
Assert ($recommended.data[1].options[0].value -eq 73) 'Personal audio setting overwritten'
Assert (Get-RecommendedSettingsMode $c $state) 'Recommended choice not saved'
$paths=Get-SettingsModePaths $c $state
$customBackupHash=(Get-FileHash -LiteralPath $paths.custom).Hash
# Simulate independent changes from the game after explicit preset enable.
$recommended.data[0].options[1].value='DLSS';$recommended.data[0].options[1].index=1
$recommended.data[0].options[2].value=0.9
Write-Json $c.settingsFile $recommended
$editedPlayer=[IO.File]::ReadAllText($c.settingsFile)
[IO.File]::WriteAllText($paths.ini,"first_launch=1`nother=23`n")
Set-RecommendedSettingsMode $c $state $true -BeforeLaunch
Assert ([IO.File]::ReadAllText($c.settingsFile) -ceq $editedPlayer) 'Recommended launch reset in-game FG or sharpness'
Assert ((Get-FileHash -LiteralPath $paths.custom).Hash -ceq $customBackupHash) 'Recommended launch rewrote saved custom settings'
Assert ([IO.File]::ReadAllText($paths.ini) -ceq "first_launch=0`nother=23`n") 'Recommended launch must still suppress only the native first-launch flag'
Set-RecommendedSettingsMode $c $state $true
$reapplied=Read-Json $c.settingsFile
Assert ($reapplied.data[0].options[1].value -ceq 'Off' -and $reapplied.data[0].options[1].index -eq 0 -and $reapplied.data[0].options[2].value -eq 0.25) 'Explicit preset action must still reapply recommended FG and sharpness'
Set-RecommendedSettingsMode $c $state $false
Assert ([IO.File]::ReadAllText($c.settingsFile) -ceq $custom) 'Custom settings not restored exactly'
Assert (-not (Get-RecommendedSettingsMode $c $state)) 'Custom choice not persisted'
$newCustom=$custom.Replace('Balanced','Performance')
[IO.File]::WriteAllText($c.settingsFile,$newCustom)
Set-RecommendedSettingsMode $c $state $false -BeforeLaunch
Assert ([IO.File]::ReadAllText($c.settingsFile) -ceq $newCustom) 'Old backup replaced newer custom changes'
Set-RecommendedSettingsMode $c $state $true
Set-RecommendedSettingsMode $c $state $false
Assert ([IO.File]::ReadAllText($c.settingsFile) -ceq $newCustom) 'New custom settings not captured on next preset enable'
'PASS: Custom and Recommended launches preserve game edits; explicit preset reapplies FG/sharpness; narrow first-launch flag; personal audio retained; exact custom restore and later edits.'
}finally{
    Remove-Item Function:/Get-Process -ErrorAction SilentlyContinue
    $resolved=[IO.Path]::GetFullPath($root)
    $prefix=[IO.Path]::GetFullPath($PSScriptRoot).TrimEnd('\')+'\fixture-settings-'
    if(-not $resolved.StartsWith($prefix,[StringComparison]::OrdinalIgnoreCase)){throw 'Unsafe settings fixture cleanup path'}
    if(Test-Path -LiteralPath $resolved){Remove-Item -LiteralPath $resolved -Recurse -Force}
}
