#include "Natives/NativeFunctions.hpp"
#include "MenuPreview/Scope.hpp"
#include "MenuPreview/CameraApplication.hpp"
#include "MenuPreview/UiConsumer.hpp"
#include "MenuPreview/UiCameraCycle.hpp"
#include "MenuPreview/UiCameraCyclePolicy.hpp"
#include "MenuPreview/ProducerRoute.hpp"
#include "MenuPreview/LocalComponentPose.hpp"
#include "MenuPreview/SpatialEditor.hpp"
#include "MenuPreview/OwnedEditorBinding.hpp"
#include "Render/DynamicTextureViewport.hpp"
#include "Camera/CameraState.hpp"
#include "Framegen/Framegen.hpp"
#include <windows.h>
#include "Utils/StereoLog.hpp"
#include <RED4ext/Scripting/Natives/entEntity.hpp>
#include <RED4ext/Scripting/Natives/entIComponent.hpp>
#include <RED4ext/Scripting/Natives/Generated/ent/RenderToTextureCameraComponent.hpp>
#include <RED4ext/Scripting/Natives/Generated/game/ui/PuppetPreviewGameController.hpp>
#include <RED4ext/Scripting/Natives/Generated/game/ui/CharacterCreationPuppetPreviewGameController.hpp>
#include <RED4ext/Scripting/Functions.hpp>
#include <chrono>
#include <vector>

