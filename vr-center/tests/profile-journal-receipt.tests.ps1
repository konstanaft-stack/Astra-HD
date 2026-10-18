param([switch]$BeforeNoop,[string]$Baseline)
if($BeforeNoop -and -not $Baseline){throw "-BeforeNoop requires an explicit -Baseline source snapshot."}
$ErrorActionPreference='Stop'
. (Join-Path (Split-Path $PSScriptRoot -Parent) 'GameProfileStore.ps1')
if($BeforeNoop){$snapshot=[IO.File]::ReadAllText($Baseline);. ([scriptblock]::Create([regex]::Match($snapshot,'(?ms)^function Write-GameProfileJournal\(.*?(?=^function |\z)').Value))}
$fixture=Join-Path ([IO.Path]::GetTempPath()) ('journal-receipt-'+[guid]::NewGuid().ToString('N'))
$context=@{store=[IO.Path]::GetFullPath("$fixture/store")};$id=[guid]::NewGuid().ToString('N')
$journal=@{schema=1;id=$id;state='applying';cleanupComplete=$false;newProfile=$false;oldActiveId='current';targetId=('a'*32);before=@{};target=@{};operations=@(@{index=0;phase='planned';scope='game';path='file';before=$null;after=$null;suffix='.test';originalOwned=$false;stagedOwned=$false});errors=@();conflicts=@();startedUtc=[datetime]::UtcNow.ToString('o')}
try{
 Write-GameProfileJournal $context $journal
 $path=Join-Path $context.store "journals/$id/receipt.json";$bytes=[Convert]::ToHexString([IO.File]::ReadAllBytes($path));$oldTime=[datetime]'2001-01-01T00:00:00Z';[IO.File]::SetLastWriteTimeUtc($path,$oldTime);$oldTime=[IO.File]::GetLastWriteTimeUtc($path)
 $journal.operations[0].phase='removed';Write-GameProfileJournal $context $journal -OperationIndex 0
 if([IO.File]::GetLastWriteTimeUtc($path) -ne $oldTime){throw 'Byte-identical receipt was rewritten'}
 if([Convert]::ToHexString([IO.File]::ReadAllBytes($path)) -cne $bytes){throw 'Receipt bytes changed'}
 $op=Read-GameProfileJson (Join-Path $context.store "journals/$id/operations/000000.json");if($op.phase -cne 'removed'){throw 'Operation durability was skipped'}
 $journal.state='verified';$journal.errors=@('diagnostic');Write-GameProfileJournal $context $journal
 $result=Read-GameProfileJson $path;if($result.state -cne 'verified' -or $result.errors[0] -cne 'diagnostic'){throw 'Changed receipt was skipped'}
 $canonical=[IO.File]::ReadAllBytes($path);[IO.File]::WriteAllBytes($path,([byte[]]@(239,187,191)+$canonical));Write-GameProfileJournal $context $journal
 if([Convert]::ToHexString([IO.File]::ReadAllBytes($path)) -cne [Convert]::ToHexString($canonical)){throw 'Comparison accepted different BOM bytes as identical'}
 [IO.File]::WriteAllText($path,'foreign-invalid-json');Write-GameProfileJournal $context $journal
 if((Read-GameProfileJson $path).state -cne 'verified'){throw 'Different receipt did not retain existing overwrite behavior'}
 [IO.File]::Delete($path);Write-GameProfileJournal $context $journal
 if(-not [IO.File]::Exists($path)){throw 'Missing receipt not recreated'}
 $held=[IO.File]::Open($path,[IO.FileMode]::Open,[IO.FileAccess]::Read,[IO.FileShare]::None)
 try{$denied=$false;try{Write-GameProfileJournal $context $journal}catch{$denied=$true};if(-not $denied){throw 'Locked unreadable receipt bypassed existing atomic-write failure'}}finally{$held.Dispose()}
 if((Read-GameProfileJson $path).state -cne 'verified'){throw 'Failed fallback damaged receipt'}
 'PASS: exact byte no-op preserves receipt; operation/state/error/missing/foreign/read-failure fallback and atomic failure behavior preserved.'
}finally{if([IO.Directory]::Exists($fixture) -and [IO.Path]::GetFullPath($fixture).StartsWith([IO.Path]::GetTempPath(),[StringComparison]::OrdinalIgnoreCase)){Remove-Item -LiteralPath $fixture -Recurse -Force}}
