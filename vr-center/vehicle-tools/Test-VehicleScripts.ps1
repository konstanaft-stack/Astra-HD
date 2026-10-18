param([Parameter(Mandatory)][string]$GamePath,[Parameter(Mandatory)][string]$Packages)
$ErrorActionPreference='Stop'
$game=$GamePath
# Packages are supplied explicitly.
$manifest=Get-Content -LiteralPath (Join-Path $packages 'approved-content.prepared.json') -Raw|ConvertFrom-Json
$run=Join-Path $packages ('compile-'+[DateTime]::UtcNow.ToString('HHmmss'))
if(Test-Path -LiteralPath $run){throw 'New compilation directory required'}
$scripts=Join-Path $run 'scripts';$cache=Join-Path $run 'cache';$compiler=Join-Path $run 'compiler'
foreach($dir in @($scripts,$cache,$compiler)){[void][IO.Directory]::CreateDirectory($dir)}
$inputs=@()
foreach($scope in @('r6/scripts','red4ext/plugins')){
    $root=Join-Path $game $scope
    foreach($file in Get-ChildItem -LiteralPath $root -File -Recurse -Filter '*.reds'){
        $relative=[IO.Path]::GetRelativePath($root,$file.FullName)
        if($scope -eq 'red4ext/plugins'){$relative='_red4ext_dependencies/'+$relative}
        $destination=Join-Path $scripts $relative
        [void][IO.Directory]::CreateDirectory((Split-Path $destination -Parent))
        Copy-Item -LiteralPath $file.FullName -Destination $destination
        $inputs+=@{source=$file.FullName;sha256=(Get-FileHash -LiteralPath $file.FullName).Hash}
    }
}
$added=@()
# Compile exactly the selected manifest, never unrelated files left in staging.
foreach($package in $manifest.packages){foreach($entry in $package.files){
    if($entry.path -notmatch '^r6/scripts/(.+\.reds)$'){continue}
    $relative=$Matches[1];$source=Join-Path (Join-Path $packages 'stage') $entry.path
    if((Get-FileHash -LiteralPath $source).Hash -ne $entry.sha256){throw 'Staged script hash mismatch'}
    $destination=Join-Path $scripts $relative
    if(Test-Path -LiteralPath $destination){throw ('Duplicate script: '+$relative)}
    [void][IO.Directory]::CreateDirectory((Split-Path $destination -Parent))
    Copy-Item -LiteralPath $source -Destination $destination
    $added+=@{source=$source;sha256=$entry.sha256}
}}
$base=Join-Path $game 'r6/cache/final.redscripts';$baseHash=(Get-FileHash -LiteralPath $base).Hash
Copy-Item -LiteralPath $base -Destination (Join-Path $cache 'final.redscripts')
foreach($name in @('scc.exe','scc_lib.dll')){Copy-Item -LiteralPath (Join-Path $game ('engine/tools/'+$name)) -Destination (Join-Path $compiler $name)}
$out=Join-Path $run 'compiler.stdout.log';$err=Join-Path $run 'compiler.stderr.log'
$p=Start-Process -FilePath (Join-Path $compiler 'scc.exe') -ArgumentList @('-compile',$scripts,'-customCacheDir',$cache,'-threads','2') -WorkingDirectory $run -WindowStyle Hidden -PassThru -RedirectStandardOutput $out -RedirectStandardError $err
try{
    $timedOut=-not $p.WaitForExit(45000)
    if($timedOut){$p.Kill();$p.WaitForExit()}
    $log=[IO.File]::ReadAllText($out)+[IO.File]::ReadAllText($err)
    $changed=@($inputs|Where-Object {(Get-FileHash -LiteralPath $_.source).Hash -ne $_.sha256})
    $result=@{run=$run;exitCode=$p.ExitCode;timedOut=$timedOut;currentScripts=$inputs.Count;addedScripts=$added.Count;
        errors=([regex]::Matches($log,'\[ERROR')).Count;complete=$log.Contains('Compilation complete');
        gameScriptsUnchanged=($changed.Count -eq 0);baseCacheUnchanged=((Get-FileHash -LiteralPath $base).Hash -eq $baseHash)}
    $result.pass=($result.exitCode -eq 0 -and -not $timedOut -and $result.errors -eq 0 -and $result.complete -and $result.gameScriptsUnchanged -and $result.baseCacheUnchanged)
    [IO.File]::WriteAllText((Join-Path $run 'result.json'),($result|ConvertTo-Json),[Text.UTF8Encoding]::new($false))
    [IO.File]::WriteAllText((Join-Path $packages 'compile-latest.txt'),$run,[Text.UTF8Encoding]::new($false))
    $result|ConvertTo-Json -Compress
    if(-not $result.pass){$log -split '\r?\n'|Where-Object {$_ -match 'ERROR|Error|error'}|Select-Object -First 20;throw 'Vehicle script compilation failed'}
}finally{$p.Dispose()}