namespace cvr::menu_preview {
namespace {
using Controller=RED4ext::game::ui::PuppetPreviewGameController;
using Entity=RED4ext::ent::Entity;
using Component=RED4ext::ent::IComponent;
using RttCamera=RED4ext::ent::RenderToTextureCameraComponent;
void RequestEditorSourceExtent(RttCamera* camera,const Metadata& scope)noexcept {
    uint32_t canvasWidth{},canvasHeight{};
    if(!camera||scope.screen!=2||!UiStereoRequested()||!ReadUiCanvasExtent(canvasWidth,canvasHeight))return;
    const auto target=PlanEditorSourceExtent(canvasWidth);if(!target)return;
    __try {
        auto* resource=*reinterpret_cast<void**>(reinterpret_cast<uintptr_t>(camera)+0x1E8);if(!resource)return;
        const auto* data=static_cast<const uint8_t*>(resource);
        const auto width=*reinterpret_cast<const uint32_t*>(data+0x4C),height=*reinterpret_cast<const uint32_t*>(data+0x50);
        if(width==target.width&&height==target.height&&camera->resolutionWidth==target.width&&camera->resolutionHeight==target.height)return;
        const bool dispatched=cvr::render::RequestDynamicTextureViewport(resource,target.width,target.height);
        if(dispatched){
            camera->resolutionWidth=target.width;camera->resolutionHeight=target.height;camera->aspectRatio=4.f/3.f;
            *reinterpret_cast<volatile int32_t*>(reinterpret_cast<uintptr_t>(camera)+0xA00)=1;
        }
        static unsigned rows{};if(rows++<8)Log("[SpatialEditor] source-extent epoch=%u setup=%u resource=%p viewport=%ux%u target=%ux%u scaleToViewport=%u dispatched=%u; GPU extent must be observed separately\n",scope.screenEpoch,scope.setupID,resource,width,height,target.width,target.height,unsigned(*(data+0x48)),unsigned(dispatched));
    } __except(EXCEPTION_EXECUTE_HANDLER) {}
}
struct Scope {
    Metadata metadata;
    RED4ext::WeakHandle<Controller> controller;
    RED4ext::WeakHandle<Entity> owner;
    RED4ext::WeakHandle<Component> camera;
    RED4ext::WeakHandle<Component> secondary;
    std::array<std::shared_ptr<const NativePreviewSource>,2> sources;
    uintptr_t primaryResource{},secondaryResource{};
    bool dual{};
};
struct Locked {
    RED4ext::Handle<Controller> controller;
    RED4ext::Handle<Entity> owner;
    RED4ext::Handle<Component> camera;
    RED4ext::Handle<Component> secondary;
};
ScopeStore<Scope> scopes;
EditorBindingClock bindingClock;
struct OwnedSecondary {
    RED4ext::WeakHandle<Entity> owner;RED4ext::WeakHandle<Component> camera;
    uint32_t epoch{};uint64_t entityID{};
};
OwnedSecondary ownedSecondary;
// BEGIN category lifetime policy
struct AppearanceLifetimeIdentity {
    uint32_t epoch{};uint64_t controller{},owner{},entityID{};
    bool Allows(const Metadata& m,bool epochOpen)const noexcept {
        return epochOpen&&m.screen==2&&m.screenEpoch&&m.controllerIdentity&&m.entityIdentity&&m.entityID&&
            (!epoch||(epoch==m.screenEpoch&&controller==m.controllerIdentity&&owner==m.entityIdentity&&entityID==m.entityID));
    }
    void Remember(const Metadata& m)noexcept {
        epoch=m.screenEpoch;controller=m.controllerIdentity;owner=m.entityIdentity;entityID=m.entityID;
    }
    void Clear()noexcept {*this={};}
};
// END category lifetime policy
AppearanceLifetimeIdentity appearanceLifetime;
std::atomic<uint64_t> serial{};
uint64_t Now()noexcept{return PublicationNowMs();}
// CET serializes script callbacks, but their OS thread is not stable across
// Inventory initialization and onUpdate (observed in sim-06). These entrypoints
// are reached only through REDscript; renderer readers use immutable ScopeStore
// snapshots and never enumerate entity components. Serialize the three native
// mutation entrypoints without binding their lifetime to the first worker.
std::mutex scriptCallbacks;
bool IsType(RED4ext::IScriptable* object,const char* name) {
    auto* expected=RED4ext::CRTTISystem::Get()->GetClass(RED4ext::CName(name));
    return object && expected && object->GetType() && object->GetType()->IsA(expected);
}
EditorCameraCandidate CameraCandidate(const RED4ext::Handle<Component>& component){
    EditorCameraCandidate result;if(!component)return result;
    result.component=reinterpret_cast<uintptr_t>(component.GetPtr());result.owner=reinterpret_cast<uintptr_t>(component->owner);
    result.name=uint64_t(component->name);result.enabled=component->isEnabled;
    result.rtt=IsType(component.GetPtr(),"entRenderToTextureCameraComponent");
    if(result.rtt){const auto* camera=static_cast<const RttCamera*>(component.GetPtr());
        result.key=uint64_t(camera->virtualCameraName);result.texture=uint64_t(camera->dynamicTextureRes.path);}
    return result;
}
uintptr_t CameraBacking(Component* camera)noexcept{
    // SDK RaRef is the cooked path at1E0; the already observed resolved backing
    // object at1E8 is opaque. Do not resolve resources or call RTT from here.
    __try{return camera?*reinterpret_cast<const uintptr_t*>(reinterpret_cast<uintptr_t>(camera)+0x1E8):0;}
    __except(EXCEPTION_EXECUTE_HANDLER){return 0;}
}
bool SetOwnedSecondaryEnabled(const RED4ext::Handle<Entity>& owner,const RED4ext::Handle<Component>& camera,bool enabled){
    if(!owner||!ExactOwnedEditorSecondary(CameraCandidate(camera),reinterpret_cast<uintptr_t>(owner.GetPtr())))return false;
    if(camera->isEnabled==enabled)return true;
    if(owner->status!=RED4ext::EntityStatus::Attached)return false;
    auto* toggle=camera->GetType()->GetFunction("Toggle");
    RED4ext::StackArgs_t args;args.emplace_back(nullptr,&enabled);
    return toggle&&RED4ext::ExecuteFunction(camera.GetPtr(),toggle,nullptr,args)&&camera->isEnabled==enabled;
}
bool ReleaseOwnedEditorSecondary(uint32_t epoch=0){
    if(!ownedSecondary.epoch||(epoch&&ownedSecondary.epoch!=epoch))return true;
    auto owner=ownedSecondary.owner.Lock();auto camera=ownedSecondary.camera.Lock();
    if(!owner||!camera||uint64_t(owner->entityID)!=ownedSecondary.entityID||
       !ExactOwnedEditorSecondary(CameraCandidate(camera),reinterpret_cast<uintptr_t>(owner.GetPtr()))){ownedSecondary={};return true;}
    if(!SetOwnedSecondaryEnabled(owner,camera,false))return false;
    ownedSecondary={};return true;
}
bool EnableOwnedEditorSecondary(const RED4ext::Handle<Entity>& owner,const RED4ext::Handle<Component>& camera,uint32_t epoch){
    if(!owner||!epoch||!ExactOwnedEditorSecondary(CameraCandidate(camera),reinterpret_cast<uintptr_t>(owner.GetPtr())))return false;
    auto previous=ownedSecondary.camera.Lock();auto previousOwner=ownedSecondary.owner.Lock();
    if(ownedSecondary.epoch&&(previous.GetPtr()!=camera.GetPtr()||previousOwner.GetPtr()!=owner.GetPtr()||ownedSecondary.epoch!=epoch)&&
       !ReleaseOwnedEditorSecondary())return false;
    ownedSecondary.owner=owner;ownedSecondary.camera=camera;ownedSecondary.epoch=epoch;ownedSecondary.entityID=uint64_t(owner->entityID);
    return SetOwnedSecondaryEnabled(owner,camera,true);
}
bool Publish(const RED4ext::Handle<Controller>& controller,
    const RED4ext::Handle<Entity>& owner,RED4ext::Handle<Component> camera,
    uint32_t epoch,int32_t screen,bool ready,int32_t branch) {
    const auto closeAppearanceLifetime=[&]{
        if(screen==2&&scopes.Clear(epoch)){
            ReleaseOwnedEditorSecondary(epoch);bindingClock.Invalidate();
            if(appearanceLifetime.epoch==epoch)appearanceLifetime.Clear();
            ClearCameraApplication();ClearLocalComponentPose();ClearSpatialEditor();
        }
    };
    const auto reject=[&](const char* reason){
        if(scopes.Invalidate(epoch)){ClearCameraApplication();ClearLocalComponentPose();ClearSpatialEditor();}
        static uint32_t rows=0;
        if(rows++<32)Log("[MenuPreview] scope-rejected epoch=%u screen=%d reason=%s controller=%llx owner=%llx camera=%llx\n",
            epoch,screen,reason,reinterpret_cast<uintptr_t>(controller.GetPtr()),
            reinterpret_cast<uintptr_t>(owner.GetPtr()),reinterpret_cast<uintptr_t>(camera.GetPtr()));
        return false;
    };
    if(!ready){
        static uint32_t rows=0;
        if(rows++<16)Log("[MenuPreview] scope-invalidated epoch=%u screen=%d controller=%llx; resolver-not-ready-or-unresolved-owner\n",
            epoch,screen,reinterpret_cast<uintptr_t>(controller.GetPtr()));
        const bool invalidated=(screen==0 || SupportedPreviewScreen(screen)) && scopes.Invalidate(epoch);
        if(invalidated){ClearCameraApplication();ClearLocalComponentPose();ClearSpatialEditor();}
        return invalidated;
    }
    if(!scopes.IsEpochOpen(epoch))return reject("closed-or-stale-epoch");
    if(!controller){closeAppearanceLifetime();return reject("controller-null");}
    if(!owner){closeAppearanceLifetime();return reject("owner-null");}
    const bool inventory=screen==1&&branch==0&&IsType(controller.GetPtr(),"gameuiInventoryPuppetPreviewGameController");
    const bool appearance=screen==2&&(branch==1||branch==2)&&IsType(controller.GetPtr(),"gameuiCharacterCreationPuppetPreviewGameController");
    if(!inventory&&!appearance){closeAppearanceLifetime();return reject("controller-screen-or-branch");}
    if(appearance&&!IsType(owner.GetPtr(),"gameuiEntityPreviewGameObject")){closeAppearanceLifetime();return reject("appearance-owner-type");}
    if(owner->status!=RED4ext::EntityStatus::Attached){closeAppearanceLifetime();return reject("owner-not-attached");}
    // Producer readiness must not hide a lifetime substitution after a gap.
    // This compares validated identities, but never establishes a new binding.
    if(appearance&&appearanceLifetime.epoch==epoch&&
       (appearanceLifetime.controller!=reinterpret_cast<uintptr_t>(controller.GetPtr())||
        appearanceLifetime.owner!=reinterpret_cast<uintptr_t>(owner.GetPtr())||
        appearanceLifetime.entityID!=uint64_t(owner->entityID))){
        closeAppearanceLifetime();return reject("appearance-owner-changed-before-producer-ready");
    }
    uint32_t enabled=0;RED4ext::Handle<Component> only,secondary;
    std::vector<RED4ext::Handle<Component>> candidates;
    std::vector<EditorCameraCandidate> facts;
    // Membership/type selection is exclusively on the script/game thread.
    for(const auto& candidate:owner->components){
        if(!candidate)continue;
        candidates.push_back(candidate);facts.push_back(CameraCandidate(candidate));
    }
    const auto selection=SelectOwnedEditorCameras(facts,reinterpret_cast<uintptr_t>(owner.GetPtr()));
    if(appearance){
        if(selection.exactSecondary>1)return reject("owned-secondary-ambiguous");
        enabled=selection.enabledPrimary;
        for(const auto& candidate:candidates){
            if(reinterpret_cast<uintptr_t>(candidate.GetPtr())==selection.primary)only=candidate;
            if(reinterpret_cast<uintptr_t>(candidate.GetPtr())==selection.secondary)secondary=candidate;
        }
    }else for(const auto& candidate:candidates){const auto fact=CameraCandidate(candidate);
        if(fact.owner==reinterpret_cast<uintptr_t>(owner.GetPtr())&&fact.rtt&&fact.enabled){++enabled;only=candidate;}
    }
    const auto& bank=controller->cameraController;
    if(!ValidPublication(screen,epoch,ready,bank.cameraSetup.Size(),bank.activeSetup,enabled)){
        if(enabled!=1)return reject(enabled?"enabled-rtt-ambiguous":"enabled-rtt-missing");
        return reject("screen-epoch-or-active-setup-invalid");
    }
    if(appearance&&uint64_t(bank.cameraSetup[bank.activeSetup].slotName)==uint64_t(RED4ext::CName("")))return reject("appearance-slot-empty");
    if(!camera)camera=only;
    if(camera.GetPtr()!=only.GetPtr() || camera->owner!=owner.GetPtr())return reject("camera-membership-mismatch");
    const auto* rtt=static_cast<const RttCamera*>(camera.GetPtr());
    uint64_t sceneName=uint64_t(controller->sceneName),cameraRef=uint64_t(controller->cameraRef);
    if(appearance){const auto* sibling=static_cast<const RED4ext::game::ui::CharacterCreationPuppetPreviewGameController*>(controller.GetPtr());
        sceneName=branch==1?uint64_t(sibling->femaleSceneName):uint64_t(sibling->maleSceneName);
        cameraRef=branch==1?uint64_t(sibling->femaleCamera01Ref):uint64_t(sibling->maleCamera01Ref);
        if(uint64_t(rtt->virtualCameraName)!=uint64_t(RED4ext::CName("character_customization")))return reject("appearance-virtual-camera");}
    if(!sceneName || !cameraRef || !uint64_t(rtt->virtualCameraName))return reject("scene-camera-or-virtual-name-empty");
    auto next=std::make_shared<Scope>();auto& m=next->metadata;
    m.screenEpoch=epoch;m.screen=screen;m.setupID=bank.activeSetup;
    m.controllerIdentity=reinterpret_cast<uintptr_t>(controller.GetPtr());
    m.componentIdentity=reinterpret_cast<uintptr_t>(camera.GetPtr());m.entityIdentity=reinterpret_cast<uintptr_t>(owner.GetPtr());
    m.entityID=uint64_t(owner->entityID);m.sceneNameHash=sceneName;m.cameraRefHash=cameraRef;
    m.slotNameHash=uint64_t(bank.cameraSetup[bank.activeSetup].slotName);m.virtualCameraNameHash=uint64_t(rtt->virtualCameraName);
    m.publicationSerial=serial.fetch_add(1,std::memory_order_relaxed)+1;m.publishedAtMs=Now();
    next->controller=controller;next->owner=owner;next->camera=camera;
    const auto previous=scopes.Read();
    if(appearance&&!appearanceLifetime.Allows(m,scopes.IsEpochOpen(epoch))){
        closeAppearanceLifetime();return reject("appearance-owner-or-lifetime-changed");
    }
    const bool dual=appearance&&UiStereoRequested()&&!cvr::framegen::GetSettings().aerV2;
    if(dual){
        if(!secondary)return reject("owned-secondary-missing");
        if(!EnableOwnedEditorSecondary(owner,secondary,epoch))return reject("owned-secondary-enable-pending");
        next->primaryResource=CameraBacking(camera.GetPtr());next->secondaryResource=CameraBacking(secondary.GetPtr());
        if(!ReadyEditorBackings(next->primaryResource,next->secondaryResource))return reject("owned-secondary-independent-backing-pending");
        next->secondary=secondary;
    }else{
        if(!ReleaseOwnedEditorSecondary())return reject("owned-secondary-disable-pending");
        if(secondary&&!SetOwnedSecondaryEnabled(owner,secondary,false))return reject("owned-secondary-legacy-disable-pending");
        if(EditorNativeApplicationsConfigured())ClearCameraApplication();
    }
    const auto mode=dual?NativePreviewMode::NativeEditorDual:NativePreviewMode::LegacyOneView;
    const auto generation=bindingClock.Resolve({m,mode,dual?reinterpret_cast<uintptr_t>(secondary.GetPtr()):0,
        dual?EditorRightKey:0,next->primaryResource,next->secondaryResource});
    if(!generation)return reject("owned-secondary-generation-exhausted");
    next->dual=dual;
    next->sources[0]=std::make_shared<const NativePreviewSource>(NativePreviewSource{m.componentIdentity,m.virtualCameraNameHash,0,mode,generation});
    if(dual){next->sources[1]=std::make_shared<const NativePreviewSource>(NativePreviewSource{
        reinterpret_cast<uintptr_t>(secondary.GetPtr()),EditorRightKey,1,mode,generation});
        if(!ConfigureEditorNativeApplications(m,generation,*next->sources[0],*next->sources[1]))return reject("owned-secondary-configure-rejected");}
    const bool changed=!previous || previous->metadata.screenEpoch!=m.screenEpoch ||
        previous->metadata.componentIdentity!=m.componentIdentity || previous->metadata.setupID!=m.setupID ||
        previous->metadata.slotNameHash!=m.slotNameHash;
    const auto published=m;
    const bool accepted=scopes.Publish(std::move(next));
    // Only a fully validated successful publication may establish this binding.
    if(accepted&&appearance)appearanceLifetime.Remember(published);
    if(accepted)NoteSpatialEditorScope(published);
    if(accepted&&appearance){RequestEditorSourceExtent(static_cast<RttCamera*>(camera.GetPtr()),published);
        if(dual)RequestEditorSourceExtent(static_cast<RttCamera*>(secondary.GetPtr()),published);}
    if(accepted && changed) {
        Log("[MenuPreview] scope-ready epoch=%u setup=%u controller=%llx owner=%llx camera=%llx scene=%llx cameraRef=%llx virtualCamera=%llx; identity probe only\n",
            published.screenEpoch,published.setupID,
            static_cast<unsigned long long>(published.controllerIdentity),static_cast<unsigned long long>(published.entityIdentity),
            static_cast<unsigned long long>(published.componentIdentity),static_cast<unsigned long long>(published.sceneNameHash),
            static_cast<unsigned long long>(published.cameraRefHash),static_cast<unsigned long long>(published.virtualCameraNameHash));
    }
    return accepted;
}
bool AcquireScopeMember(const std::shared_ptr<const Scope>& scope,unsigned eye,Lease& out){
    if(!scope||eye>1||!scope->sources[eye]||!FreshPublication(scope->metadata.publishedAtMs,Now()))return false;
    auto locked=std::make_shared<Locked>();
    locked->controller=scope->controller.Lock();locked->owner=scope->owner.Lock();locked->camera=scope->camera.Lock();
    if(!locked->controller||!locked->owner||!locked->camera||
       reinterpret_cast<uintptr_t>(locked->camera.GetPtr())!=scope->metadata.componentIdentity)return false;
    if(scope->dual){locked->secondary=scope->secondary.Lock();
        if(!locked->secondary||!scope->sources[1]||
           reinterpret_cast<uintptr_t>(locked->secondary.GetPtr())!=scope->sources[1]->component)return false;}
    else if(eye)return false;
    // Both SDK handles stay alive. All membership/source facts were frozen on
    // the script thread; no mutable component array or script call here.
    out.metadata=scope->metadata;out.source=scope->sources[eye];out.lifetime=std::move(locked);return true;
}
}
bool TryAcquireForComponent(uintptr_t component,Lease& out)noexcept {
    out={};
    try{
        const auto scope=scopes.Read();
        if(!component||!scope)return false;
        for(unsigned eye=0;eye<2;++eye)if(scope->sources[eye]&&scope->sources[eye]->component==component)
            return AcquireScopeMember(scope,eye,out);
        return false;
    }catch(...){return false;}
}
bool TryAcquireForVirtualCamera(uint64_t key,Lease& out)noexcept {
    out={};
    try{
        const auto scope=scopes.Read();
        if(!key||!scope)return false;
        for(unsigned eye=0;eye<2;++eye)if(scope->sources[eye]&&scope->sources[eye]->cameraKey==key)
            return AcquireScopeMember(scope,eye,out);
        return false;
    }catch(...){return false;}
}
uint32_t CurrentPreviewObservationEpoch()noexcept {
    return scopes.ObservationEpoch(Now());
}
bool IsPreviewEpochOpen(std::uint32_t epoch)noexcept {
    return scopes.IsEpochOpen(epoch);
}
bool TryAcquireCurrent(Lease& out)noexcept {
    out={};
    try{
        const auto scope=scopes.Read();
        return AcquireScopeMember(scope,0,out);
    }catch(...){return false;}
}
bool ReadEditorComponentCamera(const Metadata& expected,std::array<uint8_t,32>& output)noexcept{
    try{
        Lease lease;if(expected.screen!=2||!TryAcquireForComponent(expected.componentIdentity,lease)||!SamePreview(lease.metadata,expected))return false;
        const auto* source=reinterpret_cast<const volatile uint64_t*>(expected.componentIdentity+0xE0);
        std::array<uint8_t,32> first{},second{};
        for(unsigned i=0;i<4;++i){const uint64_t part=source[i];std::memcpy(first.data()+i*8,&part,8);}
        std::atomic_signal_fence(std::memory_order_seq_cst);
        for(unsigned i=0;i<4;++i){const uint64_t part=source[i];std::memcpy(second.data()+i*8,&part,8);}
        if(first!=second)return false;output=first;return true;
    }catch(...){return false;}
}
bool PublishCameraApplicationIfCurrent(std::shared_ptr<const CameraApplication> value){
    if(!value)return false;
    std::lock_guard lock(scriptCallbacks);
    const auto scope=scopes.Read();
    if(!scope || !FreshPublication(scope->metadata.publishedAtMs,Now()) ||
       scope->metadata.screenEpoch!=value->metadata.screenEpoch || scope->metadata.setupID!=value->metadata.setupID ||
       scope->metadata.componentIdentity!=value->metadata.componentIdentity ||
       scope->metadata.virtualCameraNameHash!=value->metadata.virtualCameraNameHash)return false;
    if(scope->dual){
        const auto eye=value->source.eye;
        if(eye>1||!scope->sources[eye]||value->source!=*scope->sources[eye]||
           !SameEditorApplicationScope(scope->metadata,value->metadata))return false;
        return PublishEditorNativeApplication(std::move(value));
    }
    if(value->source.mode==NativePreviewMode::NativeEditorDual)return false;
    PublishCameraApplication(std::move(value));return true;
}
bool PublishLocalCameraApplicationIfCurrent(std::shared_ptr<const CameraApplication> value){
    if(!value)return false;std::lock_guard lock(scriptCallbacks);
    const auto scope=scopes.Read();const auto local=ReadLocalComponentPose();
    if(!scope || scope->dual || !local || !FreshPublication(scope->metadata.publishedAtMs,Now()) ||
       !SamePreview(scope->metadata,value->metadata) || !MatchesLocalComponentPose(*local,value->metadata,value->eyeDescriptor) ||
       local->eye!=value->frame.eye || local->revision!=value->localPoseRevision)return false;
    PublishCameraApplication(std::move(value));return true;
}
}

