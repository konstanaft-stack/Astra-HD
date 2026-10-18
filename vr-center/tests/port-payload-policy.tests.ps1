param([Parameter(Mandatory)][string]$Catalog)
$ErrorActionPreference='Stop'
$app=Split-Path $PSScriptRoot -Parent
$module=Join-Path $app 'PortPayloadPolicy.ps1'
if(-not(Test-Path -LiteralPath $module)){throw 'RED: collection-safe port payload policy is missing'}
. $module
function Check($ok,[string]$why){if(-not $ok){throw $why}}
function Reject([scriptblock]$action,[string]$why){$failed=$false;try{& $action|Out-Null}catch{$failed=$true};Check $failed $why}
$hash='A'*64
$port='red4ext/plugins/cyberpunkvr_stereo/cyberpunkvr_stereo.dll'
$fixture=@{
    versions=@(
        @{id='flat';kind='flat';files=@{'bin/x64/steam_api64.dll'=$hash;'@settings'=$hash}},
        @{id='astra';kind='native';label='Saved Astra';sourceCommit='known';files=@{
            $port=$hash;'bin/x64/vrport.ini'=$hash;'bin/x64/nvngx_dlssg.dll'=$hash;
            'bin/x64/plugins/cyber_engine_tweaks/mods/cyberpunkvrport_stereo/modules/camera.lua'=$hash;
            'red4ext/plugins/archivexl/archivexl.dll'=$hash;'bin/x64/plugins/cyber_engine_tweaks.asi'=$hash;
            'bin/x64/plugins/cyber_engine_tweaks/bindings.json'=$hash;'r6/cache/inputcontexts.xml'=$hash;
            'r6/scripts/equipmentex/equipmentex.reds'=$hash;'@settings'=$hash
        };noHandsFiles=@{$port=$hash;'red4ext/plugins/archivexl/archivexl.dll'=$hash}},
        @{id='luke';kind='luke';files=@{'bin/x64/dxgi.dll'=$hash;'bin/x64/realvr.ini'=$hash;'bin/x64/realrepo/cp2077/s0.dxbc'=$hash;'red4ext/plugins/codeware/codeware.dll'=$hash}},
        @{id='rawilder';kind='native';files=@{$port=$hash;'red4ext/plugins/cyberpunkvr_stereo/cyberpunkvr_visioncs.dxil'=$hash;'r6/scripts/cyberpunkvrport_weaponup/weapon.reds'=$hash;'engine/config/platform/pc/vrcam_cpu_tweaks.ini'=$hash}}
    )
    modules=@{fgFlat=@{'bin/x64/plugins/rtx40mfg.asi'=$hash;'bin/x64/global.ini'=$hash;'r6/scripts/equipmentex/equipmentex.reds'=$hash}}
    managedPaths=@($port,'bin/x64/dxgi.dll','bin/x64/vrport.ini','bin/x64/plugins/cyber_engine_tweaks/bindings.json','red4ext/plugins/archivexl/archivexl.dll','@settings','r6/cache/inputcontexts.xml')
}
$before=$fixture|ConvertTo-Json -Depth 30 -Compress
$safe=Get-CollectionSafePortDefinition $fixture 'astra'
$selected=@($safe.versions|Where-Object id -eq 'astra')[0]
Check ($selected.files.Count -eq 4 -and $selected.files[$port] -eq $hash) 'port bytes and own nested Lua survive; common framework must not be selected'
Check ($selected.id -eq 'astra' -and $selected.label -eq 'Saved Astra' -and $selected.sourceCommit -eq 'known') 'packaging must preserve version identity and source provenance'
Check ($selected.noHandsFiles.Count -eq 1) 'no-hands variant cannot reintroduce old frameworks'
Check (-not $selected.files.Contains('@settings')) 'old user settings must not enter selected port'
Check (-not $safe.versions[0].files.Contains('@settings') -and $safe.versions[0].files['bin/x64/steam_api64.dll'] -eq $hash) 'flat stock bytes survive; current settings sentinel is injected by adapter'
Check ($safe.modules.fgFlat.Count -eq 1) 'optional graphics module retains its plugin, excluding shared CET loader configuration'
Check ($safe.managedPaths.Count -eq 3) 'managed cleanup must not touch collection frameworks, input cache, global bindings or settings'
Check (($fixture|ConvertTo-Json -Depth 30 -Compress) -ceq $before) 'source bundle must not mutate'
$safe.versions[1].files[$port]='B'*64
Check ($fixture.versions[1].files[$port] -eq $hash) 'clone must not share mutable file maps'
Check ($safe.versions[2].files.Count -eq 3) 'Luke loader, settings and realrepo shader kept without Codeware'
Check ($safe.versions[3].files.Count -eq 4) 'Rawilder keeps renderer, shader, connected script and port-owned config'
Reject {Get-CollectionSafePortDefinition $fixture 'missing'} 'unknown version rejected'
$unsafe=@{versions=@(@{id='bad';kind='native';files=@{'../red4ext/plugins/cyberpunkvr_stereo/a.dll'=$hash}});modules=@{};managedPaths=@()}
Reject {Get-CollectionSafePortDefinition $unsafe 'bad'} 'unsafe relative payload rejected instead of silently accepted'

