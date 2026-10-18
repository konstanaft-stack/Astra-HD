param([string]$Module=(Join-Path $PSScriptRoot '../../scripts/GenerationLaunchOwner.ps1'))
$ErrorActionPreference='Stop'
. $Module
function Settings($provider,$legacy=$true){return @{data=@(@{group_name='/graphics/presets';options=@(@{name='FrameGeneration';value=$provider},@{name='DLSSFrameGen';value=$legacy})})}}
function Check($pass,$message){if(!$pass){throw $message}}
Check ((Get-CpvrGenerationLaunchOwner (Settings 'Off')) -eq 'owned') 'Actual provider Off must choose own route even when obsolete DLSS checkbox is true'
Check ((Get-CpvrGenerationLaunchOwner (Settings 'DLSS' $false)) -eq 'game') 'DLSS provider must choose game route even when legacy checkbox is false'
foreach($provider in @('FSR3','XeSS','unknown')){
    $threw=$false;try{Get-CpvrGenerationLaunchOwner (Settings $provider)|Out-Null}catch{$threw=$true}
    Check $threw ('Unsupported provider must not enable another generator: '+$provider)
}
foreach($bad in @(@{data=@()},@{data=@(@{group_name='/graphics/presets';options=@()})},@{data=@(@{group_name='/graphics/presets';options=@(@{name='FrameGeneration';value='Off'},@{name='FrameGeneration';value='DLSS'})})})){
    $threw=$false;try{Get-CpvrGenerationLaunchOwner $bad|Out-Null}catch{$threw=$true}
    Check $threw 'Missing/duplicate provider must fail closed'
}
'PASS: startup owner follows authoritative provider; legacy bool ignored; unsupported, missing and ambiguous values blocked'
$start=[Diagnostics.ProcessStartInfo]::new();$start.UseShellExecute=$false
$start.EnvironmentVariables['XR_RUNTIME_JSON']='preserve-runtime'
$start.EnvironmentVariables['CPVR_NATIVE_FG_STEREO']='1'
$start.EnvironmentVariables['CPVR_NATIVE_FG_POLICY_ONLY']='1'
Check ((Set-CpvrGenerationLaunchOwner $start (Settings 'Off')) -eq 'owned') 'Off launch result'
Check (!$start.EnvironmentVariables.ContainsKey('CPVR_NATIVE_FG_STEREO')) 'Off must not force native adapter'
Check (!$start.EnvironmentVariables.ContainsKey('CPVR_NATIVE_FG_POLICY_ONLY')) 'Stale diagnostic observer removed'
Check ($start.EnvironmentVariables['XR_RUNTIME_JSON'] -eq 'preserve-runtime') 'Owner selection must not change XR runtime'
Check ((Set-CpvrGenerationLaunchOwner $start (Settings 'DLSS')) -eq 'game') 'DLSS launch result'
Check ($start.EnvironmentVariables['CPVR_NATIVE_FG_STEREO'] -eq '1' -and $start.EnvironmentVariables['CPVR_NATIVE_FG_HOST_VIEW_ONLY'] -eq '1') 'Game route must prepare both required native flags'
'PASS: child-only owner flags, stale flags removed, XR runtime preserved'