void VRPreviewScopeBegin(RED4ext::IScriptable*,RED4ext::CStackFrame* frame,uint32_t* out,int64_t){
    ++frame->code;
    std::lock_guard callbackLock(cvr::menu_preview::scriptCallbacks);
    if(!cvr::menu_preview::ReleaseOwnedEditorSecondary()){if(out)*out=0;return;}
    cvr::menu_preview::bindingClock.Invalidate();
    const auto epoch=cvr::menu_preview::scopes.Begin();
    cvr::menu_preview::ClearInitialEditorFraming();
    cvr::menu_preview::appearanceLifetime.Clear();
    cvr::menu_preview::ClearLocalComponentPose();
    cvr::menu_preview::ClearSpatialEditor();
    cvr::menu_preview::ClearCameraApplication();
    if(out)*out=epoch;
    if(epoch)Log("[MenuPreview] scope-begin epoch=%u\n",epoch);
}
void VRPreviewScopeClear(RED4ext::IScriptable*,RED4ext::CStackFrame* frame,bool* out,int64_t){
    uint32_t epoch{};RED4ext::GetParameter(frame,&epoch);++frame->code;
    std::lock_guard callbackLock(cvr::menu_preview::scriptCallbacks);
    const bool result=cvr::menu_preview::scopes.Clear(epoch);if(out)*out=result;
    if(result){cvr::menu_preview::ReleaseOwnedEditorSecondary(epoch);cvr::menu_preview::bindingClock.Invalidate();}
    if(result&&cvr::menu_preview::appearanceLifetime.epoch==epoch)cvr::menu_preview::appearanceLifetime.Clear();
    if(result)cvr::menu_preview::ClearCameraApplication();
    if(result)cvr::menu_preview::ClearLocalComponentPose();
    if(result)cvr::menu_preview::ClearSpatialEditor();
    if(result)cvr::menu_preview::ClearInitialEditorFraming();
    if(result)Log("[MenuPreview] scope-clear epoch=%u\n",epoch);
}
void VRPreviewEditorActorAnchor(RED4ext::IScriptable*,RED4ext::CStackFrame* frame,bool* out,int64_t){
    RED4ext::Handle<RED4ext::game::ui::PuppetPreviewGameController> controller;
    RED4ext::Handle<RED4ext::ent::Entity> actor,player,owner;uint32_t epoch{};RED4ext::Vector4 p{},staging{};RED4ext::Quaternion q{};
    RED4ext::GetParameter(frame,&controller);RED4ext::GetParameter(frame,&actor);RED4ext::GetParameter(frame,&player);RED4ext::GetParameter(frame,&owner);
    RED4ext::GetParameter(frame,&epoch);RED4ext::GetParameter(frame,&p);RED4ext::GetParameter(frame,&q);RED4ext::GetParameter(frame,&staging);++frame->code;
    if(out)*out=false;std::lock_guard lock(cvr::menu_preview::scriptCallbacks);
    const auto reject=[&]{if(cvr::menu_preview::scopes.Clear(epoch)){cvr::menu_preview::ReleaseOwnedEditorSecondary(epoch);cvr::menu_preview::bindingClock.Invalidate();cvr::menu_preview::appearanceLifetime.Clear();cvr::menu_preview::ClearInitialEditorFraming();cvr::menu_preview::ClearSpatialEditor();cvr::menu_preview::ClearCameraApplication();}};
    const auto scope=cvr::menu_preview::scopes.Read();
    // Bounded observation only: retain the original admission expression below.
    static unsigned anchorRows{};bool observe=anchorRows<4;if(observe)++anchorRows;
    const auto observeAnchor=[&](int mathResult){if(!observe)return;
        uint32_t bits{};const auto bit=[&](unsigned n,bool value){if(value)bits|=1u<<n;};
        bit(0,bool(scope));bit(1,bool(controller));bit(2,bool(actor));bit(3,bool(player));bit(4,bool(owner));
        bit(5,!player||actor.GetPtr()!=player.GetPtr());bit(6,cvr::menu_preview::IsType(actor.GetPtr(),"NPCPuppet"));
        bit(7,!player||cvr::menu_preview::IsType(player.GetPtr(),"PlayerPuppet"));bit(8,actor&&actor->status==RED4ext::EntityStatus::Attached);
        bit(9,scope&&scope->metadata.screen==2);bit(10,scope&&scope->metadata.screenEpoch==epoch);
        bit(11,scope&&scope->metadata.controllerIdentity==reinterpret_cast<uintptr_t>(controller.GetPtr()));
        bit(12,scope&&scope->metadata.entityIdentity==reinterpret_cast<uintptr_t>(owner.GetPtr()));
        bit(13,scope&&cvr::menu_preview::FreshPublication(scope->metadata.publishedAtMs,cvr::menu_preview::Now()));
        const auto* actorType=actor&&actor->GetType()?actor->GetType()->GetName().ToString():"<null>";
        const auto* playerType=player&&player->GetType()?player->GetType()->GetName().ToString():"<null>";
        auto* rtti=RED4ext::CRTTISystem::Get();
        Log("[EditorAnchor] request epoch=%u scopeEpoch=%u bits=%04x actorStatus=%u actorType=%s playerType=%s npcLookup=%u playerLookup=%u gameNpcLookup=%u gamePlayerLookup=%u actor=%llx actorID=%llx player=%llx owner=%llx controller=%llx p=%g,%g,%g q=%g,%g,%g,%g staging=%g,%g,%g math=%d; first4 calls plus first ready per lifetime max4, math=-1 not reached/0 rejected/1 ready/2 pending\n",
            epoch,scope?scope->metadata.screenEpoch:0,bits,actor?unsigned(actor->status):255,actorType,playerType,
            unsigned(bool(rtti->GetClass(RED4ext::CName("NPCPuppet")))),unsigned(bool(rtti->GetClass(RED4ext::CName("PlayerPuppet")))),
            unsigned(bool(rtti->GetClass(RED4ext::CName("gameNPCPuppet")))),unsigned(bool(rtti->GetClass(RED4ext::CName("gamePlayerPuppet")))),
            reinterpret_cast<uintptr_t>(actor.GetPtr()),actor?uint64_t(actor->entityID):uint64_t(0),reinterpret_cast<uintptr_t>(player.GetPtr()),reinterpret_cast<uintptr_t>(owner.GetPtr()),reinterpret_cast<uintptr_t>(controller.GetPtr()),
            double(p.X),double(p.Y),double(p.Z),double(q.i),double(q.j),double(q.k),double(q.r),double(staging.X),double(staging.Y),double(staging.Z),mathResult);
    };
    if(!scope||!controller||!actor||!owner||(player&&actor.GetPtr()==player.GetPtr())||!cvr::menu_preview::IsType(actor.GetPtr(),"NPCPuppet")||(player&&!cvr::menu_preview::IsType(player.GetPtr(),"PlayerPuppet"))||actor->status!=RED4ext::EntityStatus::Attached||
       scope->metadata.screen!=2||scope->metadata.screenEpoch!=epoch||scope->metadata.controllerIdentity!=reinterpret_cast<uintptr_t>(controller.GetPtr())||
       scope->metadata.entityIdentity!=reinterpret_cast<uintptr_t>(owner.GetPtr())||!cvr::menu_preview::FreshPublication(scope->metadata.publishedAtMs,cvr::menu_preview::Now())){observeAnchor(-1);reject();return;}
    const auto result=cvr::menu_preview::PublishInitialEditorFraming(scope->metadata,reinterpret_cast<uintptr_t>(actor.GetPtr()),uint64_t(actor->entityID),reinterpret_cast<uintptr_t>(player.GetPtr()),
        {p.X,p.Y,p.Z},{q.i,q.j,q.k,q.r},{staging.X,staging.Y,staging.Z});
    static unsigned readyRows{};static uint32_t readyEpoch{};static uint64_t readyController{};
    if(result==cvr::menu_preview::InitialFramingResult::Ready&&readyRows<4&&
       (readyEpoch!=epoch||readyController!=reinterpret_cast<uintptr_t>(controller.GetPtr()))){
        ++readyRows;readyEpoch=epoch;readyController=reinterpret_cast<uintptr_t>(controller.GetPtr());observe=true;}
    observeAnchor(int(result));
    if(result==cvr::menu_preview::InitialFramingResult::Rejected)reject();
    if(out)*out=result==cvr::menu_preview::InitialFramingResult::Ready;
}
void VRPreviewScopeUpdate(RED4ext::IScriptable*,RED4ext::CStackFrame* frame,bool* out,int64_t){
    RED4ext::Handle<RED4ext::game::ui::PuppetPreviewGameController> controller;
    RED4ext::Handle<RED4ext::ent::Entity> owner;RED4ext::Handle<RED4ext::ent::IComponent> camera;
    uint32_t epoch{};int32_t screen{},branch{};bool ready{};
    RED4ext::GetParameter(frame,&controller);RED4ext::GetParameter(frame,&owner);RED4ext::GetParameter(frame,&camera);
    RED4ext::GetParameter(frame,&epoch);RED4ext::GetParameter(frame,&screen);RED4ext::GetParameter(frame,&ready);RED4ext::GetParameter(frame,&branch);++frame->code;
    std::lock_guard callbackLock(cvr::menu_preview::scriptCallbacks);
    const bool result=cvr::menu_preview::Publish(controller,owner,std::move(camera),epoch,screen,ready,branch);if(out)*out=result;
}

