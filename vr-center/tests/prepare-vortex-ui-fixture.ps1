param([string]$Fixture)
$ErrorActionPreference='Stop'
$app=[IO.Path]::GetFullPath((Split-Path $PSScriptRoot -Parent))
$audit=[IO.Path]::GetFullPath((Join-Path $app 'audit'))
if(-not $Fixture){$Fixture=Join-Path $audit ('vortex-ui-'+[guid]::NewGuid().ToString('N'))}
$Fixture=[IO.Path]::GetFullPath($Fixture)
if(-not $Fixture.StartsWith($audit.TrimEnd('\')+'\',[StringComparison]::OrdinalIgnoreCase)){throw 'Fixture must be a new child of this worktree audit directory.'}
if(Test-Path -LiteralPath $Fixture){throw 'Fixture already exists; preserve it and choose a new directory.'}
function Put($path,$text){[IO.Directory]::CreateDirectory((Split-Path $path -Parent))|Out-Null;[IO.File]::WriteAllText($path,$text,[Text.UTF8Encoding]::new($false))}
function Json($path,$value){Put $path ($value|ConvertTo-Json -Depth 35)}
$pwsh=(Get-Command pwsh -ErrorAction Stop).Source
$c=@{
    game=(Join-Path $Fixture 'game');data=(Join-Path $Fixture 'data');bundle=(Join-Path $Fixture 'bundle')
    hold=(Join-Path $Fixture 'hold.json');settingsFile=(Join-Path $Fixture 'appdata/UserSettings.json');appDataPath=(Join-Path $Fixture 'appdata')
    userStateFile=(Join-Path $Fixture 'saves/user.gls');vortex=(Join-Path $Fixture 'no-vortex.exe');vortexData=(Join-Path $Fixture 'vortex')
    vortexCatalogFile=(Join-Path $Fixture 'vortex-catalog.json');gameProfilesEnabled=$false;pwsh=$pwsh
}
Put (Join-Path $c.game 'bin/x64/Cyberpunk2077.exe') 'fixture-not-executable-never-launched'
Json (Join-Path $c.bundle 'bundle.json') @{schemaVersion=1;versions=@(@{id='flat';kind='flat';family='Плоская игра';label='Плоская игра';available=$true;files=@{}});modules=@{};managedPaths=@()}
Json $c.hold @{active=$false}
Json $c.settingsFile @{data=@(@{group_name='/graphics/presets';options=@(@{name='FrameGeneration';type='string_list';is_dynamic=$true;value='Off';index=0},@{name='DLSSFrameGen';type='bool';value=$false},@{name='DLSS';type='string_list';is_dynamic=$true;value='Balanced';index=3},@{name='TextureQuality';type='string_list';value='High';index=2;values=@('Low','Medium','High')})},@{group_name='/graphics/raytracing';options=@(@{name='RayTracing';type='bool';value=$false})});untouched='keep'}
$staging=Join-Path $c.vortexData 'cyberpunk2077/mods'
$mod=Join-Path $staging 'mod-one'
Put (Join-Path $mod 'archive/pc/mod/one.archive') 'shared-fixture-archive'
Put (Join-Path $mod 'r6/scripts/one.reds') 'fixture-script'
$files=@(Get-ChildItem -LiteralPath $mod -File -Recurse|ForEach-Object{@{relative=[IO.Path]::GetRelativePath($mod,$_.FullName).Replace('\','/');source=$_.FullName;size=[long]$_.Length;modId='mod-one'}})
Json $c.vortexCatalogFile @{schema=1;root=$c.vortexData;stagingPath=$staging;activeProfileId='stable-super-ai-id';profiles=@(
    @{id='stable-super-ai-id';name='SuperCyberpunkAI';enabledCount=1;active=$true;available=$true;reason='';files=$files},
    @{id='unavailable-profile-id';name='Недоступный профиль';enabledCount=2;active=$false;available=$false;reason='Не найдены файлы установленного мода.';files=@()},
    @{id='empty-profile-id';name='Пустой профиль';enabledCount=0;active=$false;available=$true;reason='';files=@()}
)}
# The MO2 path exists only for the old collection harness regression check.
$mo=Join-Path $Fixture 'MO2 collection'
Put (Join-Path $mo 'ModOrganizer.ini') "[General]`ngameName=Cyberpunk 2077"
Put (Join-Path $mo 'profiles/Collection/modlist.txt') '+mod'
Put (Join-Path $mo 'mods/mod/mod.txt') 'enabled'
Json (Join-Path $Fixture 'config.json') $c
foreach($script in Get-ChildItem -LiteralPath $app -Filter '*.ps1' -File){Copy-Item -LiteralPath $script.FullName -Destination (Join-Path $Fixture $script.Name)}
if(Test-Path -LiteralPath (Join-Path $app 'integrations')){Copy-Item -LiteralPath (Join-Path $app 'integrations') -Destination (Join-Path $Fixture 'integrations') -Recurse}
$manifest=@(Get-ChildItem -LiteralPath $Fixture -File -Recurse|ForEach-Object{@{path=[IO.Path]::GetRelativePath($Fixture,$_.FullName);sha256=(Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash}})
Json (Join-Path $Fixture 'fixture-files.json') $manifest
Write-Output $Fixture
