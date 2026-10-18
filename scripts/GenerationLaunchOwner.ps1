# Process-launch boundary only. Cyberpunk's FrameGeneration setting requires
# restart. This does not implement or claim a live GPU ownership handoff.
function Get-CpvrGenerationLaunchOwner {
    param([Parameter(Mandatory)][object]$Settings)
    $groups=@($Settings.data | Where-Object {$_.group_name -ceq '/graphics/presets'})
    if($groups.Count -ne 1){throw 'Не найден однозначный раздел настроек генерации кадров игры.'}
    $entries=@($groups[0].options | Where-Object {$_.name -ceq 'FrameGeneration'})
    if($entries.Count -ne 1){throw 'Не удалось однозначно прочитать игровой параметр FrameGeneration.'}
    switch -CaseSensitive ($entries[0].value) {
        'Off' { return 'owned' }
        'DLSS' { return 'game' }
        default { throw ('Для этой VR-сборки выберите в игре генерацию Off или DLSS. Сейчас выбрано: '+$entries[0].value) }
    }
}
function Set-CpvrGenerationLaunchOwner {
    param([Parameter(Mandatory)][Diagnostics.ProcessStartInfo]$StartInfo,[Parameter(Mandatory)][object]$Settings)
    $owner=Get-CpvrGenerationLaunchOwner -Settings $Settings
    foreach($name in @('CPVR_NATIVE_FG_STEREO','CPVR_NATIVE_FG_HOST_VIEW_ONLY','CPVR_NATIVE_FG_POLICY_ONLY')){
        $StartInfo.EnvironmentVariables.Remove($name)
    }
    $StartInfo.EnvironmentVariables['CPVR_FG_SESSION_OWNER']=$owner
    if($owner -eq 'game'){
        $StartInfo.EnvironmentVariables['CPVR_NATIVE_FG_STEREO']='1'
        $StartInfo.EnvironmentVariables['CPVR_NATIVE_FG_HOST_VIEW_ONLY']='1'
    }
    return $owner
}
