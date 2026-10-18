$ErrorActionPreference='Stop'
. (Join-Path (Split-Path $PSScriptRoot -Parent) 'GameProfileStore.ps1')
$fixture=Join-Path ([IO.Path]::GetTempPath()) ('journal-append-failure-'+[guid]::NewGuid().ToString('N'))
$game=Join-Path $fixture 'game';$data=Join-Path $fixture 'data'
function Put($path,$text){[IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($path))|Out-Null;[IO.File]::WriteAllText($path,$text)}
function Check($ok,$why){if(-not $ok){throw $why};Write-Output "PASS: $why"}
$store=@{data=$data;scopes=@(@{id='game';root=$game;kind='directory';requiredFiles=@('required.exe');exclusions=@()})}
try{
 Put "$game/required.exe" 'never executed';Put "$game/settings.ini" 'A'
 Save-GameProfile $store|Out-Null
 $b=(Copy-GameProfile $store 'B').activeId
 Put "$game/settings.ini" 'B';Save-GameProfile $store|Out-Null
 Activate-GameProfile $store 'current'|Out-Null
 Add-Type -TypeDefinition @'
using System;
using System.IO;
public sealed class PartialTransactionAuditStream : FileStream {
 public PartialTransactionAuditStream(string path):base(path,FileMode.Append,FileAccess.Write,FileShare.Read){}
 public override void Write(byte[] bytes,int offset,int count){base.Write(bytes,offset,Math.Max(1,count/2));base.Flush();throw new IOException("injected partial transaction audit append");}
}
'@
 $script:originalWriter=${function:Write-GameProfileJournal};$script:injected=$false;$script:failedId=$null
 function Write-GameProfileJournal($Context,$Journal,[Nullable[int]]$OperationIndex=$null){
  if(-not $script:injected -and $null -ne $OperationIndex -and $Journal.operations[[int]$OperationIndex].phase -eq 'placed'){
   $script:injected=$true;$script:failedId=$Journal.id
   $writer=$Journal._progressWriter
   $field=[VRCenter.JournalProgressWriter].GetField('stream',[Reflection.BindingFlags]'Instance,NonPublic')
   $field.GetValue($writer).Dispose();$field.SetValue($writer,[PartialTransactionAuditStream]::new($writer.PathName))
  }
  & $script:originalWriter $Context $Journal $OperationIndex
 }
 $failed=$false;try{Activate-GameProfile $store $b|Out-Null}catch{$failed=$true}
 Check ($failed -and $script:injected) 'partial audit append aborts the live fixture apply'
 Check ([IO.File]::ReadAllText("$game/settings.ini") -ceq 'A') 'rollback restores original bytes after partial append'
 $catalog=Get-GameProfileCatalog $store
 Check ($catalog.activeId -ceq 'current' -and $catalog.recoveryRequired.Count -eq 0) 'rolled-back fixture retains active profile and clears barrier'
 $base="$data/profile-store/journals/$script:failedId"
 $receipt=Read-GameProfileJson "$base/receipt.json"
 Check ($receipt.state -ceq 'rolled-back' -and $receipt.cleanupComplete -and $receipt.errors.Count -gt 0) 'append failure still persists atomic rollback/error receipt'
 $steps=@(Read-GameProfileOperationProgress (Get-GameProfileContext $store) $script:failedId)
 Check ($steps.Count -eq 1 -and $steps[0].phase -ceq 'renamed') 'reader retains renamed evidence and ignores only torn final record'
 foreach($name in @('progress.jsonl','before.json','target.json','plan.json')){$held=[IO.File]::Open("$base/$name",[IO.FileMode]::Open,[IO.FileAccess]::ReadWrite,[IO.FileShare]::None);$held.Dispose()}
 Check $true 'rollback finally releases progress and all immutable file handles'
 Check (@(Get-ChildItem -LiteralPath $game -Recurse -File|Where-Object Name -Like '*.profile-*').Count -eq 0) 'failed append leaves no transaction siblings'
}finally{if([IO.Directory]::Exists($fixture) -and [IO.Path]::GetFullPath($fixture).StartsWith([IO.Path]::GetTempPath(),[StringComparison]::OrdinalIgnoreCase)){Remove-Item -LiteralPath $fixture -Recurse -Force}}