# The real saved artifact is read-only: prove exact payload identity and preserve
# every connected port-owned root while removing its unrelated snapshot content.
# The exact saved catalog is supplied explicitly by the developer.
if(-not(Test-Path -LiteralPath $catalog)){throw 'Saved bundle fixture unavailable'}
$original=Get-Content -LiteralPath $catalog -Raw|ConvertFrom-Json -AsHashtable
$originalBytes=$original|ConvertTo-Json -Depth 50 -Compress
$derived=Get-CollectionSafePortDefinition $original 'astra-hd-full-metadata-fps-20261015'
$sourceVersion=@($original.versions|Where-Object id -eq 'astra-hd-full-metadata-fps-20261015')[0]
$safeVersion=@($derived.versions|Where-Object id -eq 'astra-hd-full-metadata-fps-20261015')[0]
Check ($safeVersion.files[$port] -ceq '1AEBF60991379571EA5E7723EE1AD4B9FE0EF0DFA44AE0D1BE9DF25D45C588AF') 'exact saved 89-90 FPS DLL must survive packaging'
Check ($safeVersion.files['bin/x64/vrport.ini'] -ceq '0F33BBCB99A890F92088721D0E199EBA53CE0C9DC1C1A94A004273C750948B42') 'saved Astra port INI retained'
foreach($path in @('r6/input/cyberpunkvrport_scannerhud.xml','engine/config/platform/pc/vrcam_cpu_tweaks.ini')){
    if($sourceVersion.files.Contains($path)){Check ($safeVersion.files[$path] -eq $sourceVersion.files[$path] -and $safeVersion.files.Contains($path)) ('port-owned input/config piece dropped: '+$path)}
}
foreach($path in $sourceVersion.files.Keys){
    if($path -match '^(red4ext/plugins/cyberpunkvr_stereo/|bin/x64/plugins/cyber_engine_tweaks/mods/cyberpunkvrport_[^/]+/|r6/scripts/cyberpunkvrport_[^/]+/|r6/tweaks/(vrport|vrcigarette)/)' -or $path -match '^archive/pc/mod/(cyberpunkvrport|vrcigarette|vrport_)[^/]*$'){
        Check ($safeVersion.files[$path] -eq $sourceVersion.files[$path]) ('connected port piece dropped: '+$path)
    }
}
foreach($path in $safeVersion.files.Keys){
    Check ($path -notmatch '(?i)(archivexl|codeware|tweakxl|input_loader|mod_settings|equipmentex|novaoptics|visualholster|bulletprojectileoverhaul|moreoccluders|^r6/cache/|^r6/config/|^engine/(?!config/platform/pc/vrcam_cpu_tweaks\.ini$)|^@settings$|^bin/x64/plugins/cyber_engine_tweaks(?:\.asi|/(?!mods/)))') ('collection file leaked into port: '+$path)
}
foreach($versionId in @('luke27','tofu','rawilder_upd_a6f2b0f2cb61')){
    $other=Get-CollectionSafePortDefinition $original $versionId
    $version=@($other.versions|Where-Object id -eq $versionId)[0]
    Check ($version.files.Count -gt 0) ($versionId+' must retain an actual payload')
    Check (-not $version.files.Contains('red4ext/plugins/archivexl/archivexl.dll')) ($versionId+' must not shadow collection ArchiveXL')
    if($versionId -like 'rawilder*'){Check ($version.files.Contains('engine/config/platform/pc/vrcam_cpu_tweaks.ini')) 'Rawilder port-owned engine config retained'}
}
$hands=Get-CollectionSafePortDefinition $original 'luke263'
$handsVersion=@($hands.versions|Where-Object id -eq 'luke263')[0]
foreach($path in @('bin/x64/cpvr_handstracking/xr_apilayer_cpvr_input.dll','red4ext/plugins/cyberpunkhandsport/cyberpunkhandsport.dll','r6/input/cpvr_handsscannerhud.xml','r6/scripts/cpvr_handsscannerhud/vrport_scanner_hud.reds','bin/x64/plugins/cyber_engine_tweaks/mods/cpvr_handscontrols/init.lua')){
    Check ($handsVersion.files.Contains($path)) ('selected Luke controller package lost owned component: '+$path)
}
Check (($original|ConvertTo-Json -Depth 50 -Compress) -ceq $originalBytes) 'real bundle object unchanged'
Write-Output ('PASS: collection-safe payload policy; exact 1AEB/0F33 retained, '+$safeVersion.files.Count+' selected port files; no live writes')
