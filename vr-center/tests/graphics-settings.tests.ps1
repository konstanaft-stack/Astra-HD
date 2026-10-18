$ErrorActionPreference='Stop'
$app=Split-Path $PSScriptRoot -Parent
. (Join-Path $app 'GraphicsSettings.ps1')
function Check($value,$why){if(-not $value){throw $why};Write-Output ('PASS: '+$why)}
function Assert-GameWritable($Config){} # Only this disposable fixture; production guard remains intact.
$fixture=Join-Path $app ('audit/graphics-settings-'+[guid]::NewGuid().ToString('N'))
$c=@{game="$fixture/game";data="$fixture/data";settingsFile="$fixture/appdata/UserSettings.json"}
$j=@{unknownRoot='preserved';data=@(
 @{group_name='/graphics/presets';options=@(@{name='ResolutionScaling';type='string_list';is_dynamic=$true;value='DLSS';index=1},@{name='DLSS';type='string_list';is_dynamic=$true;value='Balanced';index=3},@{name='TextureQuality';type='string_list';value='High';index=2;values=@('Low','Medium','High')},@{name='UnknownFutureOption';type='int';value=42})},
 @{group_name='/graphics/raytracing';options=@(@{name='RayTracing';type='bool';value=$true},@{name='PathTracing';type='bool';value=$false})},
 @{group_name='/audio/volume';options=@(@{name='MasterVolume';type='int';value=64})}
)}
Write-Json $c.settingsFile $j
$view=Get-GraphicsSettings $c
$dlssView=@($view.options|Where-Object name -eq 'DLSS')[0]
Check ($view.available -and $dlssView.choices.Count -eq 6 -and $dlssView.choices[2] -ceq 'Quality' -and $dlssView.choices[3] -ceq 'Balanced') 'DLSS choices use verified value/index order'
$hash=$view.hash
$changes=@{DLSS='Quality';RayTracing=$false}
$result=Set-GraphicsSettings $c $hash $changes
$after=Read-Json $c.settingsFile
$dlss=@($after.data[0].options|Where-Object name -eq 'DLSS')[0]
Check ($dlss.value -ceq 'Quality' -and $dlss.index -eq 2) 'quality value and index change together'
Check ($after.data[1].options[0].value -eq $false -and $after.unknownRoot -ceq 'preserved' -and $after.data[2].options[0].value -eq 64 -and $after.data[0].options[3].value -eq 42) 'graphics merge preserves unrelated and unknown settings'
Check ($result.available -and $result.hash -cne $hash) 'save returns current hash for subsequent edits'
$saved=(Get-FileHash $c.settingsFile).Hash
$rejected=$false;try{Set-GraphicsSettings $c $hash @{DLSS='Performance'}|Out-Null}catch{$rejected=$true}
Check ($rejected -and (Get-FileHash $c.settingsFile).Hash -ceq $saved) 'stale editor cannot overwrite newer settings'
$rejected=$false;try{Set-GraphicsSettings $c $result.hash @{MasterVolume=0}|Out-Null}catch{$rejected=$true}
Check ($rejected -and (Get-FileHash $c.settingsFile).Hash -ceq $saved) 'only exposed graphics fields can be changed'
$preset=Get-GraphicsPreset 'balanced'
Check ($preset.DLSS -ceq 'Balanced' -and $preset.RayTracing -eq $false -and -not $preset.ContainsKey('DLSSFrameGen')) 'balanced preset changes graphics without enabling frame generation'

# Current PC game: the visible FG selector, not the old hidden bool, requires restart.
$definitions=@{options=@(
 @{group_name='/graphics/presets';options=@(@{name='FrameGeneration';update_policy='require_restart';is_dynamic=$true;in_game=$true},@{name='TextureQuality';update_policy='require_confirmation';in_game=$false})},
 @{group_name='/utilities';options=@(@{name='HybridUtilization';update_policy='require_restart';in_game=$true})},
 @{group_name='/audio/misc';options=@(@{name='NoSpatialAudio';update_policy='require_restart';in_game=$false})}
)}
Write-Json "$($c.game)/r6/config/settings/platform/pc/options.json" $definitions
$j.data[0].options+=@(@{name='FrameGeneration';type='string_list';is_dynamic=$true;value='Off';index=0},@{name='DLSSFrameGen';type='bool';value=$false},@{name='FSR3_FrameGeneration';type='bool';value=$true},@{name='XESS_FrameGeneration';type='bool';value=$false},@{name='DLSS_MultiFrameGeneration';type='string_list';value='x2';index=0;values=@('x2','x3','x4')})
$j.data+=@(@{group_name='/utilities';options=@(@{name='HybridUtilization';type='name_list';value='Auto';index=0;values=@('Auto','PrioritizePCores')})},@{group_name='/audio/misc';options=@(@{name='NoSpatialAudio';type='bool';value=$false})})
Write-Json $c.settingsFile $j
$view=Get-GraphicsSettings $c
$fg=@($view.options|Where-Object name -eq 'FrameGeneration')[0]
Check ($fg.editable -and $fg.choices[0] -ceq 'Off' -and $fg.choices[1] -ceq 'DLSS' -and $fg.requiresRestart) 'visible game FG selector uses confirmed Off/DLSS values and game restart policy'
Check (@($view.options|Where-Object name -eq 'DLSSFrameGen').Count -eq 0) 'hidden legacy FG bool is not a competing UI switch'
Check (@($view.options|Where-Object name -eq 'TextureQuality')[0].menuOnly -and -not @($view.options|Where-Object name -eq 'TextureQuality')[0].requiresRestart) 'main-menu-only textures are distinguished from restart requirement'
$view=Set-GraphicsSettings $c $view.hash @{FrameGeneration='DLSS';HybridUtilization='PrioritizePCores'}
$doc=Read-Json $c.settingsFile;$fgOptions=$doc.data[0].options
Check (@($fgOptions|Where-Object name -eq 'FrameGeneration')[0].value -ceq 'DLSS' -and @($fgOptions|Where-Object name -eq 'FrameGeneration')[0].index -eq 1 -and @($fgOptions|Where-Object name -eq 'DLSSFrameGen')[0].value -and -not @($fgOptions|Where-Object name -eq 'FSR3_FrameGeneration')[0].value) 'enabling game FG persists selector/index and consistent legacy flags'
$view=Set-GraphicsSettings $c $view.hash @{FrameGeneration='Off'}
$doc=Read-Json $c.settingsFile
Check (@($doc.data[0].options|Where-Object name -eq 'FrameGeneration')[0].index -eq 0 -and -not @($doc.data[0].options|Where-Object name -eq 'DLSSFrameGen')[0].value -and $doc.data[2].options[0].value -eq 64) 'disabling game FG clears its flag and leaves other audio fields intact'
Check (@($view.options|Where-Object name -eq 'HybridUtilization')[0].requiresRestart -and @($view.options|Where-Object name -eq 'NoSpatialAudio')[0].invertDisplay) 'CPU and audio restart settings are exposed with unambiguous spatial audio polarity'
Check (-not $preset.ContainsKey('FrameGeneration')) 'quality preset never changes the game FG selection'
Write-Output ('PASS: evidence '+$fixture)
