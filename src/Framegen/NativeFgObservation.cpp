#include "Framegen/NativeFgObservation.hpp"
#include "Framegen/NativeFgReadback.hpp"
#include "Framegen/NativeFgOutput.hpp"
#include "Framegen/DeferredNativeCapture.hpp"
#include "Framegen/SrHistoryIntegration.hpp"
#include "Camera/ImagePoseIdentity.hpp"
#include "Hooks/SwapChainInternal.hpp"
#include "Render/RenderProbeScope.hpp"
#include "Render/ResourceAllocation.hpp"
#include "Framegen/RecordingBufferStates.hpp"
#include <nvsdk_ngx_defs_dlssg.h>
#include <atomic>
#include <array>
#include <cstring>
#include <mutex>
#include <tuple>
#include <windows.h>

extern void Log(const char*,...);
namespace cvr::framegen {
namespace {
bool NativeFgOutputObservationEnabled(){
    static const bool value=[] {char flag[8]{};return GetEnvironmentVariableA(
        "CPVR_NATIVE_FG_OUTPUT_DIAG",flag,sizeof(flag))==1&&flag[0]=='1';}();
    return value;
}
std::atomic<uint64_t> calls{};
std::atomic<uint32_t> worldSamples{};
struct TrackedResource {
    ID3D12Resource* resource{};
    uint32_t state{},transitions{};
    uint32_t lastSubresource{},lastFlags{};
    uint32_t aliasBarriers{};
    bool known{},split{};
};
struct EvaluationScope {
    uint64_t serial{};
    uint64_t recording{};
    uint64_t evaluationRecording{};
    bool inCall{},log{};
    bool writesGuarded{};
    uint32_t legacyBarriers{},enhancedBarriers{};
    ID3D12GraphicsCommandList* list{};
    const NVSDK_NGX_Handle* handle{};
    std::optional<NativeFgReceipt> receipt;
    DeferredNativeCapture pendingCapture;
    std::array<TrackedResource,8> resources{}; // host four, then owned companion input/G/flag/real
};
thread_local EvaluationScope active;
std::array<std::atomic<ID3D12GraphicsCommandList*>,8> observedLists{};
std::array<std::atomic<uint64_t>,8> writeSerials{};
std::array<std::atomic<DWORD>,8> pendingOwners{};
std::array<std::array<std::atomic<ID3D12Resource*>,3>,8> pendingResources{};
uint64_t Recording(ID3D12GraphicsCommandList* list);
struct FlagRecording {
    std::mutex mutex;
    RecordingBufferStates<Microsoft::WRL::ComPtr<ID3D12Resource>> states;
};
std::array<FlagRecording,8> flagRecordings;
FlagRecording* FlagsFor(ID3D12GraphicsCommandList* list){
    if(!list)return nullptr;
    for(size_t i=0;i<observedLists.size();++i)if(observedLists[i].load()==list)return &flagRecordings[i];
    return nullptr;
}
Microsoft::WRL::ComPtr<ID3D12Resource> SmallFlag(ID3D12Resource* resource){
    if(!resource)return {};
    const auto d=resource->GetDesc();
    if(d.Dimension!=D3D12_RESOURCE_DIMENSION_BUFFER||d.Width<4||d.Width>64||
       !(d.Flags&D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS))return {};
    return Microsoft::WRL::ComPtr<ID3D12Resource>(resource);
}
void ObserveFlagAccess(ID3D12GraphicsCommandList* list,ID3D12Resource* resource,D3D12_RESOURCE_STATES state){
    if(cvr::stereo::probe::internalCommands)return;
    auto* record=FlagsFor(list);if(!record)return;
    const auto epoch=Recording(list);auto owner=SmallFlag(resource);if(!epoch||!owner)return;
    std::lock_guard lock(record->mutex);record->states.Access(epoch,reinterpret_cast<uintptr_t>(resource),std::move(owner),unsigned(state));
}
void InvalidateFlagRecording(ID3D12GraphicsCommandList* list){
    if(auto* record=FlagsFor(list)){const auto epoch=Recording(list);std::lock_guard lock(record->mutex);record->states.Invalidate(epoch);}
}
void ObserveFlagBarriers(ID3D12GraphicsCommandList* list,UINT count,const D3D12_RESOURCE_BARRIER* barriers){
    auto* record=FlagsFor(list);if(!record||!barriers)return;const auto epoch=Recording(list);if(!epoch)return;
    for(UINT i=0;i<count;++i){const auto& b=barriers[i];
        if(b.Type==D3D12_RESOURCE_BARRIER_TYPE_ALIASING){std::lock_guard lock(record->mutex);
            record->states.InvalidateIf(epoch,[&](uintptr_t,const auto& owner){return cvr::gpu::AliasingInvalidatesResource(owner.Get(),b.Aliasing);});}
        else if(b.Type==D3D12_RESOURCE_BARRIER_TYPE_TRANSITION){auto owner=SmallFlag(b.Transition.pResource);if(!owner)continue;
            const auto phase=(b.Flags&D3D12_RESOURCE_BARRIER_FLAG_BEGIN_ONLY)?BarrierPhase::Begin:
                (b.Flags&D3D12_RESOURCE_BARRIER_FLAG_END_ONLY)?BarrierPhase::End:BarrierPhase::Full;
            std::lock_guard lock(record->mutex);record->states.Transition(epoch,reinterpret_cast<uintptr_t>(b.Transition.pResource),std::move(owner),
                unsigned(b.Transition.StateBefore),unsigned(b.Transition.StateAfter),phase,b.Transition.Subresource==D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES);
        }
    }
}
uint64_t PendingWriteSerial(ID3D12GraphicsCommandList* list,bool arm=false){
    for(size_t i=0;i<observedLists.size();++i)if(observedLists[i].load()==list){
        if(arm){pendingOwners[i].store(GetCurrentThreadId());pendingResources[i][0].store(active.resources[0].resource);
            pendingResources[i][1].store(active.resources[1].resource);pendingResources[i][2].store(active.resources[3].resource);}
        return writeSerials[i].load();
    }return 0;
}
void MarkPendingWrite(ID3D12GraphicsCommandList* list,ID3D12Resource* destination=nullptr){
    if(cvr::stereo::probe::internalCommands)return;
    for(size_t i=0;i<observedLists.size();++i)if(observedLists[i].load()==list){
        if(!destination||pendingResources[i][0].load()==destination||pendingResources[i][1].load()==destination||pendingResources[i][2].load()==destination)
            ++writeSerials[i];
        return;
    }
}
void MarkForeignPendingBarrier(ID3D12GraphicsCommandList* list){
    if(cvr::stereo::probe::internalCommands)return;
    for(size_t i=0;i<observedLists.size();++i)if(observedLists[i].load()==list){
        if(pendingOwners[i].load()!=GetCurrentThreadId())++writeSerials[i];
        return;
    }
}
struct CloseMessage {uint32_t id{};char text[1800]{};};
struct CloseObservation {
    ID3D12GraphicsCommandList* list{};uint64_t call{},recording{};unsigned messages{};
    std::array<CloseMessage,16> details{};
    std::array<TrackedResource,8> resources{};
};
thread_local CloseObservation* closing{};
using CloseFn=HRESULT(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*);
uint64_t Recording(ID3D12GraphicsCommandList* list);
HRESULT STDMETHODCALLTYPE ObserveClose(ID3D12GraphicsCommandList* list){
    const auto original=GetOriginalMethod<CloseFn>(*reinterpret_cast<void***>(list),9);
    if(!original)return E_UNEXPECTED;
    struct Restore {CloseObservation* prior;~Restore(){closing=prior;}} restore{closing};
    const bool tracked=NativeFgOutputObservationEnabled()&&active.serial&&active.list==list&&
        active.evaluationRecording&&active.evaluationRecording==Recording(list);
    closing=nullptr;
    if(!tracked)return original(list);
    CloseObservation observed{};observed.list=list;observed.call=active.serial;observed.recording=Recording(list);observed.resources=active.resources;
    closing=&observed;
    // Exactly one original call, unchanged HRESULT. No Reset, retry or resource
    // mutation: this locates the first failure rather than masking the SL loop.
    const HRESULT result=original(list);
    if(FAILED(result)||observed.messages){
        static std::atomic<unsigned> failures{},successfulWithErrors{};
        if(FAILED(result)?++failures<=4:++successfulWithErrors<=1){
            Log("[NATIVE-FG-CLOSE] tick=%llu thread=%lu call=%llu recording=%llu list=%p hr=%08X validationMessages=%u\n",
                GetTickCount64(),GetCurrentThreadId(),observed.call,observed.recording,list,unsigned(result),observed.messages);
            for(unsigned i=0;i<observed.messages&&i<observed.details.size();++i){const auto& m=observed.details[i];
                Log("[NATIVE-FG-CLOSE-VALIDATION] call=%llu id=%u %s\n",observed.call,m.id,m.text);}
            for(unsigned i=0;i<observed.resources.size();++i){const auto& r=observed.resources[i];
                // Addresses only: an invalid resource is precisely what Close
                // may be reporting, so do not dereference diagnostic pointers.
                Log("[NATIVE-FG-CLOSE-RESOURCE] call=%llu slot=%u resource=%p state=%u known=%d split=%d transitions=%u aliases=%u\n",
                    observed.call,i,r.resource,r.state,int(r.known),int(r.split),r.transitions,r.aliasBarriers);
            }
        }
    }
    return result;
}
using CopyBufferFn=void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*,ID3D12Resource*,UINT64,ID3D12Resource*,UINT64,UINT64);
using CopyResourceFn=void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*,ID3D12Resource*,ID3D12Resource*);
void NoteCopyDestination(ID3D12GraphicsCommandList* list,ID3D12Resource* destination){
    ObserveFlagAccess(list,destination,D3D12_RESOURCE_STATE_COPY_DEST);
    if(!active.serial||active.list!=list||cvr::stereo::probe::internalCommands)return;
    for(auto& r:active.resources)if(r.resource==destination){
        // A post-evaluation write would replace the pending evaluation's pixels
        // or permission flag. Do not capture it later under the older receipt.
        if(!active.inCall && (&r==&active.resources[0]||&r==&active.resources[1]||&r==&active.resources[3]))
            active.pendingCapture.Cancel();
        // CopyBufferRegion/CopyResource requires COPY_DEST, including implicit
        // COMMON promotion. This records the destination's state at this point.
        r.state=D3D12_RESOURCE_STATE_COPY_DEST;r.known=true;r.split=false;
    }
}
void STDMETHODCALLTYPE ObserveCopyBuffer(ID3D12GraphicsCommandList* list,ID3D12Resource* destination,UINT64 destinationOffset,
    ID3D12Resource* source,UINT64 sourceOffset,UINT64 bytes){
    const auto original=GetOriginalMethod<CopyBufferFn>(*reinterpret_cast<void***>(list),15);
    if(original){MarkPendingWrite(list,destination);original(list,destination,destinationOffset,source,sourceOffset,bytes);NoteCopyDestination(list,destination);}
}
void STDMETHODCALLTYPE ObserveCopyResource(ID3D12GraphicsCommandList* list,ID3D12Resource* destination,ID3D12Resource* source){
    const auto original=GetOriginalMethod<CopyResourceFn>(*reinterpret_cast<void***>(list),17);
    if(original){MarkPendingWrite(list,destination);original(list,destination,source);NoteCopyDestination(list,destination);
        SrObserveTaggedCopy(list,destination,source);}
}
constexpr GUID recordingKey{0x9a6db9d2,0x8dd9,0x4d59,{0xb7,0xc4,0xf4,0xab,0x6c,0x30,0x75,0x5b}};
std::atomic<uint64_t> recordingSerial{};
uint64_t Recording(ID3D12GraphicsCommandList* list){
    uint64_t value{};UINT bytes=sizeof(value);
    return list&&SUCCEEDED(list->GetPrivateData(recordingKey,&bytes,&value))&&bytes==sizeof(value)?value:0;
}
// Any unclassified GPU write ends the deferred window. This is intentionally
// conservative: it may drop a phase but can never attribute later pixels to NGX.
template<size_t Slot,auto Method> struct NativeWriteHook;
template<size_t Slot,class Interface,class... Args,void(STDMETHODCALLTYPE Interface::*Method)(Args...)>
struct NativeWriteHook<Slot,Method> {
    static void STDMETHODCALLTYPE Call(Interface* self,Args... args){
        auto* list=static_cast<ID3D12GraphicsCommandList*>(self);
        if constexpr(Slot==80)if(active.list==list)++active.enhancedBarriers;
        MarkPendingWrite(list);
        if(active.list==list&&!active.inCall&&!cvr::stereo::probe::internalCommands)active.pendingCapture.Cancel();
        using Fn=void(STDMETHODCALLTYPE*)(Interface*,Args...);
        // The registry preserves the first original for a vtable/slot. Avoid
        // taking its global mutex on every draw/dispatch in the entire game.
        static thread_local void** cachedTable{};
        static thread_local Fn cachedOriginal{};
        auto** table=*reinterpret_cast<void***>(self);
        if(cachedTable!=table||!cachedOriginal){cachedOriginal=GetOriginalMethod<Fn>(table,Slot);cachedTable=table;}
        if(cachedOriginal)cachedOriginal(self,args...);
        if constexpr(Slot==49||Slot==50){
            if(cachedOriginal)ObserveFlagAccess(list,std::get<2>(std::tuple<Args...>(args...)),D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        }
        if constexpr(Slot==80)if(cachedOriginal)InvalidateFlagRecording(list); // Enhanced states are not translated speculatively.
    }
    static bool Install(void** table){return PatchVtableMethod(table,Slot,reinterpret_cast<void*>(&Call));}
};
template<class Interface,class Install> bool GuardExtendedWrites(ID3D12GraphicsCommandList* list,Install install){
    Microsoft::WRL::ComPtr<Interface> extended;const auto hr=list->QueryInterface(IID_PPV_ARGS(&extended));
    if(hr==E_NOINTERFACE)return true;
    return SUCCEEDED(hr)&&extended&&static_cast<ID3D12GraphicsCommandList*>(extended.Get())==list&&
        *reinterpret_cast<void***>(extended.Get())==*reinterpret_cast<void***>(list)&&install(*reinterpret_cast<void***>(list));
}
bool GuardNativeWrites(ID3D12GraphicsCommandList* list){
    auto** table=*reinterpret_cast<void***>(list);bool ok=true;
#define GUARD(slot,type,method) ok=NativeWriteHook<slot,&type::method>::Install(table)&&ok
    GUARD(12,ID3D12GraphicsCommandList,DrawInstanced);GUARD(13,ID3D12GraphicsCommandList,DrawIndexedInstanced);
    GUARD(14,ID3D12GraphicsCommandList,Dispatch);GUARD(16,ID3D12GraphicsCommandList,CopyTextureRegion);
    GUARD(18,ID3D12GraphicsCommandList,CopyTiles);GUARD(19,ID3D12GraphicsCommandList,ResolveSubresource);
    GUARD(27,ID3D12GraphicsCommandList,ExecuteBundle);GUARD(48,ID3D12GraphicsCommandList,ClearRenderTargetView);
    GUARD(49,ID3D12GraphicsCommandList,ClearUnorderedAccessViewUint);GUARD(50,ID3D12GraphicsCommandList,ClearUnorderedAccessViewFloat);
    GUARD(51,ID3D12GraphicsCommandList,DiscardResource);GUARD(54,ID3D12GraphicsCommandList,ResolveQueryData);
    GUARD(59,ID3D12GraphicsCommandList,ExecuteIndirect);
    ok=GuardExtendedWrites<ID3D12GraphicsCommandList1>(list,[](void** table){bool ok=true;
        GUARD(60,ID3D12GraphicsCommandList1,AtomicCopyBufferUINT);GUARD(61,ID3D12GraphicsCommandList1,AtomicCopyBufferUINT64);
        GUARD(64,ID3D12GraphicsCommandList1,ResolveSubresourceRegion);return ok;})&&ok;
    ok=GuardExtendedWrites<ID3D12GraphicsCommandList2>(list,[](void** table){bool ok=true;
        GUARD(66,ID3D12GraphicsCommandList2,WriteBufferImmediate);return ok;})&&ok;
    ok=GuardExtendedWrites<ID3D12GraphicsCommandList4>(list,[](void** table){bool ok=true;
        GUARD(68,ID3D12GraphicsCommandList4,BeginRenderPass);GUARD(69,ID3D12GraphicsCommandList4,EndRenderPass);
        GUARD(70,ID3D12GraphicsCommandList4,InitializeMetaCommand);GUARD(71,ID3D12GraphicsCommandList4,ExecuteMetaCommand);
        GUARD(72,ID3D12GraphicsCommandList4,BuildRaytracingAccelerationStructure);
        GUARD(73,ID3D12GraphicsCommandList4,EmitRaytracingAccelerationStructurePostbuildInfo);
        GUARD(74,ID3D12GraphicsCommandList4,CopyRaytracingAccelerationStructure);GUARD(76,ID3D12GraphicsCommandList4,DispatchRays);return ok;})&&ok;
    ok=GuardExtendedWrites<ID3D12GraphicsCommandList6>(list,[](void** table){bool ok=true;
        GUARD(79,ID3D12GraphicsCommandList6,DispatchMesh);return ok;})&&ok;
    ok=GuardExtendedWrites<ID3D12GraphicsCommandList7>(list,[](void** table){bool ok=true;
        GUARD(80,ID3D12GraphicsCommandList7,Barrier);return ok;})&&ok;
    ok=GuardExtendedWrites<ID3D12GraphicsCommandList10>(list,[](void** table){bool ok=true;
        GUARD(84,ID3D12GraphicsCommandList10,SetProgram);GUARD(85,ID3D12GraphicsCommandList10,DispatchGraph);return ok;})&&ok;
#undef GUARD
    return ok;
}
std::array<std::atomic<bool>,8> guardedLists{};
bool TrackList(ID3D12GraphicsCommandList* list){
    for(size_t i=0;i<observedLists.size();++i){auto& slot=observedLists[i];auto* known=slot.load();if(known==list)return guardedLists[i].load();
        if(!known&&slot.compare_exchange_strong(known,list)){
            auto** table=*reinterpret_cast<void***>(list);
            const auto buffer=PatchVtableMethod(table,15,reinterpret_cast<void*>(&ObserveCopyBuffer));
            const auto resource=PatchVtableMethod(table,17,reinterpret_cast<void*>(&ObserveCopyResource));
            const bool guarded=buffer&&resource&&GuardNativeWrites(list);guardedLists[i].store(guarded);
            if(NativeFgOutputObservationEnabled()){
                const auto close=PatchVtableMethod(table,9,reinterpret_cast<void*>(&ObserveClose));
                Log("[NATIVE-FG-CLOSE] observer list=%p installed=%d\n",list,int(close));
            }
            Log("[NATIVE-FG-COPY] list=%p bufferHook=%d resourceHook=%d writesGuarded=%d\n",list,int(buffer),int(resource),int(guarded));return guarded;
        }}
    return false;
}
void InvalidateStates(){for(auto& resource:active.resources){resource.known=false;resource.split=false;resource.transitions=0;}}
bool Sample(uint64_t serial){return serial<=32||(serial%300>=1&&serial%300<=3);}
struct ResourceObservation {
    ID3D12Resource* resource{};
    NVSDK_NGX_Result getResult{};
    D3D12_RESOURCE_DESC description{};
};
ResourceObservation Resource(const NVSDK_NGX_Parameter* parameters,const char* key){
    ResourceObservation out;
    out.getResult=parameters->Get(key,&out.resource);
    if(NVSDK_NGX_SUCCEED(out.getResult)&&out.resource)out.description=out.resource->GetDesc();
    else out.resource=nullptr;
    return out;
}
bool CopyMatrix(void* pointer,float* out) noexcept {
    if(!pointer)return false;
    __try {std::memcpy(out,pointer,16*sizeof(float));return true;}
    __except(EXCEPTION_EXECUTE_HANDLER){return false;}
}
void RecordGuides(uint64_t serial,const NVSDK_NGX_Parameter* params,const camera::ImagePoseIdentity& image){
    const auto source=image.pose.mainEyeSource.sourceId;
    if(!image||!((source>=8&&source<12)||(source>=128&&source<136)))return;
    const auto sr=ReadSrHistoryStatus();
    const auto guide=AcquireSrFrameGenerationGuide(image.pose);
    Log("[NATIVE-FG-SR-GUIDE] call=%llu source=%lld eye=%u matched=%d previous=%lld motion=%p depth=%p fence=%llu completed=%llu reset=%d\n",
        serial,static_cast<long long>(source),unsigned(image.pose.mainEyeSource.physicalEye),int(bool(guide)),
        guide?static_cast<long long>(guide->history[0].key.source.sourceId):-1,guide?guide->motion.Get():nullptr,guide?guide->depth.Get():nullptr,
        guide?guide->fenceValue:0,guide?guide->fence->GetCompletedValue():0,guide?int(guide->reset):-1);
    Log("[NATIVE-FG-GUIDE] call=%llu source=%lld eye=%u srSource=%lld srStage=%u srCorrected=%llu srPassthrough=%llu srEye=%u srReset=%d srResetKnown=%d srDelta=%.9g\n",
        serial,static_cast<long long>(source),unsigned(image.pose.mainEyeSource.physicalEye),static_cast<long long>(sr.sourceId),
        unsigned(sr.stage),sr.correctedEvaluations,sr.passthroughEvaluations,sr.lastPhysicalEye,sr.lastReset,int(sr.lastResetKnown),double(sr.lastDeltaMsec));
    const char* resources[]{"DLSSG.MVecs","DLSSG.Depth","DLSSG.HUDLess","DLSSG.UI"};
    for(const auto* key:resources){const auto r=Resource(params,key);
        Log("[NATIVE-FG-GUIDE-RESOURCE] call=%llu key=%s get=%08X resource=%p width=%llu height=%u format=%u\n",
            serial,key,unsigned(r.getResult),r.resource,static_cast<unsigned long long>(r.description.Width),r.description.Height,unsigned(r.description.Format));}
    const char* floats[]{"DLSSG.MvecScaleX","DLSSG.MvecScaleY","DLSSG.JitterOffsetX","DLSSG.JitterOffsetY",
        "DLSSG.CameraPinholeOffsetX","DLSSG.CameraPinholeOffsetY","DLSSG.CameraPosX","DLSSG.CameraPosY","DLSSG.CameraPosZ",
        "DLSSG.CameraNear","DLSSG.CameraFar","DLSSG.CameraFOV","DLSSG.CameraAspectRatio"};
    for(const auto* key:floats){float value{};const auto result=params->Get(key,&value);
        Log("[NATIVE-FG-GUIDE-FLOAT] call=%llu key=%s get=%08X value=%.9g\n",serial,key,unsigned(result),double(value));}
    const char* integers[]{"DLSSG.CameraMotionIncluded","DLSSG.DepthInverted","DLSSG.MvecJittered","DLSSG.MvecDilated",
        "DLSSG.InvertXAxis","DLSSG.InvertYAxis","DLSSG.Reset","DLSSG.NotRenderingGameFrames",
        "DLSSG.MVecsSubrectBaseX","DLSSG.MVecsSubrectBaseY","DLSSG.MVecsSubrectWidth","DLSSG.MVecsSubrectHeight",
        "DLSSG.DepthSubrectBaseX","DLSSG.DepthSubrectBaseY","DLSSG.DepthSubrectWidth","DLSSG.DepthSubrectHeight"};
    for(const auto* key:integers){unsigned value{};const auto result=params->Get(key,&value);
        Log("[NATIVE-FG-GUIDE-UINT] call=%llu key=%s get=%08X value=%u\n",serial,key,unsigned(result),value);}
    const char* matrices[]{"DLSSG.CameraViewToClip","DLSSG.ClipToCameraView","DLSSG.ClipToLensClip","DLSSG.ClipToPrevClip","DLSSG.PrevClipToClip"};
    for(const auto* key:matrices){void* pointer{};float m[16]{};const auto result=params->Get(key,&pointer);
        const bool known=NVSDK_NGX_SUCCEED(result)&&CopyMatrix(pointer,m);
        Log("[NATIVE-FG-GUIDE-MATRIX] call=%llu key=%s get=%08X known=%d value=%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g\n",
            serial,key,unsigned(result),int(known),m[0],m[1],m[2],m[3],m[4],m[5],m[6],m[7],m[8],m[9],m[10],m[11],m[12],m[13],m[14],m[15]);}
}
void Record(const char* phase,uint64_t serial,ID3D12GraphicsCommandList* list,
    const NVSDK_NGX_Handle* handle,const NVSDK_NGX_Parameter* params,uint32_t result){
    if(!params){Log("[NATIVE-FG-EVAL] phase=%s call=%llu paramsMissing=1 result=%08X\n",phase,serial,result);return;}
    unsigned long long frame{};unsigned count{},index{},reset{};
    const auto frameResult=params->Get(NVSDK_NGX_DLSSG_Parameter_BackbufferFrameID,&frame);
    const auto countResult=params->Get(NVSDK_NGX_DLSSG_Parameter_MultiFrameCount,&count);
    const auto indexResult=params->Get(NVSDK_NGX_DLSSG_Parameter_MultiFrameIndex,&index);
    const auto resetResult=params->Get("DLSSG.Reset",&reset);
    const auto input=Resource(params,NVSDK_NGX_DLSSG_Parameter_Backbuffer);
    const auto interpolated=Resource(params,NVSDK_NGX_DLSSG_Parameter_OutputInterpolated);
    const auto real=Resource(params,NVSDK_NGX_DLSSG_Parameter_OutputReal);
    const auto disable=Resource(params,NVSDK_NGX_DLSSG_Parameter_OutputDisableInterpolation);
    // An image label is an observation only. It does not prove that this
    // evaluation's GPU work has run or that interpolation was not disabled.
    const auto image=input.resource?camera::ReadImagePose(input.resource):camera::ImagePoseIdentity{};
    if(phase[0]=='b')RecordGuides(serial,params,image);
    const auto& pose=image.pose;const auto& source=pose.mainEyeSource;
    Log("[NATIVE-FG-EVAL] phase=%s call=%llu result=%08X list=%p handle=%p frame=%llu frameGet=%08X count=%u countGet=%08X index=%u indexGet=%08X reset=%u resetGet=%08X input=%p inputGet=%08X inputSize=%llux%u inputFormat=%u interpolated=%p interpolatedGet=%08X interpolatedSize=%llux%u interpolatedFormat=%u real=%p realGet=%08X disable=%p disableGet=%08X imageKnown=%d imageGeneration=%llu queue=%p source=%lld eye=%u renderFrame=%u renderFrameKnown=%d pose=%llu write=%llu\n",
        phase,serial,result,list,handle,frame,unsigned(frameResult),count,unsigned(countResult),index,unsigned(indexResult),reset,unsigned(resetResult),
        input.resource,unsigned(input.getResult),static_cast<unsigned long long>(input.description.Width),input.description.Height,unsigned(input.description.Format),
        interpolated.resource,unsigned(interpolated.getResult),static_cast<unsigned long long>(interpolated.description.Width),interpolated.description.Height,unsigned(interpolated.description.Format),
        real.resource,unsigned(real.getResult),disable.resource,unsigned(disable.getResult),int(bool(image)),image.generation,reinterpret_cast<void*>(image.queue),
        static_cast<long long>(source.sourceId),unsigned(source.physicalEye),pose.renderFrameId,int(pose.hasRenderFrameId),pose.poseId,pose.writeId);
}
}
bool TraceNativeFgCloseValidation(uint32_t id,const char* message) noexcept {
    if(!closing)return false;
    const auto index=closing->messages++;
    if(index<closing->details.size()){
        auto& detail=closing->details[index];detail.id=id;
        if(message){const auto length=strnlen_s(message,sizeof(detail.text)-1);std::memcpy(detail.text,message,length);detail.text[length]=0;}
    }
    return true;
}
bool EnsureNativeResourceCopyObservation(ID3D12GraphicsCommandList* list) noexcept {
    if(!list)return false;
    try{return PatchVtableMethod(*reinterpret_cast<void***>(list),17,reinterpret_cast<void*>(&ObserveCopyResource));}
    catch(...){return false;}
}
void ObserveNativeFgStereoResult(ID3D12GraphicsCommandList*list,const NVSDK_NGX_Handle*handle,const NativeFgReceipt&receipt) noexcept {
    if(active.inCall&&active.list==list&&active.handle==handle&&ValidNativeFgReceipt(receipt))active.receipt=receipt;
}
bool CopyNativeFgInitialPermission(ID3D12GraphicsCommandList*list,ID3D12Resource*destination) noexcept {
    const auto reject=[&](const char* reason){
        // Bounded observation only: do not infer a state, change a barrier or
        // substitute an allow/deny value when the native flag is unknown.
        static std::atomic<uint64_t> failures{};const auto n=++failures;
        if(n<=8||n%512==0){const auto& flag=active.resources[3];D3D12_RESOURCE_DESC desc{};
            D3D12_HEAP_PROPERTIES heap{};D3D12_HEAP_FLAGS flags{};HRESULT hr=E_POINTER;
            if(active.inCall&&active.list==list&&active.recording&&active.recording==Recording(list)&&flag.resource){
                desc=flag.resource->GetDesc();hr=flag.resource->GetHeapProperties(&heap,&flags);}
            Log("[NATIVE-FG-PERMISSION] failure=%llu reason=%s call=%llu inCall=%d list=%p activeList=%p recording=%llu currentRecording=%llu flag=%p known=%d split=%d state=%u legacy=%u enhanced=%u dimension=%u width=%llu resourceFlags=%u heapHr=%08X heapType=%u heapFlags=%u\n",
                n,reason,active.serial,int(active.inCall),list,active.list,active.recording,Recording(list),flag.resource,int(flag.known),int(flag.split),flag.state,
                active.legacyBarriers,active.enhancedBarriers,unsigned(desc.Dimension),desc.Width,unsigned(desc.Flags),unsigned(hr),unsigned(heap.Type),unsigned(flags));
        }return false;
    };
    if(!active.inCall||active.list!=list||!active.recording||active.recording!=Recording(list)||!destination)return reject("scope");
    const auto flag=active.resources[3];if(!flag.resource||!flag.known||flag.split)return reject("state");
    const auto desc=flag.resource->GetDesc();if(desc.Dimension!=D3D12_RESOURCE_DIMENSION_BUFFER||desc.Width<4)return false;
    const auto old=D3D12_RESOURCE_STATES(flag.state);
    D3D12_RESOURCE_BARRIER b{};b.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition={flag.resource,D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,old,D3D12_RESOURCE_STATE_COPY_SOURCE};
    if(old!=D3D12_RESOURCE_STATE_COPY_SOURCE)list->ResourceBarrier(1,&b);
    list->CopyBufferRegion(destination,0,flag.resource,0,4);
    std::swap(b.Transition.StateBefore,b.Transition.StateAfter);
    if(old!=D3D12_RESOURCE_STATE_COPY_SOURCE)list->ResourceBarrier(1,&b);
    return true;
}
void BeginNativeFgCompanionOutput(ID3D12GraphicsCommandList*list,ID3D12Resource*input,ID3D12Resource*generated,ID3D12Resource*disable,ID3D12Resource*realOutput) noexcept {
    if(!active.inCall||active.list!=list||!active.recording||active.recording!=Recording(list))return;
    ID3D12Resource* resources[]{input,generated,disable,realOutput};
    for(unsigned i=0;i<4;++i){auto& tracked=active.resources[4+i];tracked={};tracked.resource=resources[i];
        tracked.state=i?D3D12_RESOURCE_STATE_UNORDERED_ACCESS:D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;tracked.known=true;}
}
bool EndNativeFgCompanionOutput(ID3D12GraphicsCommandList*list) noexcept {
    if(!active.inCall||active.list!=list||!active.recording||active.recording!=Recording(list))return false;
    for(unsigned i=4;i<8;++i)if(!active.resources[i].resource||!active.resources[i].known||active.resources[i].split)return false;
    if(active.log)for(unsigned i=4;i<8;++i)Log("[PAIRED-FG-STATE] call=%llu role=%u state=%u transitions=%u aliases=%u\n",active.serial,i-4,active.resources[i].state,active.resources[i].transitions,active.resources[i].aliasBarriers);
    for(unsigned i=4;i<8;++i){const auto resource=active.resources[i];const auto wanted=i==4?D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE:D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        if(resource.state!=unsigned(wanted)){D3D12_RESOURCE_BARRIER b{};b.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            b.Transition={resource.resource,D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,D3D12_RESOURCE_STATES(resource.state),wanted};list->ResourceBarrier(1,&b);}
        active.resources[i]={};
    }return true;
}
void RecordNativeFgCompanionOutput(ID3D12GraphicsCommandList*list,ID3D12Resource*real,ID3D12Resource*generated,
        ID3D12Resource*disable,const NativeFgReceipt&receipt) noexcept {
    if(!active.inCall||active.list!=list||!active.recording||active.recording!=Recording(list)||!receipt.paired)return;
    if(active.resources[4].resource!=real||active.resources[5].resource!=generated||active.resources[6].resource!=disable)return;
    for(unsigned i=4;i<7;++i)if(!active.resources[i].known||active.resources[i].split)return;
    RecordNativeFgOutput(list,real,D3D12_RESOURCE_STATES(active.resources[4].state),generated,D3D12_RESOURCE_STATES(active.resources[5].state),
        disable,D3D12_RESOURCE_STATES(active.resources[6].state),receipt,active.serial,active.recording);
}
void TryCaptureNativeOutput(ID3D12GraphicsCommandList* list){
    if(active.list!=list||!active.receipt)return;
    const bool known=active.resources[0].known&&!active.resources[0].split&&
        active.resources[1].known&&!active.resources[1].split&&active.resources[3].known&&!active.resources[3].split;
    if(!active.pendingCapture.Take(Recording(list),known,PendingWriteSerial(list)))return;
    RecordNativeFgOutput(list,active.resources[0].resource,D3D12_RESOURCE_STATES(active.resources[0].state),
        active.resources[1].resource,D3D12_RESOURCE_STATES(active.resources[1].state),
        active.resources[3].resource,D3D12_RESOURCE_STATES(active.resources[3].state),*active.receipt,active.serial,active.recording);
}
NativeFgObservationTicket BeginNativeFgObservation(ID3D12GraphicsCommandList* list,const NVSDK_NGX_Handle* handle,
    const NVSDK_NGX_Parameter* params) noexcept {
    const bool output=NativeFgStereoRequested();
    if((!NativeFgOutputObservationEnabled()&&!output)||active.inCall)return {};
    const auto serial=++calls;
    bool record=Sample(serial);
    try{
        if(params&&(worldSamples.load()<16||NativeFgPixelsWanted())){
            const auto input=Resource(params,NVSDK_NGX_DLSSG_Parameter_Backbuffer);
            const auto image=input.resource?camera::ReadImagePose(input.resource):camera::ImagePoseIdentity{};
            if(image&&image.pose.mainEyeSource&&image.pose.mainEyeSource.sourceId>=8)
                record|=worldSamples.fetch_add(1)<16||(NativeFgPixelsWanted()&&image.pose.mainEyeSource.sourceId>=128);
        }
        const bool trace=record;record|=output;
        if(record){
            const bool writesGuarded=TrackList(list);
            MarkPendingWrite(list); // next evaluation also invalidates another thread's older ticket
            if(!Recording(list)){const uint64_t generation=++recordingSerial;list->SetPrivateData(recordingKey,sizeof(generation),&generation);}
            const auto recording=Recording(list);
            const auto previous=active;
            active={};active.serial=serial;active.list=list;active.handle=handle;active.recording=recording;active.evaluationRecording=recording;active.inCall=true;active.log=trace;
            active.writesGuarded=writesGuarded;
            if(params){
                const char* keys[]{NVSDK_NGX_DLSSG_Parameter_Backbuffer,NVSDK_NGX_DLSSG_Parameter_OutputInterpolated,
                    NVSDK_NGX_DLSSG_Parameter_OutputReal,NVSDK_NGX_DLSSG_Parameter_OutputDisableInterpolation};
                for(unsigned i=0;i<4;++i){
                    const auto pointer=Resource(params,keys[i]).resource;
                    active.resources[i].resource=pointer;
                    if(recording&&previous.list==list&&previous.recording==recording)
                        for(const auto& prior:previous.resources)if(pointer&&prior.resource==pointer){active.resources[i]=prior;break;}
                }
                // Initial flag setup can precede this scope or belong to the
                // other alternating flag. Only the same recording is usable.
                if(auto* record=FlagsFor(list)){
                    auto& flag=active.resources[3];const auto covered=SmallFlag(flag.resource);std::lock_guard lock(record->mutex);
                    const auto known=record->states.Seed(recording,reinterpret_cast<uintptr_t>(flag.resource),{flag.state,flag.known,flag.split},bool(covered));
                    flag.state=known.state;flag.known=known.known;flag.split=known.split;
                    if(active.log)Log("[NATIVE-FG-PERMISSION-SEED] call=%llu recording=%llu flag=%p state=%u known=%d split=%d\n",
                        serial,recording,flag.resource,flag.state,int(flag.known),int(flag.split));
                }
            }
            if(active.log)Record("before",serial,list,handle,params,0);
        }
    }catch(...){active={};record=false;Log("[NATIVE-FG-EVAL] call=%llu observationException=1\n",serial);}
    return {serial,record};
}
void EndNativeFgObservation(NativeFgObservationTicket ticket,ID3D12GraphicsCommandList* list,const NVSDK_NGX_Handle* handle,
    const NVSDK_NGX_Parameter* params,NVSDK_NGX_Result result) noexcept {
    if(!ticket.record)return;
    try{
        if(active.log)Record("after",ticket.serial,list,handle,params,unsigned(result));
        if(active.log)Log("[NATIVE-FG-BARRIER-COVERAGE] call=%llu legacy=%u enhanced=%u\n",ticket.serial,active.legacyBarriers,active.enhancedBarriers);
        const char* roles[]{"input","interpolated","real","disable"};
        if(active.log)for(unsigned i=0;i<4;++i){const auto& r=active.resources[i];
            Log("[NATIVE-FG-STATE] call=%llu role=%s resource=%p transitions=%u state=%u known=%d split=%d subresource=%u flags=%u aliases=%u committed=%d\n",
                ticket.serial,roles[i],r.resource,r.transitions,r.state,int(r.known),int(r.split),r.lastSubresource,r.lastFlags,r.aliasBarriers,int(cvr::gpu::IsKnownCommittedResource(r.resource)));}
        if(NVSDK_NGX_SUCCEED(result)&&active.receipt&&active.recording&&active.recording==Recording(list)){
            active.pendingCapture.Arm(active.recording,PendingWriteSerial(list,true));TryCaptureNativeOutput(list);
            if(!active.writesGuarded)active.pendingCapture.Cancel();
        }
        if(NVSDK_NGX_SUCCEED(result)&&params&&NativeFgPixelsWanted()&&active.recording&&active.recording==Recording(list)&&
           active.resources[0].known&&!active.resources[0].split&&active.resources[1].known&&!active.resources[1].split){
            const auto input=Resource(params,NVSDK_NGX_DLSSG_Parameter_Backbuffer);
            const auto output=Resource(params,NVSDK_NGX_DLSSG_Parameter_OutputInterpolated);
            const auto disable=Resource(params,NVSDK_NGX_DLSSG_Parameter_OutputDisableInterpolation);
            if(input.resource==active.resources[0].resource&&output.resource==active.resources[1].resource){
                unsigned count{},index{};params->Get(NVSDK_NGX_DLSSG_Parameter_MultiFrameCount,&count);params->Get(NVSDK_NGX_DLSSG_Parameter_MultiFrameIndex,&index);
                RecordNativeFgReadback(list,input.resource,D3D12_RESOURCE_STATES(active.resources[0].state),
                    output.resource,D3D12_RESOURCE_STATES(active.resources[1].state),disable.resource,
                    active.resources[3].known&&!active.resources[3].split,D3D12_RESOURCE_STATES(active.resources[3].state),
                    camera::ReadImagePose(input.resource),ticket.serial,active.recording,count,index);
            }
        }
    }catch(...){Log("[NATIVE-FG-EVAL] call=%llu observationException=1\n",ticket.serial);}
    // Continue observing the caller's post-evaluation transitions and the next
    // recording's pre-evaluation transitions. Reset serials invalidate history.
    active.inCall=false;
}
void ObserveNativeFgBarriers(ID3D12GraphicsCommandList* list,UINT count,const D3D12_RESOURCE_BARRIER* barriers) noexcept {
    MarkForeignPendingBarrier(list);
    ObserveFlagBarriers(list,count,barriers);
    if(!active.serial||active.list!=list||!barriers)return;
    active.legacyBarriers+=count;
    const auto recording=Recording(list);
    if(!recording||recording!=active.recording){active.pendingCapture.Cancel();InvalidateStates();active.recording=recording;}
    for(UINT i=0;i<count;++i){const auto& b=barriers[i];
        for(auto& r:active.resources){
            if(!r.resource)continue;
            if(b.Type==D3D12_RESOURCE_BARRIER_TYPE_ALIASING){
                if(cvr::gpu::AliasingInvalidatesResource(r.resource,b.Aliasing)){
                    active.pendingCapture.Cancel();
                    if(active.log)Log("[NATIVE-FG-ALIAS] call=%llu resource=%p before=%p after=%p state=%u known=%d\n",active.serial,r.resource,b.Aliasing.pResourceBefore,b.Aliasing.pResourceAfter,r.state,int(r.known));
                    ++r.aliasBarriers;r.known=false;r.split=false;
                }
            }else if(b.Type==D3D12_RESOURCE_BARRIER_TYPE_TRANSITION&&b.Transition.pResource==r.resource){
                if(active.log)Log("[NATIVE-FG-PHASE-BARRIER] call=%llu inCall=%d role=%u before=%u after=%u flags=%u sub=%u\n",
                    active.serial,int(active.inCall),unsigned(&r-active.resources.data()),unsigned(b.Transition.StateBefore),
                    unsigned(b.Transition.StateAfter),unsigned(b.Flags),b.Transition.Subresource);
                ++r.transitions;r.state=unsigned(b.Transition.StateAfter);
                r.lastSubresource=b.Transition.Subresource;r.lastFlags=unsigned(b.Flags);
                const bool full=b.Transition.Subresource==D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                if(b.Flags&D3D12_RESOURCE_BARRIER_FLAG_BEGIN_ONLY){r.known=false;r.split=true;}
                else if(b.Flags&D3D12_RESOURCE_BARRIER_FLAG_END_ONLY){r.known=full&&r.split;r.split=false;}
                else {r.known=full;r.split=false;}
            }
        }
    }
    // The hook runs AFTER the original ResourceBarrier. All resources in this
    // barrier batch now have their observed StateAfter, including phase 1's
    // output which the caller transitions only after NGX has returned.
    if(!active.inCall)TryCaptureNativeOutput(list);
}
void ResetNativeFgObservation(ID3D12GraphicsCommandList* list) noexcept {
    NativeFgReadbackReset(list);
    NativeFgOutputReset(list);
    if((!NativeFgOutputObservationEnabled()&&!NativeFgStereoRequested())||!list)return;
    bool observed=false;for(auto& slot:observedLists)if(slot.load()==list){observed=true;break;}
    if(!observed)return;
    const uint64_t generation=++recordingSerial;
    const auto stored=SUCCEEDED(list->SetPrivateData(recordingKey,sizeof(generation),&generation));
    if(auto* record=FlagsFor(list)){std::lock_guard lock(record->mutex);record->states.Reset(generation);}
    if(active.list==list){active.pendingCapture.Cancel();InvalidateStates();active.recording=stored?generation:0;}
}
}
