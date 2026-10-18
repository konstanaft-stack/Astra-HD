param([Parameter(Mandatory)][string]$AuditDirectory)
$ErrorActionPreference='Stop'
$repo=(Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
$a=[IO.Path]::GetFullPath($AuditDirectory)
[IO.Directory]::CreateDirectory($a)|Out-Null
$source=Join-Path $repo 'src/Framegen/NativeFgObservation.cpp'
$text=[IO.File]::ReadAllText($source)
$first=$text.IndexOf('template<size_t Slot,auto Method> struct NativeWriteHook;')
$last=$text.IndexOf('template<class Interface,class Install> bool GuardExtendedWrites',$first)
if($first -lt 0 -or $last -lt 0){throw 'Production hook boundaries changed'}
$hook=$text.Substring($first,$last-$first)
$sharedFirst=$text.IndexOf('std::array<std::atomic<ID3D12GraphicsCommandList*>,8> observedLists{};')
$sharedLast=$text.IndexOf('struct CloseMessage',$sharedFirst)
if($sharedFirst -lt 0 -or $sharedLast -lt 0){throw 'Shared write observer boundaries changed'}
$shared=$text.Substring($sharedFirst,$sharedLast-$sharedFirst)
$prefix=@'
#include <d3d12.h>
#include <cstdio>
#include <cstdlib>
#include <array>
#include <atomic>
#include <thread>
#include "Framegen/DeferredNativeCapture.hpp"
namespace cvr::stereo::probe { bool internalCommands{}; }
using cvr::framegen::DeferredNativeCapture;
struct Active { ID3D12GraphicsCommandList* list{}; bool inCall{}; DeferredNativeCapture pendingCapture; struct Resource{ID3D12Resource* resource{};};std::array<Resource,8> resources{}; };
thread_local Active active;
void* originals[86]{};
std::atomic<unsigned> lookups{};
template<class Fn> Fn GetOriginalMethod(void**,size_t slot){++lookups;return reinterpret_cast<Fn>(originals[slot]);}
bool PatchVtableMethod(void**,size_t,void*){return true;}
'@
$suffix=@'
unsigned forwarded{};
void Check(bool ok,const char* why){if(!ok){std::fprintf(stderr,"FAIL: %s\n",why);std::exit(1);}}
void STDMETHODCALLTYPE Clear(ID3D12GraphicsCommandList*,D3D12_GPU_DESCRIPTOR_HANDLE,D3D12_CPU_DESCRIPTOR_HANDLE,ID3D12Resource*,const UINT*,UINT n,const D3D12_RECT*){Check(n==7,"clear arguments forwarded");++forwarded;}
void STDMETHODCALLTYPE Dispatch(ID3D12GraphicsCommandList*,UINT x,UINT y,UINT z){Check(x==2&&y==3&&z==4,"dispatch arguments forwarded");++forwarded;}
void STDMETHODCALLTYPE Texture(ID3D12GraphicsCommandList*,const D3D12_TEXTURE_COPY_LOCATION*,UINT x,UINT y,UINT z,const D3D12_TEXTURE_COPY_LOCATION*,const D3D12_BOX*){Check(x==2&&y==3&&z==4,"texture arguments forwarded");++forwarded;}
int main(){
    void* table[86]{};struct {void** table;} fake{table};
    auto* list=reinterpret_cast<ID3D12GraphicsCommandList*>(&fake);active.list=list;observedLists[0].store(list);
    auto arm=[&](uint64_t r){active.pendingCapture.Arm(r,PendingWriteSerial(list,true));};
    auto take=[&](uint64_t r,bool known){return active.pendingCapture.Take(r,known,PendingWriteSerial(list));};
    originals[49]=reinterpret_cast<void*>(&Clear);originals[14]=reinterpret_cast<void*>(&Dispatch);originals[16]=reinterpret_cast<void*>(&Texture);
    arm(1);Check(!take(1,false),"unknown G waits");
    NativeWriteHook<49,&ID3D12GraphicsCommandList::ClearUnorderedAccessViewUint>::Call(list,{},{},nullptr,nullptr,7,nullptr);
    Check(!take(1,true),"UAV clear must invalidate deferred G");
    arm(2);NativeWriteHook<16,&ID3D12GraphicsCommandList::CopyTextureRegion>::Call(list,nullptr,2,3,4,nullptr,nullptr);
    Check(!take(2,true),"texture write must invalidate deferred G");
    arm(3);NativeWriteHook<14,&ID3D12GraphicsCommandList::Dispatch>::Call(list,2,3,4);
    Check(!take(3,true),"dispatch must invalidate deferred G");
    arm(4);active.inCall=true;NativeWriteHook<14,&ID3D12GraphicsCommandList::Dispatch>::Call(list,2,3,4);active.inCall=false;
    Check(!take(4,true),"another native evaluation invalidates older pending data");
    arm(5);cvr::stereo::probe::internalCommands=true;NativeWriteHook<14,&ID3D12GraphicsCommandList::Dispatch>::Call(list,2,3,4);cvr::stereo::probe::internalCommands=false;
    Check(take(5,true),"internal capture does not invalidate itself");
    arm(6);std::thread writer([&]{NativeWriteHook<14,&ID3D12GraphicsCommandList::Dispatch>::Call(list,2,3,4);});writer.join();
    Check(!take(6,true),"sequential handoff to another recording thread invalidates old pixels");
    arm(7);std::thread barrier([&]{MarkForeignPendingBarrier(list);});barrier.join();
    Check(!take(7,true),"foreign recording thread cannot change tracked states or aliases");
    arm(8);MarkForeignPendingBarrier(list);Check(take(8,true),"owner's confirming barrier preserves pending capture");
    Check(forwarded==6,"original called exactly once for every observed operation");
    Check(lookups==4,"hot callbacks must not acquire global original-method mutex on every draw/dispatch");
    void* secondTable[86]{};decltype(fake) second{secondTable};
    NativeWriteHook<14,&ID3D12GraphicsCommandList::Dispatch>::Call(reinterpret_cast<ID3D12GraphicsCommandList*>(&second),2,3,4);
    NativeWriteHook<14,&ID3D12GraphicsCommandList::Dispatch>::Call(list,2,3,4);
    Check(lookups==6&&forwarded==8,"changing and returning to a vtable refreshes original safely");
    std::puts("PASS: production callbacks, original forwarding, internal exclusion, cross-thread recording handoff");
}
'@
[IO.File]::WriteAllText("$a/production-hooks.cpp",$prefix+"`n"+$shared+"`n"+$hook+"`n"+$suffix,[Text.UTF8Encoding]::new($false))
$c='<WORKSPACE_ROOT>/tools/Cyberpunk-Modding/MSVC-2022/msvc/VC/Tools/MSVC/14.44.35207'
$sdk='<WORKSPACE_ROOT>/tools/Cyberpunk-Modding/MSVC-2022/msvc/Windows Kits/10'
$oldInclude=$env:INCLUDE;$oldLib=$env:LIB
try{
    $env:INCLUDE="$c/include;$sdk/Include/10.0.26100.0/ucrt;$sdk/Include/10.0.26100.0/shared;$sdk/Include/10.0.26100.0/um"
    $env:LIB="$c/lib/x64;$sdk/Lib/10.0.26100.0/ucrt/x64;$sdk/Lib/10.0.26100.0/um/x64"
    foreach($name in @('lifetime','production-hooks')){
        $file=if($name -eq 'lifetime'){Join-Path $PSScriptRoot 'deferred_capture.cpp'}else{"$a/production-hooks.cpp"}
        & "$c/bin/Hostx64/x64/cl.exe" /nologo /EHsc /std:c++20 /W4 "/I$repo/include" $file "/Fo$a/$name.obj" "/Fe$a/$name.exe" 2>&1 | Set-Content "$a/$name-build.log"
        if($LASTEXITCODE){throw "$name compile failed"}
        & "$a/$name.exe" 2>&1 | Tee-Object -FilePath "$a/$name-result.log"
        if($LASTEXITCODE){throw "$name regression failed"}
    }
    @{source_sha256=(Get-FileHash $source).Hash; lifetime='PASS';production_callbacks='PASS';time=(Get-Date -Format o)}|ConvertTo-Json|Set-Content "$a/RESULT.json"
}finally{$env:INCLUDE=$oldInclude;$env:LIB=$oldLib}