void VRPreviewLocalHalfIpd(RED4ext::IScriptable*,RED4ext::CStackFrame* frame,float* out,int64_t){
    ++frame->code;
    static const bool enabled=[] {
        char local[24]{},descriptor[24]{};
        const auto n=GetEnvironmentVariableA("CPVR_MENU_LOCAL_CAMERA_TEST",local,sizeof(local));
        const auto d=GetEnvironmentVariableA("CPVR_MENU_STEREO_CAMERA_TEST",descriptor,sizeof(descriptor));
        const bool conflict=d && (d>=sizeof(descriptor) || cvr::menu_preview::ParseCameraTestMode(descriptor)!=cvr::menu_preview::CameraTestMode::Off);
        if(conflict && n)Log("[MenuPreview] local-camera diagnostic rejected: descriptor test also enabled\n");
        return (cvr::menu_preview::UiStereoRequested()||(n==4 && std::strcmp(local,"left")==0)||(n==5 && std::strcmp(local,"right")==0)) && !conflict;
    }();
    const float half=enabled?GetDesiredHalfIpd():0;
    static uint32_t modeRows{};
    if(modeRows++<4)Log("[MenuPreview] local-mode uiRequested=%u enabled=%u desiredHalfIpd=%g\n",unsigned(cvr::menu_preview::UiStereoRequested()),unsigned(enabled),double(half));
    char local[24]{};GetEnvironmentVariableA("CPVR_MENU_LOCAL_CAMERA_TEST",local,sizeof(local));
    if(out)*out=std::isfinite(half)&&half>0&&half<=.15f?(!cvr::menu_preview::UiStereoRequested()&&std::strcmp(local,"right")==0?-half:half):0;
}
void VRPreviewStereoUiEnabled(RED4ext::IScriptable*,RED4ext::CStackFrame* frame,bool* out,int64_t){
    ++frame->code;if(out)*out=cvr::menu_preview::UiStereoRequested();
}
void VRPreviewStereoUiPairSubmitted(RED4ext::IScriptable*,RED4ext::CStackFrame* frame,bool* out,int64_t){
    uint32_t epoch{};RED4ext::GetParameter(frame,&epoch);++frame->code;
    if(out)*out=cvr::menu_preview::UiStereoRequested()&&cvr::menu_preview::UiPairSubmitted(epoch);
}
void VRPreviewStereoUiCanAdvance(RED4ext::IScriptable*,RED4ext::CStackFrame* frame,bool* out,int64_t){
    uint32_t epoch{};int32_t eye{};RED4ext::GetParameter(frame,&epoch);RED4ext::GetParameter(frame,&eye);++frame->code;
    if(out)*out=false;if(!cvr::menu_preview::UiStereoRequested())return;
    const auto pose=cvr::menu_preview::ReadLocalComponentPose();const auto ack=cvr::menu_preview::ReadAcknowledgedUiCapture();
    if(!pose||!pose->verified||!ack||!cvr::menu_preview::SamePreview(pose->metadata,ack->scope))return;
    if(out)*out=pose->metadata.screenEpoch==epoch&&pose->eye==uint32_t(eye)&&
        cvr::menu_preview::CapturedCameraRevision(epoch,uint32_t(eye),pose->revision,ack->scope.screenEpoch,ack->eye,ack->cameraRevision);
}
void VRPreviewLocalClock(RED4ext::IScriptable*,RED4ext::CStackFrame* frame,float* out,int64_t){
    ++frame->code;
    static const auto origin=std::chrono::steady_clock::now();
    if(out)*out=std::chrono::duration<float>(std::chrono::steady_clock::now()-origin).count();
}
void VRPreviewLocalCamera(RED4ext::IScriptable*,RED4ext::CStackFrame* frame,RED4ext::Handle<RED4ext::ent::IPlacedComponent>* out,int64_t){
    RED4ext::Handle<RED4ext::game::ui::PuppetPreviewGameController> controller;uint32_t epoch{};int32_t setup{};
    RED4ext::GetParameter(frame,&controller);RED4ext::GetParameter(frame,&epoch);RED4ext::GetParameter(frame,&setup);++frame->code;
    if(out)*out={};
    std::lock_guard lock(cvr::menu_preview::scriptCallbacks);
    const auto scope=cvr::menu_preview::scopes.Read();
    if(!scope || !controller || scope->metadata.controllerIdentity!=reinterpret_cast<uintptr_t>(controller.GetPtr()) ||
       scope->metadata.screenEpoch!=epoch || int32_t(scope->metadata.setupID)!=setup ||
       int32_t(controller->cameraController.activeSetup)!=setup ||
       !cvr::menu_preview::FreshPublication(scope->metadata.publishedAtMs,cvr::menu_preview::Now()))return;
    if(scope->metadata.screen==2){const auto& bank=controller->cameraController;
        if(setup<0 || uint32_t(setup)>=bank.cameraSetup.Size() || !scope->metadata.slotNameHash ||
           scope->metadata.slotNameHash!=uint64_t(bank.cameraSetup[uint32_t(setup)].slotName))return;}
    auto camera=scope->camera.Lock();auto owner=scope->owner.Lock();
    if(!camera || !camera->isEnabled || !owner || owner->status!=RED4ext::EntityStatus::Attached || camera->owner!=owner.GetPtr() ||
       reinterpret_cast<uintptr_t>(camera.GetPtr())!=scope->metadata.componentIdentity)return;
    if(out)*out=RED4ext::Handle<RED4ext::ent::IPlacedComponent>(static_cast<RED4ext::ent::IPlacedComponent*>(camera.GetPtr()));
    static uint32_t rows=0;static uint64_t lastTick{};static int32_t lastWorld[3]{};
    int32_t world[3]{};std::memcpy(world,reinterpret_cast<const uint8_t*>(camera.GetPtr())+0xE0,12);
    const auto now=cvr::menu_preview::Now();
    if(rows<24 && (!rows || std::memcmp(lastWorld,world,12) || now-lastTick>=500)){
        ++rows;lastTick=now;std::memcpy(lastWorld,world,12);
        Log("[MenuPreview] local-camera-observed epoch=%u setup=%d component=%llx worldFP=%d,%d,%d worldFloat=%g,%g,%g; typed getter, no pose writes\n",
            epoch,setup,scope->metadata.componentIdentity,world[0],world[1],world[2],double(world[0])/131072.,double(world[1])/131072.,double(world[2])/131072.);
    }
}

