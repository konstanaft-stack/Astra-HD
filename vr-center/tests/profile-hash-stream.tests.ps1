$ErrorActionPreference='Stop'
. (Join-Path (Split-Path $PSScriptRoot -Parent) 'ProfilePathChecks.ps1')
Add-Type -TypeDefinition @'
using System;
using System.IO;
public sealed class ProfileReadProbe : Stream {
 readonly MemoryStream inner; readonly int shortRead; readonly int failAt;
 public int Calls,MaxRequest; public bool Closed; public byte[] LastBuffer;
 public ProfileReadProbe(byte[] bytes,int chunk=Int32.MaxValue,int fail=-1){inner=new MemoryStream(bytes);shortRead=chunk;failAt=fail;}
 public override int Read(byte[] buffer,int offset,int count){LastBuffer=buffer;Calls++;MaxRequest=Math.Max(MaxRequest,count);if(failAt>=0&&Calls>=failAt)throw new IOException("injected read failure");return inner.Read(buffer,offset,Math.Min(count,shortRead));}
 public bool ReturnedBufferCleared { get { if(LastBuffer==null)return false; foreach(byte value in LastBuffer)if(value!=0)return false;return true; } }
 public override int Read(Span<byte> buffer){Calls++;MaxRequest=Math.Max(MaxRequest,buffer.Length);if(failAt>=0&&Calls>=failAt)throw new IOException("injected read failure");return inner.Read(buffer.Slice(0,Math.Min(buffer.Length,shortRead)));}
 public override bool CanRead=>true;public override bool CanSeek=>true;public override bool CanWrite=>false;public override long Length=>inner.Length;public override long Position{get=>inner.Position;set=>inner.Position=value;}
 public override long Seek(long o,SeekOrigin s)=>inner.Seek(o,s);public override void Flush(){}public override void SetLength(long v)=>throw new NotSupportedException();public override void Write(byte[] b,int o,int c)=>throw new NotSupportedException();
 protected override void Dispose(bool disposing){Closed=true;if(disposing)inner.Dispose();base.Dispose(disposing);}
}
'@
foreach($length in @(0,1,4095,4096,4097,131071,131072,131073,1048583)){
 $bytes=[byte[]]::new($length);[Random]::new(740+$length).NextBytes($bytes)
 foreach($position in @(0,[Math]::Min(7,$length))){foreach($chunk in @(17,[int]::MaxValue)){
  $standard=[ProfileReadProbe]::new($bytes,$chunk);$candidate=[ProfileReadProbe]::new($bytes,$chunk)
  try{
   $standard.Position=$position;$candidate.Position=$position
   $expected=[Security.Cryptography.SHA256]::HashData($standard);$actual=[VRCenter.ProfilePathChecks]::HashStream($candidate)
   if([Convert]::ToHexString($expected) -cne [Convert]::ToHexString($actual)){throw "SHA mismatch length=$length position=$position chunk=$chunk"}
   if($candidate.Position -ne $standard.Position -or $candidate.Closed){throw 'Position/stream ownership changed'}
   if($candidate.MaxRequest -ne 131072){throw 'Read request is not bounded 128KiB'}
   if(-not $candidate.ReturnedBufferCleared){throw 'Returned pooled buffer not cleared'}
  }finally{$standard.Dispose();$candidate.Dispose()}
 }}
}
$failure=[ProfileReadProbe]::new([byte[]]::new(1048583),131072,2)
try{$failed=$false;try{[VRCenter.ProfilePathChecks]::HashStream($failure)|Out-Null}catch{$failed=$_.Exception.GetBaseException().Message -eq 'injected read failure'};if(-not $failed -or $failure.Closed -or -not $failure.ReturnedBufferCleared){throw 'Read error/input ownership/finally buffer clearing changed'}}finally{$failure.Dispose()}
$bytes=[byte[]]::new(1048583);$warm=[ProfileReadProbe]::new($bytes);[VRCenter.ProfilePathChecks]::HashStream($warm)|Out-Null;$warm.Dispose()
$probe=[ProfileReadProbe]::new($bytes);$before=[GC]::GetAllocatedBytesForCurrentThread();[VRCenter.ProfilePathChecks]::HashStream($probe)|Out-Null;$allocated=[GC]::GetAllocatedBytesForCurrentThread()-$before
if($probe.Calls -ne 10){throw ('Unexpected chunk read count: '+$probe.Calls)}
if($allocated -ge 65536){throw 'Warm helper allocated a large per-stream buffer'}
$probe.Dispose()
@{allocationBytes=$allocated;streamBytes=$bytes.Length;readCalls=10}|ConvertTo-Json
'PASS: exact SHA/current position/short reads/EOF/errors/input ownership/bounded reads across boundary sizes.'
