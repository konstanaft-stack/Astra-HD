$ErrorActionPreference='Stop'
. (Join-Path (Split-Path $PSScriptRoot -Parent) 'GameProfileStore.ps1')
$fixture=Join-Path ([IO.Path]::GetTempPath()) ('journal-progress-'+[guid]::NewGuid().ToString('N'))
$context=@{store=[IO.Path]::GetFullPath("$fixture/store");config=@{}}
$id=[guid]::NewGuid().ToString('N');$sha='A'*64
$op=@{index=0;phase='planned';scope='game';path='file';before=$null;after=$null;suffix='.test';originalHash=$null;originalOwned=$false;stagedOwned=$false}
$journal=@{_progressOwned=$true;schema=1;id=$id;state='applying';cleanupComplete=$false;newProfile=$false;oldActiveId='current';targetId=('a'*32);before=@{schema=1;files=@()};target=@{schema=1;files=@()};operations=@($op);errors=@();conflicts=@();startedUtc=[datetime]::UtcNow.ToString('o')}
function Check($ok,$why){if(-not $ok){throw $why};Write-Output "PASS: $why"}
function Reject([scriptblock]$action,$why){$failed=$false;try{$null=& $action}catch{$failed=$true};Check $failed $why}
try{
 Write-GameProfileJournal $context $journal
 $op.phase='renamed';Write-GameProfileJournal $context $journal -OperationIndex 0
 $path=Join-Path $context.store "journals/$id/progress.jsonl"
 Check ([IO.File]::Exists($path)) 'operation progress uses one append log'
 Check (-not(Test-Path -LiteralPath (Join-Path $context.store "journals/$id/operations"))) 'new progress creates no per-operation directory'
 $op.phase='removed';$op.originalHash=$sha;Write-GameProfileJournal $context $journal -OperationIndex 0
 $steps=@(Read-GameProfileOperationProgress $context $id)
 Check ($steps.Count -eq 2 -and $steps[0].phase -ceq 'renamed' -and $steps[1].originalHash -ceq $sha) 'reader preserves complete progress history'
 $beforePath=Join-Path $context.store "journals/$id/before.json"
 Reject {$s=[IO.File]::Open($beforePath,[IO.FileMode]::Open,[IO.FileAccess]::Write,[IO.FileShare]::ReadWrite);$s.Dispose()} 'owned session pins immutable parts against replacement/writes'
 Reject {[IO.File]::Move($beforePath,($beforePath+'.foreign'),$false)} 'pinned immutable part cannot be renamed/replaced'
 Reject {Write-GameProfileJournal @{store="$fixture/wrong-store"} $journal -OperationIndex 0} 'writer cannot be reused across store contexts'
 Check (-not(Test-Path -LiteralPath "$fixture/wrong-store")) 'cross-context rejection precedes filesystem writes'
 Write-GameProfileJson (Join-Path $context.store 'catalog.json') @{schema=1;initialized=$true;activeId='current';profiles=@(@{id='current'})}
 Write-GameProfileJson (Join-Path $context.store 'profiles/current.json') @{schema=1;id='current';files=@()}
 $legacy=@{};foreach($key in $op.Keys){$legacy[$key]=$op[$key]};$legacy.originalHash='B'*64
 Write-GameProfileJson (Join-Path $context.store "journals/$id/operations/000000.json") $legacy
 $refs=Get-GameProfileReferences $context ''
 Check ($refs.ContainsKey($sha) -and $refs.ContainsKey('B'*64)) 'CAS references union new progress and legacy records'
 Remove-Item -LiteralPath (Join-Path $context.store "journals/$id/operations/000000.json")
 Reject {$s=[IO.File]::Open($path,[IO.FileMode]::Open,[IO.FileAccess]::Write,[IO.FileShare]::ReadWrite);$s.Dispose()} 'held writer excludes foreign writers'
 Close-GameProfileJournalProgress $journal
 $held=[IO.File]::Open($path,[IO.FileMode]::Open,[IO.FileAccess]::Write,[IO.FileShare]::None);$held.Dispose()
 Check $true 'explicit close releases retained stream'
 $held=[IO.File]::Open($beforePath,[IO.FileMode]::Open,[IO.FileAccess]::Write,[IO.FileShare]::None);$held.Dispose()
 Check $true 'explicit close releases immutable part handles'
 Reject {$new=[VRCenter.JournalProgressWriter]::new((Join-Path $fixture 'ctor-failed.jsonl'),[string[]]@($beforePath,(Join-Path $fixture 'missing-part.json')),$beforePath);$new.Dispose()} 'partial constructor failure is rejected'
 $held=[IO.File]::Open($beforePath,[IO.FileMode]::Open,[IO.FileAccess]::Write,[IO.FileShare]::None);$held.Dispose()
 Check $true 'partial constructor failure releases already opened immutable handles'
 $unavailable=[VRCenter.JournalProgressWriter]::new((Join-Path $fixture 'proof-failed.jsonl'),[string[]]@($beforePath),$beforePath)
 $pins=[VRCenter.JournalProgressWriter].GetField('immutableHandles',[Reflection.BindingFlags]'Instance,NonPublic').GetValue($unavailable)
 $pins[0].Dispose()
 try{Reject {$unavailable.Append($unavailable.PathName,'{}')} 'unavailable live handle proof fails closed'}finally{$unavailable.Dispose()}
 [IO.File]::AppendAllText($path,'{"partial":')
 Check (@(Read-GameProfileOperationProgress $context $id).Count -eq 2) 'only incomplete final record is ignored'
 [IO.File]::AppendAllText($path,"`n")
 Reject {Read-GameProfileOperationProgress $context $id} 'complete corrupt record is rejected'
 Reject {Get-GameProfileReferences $context ''} 'corrupt complete progress fails closed before CAS collection'
 $lines=[IO.File]::ReadAllLines($path);[IO.File]::WriteAllText($path,($lines[0]+"`n"+$lines[0]+"`n"))
 Reject {Read-GameProfileOperationProgress $context $id} 'duplicate sequence is rejected'
 $valid=$lines[0]|ConvertFrom-Json -AsHashtable;$valid.id='f'*32;[IO.File]::WriteAllText($path,((ConvertTo-Json $valid -Depth 25 -Compress)+"`n"))
 Reject {Read-GameProfileOperationProgress $context $id} 'foreign journal identity is rejected'
 $valid.id=$id;$valid.operation.index=1;[IO.File]::WriteAllText($path,((ConvertTo-Json $valid -Depth 25 -Compress)+"`n"))
 Reject {Read-GameProfileOperationProgress $context $id} 'out-of-range operation index is rejected'
 [IO.File]::WriteAllText($path,(('x'*1048576)+"`n"))
 Reject {Read-GameProfileOperationProgress $context $id} 'oversized record is bounded and rejected'
 [IO.File]::Delete($path)
 $legacy=@{};foreach($key in $op.Keys){$legacy[$key]=$op[$key]};$legacy.originalHash='B'*64
 Write-GameProfileJson (Join-Path $context.store "journals/$id/operations/000000.json") $legacy
 Check (@(Read-GameProfileOperationProgress $context $id)[0].originalHash -ceq ('B'*64)) 'legacy operation JSON remains readable'
 Write-GameProfileJson (Join-Path $context.store 'catalog.json') @{schema=1;initialized=$true;activeId='current';profiles=@(@{id='current'})}
 Write-GameProfileJson (Join-Path $context.store 'profiles/current.json') @{schema=1;id='current';files=@()}
 $refs=Get-GameProfileReferences $context ''
 Check ($refs.ContainsKey('B'*64)) 'CAS references include legacy progress originalHash'
 # A different journal owns a fresh log; an existing foreign file is never adopted.
 $foreignId=[guid]::NewGuid().ToString('N');$journal.id=$foreignId
 $foreign=Join-Path $context.store "journals/$foreignId/progress.jsonl";[IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($foreign))|Out-Null;[IO.File]::WriteAllText($foreign,'foreign')
 Reject {Write-GameProfileJournal $context $journal -OperationIndex 0} 'foreign existing progress is preserved'
 Check ([IO.File]::ReadAllText($foreign) -ceq 'foreign') 'failed progress open does not overwrite foreign bytes'
 # Subsequent catch/rollback calls must not append behind a failed tail, but
 # must still persist state/errors so the original byte-based Repair can run.
 $journal.errors=@('append failure');$journal.state='rolled-back';Write-GameProfileJournal $context $journal -OperationIndex 0
 Check ((Read-GameProfileJson (Join-Path $context.store "journals/$foreignId/receipt.json")).state -ceq 'rolled-back') 'poisoned progress still permits atomic error/state receipts'
 Check ([IO.File]::ReadAllText($foreign) -ceq 'foreign') 'poisoned progress does not append subsequent records'
 $nativePath=Join-Path $fixture 'native-failed.jsonl';$native=[VRCenter.JournalProgressWriter]::new($nativePath)
 try{
  Reject {$native.Append($nativePath,('x'*1048576))} 'writer bounds oversized records'
  Reject {$native.Append($nativePath,'{}')} 'failed native writer refuses subsequent append'
 }finally{$native.Dispose()}
 $held=[IO.File]::Open($nativePath,[IO.FileMode]::Open,[IO.FileAccess]::Write,[IO.FileShare]::None);$held.Dispose()
 Check $true 'failed writer disposal releases lock'
 Add-Type -TypeDefinition @'
using System;
using System.IO;
public sealed class PartialAuditWriteStream : FileStream {
 public PartialAuditWriteStream(string path) : base(path,FileMode.Append,FileAccess.Write,FileShare.Read) {}
 public override void Write(byte[] bytes,int offset,int count) { base.Write(bytes,offset,Math.Max(1,count/2));base.Flush();throw new IOException("injected partial append"); }
}
'@
 $partialPath=Join-Path $fixture 'partial-write.jsonl';$partial=[VRCenter.JournalProgressWriter]::new($partialPath)
 $field=[VRCenter.JournalProgressWriter].GetField('stream',[Reflection.BindingFlags]'Instance,NonPublic')
 $field.GetValue($partial).Dispose();$field.SetValue($partial,[PartialAuditWriteStream]::new($partialPath))
 try{
  Reject {$partial.Append($partialPath,'{"complete":true}')} 'partial append failure poisons writer'
  $length=[IO.FileInfo]::new($partialPath).Length
  Reject {$partial.Append($partialPath,'{"next":true}')} 'no new record follows a torn failed tail'
  Check ([IO.FileInfo]::new($partialPath).Length -eq $length) 'failed writer leaves partial evidence unchanged'
 }finally{$partial.Dispose()}
}finally{if(Get-Command Close-GameProfileJournalProgress -ErrorAction SilentlyContinue){Close-GameProfileJournalProgress $journal};if([IO.Directory]::Exists($fixture) -and [IO.Path]::GetFullPath($fixture).StartsWith([IO.Path]::GetTempPath(),[StringComparison]::OrdinalIgnoreCase)){Remove-Item -LiteralPath $fixture -Recurse -Force}}