namespace {
RED4ext::Handle<RED4ext::ent::IComponent> LocalCameraForReceipt(
    const RED4ext::Handle<RED4ext::game::ui::PuppetPreviewGameController>& controller,uint32_t epoch,int32_t setup,
    cvr::menu_preview::Metadata& metadata){
    // Caller holds scriptCallbacks, shared with Begin/Clear/Update.
    char local[24]{},descriptor[24]{};
    GetEnvironmentVariableA("CPVR_MENU_LOCAL_CAMERA_TEST",local,sizeof(local));
    const auto d=GetEnvironmentVariableA("CPVR_MENU_STEREO_CAMERA_TEST",descriptor,sizeof(descriptor));
    if((!cvr::menu_preview::UiStereoRequested() && std::strcmp(local,"left") && std::strcmp(local,"right")) || d>=sizeof(descriptor) ||
       cvr::menu_preview::ParseCameraTestMode(descriptor)!=cvr::menu_preview::CameraTestMode::Off)return {};
    const auto scope=cvr::menu_preview::scopes.Read();
    if(!scope || !controller || !((scope->metadata.screen==1&&setup==0)||(scope->metadata.screen==2&&setup>=0&&scope->metadata.slotNameHash)) || scope->metadata.controllerIdentity!=reinterpret_cast<uintptr_t>(controller.GetPtr()) ||
       scope->metadata.screenEpoch!=epoch || scope->metadata.setupID!=uint32_t(setup) || controller->cameraController.activeSetup!=uint32_t(setup) ||
       !cvr::menu_preview::FreshPublication(scope->metadata.publishedAtMs,cvr::menu_preview::Now()))return {};
    const auto& bank=controller->cameraController;
    if(uint32_t(setup)>=bank.cameraSetup.Size() ||
       (scope->metadata.screen==2 && scope->metadata.slotNameHash!=uint64_t(bank.cameraSetup[uint32_t(setup)].slotName)))return {};
    auto camera=scope->camera.Lock();auto owner=scope->owner.Lock();
    if(!camera || !camera->isEnabled || !owner || owner->status!=RED4ext::EntityStatus::Attached || camera->owner!=owner.GetPtr() ||
       reinterpret_cast<uintptr_t>(camera.GetPtr())!=scope->metadata.componentIdentity)return {};
    metadata=scope->metadata;return camera;
}
}
void VRPreviewLocalPrepare(RED4ext::IScriptable*,RED4ext::CStackFrame* frame,bool* out,int64_t){
    RED4ext::Handle<RED4ext::game::ui::PuppetPreviewGameController> controller;uint32_t epoch{};int32_t setup{};
    RED4ext::GetParameter(frame,&controller);RED4ext::GetParameter(frame,&epoch);RED4ext::GetParameter(frame,&setup);++frame->code;
    if(out)*out=false;std::lock_guard lock(cvr::menu_preview::scriptCallbacks);
    auto prepared=std::make_shared<cvr::menu_preview::LocalComponentPose>();
    auto camera=LocalCameraForReceipt(controller,epoch,setup,prepared->metadata);if(!camera)return;
    // The editor's own controller resets local position while settling and
    // changing body slots. Its native render-camera path owns VR eye poses.
    // Inventory retains the existing component-pose diagnostic and cycle.
    if(prepared->metadata.screen==2&&cvr::menu_preview::UiStereoRequested())return;
    std::memcpy(prepared->baseline.data(),reinterpret_cast<const uint8_t*>(camera.GetPtr())+0xE0,32);
    cvr::menu_preview::localComponentPose.store(std::move(prepared),std::memory_order_release);
    if(out)*out=true;
}
void VRPreviewLocalPublish(RED4ext::IScriptable*,RED4ext::CStackFrame* frame,bool* out,int64_t){
    RED4ext::Handle<RED4ext::game::ui::PuppetPreviewGameController> controller;uint32_t epoch{};int32_t setup{},eye{};
    RED4ext::GetParameter(frame,&controller);RED4ext::GetParameter(frame,&epoch);RED4ext::GetParameter(frame,&setup);RED4ext::GetParameter(frame,&eye);++frame->code;
    if(out)*out=false;std::lock_guard lock(cvr::menu_preview::scriptCallbacks);
    const auto baseline=cvr::menu_preview::ReadLocalComponentPose();if(!baseline || eye<0 || eye>1)return;
    cvr::menu_preview::Metadata metadata;auto camera=LocalCameraForReceipt(controller,epoch,setup,metadata);
    if(!camera || !cvr::menu_preview::SamePreview(baseline->metadata,metadata))return;
    auto applied=std::make_shared<cvr::menu_preview::LocalComponentPose>(*baseline);applied->metadata=metadata;applied->eye=uint32_t(eye);
    std::memcpy(applied->applied.data(),reinterpret_cast<const uint8_t*>(camera.GetPtr())+0xE0,32);
    if(!cvr::menu_preview::VerifyLocalEyeDelta(*applied,GetDesiredHalfIpd())){
        static uint32_t rows{};
        if(rows++<8)Log("[MenuPreview] local-component-pose-rejected epoch=%u eye=%d reason=native-world-delta-or-quaternion\n",epoch,eye);
        return;
    }
    static uint64_t revision{};
    applied->verified=true;applied->publishedAtMs=cvr::menu_preview::Now();applied->revision=++revision;
    Log("[MenuPreview] local-component-pose-published epoch=%u setup=%d component=%llx eye=%d; measured native component pose, output consumption pending\n",epoch,setup,metadata.componentIdentity,eye);
    cvr::menu_preview::localComponentPose.store(std::move(applied),std::memory_order_release);if(out)*out=true;
}
void VRPreviewLocalReset(RED4ext::IScriptable*,RED4ext::CStackFrame* frame,bool* out,int64_t){
    uint32_t epoch{};RED4ext::GetParameter(frame,&epoch);++frame->code;
    std::lock_guard lock(cvr::menu_preview::scriptCallbacks);
    const auto local=cvr::menu_preview::ReadLocalComponentPose();const bool match=local && local->metadata.screenEpoch==epoch;
    if(match){cvr::menu_preview::ClearLocalComponentPose();cvr::menu_preview::ClearCameraApplication();cvr::menu_preview::ClearSpatialEditor();}
    if(out)*out=match;
}
