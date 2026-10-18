$ErrorActionPreference='Stop'
# Cache only successful full SHA1 checks. NTFS identity is collected from the
# same read-only shared handle used for SHA1; writers/deleters cannot race it.
# API references: Microsoft FILE_BASIC_INFO / BY_HANDLE_FILE_INFORMATION /
# GetFileInformationByHandleEx / GetVolumeInformationByHandleW.
function Initialize-StockIdentityNative {
    if('VRPorts.StockIdentityV1' -as [type]){return}
    Add-Type -TypeDefinition @'
using System;
using System.IO;
using System.Text;
using System.Runtime.InteropServices;
using System.Security.Cryptography;
using Microsoft.Win32.SafeHandles;
namespace VRPorts {
 public sealed class StockCheck { public bool Verified; public bool CacheHit; public long BytesHashed; public string Identity; }
 public static class StockIdentityV1 {
  [StructLayout(LayoutKind.Sequential)] struct Basic { public long Creation, Access, Write, Change; public uint Attributes; }
  [StructLayout(LayoutKind.Sequential)] struct Info { public uint Attributes; public System.Runtime.InteropServices.ComTypes.FILETIME Creation, Access, Write; public uint Volume, SizeHigh, SizeLow, Links, IndexHigh, IndexLow; }
  [DllImport("kernel32.dll", SetLastError=true)] static extern bool GetFileInformationByHandle(SafeFileHandle h,out Info i);
  [DllImport("kernel32.dll", SetLastError=true)] static extern bool GetFileInformationByHandleEx(SafeFileHandle h,int kind,out Basic i,uint size);
  [DllImport("kernel32.dll", CharSet=CharSet.Unicode, SetLastError=true)] static extern bool GetVolumeInformationByHandleW(SafeFileHandle h,StringBuilder name,uint nameSize,out uint serial,out uint max,out uint flags,StringBuilder fs,uint fsSize);
  [DllImport("kernel32.dll", CharSet=CharSet.Unicode, SetLastError=true)] static extern uint GetFinalPathNameByHandleW(SafeFileHandle h,StringBuilder path,uint size,uint flags);
  static string Identity(FileStream file,string path) {
   Info i; Basic b; uint serial,max,flags; var fs=new StringBuilder(64);var name=new StringBuilder(261);var final=new StringBuilder(32768);
   if(!GetFileInformationByHandle(file.SafeFileHandle,out i) || !GetFileInformationByHandleEx(file.SafeFileHandle,0,out b,(uint)Marshal.SizeOf(typeof(Basic))) || !GetVolumeInformationByHandleW(file.SafeFileHandle,name,261,out serial,out max,out flags,fs,64)) return null;
   if(!String.Equals(fs.ToString(),"NTFS",StringComparison.OrdinalIgnoreCase) || b.Change<=0 || (i.IndexHigh==0 && i.IndexLow==0))return null;
   uint n=GetFinalPathNameByHandleW(file.SafeFileHandle,final,32768,0);if(n==0 || n>=32768)return null;
   string resolved=final.ToString();if(resolved.StartsWith(@"\\?\UNC\"))resolved=@"\\"+resolved.Substring(8);else if(resolved.StartsWith(@"\\?\"))resolved=resolved.Substring(4);
   if(!String.Equals(Path.GetFullPath(path),resolved,StringComparison.OrdinalIgnoreCase) || (b.Attributes&0x400)!=0)throw new IOException("Stock path resolves through a reparse point.");
   return "ntfs-v1|"+i.Volume+"|"+i.IndexHigh+"|"+i.IndexLow+"|"+file.Length+"|"+b.Write+"|"+b.Change;
  }
  public static StockCheck Check(string path,string expected,long size,string cached) {
   using(var file=new FileStream(path,FileMode.Open,FileAccess.Read,FileShare.Read)) {
    var result=new StockCheck();if(file.Length!=size)return result;
    string identity=null;
    try { identity=Identity(file,path); } catch(DllNotFoundException){} catch(EntryPointNotFoundException){} catch(BadImageFormatException){}
    if(identity!=null && String.Equals(identity,cached,StringComparison.Ordinal)) { result.Verified=true;result.CacheHit=true;result.Identity=identity;return result; }
    using(var sha=SHA1.Create()) { var hash=BitConverter.ToString(sha.ComputeHash(file)).Replace("-","");result.BytesHashed=file.Length;result.Verified=String.Equals(hash,expected,StringComparison.OrdinalIgnoreCase); }
    // Recheck under the same handle before allowing this identity to persist.
    if(result.Verified && identity!=null && String.Equals(identity,Identity(file,path),StringComparison.Ordinal))result.Identity=identity;
    return result;
   }
  }
 }
}
'@
}
function Assert-StockIdentityNoReparse([string]$Path){
    $current=[IO.Path]::GetFullPath($Path)
    while($current){
        $item=Get-Item -LiteralPath $current -Force -ErrorAction Stop
        if(($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0){throw 'Shared stock path contains a reparse point.'}
        $parent=Split-Path $current -Parent
        if($parent -eq $current){break};$current=$parent
    }
}
function Test-VerifiedStockIdentity([string]$Path,[string]$ExpectedSha1,[long]$ExpectedSize,[string]$CacheDirectory){
    if($ExpectedSha1 -notmatch '^[a-fA-F0-9]{40}$' -or $ExpectedSize -lt 0){throw 'Invalid shared stock expectation.'}
    $full=[IO.Path]::GetFullPath($Path)
    Assert-StockIdentityNoReparse $full
    $keyText=$full.ToUpperInvariant()+'|'+$ExpectedSha1.ToUpperInvariant()
    $sha=[Security.Cryptography.SHA256]::Create()
    try{$key=[Convert]::ToHexString($sha.ComputeHash([Text.Encoding]::UTF8.GetBytes($keyText)))}finally{$sha.Dispose()}
    $cachePath=Join-Path $CacheDirectory ($key+'.json');$cached=$null;$cacheSafe=$true
    # Cached proof must never be read through a linked directory or file.
    # Unsafe/unavailable cache metadata only disables the optimization.
    try{
        if(Test-Path -LiteralPath $CacheDirectory){Assert-StockIdentityNoReparse $CacheDirectory}
        if(Test-Path -LiteralPath $cachePath){Assert-StockIdentityNoReparse $cachePath}
    }catch{$cacheSafe=$false}
    if($cacheSafe){try{
        $record=Get-Content -LiteralPath $cachePath -Raw -ErrorAction Stop|ConvertFrom-Json -ErrorAction Stop
        if($record.schema -eq 1 -and $record.path -ceq $full -and $record.expectedSha1 -ceq $ExpectedSha1.ToUpperInvariant() -and $record.size -eq $ExpectedSize -and $record.identity -match '^ntfs-v1\|'){$cached=[string]$record.identity}
    }catch{}}
    Initialize-StockIdentityNative
    $result=[VRPorts.StockIdentityV1]::Check($full,$ExpectedSha1,$ExpectedSize,$cached)
    if($cacheSafe -and $result.Verified -and $result.Identity -and -not $result.CacheHit){
        # Cache errors cannot turn a verified stock file into a failed profile
        # operation. The next attempt simply performs the full SHA1 again.
        $temporary=$null
        try{
            $null=[IO.Directory]::CreateDirectory($CacheDirectory)
            Assert-StockIdentityNoReparse $CacheDirectory
            $temporary=Join-Path $CacheDirectory ([guid]::NewGuid().ToString('N')+'.tmp')
            $json=@{schema=1;path=$full;expectedSha1=$ExpectedSha1.ToUpperInvariant();size=$ExpectedSize;identity=$result.Identity}|ConvertTo-Json -Compress
            [IO.File]::WriteAllText($temporary,$json,[Text.UTF8Encoding]::new($false))
            [IO.File]::Move($temporary,$cachePath,$true)
        }catch{}finally{if($temporary -and [IO.File]::Exists($temporary)){[IO.File]::Delete($temporary)}}
    }
    return $result
}
