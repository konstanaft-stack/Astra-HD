$ErrorActionPreference='Stop'
. (Join-Path (Split-Path $PSScriptRoot -Parent) 'GameProfileStore.ps1')
Add-Type -TypeDefinition @'
using System;
using System.IO;
using System.Threading;
public static class ProfileLockFixture {
  public static Timer Hold(string path, int milliseconds) {
    var stream = File.Open(path, FileMode.Open, FileAccess.Read, FileShare.Read);
    return new Timer(_ => stream.Dispose(), null, milliseconds, Timeout.Infinite);
  }
}
'@
$fixture=Join-Path ([IO.Path]::GetTempPath()) ('vr-profile-lock-'+[guid]::NewGuid().ToString('N'))
[IO.Directory]::CreateDirectory($fixture)|Out-Null
$path=Join-Path $fixture 'receipt.json'
Write-GameProfileJson $path @{state='prepared'}
$timer=[ProfileLockFixture]::Hold($path,180)
try {
    Write-GameProfileJson $path @{state='applying'}
    if((Read-GameProfileJson $path).state -ne 'applying'){throw 'Transiently locked journal was not updated'}
} finally { $timer.Dispose() }
$source=Join-Path $fixture 'original.json';$target=Join-Path $fixture 'moved.json'
[IO.File]::WriteAllText($source,'unchanged')
$timer=[ProfileLockFixture]::Hold($source,180)
try {
    Move-GameProfileFile $source $target $false
    if([IO.File]::ReadAllText($target) -ne 'unchanged'){throw 'Move changed payload'}
} finally { $timer.Dispose() }
# A lasting lock must still fail, with both files preserved.
$held=[IO.File]::Open($target,[IO.FileMode]::Open,[IO.FileAccess]::Read,[IO.FileShare]::Read)
$failed=$false
try { Move-GameProfileFile $target $source $false } catch { $failed=$true } finally { $held.Dispose() }
if(-not $failed -or -not(Test-Path -LiteralPath $target) -or (Test-Path -LiteralPath $source)){throw 'Persistent lock must fail without moving or deleting the original'}
Write-Output 'PASS: transient journal/source locks recover; persistent locks preserve originals and fail.'
