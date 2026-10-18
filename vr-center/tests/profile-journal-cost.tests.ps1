param([int]$Operations=500)
$ErrorActionPreference='Stop'
. (Join-Path (Split-Path $PSScriptRoot -Parent) 'GameProfileStore.ps1')
$fixture=Join-Path ([IO.Path]::GetTempPath()) ('journal-cost-'+[guid]::NewGuid().ToString('N'))
$context=@{store=[IO.Path]::GetFullPath("$fixture/store")}
$rows=@(0..($Operations-1)|ForEach-Object {@{index=$_;scope='game';path="mods/deep/$_.json";before=@{scope='game';path="mods/deep/$_.json";sha256=('A'*64);size=20};after=$null;phase='planned';suffix=".profile-test-$_";originalHash=$null;originalOwned=$false;stagedOwned=$false}})
$journal=@{_progressOwned=$true;schema=1;id=[guid]::NewGuid().ToString('N');state='applying';cleanupComplete=$false;newProfile=$false;oldActiveId='current';targetId=('a'*32);before=@{files=@();directories=@()};target=@{files=@();directories=@()};operations=$rows;errors=@();conflicts=@();startedUtc=[datetime]::UtcNow.ToString('o')}
try{
 Write-GameProfileJournal $context $journal
 $script:cost=@{writes=0;writeSeconds=0.0;receiptWrites=0;receiptWriteSeconds=0.0;operationWrites=0;operationWriteSeconds=0.0;plainChecks=0;plainSeconds=0.0;storePaths=0;storePathSeconds=0.0;serializations=0;serializationSeconds=0.0}
 $script:oldWrite=${function:Write-GameProfileJson};$script:oldPlain=${function:Assert-GameProfilePlainPath};$script:oldStore=${function:Get-GameProfileStorePath}
 function Write-GameProfileJson([string]$Path,$Value){$t=[Diagnostics.Stopwatch]::StartNew();try{& $script:oldWrite $Path $Value}finally{$t.Stop();$script:cost.writes++;$script:cost.writeSeconds+=$t.Elapsed.TotalSeconds;if($Path.EndsWith('receipt.json')){$script:cost.receiptWrites++;$script:cost.receiptWriteSeconds+=$t.Elapsed.TotalSeconds}else{$script:cost.operationWrites++;$script:cost.operationWriteSeconds+=$t.Elapsed.TotalSeconds}}}
 function Assert-GameProfilePlainPath([string]$Path){$t=[Diagnostics.Stopwatch]::StartNew();try{& $script:oldPlain $Path}finally{$t.Stop();$script:cost.plainChecks++;$script:cost.plainSeconds+=$t.Elapsed.TotalSeconds}}
 function Get-GameProfileStorePath($Context,[string]$Relative){$t=[Diagnostics.Stopwatch]::StartNew();try{& $script:oldStore $Context $Relative}finally{$t.Stop();$script:cost.storePaths++;$script:cost.storePathSeconds+=$t.Elapsed.TotalSeconds}}
 function ConvertTo-Json($InputObject,[int]$Depth,[switch]$Compress){$t=[Diagnostics.Stopwatch]::StartNew();try{Microsoft.PowerShell.Utility\ConvertTo-Json -InputObject $InputObject -Depth $Depth -Compress:$Compress}finally{$t.Stop();$script:cost.serializations++;$script:cost.serializationSeconds+=$t.Elapsed.TotalSeconds}}
 $timer=[Diagnostics.Stopwatch]::StartNew();foreach($op in $rows){$op.phase='removed';$op.originalOwned=$true;Write-GameProfileJournal $context $journal -OperationIndex $op.index};$timer.Stop()
 $script:cost.totalSeconds=$timer.Elapsed.TotalSeconds;$script:cost.operations=$Operations
 Microsoft.PowerShell.Utility\ConvertTo-Json -InputObject $script:cost
}finally{Close-GameProfileJournalProgress $journal;if([IO.Directory]::Exists($fixture) -and [IO.Path]::GetFullPath($fixture).StartsWith([IO.Path]::GetTempPath(),[StringComparison]::OrdinalIgnoreCase)){Remove-Item -LiteralPath $fixture -Recurse -Force}}
