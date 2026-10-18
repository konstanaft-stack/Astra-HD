#include "Framegen/AerModeOwner.hpp"
#include "Framegen/SourceFrameSystem.hpp"
#include "Framegen/VrcamOffReadback.hpp"
#include "Camera/PoseIdentity.hpp"
#include <RED4ext/RED4ext.hpp>
#include <RED4ext/Scripting/Functions.hpp>
#include <RED4ext/Scripting/Natives/entEntity.hpp>
#include <RED4ext/Scripting/Natives/ScriptGameInstance.hpp>
#include <mutex>
#include <cstring>
#include <vector>

extern void Log(const char*,...);

namespace cvr::framegen {
namespace {
using EntityHandle=RED4ext::Handle<RED4ext::IScriptable>;
VrcamOffReadback DisableAndReadVrcams(const EntityHandle& handle){
    if(!handle)return {};
    auto* rtti=RED4ext::CRTTISystem::Get();auto* entityType=rtti->GetClass("entEntity");
    auto* cameraType=rtti->GetClass("entRenderToTextureCameraComponent");
    auto* type=handle->GetType();
    if(!entityType||!cameraType||!type||!type->IsA(entityType))return {};
    auto* entity=reinterpret_cast<RED4ext::ent::Entity*>(handle.GetPtr());
    if(entity->status!=RED4ext::EntityStatus::Attached)return {};
    // Snapshot strong component handles before Toggle, matching the copied
    // GetComponents array used by the author's Lua selector.
    std::vector<RED4ext::Handle<RED4ext::ent::IComponent>> cameras;
    for(const auto& component:entity->components)if(component){
        const char* name=component->name.ToString();
        if(name&&std::strncmp(name,"vrcam_",6)==0)cameras.push_back(component);
    }
    VrcamOffReadback result{uint32_t(cameras.size()),true,true};
    for(const auto& component:cameras){
        auto* componentType=component->GetType();
        if(component->owner!=entity||!componentType||!componentType->IsA(cameraType)){
            result.readable=result.allDisabled=false;continue;
        }
        if(component->isEnabled){
            auto* toggle=componentType->GetFunction("Toggle");
            bool off=false;RED4ext::StackArgs_t args;args.emplace_back(nullptr,&off);
            if(!toggle||!RED4ext::ExecuteFunction(component.GetPtr(),toggle,nullptr,args))result.allDisabled=false;
        }
        // A successful call is not proof: delayed disabling is retried next
        // FrameBegin and acknowledged only after the actual field reads false.
        if(component->isEnabled)result.allDisabled=false;
    }
    return result;
}
struct Owner {
    std::mutex mutex;
    std::mutex nativeReadMutex;
    VrcamOffEntities<EntityHandle> nativeEntities;
    uint32_t nativeLogToken{},nativeLogCount{};bool nativeLogAccepted{};
    AerScenePolicy policy;
    bool cameraActive{},stopped{};
    int64_t lastPublishedSourceId=-1,lastRequestedPoseTimeNs{},lastRuntimeTimeNs{};
    uint64_t lastSceneOrdinal{};
    void Scene(const SourceSceneFrame& frame) {
        uint32_t token{};VrcamOffReadback readback;
        if(!frame.valid||frame.paused){
            // Release engine entity handles on its own world/pause callback,
            // not later from XR teardown after the game world has gone away.
            std::lock_guard readLock(nativeReadMutex);nativeEntities.Reset(frame.worldEpoch);
        }
        // Reuse the existing game-thread FrameBegin, player lookup and Toggle.
        // No Lua runtime is needed for this proof when CET is intentionally off.
        if(frame.valid&&!frame.paused&&!GetModuleHandleW(L"cyber_engine_tweaks.asi")){
            {std::lock_guard lock(mutex);
             if(!stopped&&policy.Epochs().gameWorld==frame.worldEpoch)token=policy.OffRequestToken();}
            if(token){
                std::unique_lock readLock(nativeReadMutex,std::try_to_lock);
                if(readLock.owns_lock()){
                    RED4ext::ScriptGameInstance game;EntityHandle player;
                    if(RED4ext::ExecuteGlobalFunction("GetPlayer;GameInstance",&player,game))
                        readback=nativeEntities.Observe(frame.worldEpoch,player,DisableAndReadVrcams);
                }
            }
        }
        std::lock_guard lock(mutex);
        if(stopped)return;
        if(token){
            const auto currentToken=policy.OffRequestToken();
            const bool accepted=policy.ReportVrcamOff(token,readback.inspected,bool(readback));
            if(!accepted&&token==currentToken){camera::InvalidateMainEyeSource();cameraActive=false;}
            if(token!=nativeLogToken||accepted!=nativeLogAccepted||readback.inspected!=nativeLogCount){
                nativeLogToken=token;nativeLogAccepted=accepted;nativeLogCount=readback.inspected;
                Log("[AER-NATIVE-OFF] world=%llu token=%u inspected=%u readable=%d allDisabled=%d accepted=%d\n",
                    static_cast<unsigned long long>(frame.worldEpoch),token,readback.inspected,
                    int(readback.readable),int(readback.allDisabled),int(accepted));
            }
        }
        const auto decision=policy.OnSceneFrame(frame);
        if(decision.ignored)return;
        if(decision.invalidate) {camera::RequestMainEyeSource(false);cameraActive=false;}
        if(!decision.active) {camera::RequestMainEyeSource(false);cameraActive=false;return;}
        const auto revision=camera::RequestMainEyeSource(true);
        cameraActive=camera::PublishMainEyeSceneFrame(revision,decision.frame,decision.sourceId,
            decision.eye,{revision,decision.epochs.gameWorld,true},decision.clock);
        if(cameraActive) {
            lastPublishedSourceId=decision.sourceId;lastRuntimeTimeNs=decision.clock.runtimePredictedTimeNs;
            lastSceneOrdinal=decision.frame.ordinal;
            if(const auto receipt=camera::AcquireMainEyeSourceForWrite(decision.epochs.hmdWorld))lastRequestedPoseTimeNs=receipt->requestedPoseTimeNs;
        }
        if(!cameraActive)camera::InvalidateMainEyeSource();
    }
};
std::mutex lifetimeMutex;
std::shared_ptr<Owner> owner;
std::shared_ptr<Owner> Current() {std::lock_guard lock(lifetimeMutex);return owner;}
void GetOffRequest(RED4ext::IScriptable*,RED4ext::CStackFrame* frame,int32_t* result,int64_t) {
    frame->code++;
    int32_t token=0;
    if(auto value=Current()){std::lock_guard lock(value->mutex);token=int32_t(value->policy.OffRequestToken());}
    if(result)*result=token;
}
void GetWorldGeneration(RED4ext::IScriptable*,RED4ext::CStackFrame* frame,int32_t* result,int64_t) {
    frame->code++;int32_t epoch=0;
    if(auto value=Current()) {
        std::lock_guard lock(value->mutex);const auto current=value->policy.Epochs().gameWorld;
        if(current<=INT32_MAX)epoch=int32_t(current);
    }
    if(result)*result=epoch;
}
void ConfirmOff(RED4ext::IScriptable*,RED4ext::CStackFrame* frame,bool* result,int64_t) {
    int32_t token{},count{};bool verified{};
    RED4ext::GetParameter(frame,&token);RED4ext::GetParameter(frame,&count);RED4ext::GetParameter(frame,&verified);
    frame->code++;bool accepted=false;
    if(auto value=Current()) {
        std::lock_guard lock(value->mutex);
        const auto currentToken=value->policy.OffRequestToken();
        accepted=value->policy.ReportVrcamOff(token>0?uint32_t(token):0,count>0?uint32_t(count):0,verified);
        // A stale report must not revoke a newer world's acknowledgement.
        if(!accepted && token>0 && uint32_t(token)==currentToken) {
            camera::InvalidateMainEyeSource();value->cameraActive=false;
        }
    }
    if(result)*result=accepted;
}
}
void InstallAerModeOwner() {
    std::lock_guard lock(lifetimeMutex);
    if(owner)return;
    owner=std::make_shared<Owner>();
    SetSourceSceneSink([](const SourceSceneFrame& scene,void* context){static_cast<Owner*>(context)->Scene(scene);},owner);
}
void UpdateAerModeRuntime(const AerRuntimeSnapshot& runtime) {
    if(auto value=Current()) {
        std::lock_guard lock(value->mutex);
        if(value->stopped)return;
        const auto before=value->policy.Generation();value->policy.UpdateRuntime(runtime);
        if(before!=value->policy.Generation()) {camera::RequestMainEyeSource(false);value->cameraActive=false;}
    }
}
AerModeSnapshot ReadAerModeSnapshot() {
    if(auto value=Current()) {
        std::lock_guard lock(value->mutex);
        return {value->policy.Requested(),value->policy.Ready(),value->cameraActive,
                value->policy.Generation(),value->policy.Epochs(),value->policy.Fit(),
                value->lastPublishedSourceId,value->lastRequestedPoseTimeNs,value->lastRuntimeTimeNs,value->lastSceneOrdinal};
    }
    return {};
}
void ObserveAerAccepted(uint64_t generation,MainEyeSourceEpochs epochs,const SourceDisplaySubmission& value) {
    if(auto state=Current()) {std::lock_guard lock(state->mutex);state->policy.ObserveAccepted(generation,epochs,value);}
}
void ObserveAerNativeAccepted(uint64_t generation,MainEyeSourceEpochs epochs,const SourceDisplayContinuousSubmission& value) {
    if(auto state=Current()) {std::lock_guard lock(state->mutex);state->policy.ObserveAcceptedContinuous(generation,epochs,value);}
}
void StopAerModeOwner() {
    std::lock_guard lifetimeLock(lifetimeMutex);
    auto previous=std::move(owner);
    if(previous) {
        std::lock_guard lock(previous->mutex);previous->stopped=true;previous->cameraActive=false;
        previous->policy.UpdateRuntime({});camera::RequestMainEyeSource(false);
    }
    SetSourceSceneSink(nullptr,{});
}
void RegisterAerModeNatives() {
    auto* rtti=RED4ext::CRTTISystem::Get();
    RED4ext::CBaseFunction::Flags flags={.isNative=true,.isStatic=true};
    auto* request=RED4ext::CGlobalFunction::Create("GetVRAerVrcamOffRequest","GetVRAerVrcamOffRequest",&GetOffRequest);
    request->flags=flags;request->SetReturnType("Int32");rtti->RegisterFunction(request);
    auto* world=RED4ext::CGlobalFunction::Create("GetVRAerWorldGeneration","GetVRAerWorldGeneration",&GetWorldGeneration);
    world->flags=flags;world->SetReturnType("Int32");rtti->RegisterFunction(world);
    auto* confirm=RED4ext::CGlobalFunction::Create("ConfirmVRAerVrcamOff","ConfirmVRAerVrcamOff",&ConfirmOff);
    confirm->flags=flags;confirm->AddParam("Int32","token");confirm->AddParam("Int32","inspected");
    confirm->AddParam("Bool","allDisabled");confirm->SetReturnType("Bool");rtti->RegisterFunction(confirm);
}
}
