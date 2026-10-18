$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot '../StockIdentityCache.ps1')
$fixture=Join-Path ([IO.Path]::GetTempPath()) ('stock-identity-'+[guid]::NewGuid().ToString('N'))
$null=New-Item -ItemType Directory -Path $fixture
function Check($condition,$message){if(-not $condition){throw $message}}
try{
    $file=Join-Path $fixture 'stock.bin';$cache=Join-Path $fixture 'cache'
    [IO.File]::WriteAllText($file,'verified original stock')
    $expected=(Get-FileHash -LiteralPath $file -Algorithm SHA1).Hash
    $size=(Get-Item -LiteralPath $file).Length
    $first=Test-VerifiedStockIdentity $file $expected $size $cache
    Check ($first.Verified -and -not $first.CacheHit -and $first.BytesHashed -eq $size) 'First access must hash all bytes'
    $warm=Test-VerifiedStockIdentity $file $expected $size $cache
    Check ($warm.Verified -and $warm.CacheHit -and $warm.BytesHashed -eq 0) 'Warm identity must avoid reading content'
    $child=Join-Path $fixture 'child.ps1'
    [IO.File]::WriteAllText($child,'. $args[0];Test-VerifiedStockIdentity $args[1] $args[2] ([long]$args[3]) $args[4]|ConvertTo-Json -Compress')
    $persisted=(& pwsh -NoProfile -File $child (Join-Path $PSScriptRoot '../StockIdentityCache.ps1') $file $expected $size $cache)|ConvertFrom-Json
    Check ($LASTEXITCODE -eq 0 -and $persisted.CacheHit -and $persisted.BytesHashed -eq 0) 'Verified cache must survive a fresh process'
    # Exercise API-unavailable identity in a disposable module copy, without a
    # production bypass switch or any substitution of the SHA1 implementation.
    $fallbackModule=Join-Path $fixture 'StockIdentityFallback.ps1'
    $source=[IO.File]::ReadAllText((Join-Path $PSScriptRoot '../StockIdentityCache.ps1'))
    $needle='static string Identity(FileStream file,string path) {'
    Check ($source.Contains($needle)) 'Fallback injection must target the identity boundary'
    [IO.File]::WriteAllText($fallbackModule,$source.Replace($needle,'static string Identity(FileStream file,string path) { return null; } static string UnavailableIdentity(FileStream file,string path) {'))
    $fallback=(& pwsh -NoProfile -File $child $fallbackModule $file $expected $size $cache)|ConvertFrom-Json
    Check ($LASTEXITCODE -eq 0 -and $fallback.Verified -and -not $fallback.CacheHit -and $fallback.BytesHashed -eq $size) 'Missing identity API must fully hash even with a warm cache'
    $writer=[IO.File]::Open($file,[IO.FileMode]::Open,[IO.FileAccess]::Write,[IO.FileShare]::ReadWrite)
    try{$blocked=$false;try{Test-VerifiedStockIdentity $file $expected $size $cache|Out-Null}catch{$blocked=$true};Check $blocked 'An existing writer must block even a cache hit'}finally{$writer.Dispose()}
    $mtime=[IO.File]::GetLastWriteTimeUtc($file)
    [IO.File]::WriteAllText($file,'corrupt! original stock')
    Check ((Get-Item -LiteralPath $file).Length -eq $size) 'Fixture must preserve size'
    [IO.File]::SetLastWriteTimeUtc($file,$mtime)
    $edited=Test-VerifiedStockIdentity $file $expected $size $cache
    Check (-not $edited.Verified -and -not $edited.CacheHit -and $edited.BytesHashed -eq $size) 'Restored mtime must not hide changed bytes'
    [IO.File]::Delete($file);[IO.File]::WriteAllText($file,'verified original stock');[IO.File]::SetLastWriteTimeUtc($file,$mtime)
    $replaced=Test-VerifiedStockIdentity $file $expected $size $cache
    Check ($replaced.Verified -and -not $replaced.CacheHit) 'Replaced file must hash again'
    $wrong=Test-VerifiedStockIdentity $file ('0'*40) $size $cache
    Check (-not $wrong.Verified -and -not $wrong.CacheHit -and $wrong.BytesHashed -eq $size) 'Changed expected SHA1 must hash and reject'
    Get-ChildItem -LiteralPath $cache -File | ForEach-Object {[IO.File]::WriteAllText($_.FullName,'invalid JSON')}
    $corrupt=Test-VerifiedStockIdentity $file $expected $size $cache
    Check ($corrupt.Verified -and -not $corrupt.CacheHit) 'Corrupt cache must cause full hashing'
    $cacheReal=Join-Path $fixture 'cache-real';[IO.Directory]::Move($cache,$cacheReal)
    $null=New-Item -ItemType Junction -Path $cache -Target $cacheReal
    try{
        $linkedCache=Test-VerifiedStockIdentity $file $expected $size $cache
        Check ($linkedCache.Verified -and -not $linkedCache.CacheHit -and $linkedCache.BytesHashed -eq $size) 'Cache behind a junction must not be trusted'
    }finally{[IO.Directory]::Delete($cache);[IO.Directory]::Move($cacheReal,$cache)}
    $link=Join-Path $fixture 'junction';$null=New-Item -ItemType Junction -Path $link -Target $fixture
    $rejected=$false;try{Test-VerifiedStockIdentity (Join-Path $link 'stock.bin') $expected $size $cache|Out-Null}catch{$rejected=$true}
    Check $rejected 'Reparse ancestry must be rejected'
    [IO.Directory]::Delete($link)
    'PASS: first hash, warm no-content-read, fresh-process cache, API fallback, writer lock, restored-mtime edit, replacement, changed expectation, corrupt cache, cache junction, stock reparse ancestry'
}finally{
    if(Test-Path -LiteralPath (Join-Path $fixture 'junction')){[IO.Directory]::Delete((Join-Path $fixture 'junction'))}
    Remove-Item -LiteralPath $fixture -Recurse -Force
}
