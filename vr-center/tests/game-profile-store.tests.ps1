$ErrorActionPreference='Stop'
$module=Join-Path (Split-Path $PSScriptRoot -Parent) 'GameProfileStore.ps1'
if(-not(Test-Path -LiteralPath $module)){throw 'Game profile storage kernel missing: virtual catalog/autosave/isolation are not implemented.'}
. $module
$fixture=Join-Path $PSScriptRoot ('fixture-profile-store-'+[guid]::NewGuid().ToString('N'))
$game=Join-Path $fixture 'game';$settings=Join-Path $fixture 'settings/UserSettings.json';$data=Join-Path $fixture 'data'
$config=@{data=$data;scopes=@(
    @{id='game';root=$game;kind='directory';exclusions=@('saves');requiredFiles=@('bin/game.exe')},
    @{id='settings';root=$settings;kind='file';exclusions=@();requiredFiles=@('.')}
)}
function Assert-Test($ok,[string]$why){if(-not $ok){throw $why};Write-Output ('PASS: '+$why)}
function Reject([scriptblock]$work,[string]$why){$failed=$false;try{& $work|Out-Null}catch{$failed=$true};Assert-Test $failed $why}
function Put([string]$path,[string]$text){[IO.Directory]::CreateDirectory((Split-Path $path -Parent))|Out-Null;[IO.File]::WriteAllText($path,$text,[Text.UTF8Encoding]::new($true))}
function Text([string]$path){[IO.File]::ReadAllText($path)}
function Hash([string]$path){(Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash}
function Manifest([string]$id){Get-Content -LiteralPath "$data/profile-store/profiles/$id.json" -Raw|ConvertFrom-Json -AsHashtable}
function Object([string]$value){
    $source=Join-Path $fixture ('object-'+[guid]::NewGuid().ToString('N'));Put $source $value;$sha=Hash $source
    return Import-GameProfileObject -StoreConfig $config -SourcePath $source -ExpectedSha256 $sha
}
try{
    Put "$game/bin/game.exe" 'required-executable'
    Put "$game/mods/a.txt" 'A';Put "$game/cache/generated.json" '{"cache":"A"}'
    Put "$game/saves/progress.dat" 'progress-must-stay';Put $settings 'settings-A'
    $initial=Get-GameProfileCatalog -StoreConfig $config
    Assert-Test (-not $initial.initialized -and $initial.activeId -eq 'current') 'catalog is lazy virtual current'
    Assert-Test (-not(Test-Path -LiteralPath "$data/profile-store")) 'read-only catalog does not create store/CAS'
    $saved=Save-GameProfile -StoreConfig $config
    Assert-Test ($saved.initialized -and $saved.activeId -eq 'current') 'explicit save materializes current without changing ID'
    $manifest=Manifest 'current'
    $oldA=@($manifest.files|Where-Object path -eq 'mods/a.txt')[0].sha256
    Assert-Test (@($manifest.files|Where-Object path -eq 'cache/generated.json').Count -eq 1) 'scope policy snapshots cache/JSON ordinary files'
    Assert-Test (@($manifest.files|Where-Object path -like 'saves/*').Count -eq 0) 'progress exclusion is preserved'
    $again=Save-GameProfile -StoreConfig $config
    Assert-Test ($again.profiles.Count -eq 1 -and $again.activeId -eq 'current') 'repeated save does not create profile IDs'

    $exe=Object 'required-executable';$cleanSettings=Object 'settings-CLEAN';$cleanConfig=Object 'config-CLEAN'
    $template=@{schema=1;scopes=@(@{id='game';kind='directory'},@{id='settings';kind='file'});files=@(
        @{scope='game';path='bin/game.exe';sha256=$exe.sha256;size=$exe.size},
        @{scope='game';path='config/settings.ini';sha256=$cleanConfig.sha256;size=$cleanConfig.size},
        @{scope='settings';path='.';sha256=$cleanSettings.sha256;size=$cleanSettings.size}
    )}
    Import-GameProfileTemplate -StoreConfig $config -Id clean -Manifest $template|Out-Null
    Put "$game/mods/a.txt" 'A-edited';Put $settings 'settings-A-edited'
    $a='current';$b=(New-GameProfile -StoreConfig $config -Name B -TemplateId clean -VR).activeId
    Assert-Test ($b -match '^[a-f0-9]{32}$') 'new profile has independent stable GUID'
    Assert-Test (-not(Test-Path "$game/mods/a.txt") -and -not(Test-Path "$game/cache/generated.json")) 'new profile uses fresh template, not current mods/cache'
    Assert-Test ((Text $settings) -eq 'settings-CLEAN' -and (Text "$game/config/settings.ini") -eq 'config-CLEAN') 'template settings applied byte copies'
    Assert-Test ((Text "$game/saves/progress.dat") -eq 'progress-must-stay') 'excluded progress is not touched by apply'
    Put "$game/mods/b.txt" 'B-edited';Put $settings 'settings-B'
    Activate-GameProfile -StoreConfig $config -Id $a|Out-Null
    Assert-Test ((Text "$game/mods/a.txt") -eq 'A-edited' -and (Text $settings) -eq 'settings-A-edited') 'A→B autosaved current A edits'
    Assert-Test (-not(Test-Path "$game/mods/b.txt")) 'switch removes only target-missing ordinary files'
    Activate-GameProfile -StoreConfig $config -Id $b|Out-Null
    Assert-Test ((Text "$game/mods/b.txt") -eq 'B-edited' -and (Text $settings) -eq 'settings-B') 'B→A autosaved B and switch isolates it'
    $c=(New-GameProfile -StoreConfig $config -Name C -TemplateId clean).activeId
    Assert-Test ($c -ne $b -and -not(Test-Path "$game/mods/b.txt")) 'clean C starts fresh rather than cloning B'
    $aManifest=Manifest $a;$uniqueA=@($aManifest.files|Where-Object path -eq 'mods/a.txt')[0].sha256
    $sharedExe=@($aManifest.files|Where-Object path -eq 'bin/game.exe')[0].sha256
    $foreignObject=Object 'unreferenced-foreign-object'
    Remove-GameProfile -StoreConfig $config -Id $a|Out-Null
    $catalog=Get-GameProfileCatalog -StoreConfig $config
    Assert-Test ($catalog.activeId -eq $c -and @($catalog.profiles|Where-Object id -eq $b).Count -eq 1 -and -not(Test-Path "$data/profile-store/profiles/$a.json")) 'inactive delete preserves active and other profiles'
    Assert-Test (-not(Test-Path "$data/profile-store/objects/$uniqueA")) 'inactive delete collects its unique unreferenced CAS object'
    Assert-Test (-not(Test-Path "$data/profile-store/objects/$oldA")) 'inactive delete also collects obsolete autosave revisions owned by the same ID'
    Assert-Test ((Test-Path "$data/profile-store/objects/$sharedExe") -and (Test-Path "$data/profile-store/objects/$($foreignObject.sha256)")) 'delete preserves shared/base and unrelated CAS objects'
    Reject {Remove-GameProfile -StoreConfig $config -Id $c} 'active delete rejected'

    $layoutObject=Object 'layout-payload'
    Put "$game/config/layout-node" 'file-layout-before'
    Save-GameProfile -StoreConfig $config|Out-Null
    $layoutMeta=Hash "$data/profile-store/profiles/$c.json";$layoutCatalog=Hash "$data/profile-store/catalog.json"
    $layout=($template|ConvertTo-Json -Depth 15|ConvertFrom-Json -AsHashtable)
    $layout.files+=,@{scope='game';path='config/layout-node/child.ini';sha256=$layoutObject.sha256;size=$layoutObject.size}
    Import-GameProfileTemplate -StoreConfig $config -Id layout-dir -Manifest $layout|Out-Null
    Reject {New-GameProfile -StoreConfig $config -Name layout -TemplateId layout-dir} 'file-to-directory transition preflight rejected'
    Assert-Test ((Text "$game/config/layout-node") -eq 'file-layout-before' -and (Hash "$data/profile-store/profiles/$c.json") -eq $layoutMeta -and (Hash "$data/profile-store/catalog.json") -eq $layoutCatalog) 'file-to-directory rejection precedes autosave and all live mutation'
    [IO.File]::Delete("$game/config/layout-node")
    Put "$game/config/layout-node/child.ini" 'directory-layout-before'
    Save-GameProfile -StoreConfig $config|Out-Null
    $layoutMeta=Hash "$data/profile-store/profiles/$c.json";$layoutCatalog=Hash "$data/profile-store/catalog.json"
    $layout.files[-1].path='config/layout-node'
    Import-GameProfileTemplate -StoreConfig $config -Id layout-file -Manifest $layout|Out-Null
    Reject {New-GameProfile -StoreConfig $config -Name layout -TemplateId layout-file} 'directory-to-file transition preflight rejected'
    Assert-Test ((Text "$game/config/layout-node/child.ini") -eq 'directory-layout-before' -and (Hash "$data/profile-store/profiles/$c.json") -eq $layoutMeta -and (Hash "$data/profile-store/catalog.json") -eq $layoutCatalog) 'directory-to-file rejection precedes autosave and all live mutation'
    [IO.File]::Delete("$game/config/layout-node/child.ini");[IO.Directory]::Delete("$game/config/layout-node")
    Save-GameProfile -StoreConfig $config|Out-Null

    $originalSpace=(Get-Command Get-GameProfileVolumeSpace).ScriptBlock
    function Get-GameProfileVolumeSpace([string]$Path){@{volume=[IO.Path]::GetPathRoot($Path);availableBytes=0L}}
    Put "$game/cache/capacity.txt" 'new-CAS-needs-space'
    $newCas=Hash "$game/cache/capacity.txt";$spaceMeta=Hash "$data/profile-store/profiles/$c.json"
    Reject {Save-GameProfile -StoreConfig $config} 'insufficient store capacity rejected before CAS copy'
    Assert-Test (-not(Test-Path "$data/profile-store/objects/$newCas") -and (Hash "$data/profile-store/profiles/$c.json") -eq $spaceMeta) 'capacity failure does not partially save metadata/CAS'
    [IO.File]::Delete("$game/cache/capacity.txt")
    $spaceSettings=Hash $settings
    Reject {Activate-GameProfile -StoreConfig $config -Id $b} 'insufficient target volume capacity rejected before apply'
    Assert-Test ((Hash $settings) -eq $spaceSettings -and (Hash "$data/profile-store/profiles/$c.json") -eq $spaceMeta -and -not(Test-Path "$game/mods/b.txt")) 'target capacity rejection precedes autosave/live writes'
    Set-Item -Path Function:Get-GameProfileVolumeSpace -Value $originalSpace

    $beforeSettings=Hash $settings;$beforeExe=Hash "$game/bin/game.exe"
    $bad=($template|ConvertTo-Json -Depth 15|ConvertFrom-Json -AsHashtable);$bad.files=@()
    Reject {Import-GameProfileTemplate -StoreConfig $config -Id empty -Manifest $bad} 'empty template cannot omit required executable/settings'
    $bad=($template|ConvertTo-Json -Depth 15|ConvertFrom-Json -AsHashtable);$bad.schema='1'
    Reject {Import-GameProfileTemplate -StoreConfig $config -Id typed -Manifest $bad} 'manifest schema must be numeric'
    $bad=($template|ConvertTo-Json -Depth 15|ConvertFrom-Json -AsHashtable);$bad.files[0].size=$true
    Reject {Import-GameProfileTemplate -StoreConfig $config -Id typed -Manifest $bad} 'file size cannot be a coerced Boolean'
    $bad=($template|ConvertTo-Json -Depth 15|ConvertFrom-Json -AsHashtable);$bad.files[0].path='../outside.txt'
    Reject {Import-GameProfileTemplate -StoreConfig $config -Id traversal -Manifest $bad} 'manifest traversal rejected'
    Reject {Activate-GameProfile -StoreConfig $config -Id '../escape'} 'invalid profile ID rejected'
    $overlap=@{data=$game;scopes=$config.scopes}
    Reject {Save-GameProfile -StoreConfig $overlap} 'store/scope overlap rejected'
    $overlap=@{data=$data;scopes=@($config.scopes[0],@{id='nested';root="$game/config";kind='directory';requiredFiles=@();exclusions=@()})}
    Reject {Save-GameProfile -StoreConfig $overlap} 'overlapping scopes rejected'
    $junction=Join-Path $game 'linked';$outside=Join-Path $fixture 'outside'
    [IO.Directory]::CreateDirectory($outside)|Out-Null
    New-Item -ItemType Junction -Path $junction -Target $outside|Out-Null
    Reject {Save-GameProfile -StoreConfig $config} 'reparse scope entry rejected'
    Remove-Item -LiteralPath $junction -Force

    $bManifest=Manifest $b;$settingsObject=@($bManifest.files|Where-Object scope -eq settings)[0].sha256
    $objectPath="$data/profile-store/objects/$settingsObject";$originalBytes=[IO.File]::ReadAllBytes($objectPath)
    Put $objectPath 'corrupt-CAS'
    Reject {Activate-GameProfile -StoreConfig $config -Id $b} 'corrupt target CAS rejected before mutation'
    Assert-Test ((Hash $settings) -eq $beforeSettings -and (Hash "$game/bin/game.exe") -eq $beforeExe) 'corrupt CAS rejection leaves live files unchanged'
    [IO.File]::WriteAllBytes($objectPath,$originalBytes)

    $originalCopy=(Get-Command Copy-GameProfileFile).ScriptBlock;$script:copyCount=0
    function Copy-GameProfileFile([string]$Source,[string]$Target){$script:copyCount++;if($script:copyCount -eq 2){throw 'injected mid-copy failure'};& $originalCopy $Source $Target}
    Reject {Activate-GameProfile -StoreConfig $config -Id $b} 'mid-install copy failure rejected'
    Assert-Test ((Hash $settings) -eq $beforeSettings -and (Hash "$game/bin/game.exe") -eq $beforeExe -and (Get-GameProfileCatalog -StoreConfig $config).activeId -eq $c) 'rollback retains prior file bytes and active ID'
    Set-Item -Path Function:Copy-GameProfileFile -Value $originalCopy

    Put "$fixture/outside-hardlink.txt" 'hardlink-original'
    New-Item -ItemType HardLink -Path "$game/mods/shared.txt" -Target "$fixture/outside-hardlink.txt" -Force|Out-Null
    Put "$fixture/outside-config-link.txt" 'mutable-linked-config'
    [IO.File]::Delete("$game/config/settings.ini")
    New-Item -ItemType HardLink -Path "$game/config/settings.ini" -Target "$fixture/outside-config-link.txt"|Out-Null
    Save-GameProfile -StoreConfig $config|Out-Null
    $cManifest=Manifest $c;$shared=@($cManifest.files|Where-Object path -eq 'mods/shared.txt')[0].sha256
    Put "$fixture/outside-hardlink.txt" 'hardlink-edited'
    Assert-Test ((Hash "$data/profile-store/objects/$shared") -eq $shared) 'CAS copies do not share mutable hardlink bytes'
    Activate-GameProfile -StoreConfig $config -Id $b|Out-Null
    Assert-Test ((Text "$fixture/outside-hardlink.txt") -eq 'hardlink-edited') 'apply/removal never rewrites another hardlink path'
    Assert-Test ((Text "$fixture/outside-config-link.txt") -eq 'mutable-linked-config' -and (Text "$game/config/settings.ini") -eq 'config-CLEAN') 'apply replaces a hardlinked file without mutating its other link'
    $lock=[IO.File]::Open("$data/profile-store/store.lock",[IO.FileMode]::OpenOrCreate,[IO.FileAccess]::ReadWrite,[IO.FileShare]::None)
    try{Reject {Save-GameProfile -StoreConfig $config} 'concurrent store writer rejected'}finally{$lock.Dispose()}

    $script:copyCount=0
    function Copy-GameProfileFile([string]$Source,[string]$Target){& $originalCopy $Source $Target;$script:copyCount++;if($script:copyCount -eq 1){Put "$game/mods/external.txt" 'external-new-data'}}
    Reject {Activate-GameProfile -StoreConfig $config -Id $c} 'new external file during apply aborts verification'
    Assert-Test ((Text "$game/mods/external.txt") -eq 'external-new-data' -and (Get-GameProfileCatalog -StoreConfig $config).activeId -eq $b) 'rollback preserves unexpected new data and previous active ID'
    Set-Item -Path Function:Copy-GameProfileFile -Value $originalCopy
    [IO.File]::Delete("$game/mods/external.txt")

    $script:copyCount=0
    function Copy-GameProfileFile([string]$Source,[string]$Target){& $originalCopy $Source $Target;$script:copyCount++;if($script:copyCount -eq 1){Put "$game/config/settings.ini" 'foreign-replacement'}}
    Reject {Activate-GameProfile -StoreConfig $config -Id $c} 'foreign replacement during atomic apply fails closed'
    $blocked=Get-GameProfileCatalog -StoreConfig $config
    Assert-Test ((Text "$game/config/settings.ini") -eq 'foreign-replacement' -and $blocked.recoveryRequired.Count -eq 1 -and $blocked.activeId -eq $b) 'conflicting external bytes and journal retained instead of silently overwritten'
    Reject {Save-GameProfile -StoreConfig $config} 'unresolved transaction blocks further profile mutation'
    Reject {Remove-GameProfile -StoreConfig $config -Id $c} 'pending transaction blocks inactive profile deletion'
    Set-Item -Path Function:Copy-GameProfileFile -Value $originalCopy

    $recoveryId=$blocked.recoveryRequired[0]
    Reject {Repair-GameProfileTransaction -StoreConfig $config -Id $recoveryId} 'repair does not overwrite a foreign replacement'
    Assert-Test ((Text "$game/config/settings.ini") -eq 'foreign-replacement' -and (Get-GameProfileCatalog -StoreConfig $config).recoveryRequired.Count -eq 1) 'repair conflict preserves foreign bytes and recovery evidence'
    [IO.File]::Delete("$game/config/settings.ini")
    Repair-GameProfileTransaction -StoreConfig $config -Id $recoveryId|Out-Null
    Assert-Test ((Text "$game/config/settings.ini") -eq 'config-CLEAN' -and (Get-GameProfileCatalog -StoreConfig $config).recoveryRequired.Count -eq 0) 'repair restores original sibling and clears recovery barrier'

    $configFile=Join-Path $fixture 'crash-config.json';Put $configFile ($config|ConvertTo-Json -Depth 15)
    $crashScript=Join-Path $fixture 'crash.ps1'
    $crashBody=@'
param([string]$Module,[string]$ConfigFile,[string]$ProfileId,[string]$CrashAt='placed')
$ErrorActionPreference='Stop'
. $Module
$config=Get-Content -LiteralPath $ConfigFile -Raw|ConvertFrom-Json -AsHashtable
$prefix=[IO.Path]::GetFullPath($PSScriptRoot).TrimEnd('\')+'\'
foreach($scope in $config.scopes){if(-not [IO.Path]::GetFullPath($scope.root).StartsWith($prefix,[StringComparison]::OrdinalIgnoreCase)){throw 'Crash fixture cannot target a real game'}}
$writer=(Get-Command Write-GameProfileJournal).ScriptBlock
function Write-GameProfileJournal($Context,$Journal,[Nullable[int]]$OperationIndex=$null){
    if($CrashAt -eq 'placed' -and $null -ne $OperationIndex -and $Journal.operations[[int]$OperationIndex].phase -eq 'placed'){[Diagnostics.Process]::GetCurrentProcess().Kill()}
    & $writer $Context $Journal $OperationIndex
    if($CrashAt -in @('cleanup','new-cleanup') -and $Journal.state -eq 'cleanup-pending'){[Diagnostics.Process]::GetCurrentProcess().Kill()}
}
if($CrashAt -eq 'new-cleanup'){New-GameProfile -StoreConfig $config -Name 'no-op-crash' -TemplateId clean}else{Activate-GameProfile -StoreConfig $config -Id $ProfileId}
'@
    Put $crashScript $crashBody
    $beforeCrashSettings=Hash $settings;$beforeCrashConfig=Hash "$game/config/settings.ini"
    & (Get-Command pwsh).Source -NoProfile -File $crashScript -Module $module -ConfigFile $configFile -ProfileId $c
    Assert-Test ($LASTEXITCODE -ne 0) 'disposable child crashed after canonical first write'
    $crashed=Get-GameProfileCatalog -StoreConfig $config;$crashId=$crashed.recoveryRequired[0]
    Assert-Test ($crashed.recoveryRequired.Count -eq 1 -and (Hash "$game/config/settings.ini") -ne $beforeCrashConfig) 'crash leaves a real partial apply and pending journal'
    $progress=@(Read-GameProfileOperationProgress (Get-GameProfileContext $config) $crashId | Where-Object index -eq 0)[-1]
    Assert-Test ($progress.phase -eq 'renamed') 'filesystem write preceded durable progress receipt'
    Repair-GameProfileTransaction -StoreConfig $config -Id $crashId|Out-Null
    $repaired=Get-GameProfileCatalog -StoreConfig $config
    Assert-Test ((Hash $settings) -eq $beforeCrashSettings -and (Hash "$game/config/settings.ini") -eq $beforeCrashConfig -and $repaired.activeId -eq $b -and $repaired.recoveryRequired.Count -eq 0) 'repair infers actual post-bytes and restores BEFORE after process crash'
    Assert-Test (@(Get-ChildItem -LiteralPath $game -Recurse -File|Where-Object Name -like '*.profile-*').Count -eq 0) 'repaired crash does not leave transaction artifacts for the next snapshot'
    & (Get-Command pwsh).Source -NoProfile -File $crashScript -Module $module -ConfigFile $configFile -ProfileId $c
    $legacyCrash=(Get-GameProfileCatalog -StoreConfig $config).recoveryRequired[0]
    $legacyContext=Get-GameProfileContext $config
    foreach($step in (Read-GameProfileOperationProgress $legacyContext $legacyCrash)){
        Write-GameProfileJson "$data/profile-store/journals/$legacyCrash/operations/$('{0:D6}' -f [int]$step.index).json" $step
    }
    [IO.File]::Delete("$data/profile-store/journals/$legacyCrash/progress.jsonl")
    Repair-GameProfileTransaction -StoreConfig $config -Id $legacyCrash|Out-Null
    Assert-Test ((Hash $settings) -eq $beforeCrashSettings -and (Hash "$game/config/settings.ini") -eq $beforeCrashConfig -and (Get-GameProfileCatalog -StoreConfig $config).activeId -eq $b -and (Get-GameProfileCatalog -StoreConfig $config).recoveryRequired.Count -eq 0) 'legacy schema-1 operation JSON journal still repairs original bytes and active ID'
    & (Get-Command pwsh).Source -NoProfile -File $crashScript -Module $module -ConfigFile $configFile -ProfileId $c -CrashAt cleanup
    $cleanupCrash=Get-GameProfileCatalog -StoreConfig $config;$cleanupId=$cleanupCrash.recoveryRequired[0]
    Assert-Test ($LASTEXITCODE -ne 0 -and $cleanupCrash.activeId -eq $c -and $cleanupCrash.recoveryRequired.Count -eq 1) 'crash after catalog commit retains durable cleanup barrier'
    Reject {Save-GameProfile -StoreConfig $config} 'snapshot cannot inherit unfinished transaction siblings'
    Repair-GameProfileTransaction -StoreConfig $config -Id $cleanupId|Out-Null
    Assert-Test ((Hash $settings) -eq $beforeCrashSettings -and (Hash "$game/config/settings.ini") -eq $beforeCrashConfig -and (Get-GameProfileCatalog -StoreConfig $config).activeId -eq $b -and (Get-GameProfileCatalog -StoreConfig $config).recoveryRequired.Count -eq 0) 'cleanup crash recovery restores BEFORE and previous active ID'
    $cleanId=(New-GameProfile -StoreConfig $config -Name 'clean-noop-before' -TemplateId clean).activeId
    & (Get-Command pwsh).Source -NoProfile -File $crashScript -Module $module -ConfigFile $configFile -ProfileId $c -CrashAt new-cleanup
    $noop=Get-GameProfileCatalog -StoreConfig $config;$noopId=$noop.recoveryRequired[0]
    $noopPlan=ConvertFrom-Json -InputObject ([IO.File]::ReadAllText("$data/profile-store/journals/$noopId/plan.json")) -AsHashtable -NoEnumerate
    Assert-Test ($LASTEXITCODE -ne 0 -and $noopPlan -is [array] -and $noopPlan.Count -eq 0) 'no-op metadata crash has a valid empty JSON plan'
    Repair-GameProfileTransaction -StoreConfig $config -Id $noopId|Out-Null
    Assert-Test ((Get-GameProfileCatalog -StoreConfig $config).activeId -eq $cleanId -and (Get-GameProfileCatalog -StoreConfig $config).recoveryRequired.Count -eq 0) 'empty plan repair restores previous profile metadata without phantom operation'

    $baseWriter=(Get-Command Write-GameProfileJournal).ScriptBlock;$script:collision=$null
    function Write-GameProfileJournal($Context,$Journal,[Nullable[int]]$OperationIndex=$null){
        & $baseWriter $Context $Journal $OperationIndex
        if($Journal.state -eq 'applying' -and $null -eq $OperationIndex -and -not $script:collision){
            $op=$Journal.operations[0];$script:collision=(Get-GameProfileTargetPath $Context $op.scope $op.path)+$op.suffix+'.new'
            Put $script:collision 'preexisting-foreign-stage'
        }
    }
    Reject {Activate-GameProfile -StoreConfig $config -Id $c} 'planned stage collision rejects apply'
    Assert-Test ((Test-Path -LiteralPath $script:collision) -and (Text $script:collision) -eq 'preexisting-foreign-stage') 'rollback preserves a planned new sibling it never created'
    Set-Item -Path Function:Write-GameProfileJournal -Value $baseWriter
    $collisionCatalog=Get-GameProfileCatalog -StoreConfig $config
    Assert-Test ($collisionCatalog.activeId -eq $cleanId -and $collisionCatalog.recoveryRequired.Count -eq 1) 'foreign planned sibling retains recovery barrier/evidence'
    [IO.File]::Delete($script:collision)
    Repair-GameProfileTransaction -StoreConfig $config -Id $collisionCatalog.recoveryRequired[0]|Out-Null

    $script:foreignOld=$null;$script:retainedOriginal=Join-Path $fixture 'retained-original.bin'
    function Write-GameProfileJournal($Context,$Journal,[Nullable[int]]$OperationIndex=$null){
        & $baseWriter $Context $Journal $OperationIndex
        if($Journal.state -eq 'cleanup-pending' -and -not $script:foreignOld){
            $op=@($Journal.operations|Where-Object before)[0]
            $script:foreignOld=(Get-GameProfileTargetPath $Context $op.scope $op.path)+$op.suffix+'.old'
            [IO.File]::Move($script:foreignOld,$script:retainedOriginal,$false)
            Put $script:foreignOld 'foreign-after-commit'
        }
    }
    Reject {Activate-GameProfile -StoreConfig $config -Id $c} 'post-commit foreign sibling cleanup rejected'
    Assert-Test ((Test-Path -LiteralPath $script:foreignOld) -and (Text $script:foreignOld) -eq 'foreign-after-commit') 'normal cleanup preserves replaced original sibling bytes'
    Set-Item -Path Function:Write-GameProfileJournal -Value $baseWriter
    $cleanupForeign=Get-GameProfileCatalog -StoreConfig $config
    Assert-Test ($cleanupForeign.activeId -eq $c -and $cleanupForeign.recoveryRequired.Count -eq 1) 'post-commit foreign sibling remains pending rather than falsely committed'
    Reject {Save-GameProfile -StoreConfig $config} 'foreign cleanup sibling cannot enter next profile snapshot'
    [IO.File]::Delete($script:foreignOld);[IO.File]::Move($script:retainedOriginal,$script:foreignOld,$false)
    Repair-GameProfileTransaction -StoreConfig $config -Id $cleanupForeign.recoveryRequired[0]|Out-Null
    Write-Output 'PASS: isolated A/B/C profiles, lazy catalog, autosave/stable IDs, templates, removals, guards, CAS, rollback, hardlinks, inactive delete.'
}finally{
    if(Test-Path -LiteralPath (Join-Path $game 'linked')){Remove-Item -LiteralPath (Join-Path $game 'linked') -Force}
    $resolved=[IO.Path]::GetFullPath($fixture)
    if(-not $resolved.StartsWith([IO.Path]::GetFullPath($PSScriptRoot).TrimEnd('\')+'\fixture-profile-store-',[StringComparison]::OrdinalIgnoreCase)){throw 'Unsafe fixture cleanup path'}
    if(Test-Path -LiteralPath $resolved){Remove-Item -LiteralPath $resolved -Recurse -Force}
}
