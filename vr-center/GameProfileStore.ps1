$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot 'ProfilePathChecks.ps1')

function Assert-GameProfileId([string]$Id){if($Id -cnotmatch '^(current|[0-9a-f]{32})$'){throw 'Invalid game profile ID.'}}
function Assert-GameProfileTemplateId([string]$Id){if($Id -cnotmatch '^[a-z][a-z0-9-]{0,63}$'){throw 'Invalid game profile template ID.'}}
function Assert-GameProfilePlainPath([string]$Path){
    [VRCenter.ProfilePathChecks]::Plain($Path)
}
function Assert-GameProfileRelativePath([string]$Path,[string]$Kind='directory'){
    [VRCenter.ProfilePathChecks]::Relative($Path,$Kind)
}
function Get-GameProfileStorePath($Context,[string]$Relative){
    if($Context.store -is [string] -and [IO.Path]::IsPathFullyQualified($Context.store)){return [VRCenter.ProfilePathChecks]::StorePath($Context.store,$Relative)}
    Assert-GameProfileRelativePath $Relative
    $path=[IO.Path]::GetFullPath((Join-Path $Context.store $Relative))
    if(-not $path.StartsWith($Context.store+'\',[StringComparison]::OrdinalIgnoreCase)){throw 'Profile store path escaped its root.'}
    Assert-GameProfilePlainPath $path
    return $path
}
function Test-GameProfileExcluded($Scope,[string]$Relative){
    if($Scope.exclusionPolicy -and $Scope.exclusionPolicy.culture -ceq [Globalization.CultureInfo]::CurrentCulture.Name){
        if($Scope.exclusionPolicy.rootOnly.Contains($Relative)){return $true}
        $cursor=$Relative
        while($cursor){
            if($Scope.exclusionPolicy.exact.Contains($cursor)){return $true}
            $slash=$cursor.LastIndexOf('/');if($slash -lt 0){break};$cursor=$cursor.Substring(0,$slash)
        }
        foreach($pattern in $Scope.exclusionPolicy.patterns){if($pattern.IsMatch($Relative)){return $true}}
        return $false
    }
    foreach($pattern in @($Scope.exclusions)){
        if($Relative -like $pattern -or $Relative -like ($pattern.TrimEnd('/')+'/*') -or ($pattern.EndsWith('/**') -and $Relative -ieq $pattern.Substring(0,$pattern.Length-3))){return $true}
    }
    return $false
}
function Initialize-GameProfileExclusions($Scope){
    $exact=[Collections.Generic.HashSet[string]]::new([StringComparer]::CurrentCultureIgnoreCase)
    $rootOnly=[Collections.Generic.HashSet[string]]::new([StringComparer]::InvariantCultureIgnoreCase)
    $protectedAncestors=[Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
    $protectedAllDirs=$false
    $patterns=[Collections.Generic.List[object]]::new()
    foreach($pattern in @($Scope.exclusions)){
        if($pattern -match '[*?]'){$protectedAllDirs=$true}
        $ancestor=$pattern
        while($ancestor.Contains('/')){$ancestor=$ancestor.Substring(0,$ancestor.LastIndexOf('/'));$null=$protectedAncestors.Add($ancestor)}
        if(-not [Management.Automation.WildcardPattern]::ContainsWildcardCharacters($pattern) -and -not $pattern.Contains('`')){$null=$exact.Add($pattern);continue}
        foreach($value in @($pattern,($pattern.TrimEnd('/')+'/*'))){$patterns.Add([Management.Automation.WildcardPattern]::new($value,[Management.Automation.WildcardOptions]::IgnoreCase))}
        if($pattern.EndsWith('/**')){$null=$rootOnly.Add($pattern.Substring(0,$pattern.Length-3))}
    }
    $Scope.exclusionPolicy=@{exact=$exact;rootOnly=$rootOnly;patterns=$patterns;culture=[Globalization.CultureInfo]::CurrentCulture.Name;protectedAncestors=$protectedAncestors;protectedAllDirs=$protectedAllDirs;hasExclusions=(@($Scope.exclusions).Count -gt 0)}
}
function Get-GameProfileTargetPath($Context,[string]$ScopeId,[string]$Relative){
    $scope=$Context.scopes[$ScopeId];if(-not $scope){throw 'Unknown profile scope.'}
    if($scope -is [Collections.IDictionary] -and $scope.root -is [string] -and $scope.root -and $scope.kind -in @('file','directory') -and $scope.exclusionPolicy -and $scope.exclusionPolicy.culture -ceq [Globalization.CultureInfo]::CurrentCulture.Name){return [VRCenter.ProfilePathChecks]::TargetPath($scope,$Relative)}
    Assert-GameProfileRelativePath $Relative $scope.kind
    if(Test-GameProfileExcluded $scope $Relative){throw 'Profile path is excluded by scope policy.'}
    $path=if($scope.kind -eq 'file'){$scope.root}else{[IO.Path]::GetFullPath((Join-Path $scope.root $Relative))}
    if($scope.kind -eq 'directory' -and -not $path.StartsWith($scope.root+'\',[StringComparison]::OrdinalIgnoreCase)){throw 'Profile path escaped its scope.'}
    Assert-GameProfilePlainPath $path
    return $path
}
function Get-GameProfileContext($StoreConfig){
    if(-not [IO.Path]::IsPathFullyQualified([string]$StoreConfig.data)){throw 'Profile data path must be absolute.'}
    $store=[IO.Path]::GetFullPath((Join-Path $StoreConfig.data 'profile-store')).TrimEnd('\')
    Assert-GameProfilePlainPath $store
    $scopes=@{};$required=0
    foreach($item in @($StoreConfig.scopes)){
        $id=[string]$item.id;$kind=([string]$item.kind).ToLowerInvariant()
        if($id -cnotmatch '^[a-z][a-z0-9-]{0,63}$' -or $scopes.ContainsKey($id) -or $kind -notin @('directory','file') -or -not [IO.Path]::IsPathFullyQualified([string]$item.root)){throw 'Invalid or duplicate profile scope.'}
        $root=[IO.Path]::GetFullPath([string]$item.root).TrimEnd('\')
        if($root -eq [IO.Path]::GetPathRoot($root).TrimEnd('\')){throw 'A drive root is not a profile scope.'}
        Assert-GameProfilePlainPath $root
        if($store -ieq $root -or $store.StartsWith($root+'\',[StringComparison]::OrdinalIgnoreCase) -or $root.StartsWith($store+'\',[StringComparison]::OrdinalIgnoreCase)){throw 'Profile store and scope overlap.'}
        foreach($other in $scopes.Values){if($root -ieq $other.root -or $root.StartsWith($other.root+'\',[StringComparison]::OrdinalIgnoreCase) -or $other.root.StartsWith($root+'\',[StringComparison]::OrdinalIgnoreCase)){throw 'Profile scopes overlap.'}}
        $scope=@{id=$id;root=$root;kind=$kind;exclusions=@();requiredFiles=@()}
        foreach($value in @($item.exclusions)){
            $pattern=([string]$value).Replace('\','/').TrimEnd('/')
            if(-not $pattern -or $kind -eq 'file' -or [IO.Path]::IsPathRooted($pattern) -or $pattern -match '[:"<>|\x00-\x1f]' -or @($pattern.Split('/')|Where-Object {$_ -in @('','.','..')}).Count){throw 'Invalid scope exclusion.'}
            $scope.exclusions+=,$pattern
        }
        Initialize-GameProfileExclusions $scope
        foreach($value in @($item.requiredFiles)){
            $relative=([string]$value).Replace('\','/')
            Assert-GameProfileRelativePath $relative $kind
            if(Test-GameProfileExcluded $scope $relative){throw 'A required file is excluded.'}
            if($relative -iin $scope.requiredFiles){throw 'Duplicate required file.'}
            $scope.requiredFiles+=,$relative;$required++
        }
        $scopes[$id]=$scope
    }
    if(-not $scopes.Count -or -not $required){throw 'At least one scope and an explicit required file are mandatory.'}
    return @{store=$store;scopes=$scopes;config=$StoreConfig}
}
function Read-GameProfileJson([string]$Path){if(-not(Test-Path -LiteralPath $Path)){return $null};Assert-GameProfilePlainPath $Path;return [IO.File]::ReadAllText($Path)|ConvertFrom-Json -AsHashtable -ErrorAction Stop}
function Move-GameProfileFile([string]$Source,[string]$Target,[bool]$Overwrite=$false){
    # Windows readers without FileShare.Delete can briefly hold a just-written
    # journal or payload. Retry only sharing/access errors; never change ACLs.
    $delays=@(50,100,150,250,400,500)
    for($attempt=0;;$attempt++){
        try{[IO.File]::Move($Source,$Target,$Overwrite);return}catch{
            $cause=$_.Exception.GetBaseException();$code=$cause.HResult -band 0xffff
            if($code -notin @(5,32,33) -or $attempt -ge $delays.Count){
                throw [IO.IOException]::new(('Profile file move failed: '+$Source+' -> '+$Target+'; '+$cause.Message),$cause)
            }
            Start-Sleep -Milliseconds $delays[$attempt]
        }
    }
}
function Write-GameProfileJson([string]$Path,$Value){
    Assert-GameProfilePlainPath $Path
    [IO.Directory]::CreateDirectory((Split-Path $Path -Parent))|Out-Null
    $temp=$Path+'.'+[guid]::NewGuid().ToString('N')+'.tmp'
    try{[IO.File]::WriteAllText($temp,(ConvertTo-Json -InputObject $Value -Depth 25),[Text.UTF8Encoding]::new($false));Move-GameProfileFile $temp $Path $true}finally{if(Test-Path -LiteralPath $temp){[IO.File]::Delete($temp)}}
}
function Lock-GameProfileStore($Context){
    Assert-GameProfilePlainPath $Context.store
    [IO.Directory]::CreateDirectory($Context.store)|Out-Null
    try{return [IO.File]::Open((Get-GameProfileStorePath $Context 'store.lock'),[IO.FileMode]::OpenOrCreate,[IO.FileAccess]::ReadWrite,[IO.FileShare]::None)}catch{throw ('Another profile operation is active or the store cannot be locked: '+$_.Exception.Message)}
}
function Get-GameProfileFileHash([string]$Path){
    Assert-GameProfilePlainPath $Path
    $stream=[IO.File]::Open($Path,[IO.FileMode]::Open,[IO.FileAccess]::Read,[IO.FileShare]::Read)
    try{return [Convert]::ToHexString([VRCenter.ProfilePathChecks]::HashStream($stream))}finally{$stream.Dispose()}
}
function Start-GameProfileCasSession($Context){
    if($null -ne $Context.casLeases){throw 'CAS verification session already active.'}
    $Context.casLeases=@{};$Context.casHashReads=0
}
function Stop-GameProfileCasSession($Context){
    if($null -ne $Context.casLeases){try{foreach($lease in $Context.casLeases.Values){$lease.stream.Dispose()}}finally{$Context.Remove('casLeases')}}
}
function Assert-GameProfileCasObject($Context,[string]$Path,[string]$Sha256,[long]$Size){
    Assert-GameProfilePlainPath $Path
    if($null -eq $Context.casLeases){
        if((Get-GameProfileFileHash $Path) -ne $Sha256 -or ([IO.FileInfo]::new($Path)).Length -ne $Size){throw 'Missing or corrupt profile CAS object.'};return
    }
    $expected=[IO.Path]::GetFullPath($Path);$key=$Sha256.ToUpperInvariant();$lease=$Context.casLeases[$key]
    if($lease){
        if($lease.path -cne $expected -or $lease.sha -cne $key -or $lease.size -ne $Size -or $lease.stream.Length -ne $Size){throw 'CAS verification lease identity mismatch.'}
        $resolved=[VRCenter.ProfilePathChecks]::FinalPath($lease.stream)
        if($resolved -and -not [string]::Equals($resolved,$expected,[StringComparison]::OrdinalIgnoreCase)){throw 'CAS verification lease path changed.'}
        if($resolved){return}
        # No native handle-path proof: preserve a fresh complete path hash.
        $lease.stream.Dispose();$Context.casLeases.Remove($key)
        if((Get-GameProfileFileHash $Path) -ne $key -or ([IO.FileInfo]::new($Path)).Length -ne $Size){throw 'Missing or corrupt profile CAS object.'};$Context.casHashReads++;return
    }
    $stream=[IO.File]::Open($Path,[IO.FileMode]::Open,[IO.FileAccess]::Read,[IO.FileShare]::Read)
    try{
        $actual=[Convert]::ToHexString([VRCenter.ProfilePathChecks]::HashStream($stream));$Context.casHashReads++
        if($actual -cne $key -or $stream.Length -ne $Size){throw 'Missing or corrupt profile CAS object.'}
        Assert-GameProfilePlainPath $Path
        $resolved=[VRCenter.ProfilePathChecks]::FinalPath($stream)
        if($resolved -and -not [string]::Equals($resolved,$expected,[StringComparison]::OrdinalIgnoreCase)){throw 'CAS verification lease path changed.'}
        if(-not $resolved){
            $stream.Dispose();$stream=$null
            if((Get-GameProfileFileHash $Path) -ne $key -or ([IO.FileInfo]::new($Path)).Length -ne $Size){throw 'Missing or corrupt profile CAS object.'};$Context.casHashReads++;return
        }
        $stream.Position=0
        $Context.casLeases[$key]=@{path=$expected;sha=$key;size=$Size;stream=$stream};$stream=$null
    }finally{if($null -ne $stream){$stream.Dispose()}}
}
function Sort-GameProfileRows([object[]]$Rows){
    foreach($row in $Rows){if($row -isnot [Collections.IDictionary] -or -not $row.Contains('scope') -or -not $row.Contains('path') -or $row.scope -isnot [string] -or $row.path -isnot [string]){return ,@($Rows|Sort-Object scope,path)}}
    return ,[VRCenter.ProfilePathChecks]::SortRows($Rows)
}
function Get-GameProfileInventory($Context,$Skip=@{}){
    if(Test-GameProfileNativePolicy $Context){return ,(Sort-GameProfileRows ([VRCenter.ProfilePathChecks]::Inventory($Context.scopes,$Skip)))}
    $files=[Collections.Generic.List[object]]::new()
    foreach($scope in $Context.scopes.Values){
        Assert-GameProfilePlainPath $scope.root
        if(-not(Test-Path -LiteralPath $scope.root)){continue}
        $rootAttributes=[IO.File]::GetAttributes($scope.root)
        if(($scope.kind -eq 'file') -eq [bool]($rootAttributes -band [IO.FileAttributes]::Directory)){throw 'Scope root kind does not match the filesystem.'}
        $pending=[Collections.Generic.Stack[string]]::new();$pending.Push($scope.root)
        while($pending.Count){
            $path=$pending.Pop();$attributes=[IO.File]::GetAttributes($path)
            if($attributes -band [IO.FileAttributes]::ReparsePoint){throw 'Reparse entry in a profile scope.'}
            $relative=if($scope.kind -eq 'file'){'.'}elseif($path -ieq $scope.root){''}else{$path.Substring($scope.root.Length+1).Replace('\','/')}
            if($relative -and (Test-GameProfileExcluded $scope $relative)){continue}
            if($attributes -band [IO.FileAttributes]::Directory){foreach($child in [IO.Directory]::EnumerateFileSystemEntries($path)){$pending.Push($child)};continue}
            if($Skip.ContainsKey($scope.id+'/'+$relative)){continue}
            # The enumerator supplied the absolute child under this trusted root,
            # and exclusion policy was checked above. Hashing below freshly checks
            # every ancestor for reparse points; keep relative-name validation here.
            Assert-GameProfileRelativePath $relative $scope.kind
            $size=([IO.FileInfo]::new($path)).Length
            $files.Add(@{scope=$scope.id;path=$relative;sha256=(Get-GameProfileFileHash $path);size=[long]$size})
        }
    }
    return ,(Sort-GameProfileRows $files.ToArray())
}
function Assert-GameProfileInventory($Expected,$Actual){
    if(@($Expected).Count -ne @($Actual).Count){throw ('Profile scope changed concurrently: file count differs ('+@($Expected).Count+' expected, '+@($Actual).Count+' actual).')}
    $map=@{};foreach($file in @($Expected)){$map[$file.scope+'/'+$file.path]=$file}
    foreach($file in @($Actual)){$prior=$map[$file.scope+'/'+$file.path];if(-not $prior -or $prior.sha256 -ne $file.sha256 -or [long]$prior.size -ne [long]$file.size){throw ('Profile scope changed concurrently: '+$file.scope+'/'+$file.path)}}
}
function Test-GameProfileProtectedDirectory($Scope,[string]$Relative){
    if($Relative -ne '.' -and (Test-GameProfileExcluded $Scope $Relative)){return $true}
    if($Scope.exclusionPolicy -and $Scope.exclusionPolicy.culture -ceq [Globalization.CultureInfo]::CurrentCulture.Name){
        return (($Relative -eq '.' -and $Scope.exclusionPolicy.hasExclusions) -or $Scope.exclusionPolicy.protectedAllDirs -or $Scope.exclusionPolicy.protectedAncestors.Contains($Relative))
    }
    foreach($pattern in $Scope.exclusions){
        # Excluded backup/shared trees keep their ancestors. Wildcard backup
        # rules can occur below any parent; that scope's directories stay shared.
        if($Relative -eq '.' -or $pattern -match '[*?]' -or $pattern.StartsWith($Relative+'/',[StringComparison]::OrdinalIgnoreCase)){return $true}
    }
    return $false
}
function Get-GameProfileDirectoryPath($Context,$Entry){
    $scope=$Context.scopes[$Entry.scope]
    if(-not $scope -or $scope.kind -ne 'directory'){throw 'Invalid directory profile scope.'}
    if($scope -is [Collections.IDictionary] -and $scope.root -is [string] -and $scope.root -and $Entry.path -is [string] -and $scope.exclusionPolicy -and $scope.exclusionPolicy.culture -ceq [Globalization.CultureInfo]::CurrentCulture.Name){return [VRCenter.ProfilePathChecks]::DirectoryPath($scope,$Entry.path)}
    $path=if($Entry.path -ceq '.'){$scope.root}else{Get-GameProfileTargetPath $Context $Entry.scope $Entry.path}
    Assert-GameProfilePlainPath $path;return $path
}
function Get-GameProfileDirectoryInventory($Context){
    if(Test-GameProfileNativePolicy $Context){return ,(Sort-GameProfileRows ([VRCenter.ProfilePathChecks]::DirectoryInventory($Context.scopes)))}
    $result=[Collections.Generic.List[object]]::new()
    foreach($scope in $Context.scopes.Values){
        if($scope.kind -ne 'directory' -or -not[IO.Directory]::Exists($scope.root)){continue}
        Assert-GameProfilePlainPath $scope.root
        $pending=[Collections.Generic.Stack[string]]::new();$pending.Push($scope.root)
        while($pending.Count){
            $path=$pending.Pop();$relative=if($path -ieq $scope.root){'.'}else{[IO.Path]::GetRelativePath($scope.root,$path).Replace('\','/')}
            if([IO.File]::GetAttributes($path) -band [IO.FileAttributes]::ReparsePoint){throw 'Reparse directory in profile inventory.'}
            if($relative -ne '.' -and (Test-GameProfileExcluded $scope $relative)){continue}
            if(-not(Test-GameProfileProtectedDirectory $scope $relative)){$result.Add(@{scope=$scope.id;path=$relative})}
            foreach($child in [IO.Directory]::EnumerateDirectories($path)){$pending.Push($child)}
        }
    }
    return ,(Sort-GameProfileRows $result.ToArray())
}
function Assert-GameProfileDirectories($Expected,$Actual){
    $left=@($Expected|ForEach-Object {$_.scope+'/'+$_.path}|Sort-Object)
    $right=@($Actual|ForEach-Object {$_.scope+'/'+$_.path}|Sort-Object)
    if(($left -join "`n") -ine ($right -join "`n")){throw 'Profile directory composition changed or was not restored exactly.'}
}
function Sync-GameProfileDirectories($Context,$Manifest){
    $wanted=@{}
    foreach($entry in $Manifest.directories){$wanted[$entry.scope+'/'+$entry.path]=$true;[IO.Directory]::CreateDirectory((Get-GameProfileDirectoryPath $Context $entry))|Out-Null}
    $actual=Get-GameProfileDirectoryInventory $Context
    foreach($entry in @($actual|Sort-Object @{Expression={$_.path.Length};Descending=$true})){
        if($wanted.ContainsKey($entry.scope+'/'+$entry.path)){continue}
        $path=Get-GameProfileDirectoryPath $Context $entry
        if([IO.Directory]::Exists($path)){
            if([IO.Directory]::GetFileSystemEntries($path).Count){throw ('Unexpected nonempty directory preserved: '+$path)}
            [IO.Directory]::Delete($path,$false)
        }
    }
    Assert-GameProfileDirectories $Manifest.directories (Get-GameProfileDirectoryInventory $Context)
}
function Save-GameProfileSourceObject($Context,[string]$Source,[string]$Sha256,[long]$Size){
    $target=Get-GameProfileStorePath $Context ('objects/'+$Sha256)
    if(Test-Path -LiteralPath $target){Assert-GameProfileCasObject $Context $target $Sha256 $Size;return}
    Assert-GameProfileCapacity @(@{path=$Context.store;bytes=$Size})
    [IO.Directory]::CreateDirectory((Split-Path $target -Parent))|Out-Null
    $temp=$target+'.'+[guid]::NewGuid().ToString('N')+'.tmp'
    Assert-GameProfilePlainPath $Source
    $sourceStream=[IO.File]::Open($Source,[IO.FileMode]::Open,[IO.FileAccess]::Read,[IO.FileShare]::Read)
    try{
        $output=[IO.File]::Open($temp,[IO.FileMode]::CreateNew,[IO.FileAccess]::Write,[IO.FileShare]::None)
        try{$sourceStream.CopyTo($output);$output.Flush($true)}finally{$output.Dispose()}
        if((Get-GameProfileFileHash $temp) -ne $Sha256 -or $sourceStream.Length -ne $Size){throw 'Source changed while snapshotting.'}
        Move-GameProfileFile $temp $target $false
    }finally{$sourceStream.Dispose();if(Test-Path -LiteralPath $temp){[IO.File]::Delete($temp)}}
}
function Save-GameProfileObject($Context,$Entry){Save-GameProfileSourceObject $Context (Get-GameProfileTargetPath $Context $Entry.scope $Entry.path) $Entry.sha256 ([long]$Entry.size)}
function Import-GameProfileObject {
    param($StoreConfig,[string]$SourcePath,[string]$ExpectedSha256)
    if(-not [IO.Path]::IsPathFullyQualified($SourcePath) -or $ExpectedSha256 -notmatch '^[0-9a-fA-F]{64}$'){throw 'Invalid CAS source path or expected SHA-256.'}
    Assert-GameProfilePlainPath $SourcePath
    if(-not(Test-Path -LiteralPath $SourcePath -PathType Leaf)){throw 'CAS source is not a regular file.'}
    $sha=$ExpectedSha256.ToUpperInvariant();$size=[long](Get-Item -LiteralPath $SourcePath).Length
    if((Get-GameProfileFileHash $SourcePath) -ne $sha){throw 'CAS source SHA-256 mismatch.'}
    $context=Get-GameProfileContext $StoreConfig;$lock=Lock-GameProfileStore $context
    try{
        # Explicit imports are base/foreign inputs, not objects owned by a user profile.
        Write-GameProfileJson (Get-GameProfileStorePath $context ('imports/'+$sha+'.json')) @{schema=1;sha256=$sha;size=$size}
        Save-GameProfileSourceObject $context $SourcePath $sha $size;return @{sha256=$sha;size=$size}
    }finally{$lock.Dispose()}
}
function Get-GameProfileMetadata($Manifest){return @{id=$Manifest.id;name=$Manifest.name;vr=[bool]$Manifest.vr;templateId=$Manifest.templateId;createdUtc=$Manifest.createdUtc;savedUtc=$Manifest.savedUtc;materialized=$true;complete=[bool]$Manifest.complete}}
function Get-GameProfileVirtualCatalog($Context){
    $name=if($Context.config.initialName){[string]$Context.config.initialName}else{'Текущий профиль'}
    return @{schema=1;initialized=$false;activeId='current';profiles=@(@{id='current';name=$name;vr=[bool]$Context.config.initialVR;templateId=$null;createdUtc=$null;savedUtc=$null;materialized=$false;complete=$null});recoveryRequired=@()}
}
function Read-GameProfileCatalog($Context){
    $catalog=Read-GameProfileJson (Get-GameProfileStorePath $Context 'catalog.json')
    if(-not $catalog){return Get-GameProfileVirtualCatalog $Context}
    if($catalog.schema -ne 1 -or ($catalog.schema -isnot [int] -and $catalog.schema -isnot [long]) -or $catalog.profiles -isnot [array] -or $catalog.initialized -isnot [bool] -or -not $catalog.initialized){throw 'Invalid profile catalog.'}
    Assert-GameProfileId $catalog.activeId
    $seen=@{};foreach($profile in $catalog.profiles){Assert-GameProfileId $profile.id;if($seen.ContainsKey($profile.id)){throw 'Duplicate profile catalog ID.'};$seen[$profile.id]=$true}
    if(-not $seen.ContainsKey($catalog.activeId)){throw 'Active profile is missing from catalog.'}
    return $catalog
}
function Get-GameProfileRecovery($Context){
    $pending=@();$root=Get-GameProfileStorePath $Context 'journals'
    if(Test-Path -LiteralPath $root){foreach($dir in @(Get-ChildItem -LiteralPath $root -Directory -Force)){
        if($dir.Name -cnotmatch '^[a-f0-9]{32}$'){throw 'Invalid journal directory.'}
        $receipt=Read-GameProfileJson (Get-GameProfileStorePath $Context ('journals/'+$dir.Name+'/receipt.json'))
        if(-not $receipt -or $receipt.state -notin @('committed','rolled-back','repaired') -or $receipt.cleanupComplete -isnot [bool] -or -not $receipt.cleanupComplete){$pending+=,$dir.Name}
    }}
    return ,$pending
}
function Assert-GameProfileNoRecovery($Context){$pending=Get-GameProfileRecovery $Context;if($pending.Count){throw 'An unfinished profile transaction requires recovery; journal and original files are retained.'}}
function Get-GameProfileCatalog {
    param($StoreConfig)
    $context=Get-GameProfileContext $StoreConfig
    $catalog=Read-GameProfileCatalog $context
    $catalog.recoveryRequired=Get-GameProfileRecovery $context
    return $catalog
}
function Test-GameProfileNativePolicy($Context){
    foreach($scope in $Context.scopes.Values){if(-not $scope.exclusionPolicy -or $scope.exclusionPolicy.culture -cne [Globalization.CultureInfo]::CurrentCulture.Name){return $false}}
    return $true
}
function Get-GameProfileManifestDirectories($Context,$Raw,$Files){
    $directories=@{}
    $native=Test-GameProfileNativePolicy $Context
    foreach($entry in @($Raw)){if($entry -and $entry -isnot [Collections.IDictionary]){$native=$false;break}}
    if($native){[VRCenter.ProfilePathChecks]::ManifestDirectories($Context.scopes,@($Raw),@($Files),$directories);return $directories}
    foreach($entry in @($Raw)){
        if(-not $entry){continue};$null=Get-GameProfileDirectoryPath $Context $entry
        if(-not(Test-GameProfileProtectedDirectory $Context.scopes[$entry.scope] $entry.path)){$directories[$entry.scope+'/'+$entry.path]=@{scope=$entry.scope;path=$entry.path}}
    }
    foreach($entry in $Files){
        $scope=$Context.scopes[$entry.scope];if($scope.kind -ne 'directory'){continue}
        $relative=$entry.path
        while($relative -ne '.'){
            $relative=if($relative.Contains('/')){$relative.Substring(0,$relative.LastIndexOf('/'))}else{'.'}
            if(-not(Test-GameProfileProtectedDirectory $scope $relative)){$directories[$entry.scope+'/'+$relative]=@{scope=$entry.scope;path=$relative}}
        }
    }
    return $directories
}
function ConvertTo-GameProfileManifest($Context,$Manifest,[switch]$RequireFiles){
    if($Manifest -and $Manifest -isnot [Collections.IDictionary]){$Manifest=$Manifest|ConvertTo-Json -Depth 25|ConvertFrom-Json -AsHashtable -ErrorAction Stop}
    if($Manifest.schema -ne 1 -or ($Manifest.schema -isnot [int] -and $Manifest.schema -isnot [long]) -or $Manifest.files -isnot [array] -or $Manifest.scopes -isnot [array] -or ($Manifest.ContainsKey('vr') -and $Manifest.vr -isnot [bool])){throw 'Invalid profile manifest schema.'}
    $scopes=@{};foreach($scope in $Manifest.scopes){if(-not $Context.scopes[$scope.id] -or $scope.kind -ne $Context.scopes[$scope.id].kind -or $scopes.ContainsKey($scope.id)){throw 'Manifest scope mapping mismatch.'};$scopes[$scope.id]=$true}
    if($scopes.Count -ne $Context.scopes.Count){throw 'Manifest does not declare every trusted scope.'}
    $seen=@{};$files=[Collections.Generic.List[object]]::new()
    foreach($entry in $Manifest.files){
        $null=Get-GameProfileTargetPath $Context ([string]$entry.scope) ([string]$entry.path)
        $key=$entry.scope+'/'+$entry.path
        if([string]$entry.scope -cne $Context.scopes[$entry.scope].id -or $seen.ContainsKey($key) -or [string]$entry.sha256 -notmatch '^[a-fA-F0-9]{64}$' -or ($entry.size -isnot [long] -and $entry.size -isnot [int]) -or [long]$entry.size -lt 0){throw 'Invalid or duplicate profile file entry.'}
        $seen[$key]=$true;$sha=([string]$entry.sha256).ToUpperInvariant()
        $object=Get-GameProfileStorePath $Context ('objects/'+$sha)
        Assert-GameProfileCasObject $Context $object $sha ([long]$entry.size)
        $files.Add(@{scope=[string]$entry.scope;path=[string]$entry.path;sha256=$sha;size=[long]$entry.size})
    }
    $missing=@();foreach($scope in $Context.scopes.Values){foreach($path in $scope.requiredFiles){if(-not $seen.ContainsKey($scope.id+'/'+$path)){$missing+=,@{scope=$scope.id;path=$path}}}}
    if($RequireFiles -and $missing.Count){throw 'Target/template manifest omits required files.'}
    if($Manifest.ContainsKey('directories') -and $Manifest.directories -isnot [array]){throw 'Invalid profile directory list.'}
    $directories=Get-GameProfileManifestDirectories $Context $Manifest.directories $files.ToArray()
    return @{schema=1;id=$Manifest.id;name=$Manifest.name;vr=[bool]$Manifest.vr;templateId=$Manifest.templateId;createdUtc=$Manifest.createdUtc;savedUtc=$Manifest.savedUtc;complete=($missing.Count -eq 0);missingRequired=$missing;scopes=@($Context.scopes.Values|Sort-Object id|ForEach-Object {@{id=$_.id;kind=$_.kind}});files=(Sort-GameProfileRows $files.ToArray());directories=(Sort-GameProfileRows @($directories.Values))}
}
function Save-GameProfileActive($Context,$Catalog,$Inventory=$null){
    $metadata=@($Catalog.profiles|Where-Object id -eq $Catalog.activeId)[0]
    $files=$Inventory;if($null -eq $Inventory){$files=Get-GameProfileInventory $Context}
    $directories=Get-GameProfileDirectoryInventory $Context
    $missing=@{};foreach($entry in $files){if(-not(Test-Path -LiteralPath (Get-GameProfileStorePath $Context ('objects/'+$entry.sha256)))){$missing[$entry.sha256]=$entry}}
    Assert-GameProfileCapacity @(@{path=$Context.store;bytes=[long](($missing.Values|Measure-Object size -Sum).Sum)})
    $ownerPath=Get-GameProfileStorePath $Context ('ownership/'+$metadata.id+'.json')
    $owner=Read-GameProfileJson $ownerPath;$owned=@{}
    if($owner){if($owner.id -cne $metadata.id -or $owner.hashes -isnot [array]){throw 'Invalid profile CAS ownership ledger.'};foreach($sha in $owner.hashes){if($sha -notmatch '^[0-9A-Fa-f]{64}$'){throw 'Invalid owned CAS hash.'};$owned[$sha]=$true}}
    foreach($entry in $files){$owned[$entry.sha256]=$true}
    Write-GameProfileJson $ownerPath @{schema=1;id=$metadata.id;hashes=@($owned.Keys|Sort-Object)}
    foreach($entry in $files){Save-GameProfileObject $Context $entry}
    Assert-GameProfileInventory $files (Get-GameProfileInventory $Context)
    Assert-GameProfileDirectories $directories (Get-GameProfileDirectoryInventory $Context)
    $now=[datetime]::UtcNow.ToString('o')
    $manifest=@{schema=1;id=$metadata.id;name=$metadata.name;vr=[bool]$metadata.vr;templateId=$metadata.templateId;createdUtc=$(if($metadata.createdUtc){$metadata.createdUtc}else{$now});savedUtc=$now;scopes=@($Context.scopes.Values|ForEach-Object {@{id=$_.id;kind=$_.kind}});files=@($files);directories=$directories}
    $manifest=ConvertTo-GameProfileManifest $Context $manifest
    Write-GameProfileJson (Get-GameProfileStorePath $Context ('profiles/'+$manifest.id+'.json')) $manifest
    $Catalog.profiles=@($Catalog.profiles|ForEach-Object {if($_.id -eq $manifest.id){Get-GameProfileMetadata $manifest}else{$_}})
    $Catalog.initialized=$true;$Catalog.recoveryRequired=@()
    Write-GameProfileJson (Get-GameProfileStorePath $Context 'catalog.json') $Catalog
    return @{catalog=$Catalog;manifest=$manifest}
}
function Move-GameProfileEnhancementToShared {
    param($StoreConfig,[Parameter(Mandatory)][string[]]$Paths,[Parameter(Mandatory)][string]$BackupDirectory)
    function Read-MigrationJson([string]$Path){
        Assert-GameProfilePlainPath $Path
        return [IO.File]::ReadAllText($Path)|ConvertFrom-Json -AsHashtable -DateKind String -ErrorAction Stop
    }
    $context=Get-GameProfileContext $StoreConfig;$lock=Lock-GameProfileStore $context;$plans=@();$backupReady=$false
    try{
        Assert-GameProfileNoRecovery $context
        if(Test-Path -LiteralPath $BackupDirectory){throw 'Migration backup directory already exists.'}
        $owned=@{};foreach($path in $Paths){Assert-GameProfileRelativePath $path;if($path -notlike 'bin/x64/*' -or $path -ieq 'bin/x64/nvngx_dlssg.dll'){throw 'Migration accepts only shared bin/x64 payload, never root FG.'};$owned[$path]=$true}
        $plans=@();$objects=@();$catalog=Read-GameProfileCatalog $context
        $catalogPath=Get-GameProfileStorePath $context 'catalog.json'
        if(Test-Path -LiteralPath $catalogPath){$catalog=Read-MigrationJson $catalogPath}
        foreach($kind in @('profiles','templates')){
            $folder=Get-GameProfileStorePath $context $kind
            foreach($item in @(Get-ChildItem -LiteralPath $folder -File -Filter '*.json' -ErrorAction SilentlyContinue)){
                $manifest=Read-MigrationJson $item.FullName
                if($manifest.schema -ne 1 -or $manifest.files -isnot [array]){throw 'Invalid migration manifest.'}
                $next=$manifest|ConvertTo-Json -Depth 100|ConvertFrom-Json -AsHashtable -DateKind String
                $next.files=@(foreach($entry in $manifest.files){
                    if(($entry.scope -eq 'game' -and $owned.ContainsKey($entry.path)) -or
                       ($entry.scope -eq 'collection' -and $entry.path.StartsWith('overwrite/') -and $owned.ContainsKey($entry.path.Substring(10)))){continue}
                    if($entry.scope -eq 'game' -and $entry.path -eq 'CPVR-Portable/installed.json'){
                        $source=Get-GameProfileStorePath $context ('objects/'+$entry.sha256)
                        if(-not(Test-Path -LiteralPath $source) -or (Get-FileHash -LiteralPath $source).Hash -ine $entry.sha256){throw 'Unverified receipt CAS object.'}
                        $receipt=Read-MigrationJson $source
                        if($receipt){
                            $critical=@();if($receipt.critical -is [Collections.IDictionary]){foreach($key in $receipt.critical.Keys){if(-not $owned.ContainsKey($key)){$critical+=@{path=$key;sha256=$receipt.critical[$key]}}}}
                            else{$critical=@($receipt.critical|Where-Object {-not $owned.ContainsKey([string]$_.path)})}
                            $receipt.critical=$critical;$receipt.paths=@($receipt.paths|Where-Object {-not $owned.ContainsKey([string]$_)})
                            $bytes=[Text.UTF8Encoding]::new($false).GetBytes(($receipt|ConvertTo-Json -Depth 100))
                            $sha=[Convert]::ToHexString([Security.Cryptography.SHA256]::HashData($bytes))
                            $objects+=@{sha=$sha;bytes=$bytes}
                            @{scope=$entry.scope;path=$entry.path;sha256=$sha;size=[long]$bytes.Length};continue
                        }
                    }
                    $entry
                })
                $plans+=@{path=$item.FullName;kind=$kind;name=$item.Name;beforeHash=(Get-FileHash -LiteralPath $item.FullName).Hash;next=$next}
            }
        }
        [IO.Directory]::CreateDirectory($BackupDirectory)|Out-Null
        foreach($plan in $plans){$dir=Join-Path $BackupDirectory $plan.kind;[IO.Directory]::CreateDirectory($dir)|Out-Null;[IO.File]::Copy($plan.path,(Join-Path $dir $plan.name),$false)}
        $catalogPath=Get-GameProfileStorePath $context 'catalog.json'
        if(Test-Path -LiteralPath $catalogPath){[IO.File]::Copy($catalogPath,(Join-Path $BackupDirectory 'catalog.json'),$false)}
        $backupReady=$true
        Write-GameProfileJson (Join-Path $BackupDirectory 'plan.json') @{schema=1;paths=$Paths;manifests=@($plans|ForEach-Object {@{path=$_.path;beforeHash=$_.beforeHash}});state='prepared'}
        foreach($object in $objects){
            $target=Get-GameProfileStorePath $context ('objects/'+$object.sha)
            if(-not(Test-Path -LiteralPath $target)){[IO.File]::WriteAllBytes($target,$object.bytes)}
            if((Get-FileHash -LiteralPath $target).Hash -cne $object.sha){throw 'Migration CAS collision/corruption.'}
        }
        foreach($plan in $plans){
            if((Get-FileHash -LiteralPath $plan.path).Hash -cne $plan.beforeHash){throw 'Manifest changed during migration.'}
            Write-GameProfileJson $plan.path $plan.next
            if($plan.kind -eq 'profiles'){$catalog.profiles=@($catalog.profiles|ForEach-Object {if($_.id -eq $plan.next.id){Get-GameProfileMetadata $plan.next}else{$_}})}
        }
        if(Test-Path -LiteralPath $catalogPath){Write-GameProfileJson $catalogPath $catalog}
        Write-GameProfileJson (Join-Path $BackupDirectory 'complete.json') @{schema=1;state='complete';paths=$Paths;manifests=$plans.Count;casDeleted=0}
        return $BackupDirectory
    }catch{
        $failure=$_.Exception.Message
        if($backupReady){
            foreach($plan in $plans){$saved=Join-Path (Join-Path $BackupDirectory $plan.kind) $plan.name;if(Test-Path -LiteralPath $saved){[IO.File]::Copy($saved,$plan.path,$true)}}
            $savedCatalog=Join-Path $BackupDirectory 'catalog.json';if(Test-Path -LiteralPath $savedCatalog){[IO.File]::Copy($savedCatalog,(Get-GameProfileStorePath $context 'catalog.json'),$true)}
        }
        throw $failure
    }finally{$lock.Dispose()}
}
function Save-GameProfile {
    param($StoreConfig)
    $context=Get-GameProfileContext $StoreConfig;$lock=Lock-GameProfileStore $context
    try{Start-GameProfileCasSession $context;Assert-GameProfileNoRecovery $context;return (Save-GameProfileActive $context (Read-GameProfileCatalog $context)).catalog}finally{try{Stop-GameProfileCasSession $context}finally{$lock.Dispose()}}
}
function Import-GameProfileTemplate {
    param($StoreConfig,[string]$Id,$Manifest)
    Assert-GameProfileTemplateId $Id
    $context=Get-GameProfileContext $StoreConfig;$lock=Lock-GameProfileStore $context
    try{
        $normalized=ConvertTo-GameProfileManifest $context $Manifest -RequireFiles
        $normalized.id=$Id;$normalized.name=$Id;$normalized.vr=[bool]$Manifest.vr;$normalized.templateId=$Id
        Write-GameProfileJson (Get-GameProfileStorePath $context ('templates/'+$Id+'.json')) $normalized
        return $normalized
    }finally{$lock.Dispose()}
}
function Copy-GameProfileFile([string]$Source,[string]$Target){
    Assert-GameProfilePlainPath $Source;Assert-GameProfilePlainPath $Target
    [IO.Directory]::CreateDirectory((Split-Path $Target -Parent))|Out-Null
    [IO.File]::Copy($Source,$Target,$false)
}
function Close-GameProfileJournalProgress($Journal){
    if($Journal -and $Journal.ContainsKey('_progressWriter')){
        try{$Journal._progressWriter.Dispose()}finally{$Journal.Remove('_progressWriter');$Journal.Remove('_progressKey')}
    }
}
function Read-GameProfileOperationProgress($Context,[string]$Id){
    if($Id -cnotmatch '^[a-f0-9]{32}$'){throw 'Invalid progress journal ID.'}
    $base='journals/'+$Id+'/'
    $legacy=Get-GameProfileStorePath $Context ($base+'operations')
    if(Test-Path -LiteralPath $legacy){foreach($file in @(Get-ChildItem -LiteralPath $legacy -File -Filter '*.json' -Force)){
        $step=Read-GameProfileJson (Get-GameProfileStorePath $Context ($base+'operations/'+$file.Name))
        if($null -ne $step.originalHash -and ($step.originalHash -isnot [string] -or ($step.originalHash -and $step.originalHash -notmatch '^[0-9a-fA-F]{64}$'))){throw 'Invalid recovery CAS reference.'}
        $step
    }}
    $path=Get-GameProfileStorePath $Context ($base+'progress.jsonl')
    if(-not(Test-Path -LiteralPath $path -PathType Leaf)){return}
    $planPath=Get-GameProfileStorePath $Context ($base+'plan.json')
    $plan=ConvertFrom-Json -InputObject ([IO.File]::ReadAllText($planPath)) -AsHashtable -NoEnumerate -ErrorAction Stop
    if($plan -isnot [array]){throw 'Progress plan must be an array.'}
    $sequence=0L
    $reader=[VRCenter.JournalProgressWriter+Reader]::new($path)
    try{while($reader.MoveNext()){
        $line=$reader.Current
        $record=ConvertFrom-Json -InputObject $line -AsHashtable -ErrorAction Stop
        if($record -isnot [Collections.IDictionary] -or ($record.schema -isnot [int] -and $record.schema -isnot [long]) -or $record.schema -ne 1 -or $record.id -cne $Id -or
           ($record.sequence -isnot [int] -and $record.sequence -isnot [long]) -or $record.sequence -ne ($sequence+1)){
            throw 'Invalid journal progress identity or sequence.'
        }
        $op=$record.operation
        if($op -isnot [Collections.IDictionary] -or ($op.index -isnot [int] -and $op.index -isnot [long]) -or $op.index -lt 0 -or $op.index -ge $plan.Count){throw 'Invalid journal progress index.'}
        $expected=$plan[[int]$op.index]
        if($expected.index -ne $op.index -or $op.scope -cne $expected.scope -or $op.path -cne $expected.path -or $op.suffix -cne $expected.suffix -or
           $op.phase -notin @('planned','renamed','placed','removed') -or $op.originalOwned -isnot [bool] -or $op.stagedOwned -isnot [bool] -or
           ($null -ne $op.originalHash -and ($op.originalHash -isnot [string] -or ($op.originalHash -and $op.originalHash -notmatch '^[0-9a-fA-F]{64}$')))){throw 'Invalid journal progress operation identity.'}
        foreach($part in @('before','after')){
            if([bool]$op[$part] -ne [bool]$expected[$part]){throw 'Progress operation differs from immutable plan.'}
            if($expected[$part]){foreach($field in @('scope','path','sha256','size')){if($op[$part][$field] -cne $expected[$part][$field]){throw 'Progress operation differs from immutable plan.'}}}
        }
        $sequence=[long]$record.sequence
        $op
    }}finally{$reader.Dispose()}
}
function Write-GameProfileJournal($Context,$Journal,[Nullable[int]]$OperationIndex=$null){
    $base='journals/'+$Journal.id+'/'
    $progressKey=[IO.Path]::GetFullPath($Context.store)+'|'+$Journal.id
    $writer=$null
    if($Journal._progressOwned -and $Journal.ContainsKey('_progressWriter') -and -not $Journal._progressFailed){
        if($Journal._progressKey -cne $progressKey){throw 'Progress writer belongs to another journal or store.'}
        $writer=$Journal._progressWriter
    }
    # Large manifests/plan are written once. Progress records stay small even
    # when a collection has thousands of files; no quadratic JSON write volume.
    $partPaths=@()
    if($null -eq $writer){foreach($part in @('before','target','plan')){
        $path=Get-GameProfileStorePath $Context ($base+$part+'.json')
        $partPaths+=,$path
        if(-not(Test-Path -LiteralPath $path)){$value=$Journal[$part];if($part -eq 'plan'){$value=$Journal.operations};Write-GameProfileJson $path $value}
    }}elseif($null -eq $OperationIndex){$writer.Validate()}
    if($null -ne $OperationIndex){
        if($OperationIndex -lt 0 -or $OperationIndex -ge $Journal.operations.Count -or $Journal.operations[[int]$OperationIndex].index -ne $OperationIndex){throw 'Invalid operation progress index.'}
        if(-not $Journal._progressOwned){
            # Existing schema-1 repair/diagnostic callers retain their writer.
            Write-GameProfileJson (Get-GameProfileStorePath $Context ($base+'operations/'+('{0:D6}' -f [int]$OperationIndex)+'.json')) $Journal.operations[[int]$OperationIndex]
        }elseif(-not $Journal._progressFailed){
        try{
        $progressPath=if($writer){$writer.PathName}else{Get-GameProfileStorePath $Context ($base+'progress.jsonl')}
        if(-not $Journal.ContainsKey('_progressWriter')){
            $Journal._progressWriter=[VRCenter.JournalProgressWriter]::new($progressPath,[string[]]$partPaths,(Get-GameProfileStorePath $Context ($base+'receipt.json')));$Journal._progressKey=$progressKey
            $writer=$Journal._progressWriter
        }
        if($Journal._progressKey -cne $progressKey){throw 'Progress writer belongs to another journal or store.'}
        $record=@{schema=1;id=$Journal.id;sequence=($Journal._progressWriter.Sequence+1);operation=$Journal.operations[[int]$OperationIndex]}
        $Journal._progressWriter.Append($progressPath,(ConvertTo-Json -InputObject $record -Depth 25 -Compress))
        }catch{$Journal._progressFailed=$true;throw}
        }
        # After an append failure, rollback may still update its atomic receipt.
        # Never append another record behind a potentially torn audit tail.
    }
    $receipt=@{schema=1;id=$Journal.id;state=$Journal.state;cleanupComplete=[bool]$Journal.cleanupComplete;newProfile=[bool]$Journal.newProfile;oldActiveId=$Journal.oldActiveId;targetId=$Journal.targetId;startedUtc=$Journal.startedUtc;beforeManifest='before.json';targetManifest='target.json';plan='plan.json';errors=$Journal.errors;conflicts=$Journal.conflicts}
    $receiptPath=if($writer){$writer.ReceiptPath}else{Get-GameProfileStorePath $Context ($base+'receipt.json')}
    $receiptJson=ConvertTo-Json -InputObject $receipt -Depth 25
    # An operation update often leaves the receipt byte-for-byte unchanged.
    # Keep fresh path guards and fall back to the existing atomic writer on
    # absence, different bytes or any read failure; operation writes stay durable.
    Assert-GameProfilePlainPath $receiptPath
    $unchanged=$false
    try{$unchanged=[VRCenter.ProfilePathChecks]::Utf8FileEquals($receiptPath,$receiptJson)}catch{}
    if(-not $unchanged){Write-GameProfileJson $receiptPath $receipt}
}
function Invoke-GameProfileApply($Context,$Catalog,$Before,$TargetManifest,[bool]$New){
    $journal=$null
    try{
    # CAS/required-file validation happens before the first live mutation.
    $TargetManifest=ConvertTo-GameProfileManifest $Context $TargetManifest -RequireFiles
    Assert-GameProfileInventory $Before.files (Get-GameProfileInventory $Context)
    $journal=@{schema=1;id=[guid]::NewGuid().ToString('N');state='prepared';cleanupComplete=$false;newProfile=$New;oldActiveId=$Catalog.activeId;targetId=$TargetManifest.id;before=$Before;target=$TargetManifest;operations=@();errors=@();conflicts=@();startedUtc=[datetime]::UtcNow.ToString('o');_progressOwned=$true}
    $operations=[Collections.Generic.List[object]]::new()
    $beforeMap=@{};$afterMap=@{};foreach($entry in $Before.files){$beforeMap[$entry.scope+'/'+$entry.path]=$entry};foreach($entry in $TargetManifest.files){$afterMap[$entry.scope+'/'+$entry.path]=$entry}
    $skip=@{};$handles=[Collections.Generic.List[IDisposable]]::new();$committed=$false
    foreach($key in @(@($beforeMap.Keys)+@($afterMap.Keys)|Sort-Object -Unique)){
        $old=$beforeMap[$key];$next=$afterMap[$key];if($old -and $next -and $old.sha256 -eq $next.sha256){continue}
        $entry=if($old){$old}else{$next};$suffix='.profile-'+$journal.id+'-'+$operations.Count
        # File-scope siblings are outside that scope, but stay in its trusted parent.
        $operations.Add(@{index=$operations.Count;scope=$entry.scope;path=$entry.path;before=$old;after=$next;phase='planned';suffix=$suffix;originalHash=$null;originalOwned=$false;stagedOwned=$false})
        if($Context.scopes[$entry.scope].kind -eq 'directory'){$skip[$entry.scope+'/'+$entry.path+$suffix+'.old']=$true;$skip[$entry.scope+'/'+$entry.path+$suffix+'.new']=$true}
    }
    $journal.operations=$operations.ToArray()
    Assert-GameProfileCapacity @($journal.operations|Where-Object after|ForEach-Object {@{path=(Get-GameProfileTargetPath $Context $_.scope $_.path);bytes=[long]$_.after.size}})
    Write-GameProfileJournal $Context $journal
    try{
        $journal.state='applying';Write-GameProfileJournal $Context $journal
        foreach($op in $journal.operations){
            $target=Get-GameProfileTargetPath $Context $op.scope $op.path
            $original=$target+$op.suffix+'.old';$staged=$target+$op.suffix+'.new'
            Assert-GameProfilePlainPath $original;Assert-GameProfilePlainPath $staged
            if((Test-Path -LiteralPath $original) -or (Test-Path -LiteralPath $staged)){throw 'Transaction sibling already exists.'}
            if($op.before){
                if(-not(Test-Path -LiteralPath $target -PathType Leaf)){throw 'A live profile file disappeared before replacement.'}
                Move-GameProfileFile $target $original $false;$op.originalOwned=$true;$op.phase='renamed';Write-GameProfileJournal $Context $journal -OperationIndex $op.index
                $held=[IO.File]::Open($original,[IO.FileMode]::Open,[IO.FileAccess]::Read,([IO.FileShare]::Read -bor [IO.FileShare]::Delete));$handles.Add($held)
                $op.originalHash=[Convert]::ToHexString([VRCenter.ProfilePathChecks]::HashStream($held));$held.Position=0
                if($op.originalHash -ne $op.before.sha256){throw 'Live file changed before its atomic replacement.'}
            }elseif(Test-Path -LiteralPath $target){throw 'An unexpected live file appeared before installation.'}
            if($op.after){
                Copy-GameProfileFile (Get-GameProfileStorePath $Context ('objects/'+$op.after.sha256)) $staged
                $op.stagedOwned=$true
                if((Get-GameProfileFileHash $staged) -ne $op.after.sha256){throw 'Staged copy verification failed.'}
                Move-GameProfileFile $staged $target $false;$op.stagedOwned=$false;$op.phase='placed'
                $held=[IO.File]::Open($target,[IO.FileMode]::Open,[IO.FileAccess]::Read,[IO.FileShare]::Read);$handles.Add($held)
            }else{$op.phase='removed'}
            Write-GameProfileJournal $Context $journal -OperationIndex $op.index
        }
        Assert-GameProfileInventory $TargetManifest.files (Get-GameProfileInventory $Context $skip)
        $journal.state='verified';Write-GameProfileJournal $Context $journal
        Write-GameProfileJson (Get-GameProfileStorePath $Context ('profiles/'+$TargetManifest.id+'.json')) $TargetManifest
        if($New){$Catalog.profiles+=,(Get-GameProfileMetadata $TargetManifest)}else{$Catalog.profiles=@($Catalog.profiles|ForEach-Object {if($_.id -eq $TargetManifest.id){Get-GameProfileMetadata $TargetManifest}else{$_}})}
        $Catalog.activeId=$TargetManifest.id;$Catalog.recoveryRequired=@()
        Write-GameProfileJson (Get-GameProfileStorePath $Context 'catalog.json') $Catalog;$committed=$true
        $journal.state='cleanup-pending';Write-GameProfileJournal $Context $journal
    }catch{
        $failure=$_.Exception.Message;$journal.errors+=,$failure
        foreach($handle in $handles){$handle.Dispose()};$handles.Clear()
        if(-not $committed){
            for($i=$journal.operations.Count-1;$i -ge 0;$i--){$op=$journal.operations[$i]
                try{
                    $target=Get-GameProfileTargetPath $Context $op.scope $op.path;$original=$target+$op.suffix+'.old';$staged=$target+$op.suffix+'.new'
                    if(Test-Path -LiteralPath $staged){Assert-GameProfileOwnedSibling $staged ([bool]$op.stagedOwned) $op.after.sha256;[IO.File]::Delete($staged);$op.stagedOwned=$false}
                    if($op.phase -eq 'planned'){if(Test-Path -LiteralPath $original){throw 'Preexisting original sibling preserved; operation never owned it.'};continue}
                    if(Test-Path -LiteralPath $original){$originalExpected=if($op.originalHash){$op.originalHash}else{$op.before.sha256};Assert-GameProfileOwnedSibling $original ([bool]$op.originalOwned) $originalExpected}
                    if(Test-Path -LiteralPath $target){
                        if($op.phase -ne 'placed' -or (Get-GameProfileFileHash $target) -ne $op.after.sha256){throw 'External data at target is preserved; original sibling retained.'}
                        [IO.File]::Delete($target)
                    }elseif($op.phase -eq 'placed'){throw 'External deletion is preserved; original sibling retained.'}
                    if(Test-Path -LiteralPath $original){Assert-GameProfilePlainPath $original;Move-GameProfileFile $original $target $false}
                }catch{$journal.conflicts+=,@{scope=$op.scope;path=$op.path;reason=$_.Exception.Message}}
            }
            if(-not $journal.conflicts.Count){try{Sync-GameProfileDirectories $Context $Before}catch{$journal.conflicts+=,@{reason=$_.Exception.Message}}}
            $journal.state=if($journal.conflicts.Count){'rollback-conflict'}else{'rolled-back'}
            $journal.cleanupComplete=($journal.conflicts.Count -eq 0)
        }else{$journal.state='commit-needs-review'}
        try{foreach($op in $journal.operations){if($op.phase -ne 'planned'){Write-GameProfileJournal $Context $journal -OperationIndex $op.index}};Write-GameProfileJournal $Context $journal}catch{}
        throw ('Profile transaction failed: '+$failure+'; journal='+$journal.id+'; state='+$journal.state)
    }finally{
        foreach($handle in $handles){$handle.Dispose()}
        if($committed -and $journal.state -eq 'cleanup-pending'){
            $cleanupErrors=@()
            foreach($op in $journal.operations){try{$target=Get-GameProfileTargetPath $Context $op.scope $op.path;foreach($suffix in @('.old','.new')){$path=$target+$op.suffix+$suffix;if(Test-Path -LiteralPath $path){$owned=if($suffix -eq '.old'){$op.originalOwned -and $op.phase -in @('placed','removed')}else{$op.stagedOwned};$expected=if($suffix -eq '.old'){$op.before.sha256}else{$op.after.sha256};Assert-GameProfileOwnedSibling $path ([bool]$owned) $expected;[IO.File]::Delete($path);if($suffix -eq '.old'){$op.originalOwned=$false}else{$op.stagedOwned=$false}}}}catch{$cleanupErrors+=,@{scope=$op.scope;path=$op.path;reason=$_.Exception.Message}}}
            if($cleanupErrors.Count){$journal.state='cleanup-needs-review';$journal.conflicts=$cleanupErrors;Write-GameProfileJournal $Context $journal;throw ('Profile was committed but sibling cleanup needs review; journal='+$journal.id)}
            try{Sync-GameProfileDirectories $Context $TargetManifest}catch{
                $journal.state='cleanup-needs-review';$journal.errors+=,$_.Exception.Message;Write-GameProfileJournal $Context $journal
                throw ('Profile empty-directory cleanup needs review; journal='+$journal.id)
            }
            $journal.state='committed';$journal.cleanupComplete=$true;Write-GameProfileJournal $Context $journal
        }
    }
    return $Catalog
    }finally{Close-GameProfileJournalProgress $journal}
}
function Assert-GameProfileOwnedSibling([string]$Path,[bool]$Owned,[string]$ExpectedHash){
    Assert-GameProfilePlainPath $Path
    if(-not $Owned -or $ExpectedHash -notmatch '^[0-9A-Fa-f]{64}$' -or -not(Test-Path -LiteralPath $Path -PathType Leaf)){throw 'Unowned transaction sibling preserved for recovery.'}
    $actual=Get-GameProfileFileHash $Path
    if($actual -ne $ExpectedHash){throw ('Foreign transaction sibling preserved: expected SHA '+$ExpectedHash+', actual '+$actual)}
}
function New-GameProfile {
    param($StoreConfig,[string]$Name,[string]$TemplateId,[switch]$VR)
    if(-not $Name.Trim() -or $Name.Length -gt 128 -or $Name -match '[\x00-\x1f]'){throw 'Invalid profile name.'}
    Assert-GameProfileTemplateId $TemplateId
    $context=Get-GameProfileContext $StoreConfig;$lock=Lock-GameProfileStore $context
    try{Start-GameProfileCasSession $context;
        Assert-GameProfileNoRecovery $context
        $template=Read-GameProfileJson (Get-GameProfileStorePath $context ('templates/'+$TemplateId+'.json'))
        $target=ConvertTo-GameProfileManifest $context $template -RequireFiles
        Assert-GameProfileApplyLayout $context $target
        $inventory=Get-GameProfileInventory $context;Assert-GameProfileApplyCapacity $context $target $inventory
        $current=Save-GameProfileActive $context (Read-GameProfileCatalog $context) $inventory
        $target.id=[guid]::NewGuid().ToString('N');$target.name=$Name.Trim();$target.vr=[bool]$VR;$target.templateId=$TemplateId;$target.createdUtc=[datetime]::UtcNow.ToString('o');$target.savedUtc=$target.createdUtc
        return Invoke-GameProfileApply $context $current.catalog $current.manifest $target $true
    }finally{try{Stop-GameProfileCasSession $context}finally{$lock.Dispose()}}
}
function Copy-GameProfile {
    param($StoreConfig,[string]$Name)
    if([string]::IsNullOrWhiteSpace($Name) -or $Name.Length -gt 128 -or $Name -match '[\x00-\x1f]'){throw 'Invalid profile name.'}
    $context=Get-GameProfileContext $StoreConfig;$lock=Lock-GameProfileStore $context
    try{Start-GameProfileCasSession $context;
        Assert-GameProfileNoRecovery $context
        $current=Save-GameProfileActive $context (Read-GameProfileCatalog $context)
        if(-not $current.manifest.complete){throw 'Cannot duplicate an incomplete profile.'}
        $target=$current.manifest|ConvertTo-Json -Depth 25|ConvertFrom-Json -AsHashtable
        $target.id=[guid]::NewGuid().ToString('N');$target.name=$Name.Trim()
        $target.createdUtc=[datetime]::UtcNow.ToString('o');$target.savedUtc=$target.createdUtc
        # Same live bytes, separate ownership/manifest. Publishing the catalog last
        # activates the copy without restoring or copying the identical payload.
        Write-GameProfileJson (Get-GameProfileStorePath $context ('ownership/'+$target.id+'.json')) @{schema=1;id=$target.id;hashes=@($target.files|ForEach-Object sha256|Sort-Object -Unique)}
        Write-GameProfileJson (Get-GameProfileStorePath $context ('profiles/'+$target.id+'.json')) $target
        $catalog=$current.catalog;$catalog.profiles+=,@(Get-GameProfileMetadata $target)[0];$catalog.activeId=$target.id
        Write-GameProfileJson (Get-GameProfileStorePath $context 'catalog.json') $catalog
        return $catalog
    }finally{try{Stop-GameProfileCasSession $context}finally{$lock.Dispose()}}
}
function Activate-GameProfile {
    param($StoreConfig,[string]$Id)
    Assert-GameProfileId $Id
    $context=Get-GameProfileContext $StoreConfig;$lock=Lock-GameProfileStore $context
    try{Start-GameProfileCasSession $context;
        Assert-GameProfileNoRecovery $context;$catalog=Read-GameProfileCatalog $context
        if(-not @($catalog.profiles|Where-Object id -eq $Id).Count){throw 'Profile is not in the catalog.'}
        if($catalog.activeId -eq $Id){return (Save-GameProfileActive $context $catalog).catalog}
        $target=ConvertTo-GameProfileManifest $context (Read-GameProfileJson (Get-GameProfileStorePath $context ('profiles/'+$Id+'.json'))) -RequireFiles
        if($target.id -cne $Id){throw 'Profile manifest identity mismatch.'}
        Assert-GameProfileApplyLayout $context $target
        $inventory=Get-GameProfileInventory $context;Assert-GameProfileApplyCapacity $context $target $inventory
        $current=Save-GameProfileActive $context $catalog $inventory
        return Invoke-GameProfileApply $context $current.catalog $current.manifest $target $false
    }finally{try{Stop-GameProfileCasSession $context}finally{$lock.Dispose()}}
}
function Remove-GameProfile {
    param($StoreConfig,[string]$Id)
    Assert-GameProfileId $Id
    $context=Get-GameProfileContext $StoreConfig;$lock=Lock-GameProfileStore $context
    try{
        Assert-GameProfileNoRecovery $context
        $catalog=Read-GameProfileCatalog $context
        if($catalog.activeId -eq $Id){throw 'Active game profile cannot be removed.'}
        $path=Get-GameProfileStorePath $context ('profiles/'+$Id+'.json')
        $retiredPath=Get-GameProfileStorePath $context ('deleted-profiles/'+$Id+'.json')
        $present=@($catalog.profiles|Where-Object id -eq $Id).Count -gt 0
        $retired=Read-GameProfileJson $retiredPath
        if(-not $present -and $retired -and $retired.gcState -eq 'complete'){return $catalog}
        $removed=if($present){Read-GameProfileJson $path}elseif($retired){$retired.manifest}else{$null}
        if(-not $removed -or $removed.id -cne $Id){throw 'Deleted profile manifest identity mismatch.'}
        $references=Get-GameProfileReferences $context $Id
        $candidates=@{};foreach($entry in $removed.files){if([string]$entry.sha256 -notmatch '^[0-9a-fA-F]{64}$'){throw 'Invalid owned CAS reference.'};$candidates[([string]$entry.sha256).ToUpperInvariant()]=$true}
        $ownerPath=Get-GameProfileStorePath $context ('ownership/'+$Id+'.json');$owner=Read-GameProfileJson $ownerPath
        $history=if($owner){if($owner.id -cne $Id -or $owner.hashes -isnot [array]){throw 'Invalid removed profile ownership ledger.'};$owner.hashes}elseif($retired){@($retired.ownedHashes)}else{@()}
        foreach($sha in $history){if($sha -notmatch '^[0-9a-fA-F]{64}$'){throw 'Invalid historical owned CAS hash.'};$candidates[([string]$sha).ToUpperInvariant()]=$true}
        foreach($sha in $candidates.Keys){if(-not $references.ContainsKey($sha)){$object=Get-GameProfileStorePath $context ('objects/'+$sha);if((Test-Path -LiteralPath $object) -and (-not(Test-Path -LiteralPath $object -PathType Leaf) -or (Get-GameProfileFileHash $object) -ne $sha)){throw 'Owned CAS leaf/hash validation failed before deletion.'}}}
        Write-GameProfileJson $retiredPath @{schema=1;id=$Id;gcState='pending';manifest=$removed;ownedHashes=@($candidates.Keys)}
        $catalog.profiles=@($catalog.profiles|Where-Object id -ne $Id)
        Write-GameProfileJson (Get-GameProfileStorePath $context 'catalog.json') $catalog
        if(Test-Path -LiteralPath $path){[IO.File]::Delete($path)}
        if(Test-Path -LiteralPath $ownerPath){[IO.File]::Delete($ownerPath)}
        foreach($sha in $candidates.Keys){if(-not $references.ContainsKey($sha)){$object=Get-GameProfileStorePath $context ('objects/'+$sha);if(Test-Path -LiteralPath $object){Assert-GameProfilePlainPath $object;[IO.File]::Delete($object)}}}
        Write-GameProfileJson $retiredPath @{schema=1;id=$Id;gcState='complete';manifest=$removed;ownedHashes=@($candidates.Keys)}
        return $catalog
    }finally{$lock.Dispose()}
}
function Get-GameProfileVolumeSpace([string]$Path){
    $volume=[IO.Path]::GetPathRoot([IO.Path]::GetFullPath($Path))
    $drive=[IO.DriveInfo]::new($volume)
    return @{volume=$volume;availableBytes=[long]$drive.AvailableFreeSpace}
}
function Assert-GameProfileCapacity($Requests){
    $totals=@{};$spaces=@{}
    foreach($request in @($Requests)){
        $bytes=[long]$request.bytes;if($bytes -le 0){continue}
        $volume=[IO.Path]::GetPathRoot([IO.Path]::GetFullPath([string]$request.path))
        if(-not $spaces.ContainsKey($volume)){$spaces[$volume]=Get-GameProfileVolumeSpace $request.path}
        $totals[$volume]=[long]$totals[$volume]+$bytes
    }
    foreach($volume in $totals.Keys){$need=[long]$totals[$volume]+[long][Math]::Max(65536,[Math]::Ceiling($totals[$volume]*0.01));if($need -gt [long]$spaces[$volume].availableBytes){throw ('Insufficient profile capacity on '+$volume+': '+$need+' bytes required, '+$spaces[$volume].availableBytes+' available.')}}
}
function Assert-GameProfileApplyCapacity($Context,$Manifest,$Inventory){
    $current=@{};foreach($entry in $Inventory){$current[$entry.scope+'/'+$entry.path]=$entry}
    Assert-GameProfileCapacity @($Manifest.files|Where-Object {-not $current[$_.scope+'/'+$_.path] -or $current[$_.scope+'/'+$_.path].sha256 -ne $_.sha256}|ForEach-Object {@{path=(Get-GameProfileTargetPath $Context $_.scope $_.path);bytes=[long]$_.size}})
}
function Assert-GameProfileApplyLayout($Context,$Manifest){
    $files=@{};foreach($entry in $Manifest.files){$files[$entry.scope+'/'+$entry.path]=$true}
    foreach($entry in $Manifest.files){
        $target=Get-GameProfileTargetPath $Context $entry.scope $entry.path
        $attributes=$null
        try{$attributes=[IO.File]::GetAttributes($target)}catch [IO.FileNotFoundException]{}catch [IO.DirectoryNotFoundException]{}
        if($null -ne $attributes -and ($attributes -band [IO.FileAttributes]::Directory)){throw 'Unsupported directory-to-file profile layout transition; no snapshot/apply was performed.'}
        if($Context.scopes[$entry.scope].kind -eq 'directory'){
            $segments=$entry.path.Split('/')
            for($i=1;$i -lt $segments.Length;$i++){if($files.ContainsKey($entry.scope+'/'+[string]::Join('/',$segments[0..($i-1)]))){throw 'Target manifest conflicts between file and directory paths.'}}
        }
        $parent=Split-Path -Parent $target
        while($parent){
            $attributes=$null
            try{$attributes=[IO.File]::GetAttributes($parent)}catch [IO.FileNotFoundException]{}catch [IO.DirectoryNotFoundException]{}
            if($null -ne $attributes -and -not($attributes -band [IO.FileAttributes]::Directory)){throw 'Unsupported file-to-directory profile layout transition; no snapshot/apply was performed.'}
            $parent=[IO.Path]::GetDirectoryName($parent)
        }
    }
}
function Add-GameProfileReferences($References,$Manifest){
    if(-not $Manifest -or $Manifest.schema -ne 1 -or $Manifest.files -isnot [array]){throw 'Cannot verify references from an invalid live manifest.'}
    foreach($entry in $Manifest.files){if([string]$entry.sha256 -notmatch '^[0-9a-fA-F]{64}$'){throw 'Invalid live CAS reference.'};$References[([string]$entry.sha256).ToUpperInvariant()]=$true}
}
function Get-GameProfileReferences($Context,[string]$IgnoredId){
    $references=@{}
    $catalog=Read-GameProfileCatalog $Context
    if($catalog.initialized){foreach($profile in $catalog.profiles){if($profile.id -cne $IgnoredId -and -not(Test-Path -LiteralPath (Get-GameProfileStorePath $Context ('profiles/'+$profile.id+'.json')) -PathType Leaf)){throw 'Cannot verify missing live profile manifest references.'}}}
    foreach($area in @('profiles','templates')){
        $root=Get-GameProfileStorePath $Context $area
        if(-not(Test-Path -LiteralPath $root)){continue}
        foreach($file in @(Get-ChildItem -LiteralPath $root -File -Filter '*.json' -Force)){
            if($area -eq 'profiles'){Assert-GameProfileId $file.BaseName;if($file.BaseName -ceq $IgnoredId){continue}}else{Assert-GameProfileTemplateId $file.BaseName}
            $manifest=Read-GameProfileJson (Get-GameProfileStorePath $Context ($area+'/'+$file.Name))
            if($manifest.id -cne $file.BaseName){throw 'Live manifest reference identity mismatch.'}
            Add-GameProfileReferences $references $manifest
        }
    }
    foreach($id in (Get-GameProfileRecovery $Context)){
        foreach($part in @('before','target')){Add-GameProfileReferences $references (Read-GameProfileJson (Get-GameProfileStorePath $Context ('journals/'+$id+'/'+$part+'.json')))}
        foreach($step in (Read-GameProfileOperationProgress $Context $id)){if($step.originalHash){$references[$step.originalHash]=$true}}
    }
    $imports=Get-GameProfileStorePath $Context 'imports'
    if(Test-Path -LiteralPath $imports){foreach($file in @(Get-ChildItem -LiteralPath $imports -File -Filter '*.json' -Force)){
        if($file.BaseName -notmatch '^[0-9A-Fa-f]{64}$'){throw 'Invalid imported object pin.'}
        $pin=Read-GameProfileJson (Get-GameProfileStorePath $Context ('imports/'+$file.Name))
        if($pin.sha256 -ine $file.BaseName){throw 'Imported object pin mismatch.'}
        $references[$pin.sha256]=$true
    }}
    return $references
}
function Repair-GameProfileTransaction {
    param($StoreConfig,[string]$Id)
    if($Id -cnotmatch '^[a-f0-9]{32}$'){throw 'Invalid profile transaction ID.'}
    $context=Get-GameProfileContext $StoreConfig;$lock=Lock-GameProfileStore $context
    try{
        $base='journals/'+$Id+'/'
        $receipt=Read-GameProfileJson (Get-GameProfileStorePath $context ($base+'receipt.json'))
        if(-not $receipt -or $receipt.schema -ne 1 -or $receipt.id -cne $Id){throw 'Invalid recovery receipt.'}
        if($receipt.state -in @('committed','rolled-back','repaired') -and $receipt.cleanupComplete -is [bool] -and $receipt.cleanupComplete){return Get-GameProfileCatalog $StoreConfig}
        $before=ConvertTo-GameProfileManifest $context (Read-GameProfileJson (Get-GameProfileStorePath $context ($base+'before.json')))
        $after=ConvertTo-GameProfileManifest $context (Read-GameProfileJson (Get-GameProfileStorePath $context ($base+'target.json')))
        Assert-GameProfileId $before.id;Assert-GameProfileId $after.id
        $catalog=Read-GameProfileCatalog $context
        if($receipt.oldActiveId -cne $before.id -or $receipt.targetId -cne $after.id -or $catalog.activeId -notin @($before.id,$after.id)){throw 'Recovery profile/catalog identity mismatch.'}
        $planPath=Get-GameProfileStorePath $context ($base+'plan.json')
        $plan=ConvertFrom-Json -InputObject ([IO.File]::ReadAllText($planPath)) -AsHashtable -NoEnumerate -ErrorAction Stop
        if($plan -isnot [array]){throw 'Recovery plan must be a JSON array.'}
        $beforeMap=@{};$afterMap=@{};$diff=@{}
        foreach($entry in $before.files){$beforeMap[$entry.scope+'/'+$entry.path]=$entry}
        foreach($entry in $after.files){$afterMap[$entry.scope+'/'+$entry.path]=$entry}
        foreach($key in @(@($beforeMap.Keys)+@($afterMap.Keys)|Sort-Object -Unique)){if(-not $beforeMap[$key] -or -not $afterMap[$key] -or $beforeMap[$key].sha256 -ne $afterMap[$key].sha256){$diff[$key]=$true}}
        if($plan.Count -ne $diff.Count){throw 'Recovery plan is incomplete.'}
        $skip=@{};$seen=@{};$actions=@();$conflicts=@();$index=0
        foreach($op in $plan){
            $key=$op.scope+'/'+$op.path;$suffix='.profile-'+$Id+'-'+$index
            if($op.index -ne $index -or $op.suffix -cne $suffix -or $seen.ContainsKey($key) -or -not $diff.ContainsKey($key)){throw 'Invalid recovery operation identity.'}
            $seen[$key]=$true;$old=$beforeMap[$key];$next=$afterMap[$key]
            if([bool]$op.before -ne [bool]$old -or [bool]$op.after -ne [bool]$next -or ($old -and $op.before.sha256 -ne $old.sha256) -or ($next -and $op.after.sha256 -ne $next.sha256)){throw 'Recovery operation does not match immutable manifests.'}
            $target=Get-GameProfileTargetPath $context $op.scope $op.path
            if($context.scopes[$op.scope].kind -eq 'directory'){foreach($tail in @('.old','.new','.repair-old','.repair-new')){$skip[$op.scope+'/'+$op.path+$suffix+$tail]=$true}}
            try{
                $original=$target+$suffix+'.old';$repairOld=$target+$suffix+'.repair-old'
                foreach($path in @($original,$repairOld,$target+$suffix+'.new',$target+$suffix+'.repair-new')){Assert-GameProfilePlainPath $path}
                $originalHash=if(Test-Path -LiteralPath $original){Get-GameProfileFileHash $original}else{$null}
                $repairOldHash=if(Test-Path -LiteralPath $repairOld){Get-GameProfileFileHash $repairOld}else{$null}
                $liveHash=if(Test-Path -LiteralPath $target){Get-GameProfileFileHash $target}else{$null}
                if($originalHash -and (-not $old -or $originalHash -ne $old.sha256)){throw 'Foreign original sibling preserved.'}
                if($repairOldHash -and (-not $next -or $repairOldHash -ne $next.sha256)){throw 'Foreign repair sibling preserved.'}
                $action='none'
                if($old){
                    if($liveHash -eq $old.sha256){}
                    elseif(($next -and $liveHash -eq $next.sha256) -or (-not $liveHash -and ($originalHash -or $repairOldHash -or -not $next))){$action='restore'}
                    else{throw 'Foreign replacement/deletion preserved.'}
                }elseif($liveHash){if($next -and $liveHash -eq $next.sha256){$action='remove'}else{throw 'Foreign replacement preserved.'}}
                $actions+=,@{op=$op;target=$target;original=$original;repairOld=$repairOld;old=$old;next=$next;liveHash=$liveHash;action=$action}
            }catch{$conflicts+=,@{scope=$op.scope;path=$op.path;reason=$_.Exception.Message}}
            $index++
        }
        $actual=Get-GameProfileInventory $context $skip;$actualMap=@{}
        foreach($entry in $actual){$key=$entry.scope+'/'+$entry.path;$actualMap[$key]=$entry;if(-not $diff.ContainsKey($key) -and (-not $beforeMap[$key] -or $entry.sha256 -ne $beforeMap[$key].sha256)){$conflicts+=,@{scope=$entry.scope;path=$entry.path;reason='External unplanned bytes preserved.'}}}
        foreach($key in $beforeMap.Keys){if(-not $diff.ContainsKey($key) -and -not $actualMap[$key]){$conflicts+=,@{scope=$beforeMap[$key].scope;path=$beforeMap[$key].path;reason='External unplanned deletion preserved.'}}}
        $journal=@{id=$Id;state='repairing';cleanupComplete=$false;newProfile=[bool]$receipt.newProfile;oldActiveId=$before.id;targetId=$after.id;startedUtc=$receipt.startedUtc;before=$before;target=$after;operations=$plan;errors=@($receipt.errors);conflicts=$conflicts}
        if($conflicts.Count){$journal.state='repair-conflict';Write-GameProfileJournal $context $journal;throw ('Recovery preserved foreign data; journal='+$Id)}
        Assert-GameProfileCapacity @($actions|Where-Object {$_.action -eq 'restore' -and -not(Test-Path -LiteralPath $_.original)}|ForEach-Object {@{path=$_.target;bytes=[long]$_.old.size}})
        Write-GameProfileJournal $context $journal
        try{
            foreach($action in $actions){
                $target=$action.target;$suffix=$action.op.suffix
                $currentHash=if(Test-Path -LiteralPath $target){Get-GameProfileFileHash $target}else{$null}
                if($currentHash -ne $action.liveHash){throw 'Concurrent target change during repair.'}
                if($action.action -eq 'none'){continue}
                if($currentHash){
                    if(Test-Path -LiteralPath $action.repairOld){throw 'Repair sibling already exists.'}
                    Move-GameProfileFile $target $action.repairOld $false
                    if((Get-GameProfileFileHash $action.repairOld) -ne $currentHash){if(-not(Test-Path -LiteralPath $target)){Move-GameProfileFile $action.repairOld $target $false};throw 'Concurrent replacement preserved during repair.'}
                }
                if($action.action -eq 'restore'){
                    if(Test-Path -LiteralPath $action.original){Move-GameProfileFile $action.original $target $false}else{
                        $stage=$target+$suffix+'.repair-new'
                        if(Test-Path -LiteralPath $stage){if((Get-GameProfileFileHash $stage) -ne $action.old.sha256){throw 'Incomplete repair stage preserved for review.'}}else{Copy-GameProfileFile (Get-GameProfileStorePath $context ('objects/'+$action.old.sha256)) $stage}
                        if((Get-GameProfileFileHash $stage) -ne $action.old.sha256){throw 'Recovery copy SHA mismatch.'}
                        Move-GameProfileFile $stage $target $false
                    }
                }
            }
            Assert-GameProfileInventory $before.files (Get-GameProfileInventory $context $skip)
            $journal.state='repair-cleanup-pending';Write-GameProfileJournal $context $journal
            foreach($action in $actions){foreach($tail in @('.old','.new','.repair-old','.repair-new')){
                $path=$action.target+$action.op.suffix+$tail
                if(-not(Test-Path -LiteralPath $path)){continue}
                Assert-GameProfilePlainPath $path
                $sha=Get-GameProfileFileHash $path
                $expected=if($tail -in @('.old','.repair-new')){$action.old.sha256}else{$action.next.sha256}
                if($sha -ne $expected){
                    # Partial owned stages are kept as verified evidence before unlinking.
                    if($tail -notin @('.new','.repair-new')){throw 'Foreign cleanup sibling preserved.'}
                    $evidence=Get-GameProfileStorePath $context ($base+'evidence/'+$sha+'.bin')
                    if(-not(Test-Path -LiteralPath $evidence)){Copy-GameProfileFile $path $evidence}
                    if((Get-GameProfileFileHash $evidence) -ne $sha){throw 'Recovery stage evidence verification failed.'}
                }
                [IO.File]::Delete($path)
            }}
            Assert-GameProfileInventory $before.files (Get-GameProfileInventory $context)
            Sync-GameProfileDirectories $context $before
            Write-GameProfileJson (Get-GameProfileStorePath $context ('profiles/'+$before.id+'.json')) $before
            $catalog.profiles=@($catalog.profiles|Where-Object {$_.id -ne $before.id -and (-not $journal.newProfile -or $_.id -ne $after.id)})+@((Get-GameProfileMetadata $before))
            if($journal.newProfile){$newPath=Get-GameProfileStorePath $context ('profiles/'+$after.id+'.json');if(Test-Path -LiteralPath $newPath){[IO.File]::Delete($newPath)}}
            $catalog.activeId=$before.id;$catalog.recoveryRequired=@()
            Write-GameProfileJson (Get-GameProfileStorePath $context 'catalog.json') $catalog
            $journal.state='repaired';$journal.cleanupComplete=$true;Write-GameProfileJournal $context $journal
            return Get-GameProfileCatalog $StoreConfig
        }catch{
            $journal.state='repair-conflict';$journal.errors+=,$_.Exception.Message;Write-GameProfileJournal $context $journal
            throw ('Profile recovery needs review; journal='+$Id+'; '+$_.Exception.Message)
        }
    }finally{$lock.Dispose()}
}
