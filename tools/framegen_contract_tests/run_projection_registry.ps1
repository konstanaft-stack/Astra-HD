param([Parameter(Mandatory)][string]$AuditDirectory)
$ErrorActionPreference='Stop'
$repo=(Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
$a=[IO.Path]::GetFullPath($AuditDirectory)
[IO.Directory]::CreateDirectory($a)|Out-Null
$source=Join-Path $repo 'src/Framegen/Inputs.cpp'
$text=[IO.File]::ReadAllText($source)
function Slice([string]$first,[string]$last){
    $s=$text.IndexOf($first);$e=$text.IndexOf($last,$s)
    if($s -lt 0 -or $e -lt 0){throw 'Production boundaries changed'}
    return $text.Substring($s,$e-$s)
}
$record=Slice 'struct Record {' 'struct Link {'
$reuse=Slice 'bool ProducerFinished(' 'void TraceGuides('
$acquire=Slice 'std::shared_ptr<const Inputs> AcquireInputs(' 'uint64_t InputVram()'
$release=$text.Substring($text.IndexOf('void ReleaseInputs() {')).TrimEnd()
$release=$release.Substring(0,$release.LastIndexOf('}')) # final namespace close
$prefix=@'
#define NOMINMAX
#include "Framegen/FrameProjectionMetadata.hpp"
#include <array>
#include <algorithm>
#include <mutex>
#include <cstdio>
#include <cstdlib>
#define CVR_DIAGNOSTIC(...) do{}while(0)
using namespace cvr::framegen;
namespace cvr::framegen {
'@
$globals=@'
std::mutex mutex;
std::array<std::shared_ptr<Record>,4> pools[2];
uint64_t unrelatedTagRecords{},projectionMisses[2]{};
void Log(const char*,...){}
bool projectionProducerActive=true;
bool CaptureProjectionEnabled(){return projectionProducerActive;}
'@
$suffix=@'
}
void Check(bool ok,const char* why){if(!ok){std::fprintf(stderr,"FAIL: %s\n",why);std::exit(1);}}
int main(){
    // Queue-free CPU fixture: no fake COM objects or GPU claims. Match nullptr
    // queue only here; production gets the real observed Execute queue.
    auto seed=[](uint32_t frame,bool meta){
        auto r=ReusableRecord(0,meta);Check(bool(r),"eligible record available");
        r->generation=frame;r->projectionOnly=meta;r->expected=r->submitted=7;
        r->data.frameId=frame;r->data.view=1;r->data.poseId=81;r->data.origin=9;
        r->data.poseWriteId=17;r->data.width=1114;r->data.height=1000;
        r->data.hasDepth=true;r->data.submitted=true;r->data.camera.reset=true;
        return r;
    };
    for(uint32_t f=1;f<=4;++f){auto r=seed(f,true);}
    pools[0][0]->expected=pools[0][0]->submitted=3;pools[0][0]->data.submitted=false;
    ReleaseInputs();
    Check(bool(pools[0][0]),"submitted tags awaiting SR Evaluate must survive inactive generator cleanup");
    pools[0][0]->expected=pools[0][0]->submitted=7;pools[0][0]->data.evaluated=true;pools[0][0]->data.submitted=true;
    ReleaseInputs();
    Check(AcquireFrameProjection(1,1,81,9,nullptr).has_value(),"inactive generator cleanup must not erase the active color producer's projection");
    auto p=AcquireFrameProjection(1,1,81,9,nullptr);
    Check(p&&p->srInputKnown&&p->srInputWidth==1114&&p->srInputHeight==1000&&p->poseWriteId==17&&p->reset,"projection preserves exact SR rectangle and camera identity");
    Check(!AcquireInputs(1,1,81,9,nullptr),"metadata cannot be handed to generator as full Inputs");
    Check(!AcquireFrameProjection(2,1,81,9,nullptr)&&!AcquireFrameProjection(1,1,82,9,nullptr)&&!AcquireFrameProjection(1,1,81,10,nullptr),"eye/pose/origin mismatches rejected");
    Check(!AcquireFrameProjection(1,1,81,9,reinterpret_cast<ID3D12CommandQueue*>(1)),"producer queue mismatch rejected without dereference");
    {auto r=seed(5,true);Check(r==pools[0][0],"oldest record reused while CPU snapshot remains alive");}
    Check(p->frameId==1&&!AcquireFrameProjection(1,1,81,9,nullptr),"snapshot immutable after record reuse");
    Check(AcquireFrameProjection(1,2,81,9,nullptr).has_value(),"newer records retained");
    pools[0][1]->invalid=true;Check(!AcquireFrameProjection(1,2,81,9,nullptr),"invalid record rejected");pools[0][1]->invalid=false;
    pools[0][1]->data.submitted=false;Check(!AcquireFrameProjection(1,2,81,9,nullptr),"not yet submitted rejected");pools[0][1]->data.submitted=true;
    // Full records retain the existing strong alias contract.
    pools[0][1]->projectionOnly=false;
    auto full=AcquireInputs(1,2,81,9,nullptr);Check(bool(full),"full inputs remain accessible");
    auto reusable=ReusableRecord(0,true);Check(reusable==pools[0][2],"live generator lease excludes record from reuse");reusable.reset();
    pools[0][2]->unknownProducer=true;
    pools[0][3]->submitted=3;
    reusable=ReusableRecord(0,true);Check(reusable==pools[0][0],"unknown producer and incomplete submission excluded");
    reusable.reset();full.reset();projectionProducerActive=false;ReleaseInputs();
    Check(!pools[0][0]&&!pools[0][1]&&pools[0][2]&&pools[0][3],"stopped producer releases eligible metadata, preserves unfinished records");
    std::puts("PASS: production projection registry identity, immutable metadata, bounded retention, full-input ownership");
}
'@
[IO.File]::WriteAllText("$a/production-registry.cpp",$prefix+"`n"+$record+"`n"+$globals+"`n"+$reuse+"`n"+$acquire+"`n"+$release+"`n"+$suffix,[Text.UTF8Encoding]::new($false))
$c='<WORKSPACE_ROOT>/tools/Cyberpunk-Modding/MSVC-2022/msvc/VC/Tools/MSVC/14.44.35207'
$sdk='<WORKSPACE_ROOT>/tools/Cyberpunk-Modding/MSVC-2022/msvc/Windows Kits/10'
$env:INCLUDE="$c/include;$sdk/Include/10.0.26100.0/ucrt;$sdk/Include/10.0.26100.0/shared;$sdk/Include/10.0.26100.0/um;$sdk/Include/10.0.26100.0/winrt"
$env:LIB="$c/lib/x64;$sdk/Lib/10.0.26100.0/ucrt/x64;$sdk/Lib/10.0.26100.0/um/x64"
& "$c/bin/Hostx64/x64/cl.exe" /nologo /EHsc /std:c++20 /W4 "/I$repo/include" "$a/production-registry.cpp" "/Fo$a/probe.obj" "/Fe$a/probe.exe" 2>&1 | Set-Content "$a/build.log"
if($LASTEXITCODE){Get-Content "$a/build.log";throw 'Registry compile failed'}
& "$a/probe.exe" 2>&1 | Tee-Object -FilePath "$a/result.log"
if($LASTEXITCODE){throw 'Registry regression failed'}
@{source_sha256=(Get-FileHash $source).Hash;result='PASS';scope='CPU registry, not GPU integration';time=(Get-Date -Format o)}|ConvertTo-Json|Set-Content "$a/RESULT.json"
