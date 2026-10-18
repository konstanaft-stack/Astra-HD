#include "Framegen/SrIdentityDiagnostics.hpp"
#include "Framegen/StreamlineCameraSnapshot.hpp"
#include <Windows.h>
#include <d3d12.h>
#include <sl_core_types.h>
#include <algorithm>
#include <cstring>
#include <limits>

extern "C" {__declspec(dllexport) cvr::framegen::SrIdentityTraceStorage CyberpunkVR_SrIdentityTrace;}
namespace cvr::framegen {
bool SrIdentityTraceRequested() noexcept {
    static const bool enabled=[] {
        char value[2]{};
        return GetEnvironmentVariableA("CPVR_SR_IDENTITY_TRACE",value,sizeof(value))==1 && value[0]=='1';
    }();
    return enabled;
}
uint64_t SrTraceQpc() noexcept {LARGE_INTEGER q{};QueryPerformanceCounter(&q);return uint64_t(q.QuadPart);}
SrIdentityTraceStorage::SrIdentityTraceStorage() noexcept {
    slotsOffset=uint32_t(offsetof(SrIdentityTraceStorage,slots));LARGE_INTEGER frequency{};
    if(QueryPerformanceFrequency(&frequency))qpcFrequency=uint64_t(frequency.QuadPart);
}
bool SrIdentityTraceStorage::Record(const SrTraceRecord& value) noexcept {
    attempted.fetch_add(1,std::memory_order_relaxed);
    if(value.kind<1||value.kind>3){dropped.fetch_add(1,std::memory_order_relaxed);return false;}
    calls[value.kind-1].fetch_add(1,std::memory_order_relaxed);
    uint32_t unlocked=0;
    if(!writer.compare_exchange_strong(unlocked,1,std::memory_order_acquire,std::memory_order_relaxed)){
        dropped.fetch_add(1,std::memory_order_relaxed);return false;
    }
    const auto previous=published.load(std::memory_order_relaxed);
    if(previous==(std::numeric_limits<uint64_t>::max)()){
        dropped.fetch_add(1,std::memory_order_relaxed);writer.store(0,std::memory_order_release);return false;
    }
    writeEpoch.fetch_add(1,std::memory_order_acq_rel);const auto sequence=previous+1;
    slots[(sequence-1)%Capacity]={sequence,value};published.store(sequence,std::memory_order_relaxed);
    writeEpoch.fetch_add(1,std::memory_order_release);writer.store(0,std::memory_order_release);return true;
}
void RecordSrIdentity(const SrTraceRecord& value) noexcept {CyberpunkVR_SrIdentityTrace.Record(value);}
namespace {
bool ReadBytes(const void* source,void* destination,size_t bytes) noexcept {
    __try {if(!source)return false;std::memcpy(destination,source,bytes);return true;}
    __except(EXCEPTION_EXECUTE_HANDLER){return false;}
}
struct Header {uint64_t next{};sl::StructType type{};uint64_t version{};};
static_assert(sizeof(Header)==32&&sizeof(sl::BaseStructure)==32);
bool ReadToken(const sl::FrameToken* token,uint32_t* value) noexcept {
    __try {
        if(!token||token->structType!=sl::FrameToken::s_structType||token->structVersion!=1)return false;
        *value=uint32_t(*token);return true; // Virtual SDK conversion; zero is VALID.
    } __except(EXCEPTION_EXECUTE_HANDLER){return false;}
}
SrViewportStatus ReadViewport(const void* pointer,uint32_t* value) noexcept {
    __try {
        const auto* view=static_cast<const sl::ViewportHandle*>(pointer);
        if(!view)return SrViewportStatus::Missing;
        if(view->structType!=sl::ViewportHandle::s_structType)return SrViewportStatus::Malformed;
        if(view->structVersion!=1)return SrViewportStatus::UnsupportedVersion;
        *value=uint32_t(*view);return *value==UINT32_MAX?SrViewportStatus::InvalidValue:SrViewportStatus::Known;
    } __except(EXCEPTION_EXECUTE_HANDLER){return SrViewportStatus::Malformed;}
}
void DirectViewport(SrTraceRecord& out,const void* pointer) noexcept {
    const auto status=ReadViewport(pointer,&out.slViewport);out.viewportStatus=uint32_t(status);
    if(status==SrViewportStatus::Known){out.flags|=SrViewportKnown;out.viewportsFound=1;}
}
void EvaluateViewport(SrTraceRecord& out,const sl::BaseStructure* const* inputs,uint32_t count) noexcept {
    if(!count){out.viewportStatus=uint32_t(SrViewportStatus::Missing);return;}
    if(count>SrIdentityTraceStorage::MaxInputs){out.viewportStatus=uint32_t(SrViewportStatus::Limit);return;}
    SrViewportStatus problem=SrViewportStatus::Missing;uint32_t found=0,visited=0,value=UINT32_MAX;
    for(uint32_t input=0;input<count;++input){
        const sl::BaseStructure* current{};
        if(!ReadBytes(inputs?inputs+input:nullptr,&current,sizeof(current))){problem=SrViewportStatus::Malformed;break;}
        std::array<const sl::BaseStructure*,SrIdentityTraceStorage::MaxChain> seen{};uint32_t depth=0;
        while(current){
            if(depth==seen.size()||visited==SrIdentityTraceStorage::MaxNodes){problem=SrViewportStatus::Limit;break;}
            bool repeated=false;for(uint32_t i=0;i<depth;++i)repeated|=seen[i]==current;
            if(repeated){problem=SrViewportStatus::Cycle;break;}
            seen[depth++]=current;++visited;Header header{};
            if(!ReadBytes(current,&header,sizeof(header))){problem=SrViewportStatus::Malformed;break;}
            if(header.type==sl::ViewportHandle::s_structType){
                uint32_t nextValue{};const auto status=ReadViewport(current,&nextValue);
                if(status!=SrViewportStatus::Known){problem=status;break;}
                value=nextValue;++found;
            }
            current=reinterpret_cast<const sl::BaseStructure*>(uintptr_t(header.next));
        }
        if(problem!=SrViewportStatus::Missing)break;
    }
    out.capturedCount=visited;out.viewportsFound=found;
    const auto status=problem!=SrViewportStatus::Missing?problem:found>1?SrViewportStatus::Ambiguous:found==1?SrViewportStatus::Known:SrViewportStatus::Missing;
    out.viewportStatus=uint32_t(status);
    if(status==SrViewportStatus::Known){out.slViewport=value;out.flags|=SrViewportKnown;}
}
bool ReadConstants(const sl::Constants* constants,SrTraceRecord* out) noexcept {
    __try {
        if(!constants||constants->structType!=sl::Constants::s_structType||constants->structVersion<1||constants->structVersion>2)return false;
        const auto camera=SnapshotStreamlineCamera(*constants);SrConstantsWire values{};
        std::memcpy(values.floatBits.data(),camera.values.data(),sizeof(values.floatBits));values.flags=camera.flags;
        for(size_t i=0;i<camera.valuePresent.size();++i)if(camera.valuePresent.test(i))values.valuePresent[i/32]|=1u<<(i%32);
        for(size_t i=0;i<camera.flagPresent.size();++i)if(camera.flagPresent.test(i))values.flagPresent|=1u<<i;
        std::memcpy(out->payload.data(),&values,sizeof(values));out->constantsVersion=constants->structVersion;
        out->constantFloatCount=104;out->constantFlagCount=7;out->payloadKind=1;return true;
    } __except(EXCEPTION_EXECUTE_HANDLER){return false;}
}
bool ReadTagRequest(const sl::ResourceTag* tag,SrTagWire* out) noexcept {
    __try {
        if(!tag||tag->structType!=sl::ResourceTag::s_structType||tag->structVersion!=1)return false;
        out->flags=SrTagKnown;out->type=uint32_t(tag->type);out->lifecycle=uint32_t(tag->lifecycle);
        out->left=tag->extent.left;out->top=tag->extent.top;out->width=tag->extent.width;out->height=tag->extent.height;
        out->resourceStruct=uint64_t(reinterpret_cast<uintptr_t>(tag->resource));
        if(!tag->resource)return true; // A null tag can revoke an old resource.
        const auto* resource=tag->resource;
        if(resource->structType!=sl::Resource::s_structType||resource->structVersion!=1)return false;
        out->native=uint64_t(reinterpret_cast<uintptr_t>(resource->native));out->state=resource->state;
        out->resourceType=uint32_t(resource->type);out->flags|=SrResourceKnown;
        if(resource->native&&(resource->type==sl::ResourceType::eTex2d||resource->type==sl::ResourceType::eBuffer)){
            const auto desc=static_cast<ID3D12Resource*>(resource->native)->GetDesc(); // D3D12 owner; readonly, no AddRef.
            out->actualWidth=desc.Width;out->actualHeight=desc.Height;out->format=uint32_t(desc.Format);
            out->dimension=uint32_t(desc.Dimension);out->sampleCount=desc.SampleDesc.Count;out->depthOrArraySize=desc.DepthOrArraySize;
            out->flags|=SrDescriptorKnown;
        }
        return true;
    } __except(EXCEPTION_EXECUTE_HANDLER){return false;}
}
}
void SnapshotSrConstants(SrTraceRecord& out,const sl::Constants* constants,const sl::FrameToken* token,const sl::ViewportHandle* viewport) noexcept {
    out.kind=uint32_t(SrTraceKind::Constants);out.arguments=uint64_t(reinterpret_cast<uintptr_t>(constants));
    if(ReadToken(token,&out.slFrameToken))out.flags|=SrTokenKnown;else out.flags|=SrArgumentsMalformed;
    DirectViewport(out,viewport);
    if(ReadConstants(constants,&out))out.flags|=SrConstantsKnown;else out.flags|=SrArgumentsMalformed;
}
void SnapshotSrTags(SrTraceRecord& out,const void* viewport,const void* tags,uint32_t count,void* list) noexcept {
    out.kind=uint32_t(SrTraceKind::Tags);out.commandList=uint64_t(reinterpret_cast<uintptr_t>(list));
    out.arguments=uint64_t(reinterpret_cast<uintptr_t>(tags));out.inputCount=count;out.payloadKind=2;DirectViewport(out,viewport);
    if(count>SrIdentityTraceStorage::MaxTags)out.flags|=SrTagsTruncated;
    const auto limit=std::min(count,SrIdentityTraceStorage::MaxTags);
    for(uint32_t i=0;i<limit;++i){
        SrTagWire tag{};
        if(!ReadTagRequest(tags?static_cast<const sl::ResourceTag*>(tags)+i:nullptr,&tag)){tag.flags|=SrTagMalformed;++out.tagMalformedCount;}
        std::memcpy(reinterpret_cast<unsigned char*>(out.payload.data())+i*sizeof(tag),&tag,sizeof(tag));++out.capturedCount;
    }
    // slSetTag has no token argument; RecordTags runs BEFORE original slSetTag.
    // Do not borrow a cached token or report an unobserved API result as eOk.
}
void SnapshotSrEvaluate(SrTraceRecord& out,uint32_t feature,const sl::FrameToken* token,const sl::BaseStructure* const* inputs,uint32_t count,void* list) noexcept {
    out.kind=uint32_t(SrTraceKind::Evaluate);out.feature=feature;out.commandList=uint64_t(reinterpret_cast<uintptr_t>(list));
    out.arguments=uint64_t(reinterpret_cast<uintptr_t>(inputs));out.inputCount=count;
    if(ReadToken(token,&out.slFrameToken))out.flags|=SrTokenKnown;else out.flags|=SrArgumentsMalformed;
    EvaluateViewport(out,inputs,count);
}
}
