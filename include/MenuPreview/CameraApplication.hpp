#pragma once
#include "MenuPreview/Producer.hpp"
#include "Utils/XrMath.hpp"
#include "MenuPreview/EditorFreshnessProbe.hpp"
#include "MenuPreview/NativePreviewSource.hpp"
#include <bit>

namespace cvr::menu_preview {
struct EditorOrbitView {
    XrVector3f pivot{};XrQuaternionf inverseRotation{0,0,0,1};
    uint64_t controlRevision{},origin{};float angle{};bool valid{};
};
struct PreviewProjection {XrPosef pose{{0,0,0,1},{0,0,0}};XrFovf fov{};uint64_t origin{};bool valid{};EditorOrbitView orbit{};};
struct PreviewFrameKey {
    uint32_t epoch{},setup{};uint64_t component{},virtualCamera{};
    bool operator==(const PreviewFrameKey&)const=default;
};
struct PreviewFrameEye {PreviewFrameKey key{};uint32_t nativeFrame{},eye{};uint64_t sourceSequence{};bool changed{};explicit operator bool()const{return sourceSequence!=0;}};
class PreviewFrameEyeOwner {
public:
    PreviewFrameEye Resolve(const Metadata& m,uint64_t actualKey,bool clockKnown,uint32_t frame){
        if(!clockKnown || !SupportedPreviewScreen(m.screen) || !m.screenEpoch || !m.componentIdentity ||
           !actualKey || actualKey!=m.virtualCameraNameHash)return {};
        const PreviewFrameKey key{m.screenEpoch,m.setupID,m.componentIdentity,actualKey};
        if(current_ && current_.key==key){
            if(frame==current_.nativeFrame){auto repeat=current_;repeat.changed=false;return repeat;}
            // Native 32-bit clock wrap advances normally; older frames cannot
            // assign a second eye after a newer build has already been seen.
            if(int32_t(frame-current_.nativeFrame)<=0)return {};
            current_={key,frame,current_.eye^1u,++sequence_,true};
        }else current_={key,frame,0,++sequence_,true};
        return current_;
    }
    void Reset(){current_={};}
private:PreviewFrameEye current_{};uint64_t sequence_{};
};
// Camera application only: no GPU draw/completion or UI consumption claim.
struct CameraApplication {
    Metadata metadata{};PreviewFrameEye frame{};
    NativePreviewSource source{};
    uintptr_t nativeContext{},nativeDestination{},sourceDescriptor{};
    Descriptor baseDescriptor{},eyeDescriptor{};
    uint64_t localPoseRevision{};
    uint64_t editorControlRevision{}; // same virtual camera controls for both eyes
    PreviewProjection projection{};
};
// Exact immutable value comparison. Do not compare padded structs or collapse
// distinct head/control/native destination facts under a coarse source ID.
inline bool SameCameraApplication(const CameraApplication& a,const CameraApplication& b)noexcept{
    const auto f=[](float x,float y){return std::bit_cast<uint32_t>(x)==std::bit_cast<uint32_t>(y);};
    const auto v=[&](const XrVector3f& x,const XrVector3f& y){return f(x.x,y.x)&&f(x.y,y.y)&&f(x.z,y.z);};
    const auto q=[&](const XrQuaternionf& x,const XrQuaternionf& y){return f(x.x,y.x)&&f(x.y,y.y)&&f(x.z,y.z)&&f(x.w,y.w);};
    const auto& m=a.metadata;const auto& n=b.metadata;
    const auto& x=a.projection;const auto& y=b.projection;
    // frame.changed marks first/repeat callback bookkeeping, not rendered identity.
    // The flag and upstream eye-ownership guards remain unchanged.
    return m.screenEpoch==n.screenEpoch&&m.setupID==n.setupID&&m.screen==n.screen&&
        m.controllerIdentity==n.controllerIdentity&&m.componentIdentity==n.componentIdentity&&m.entityIdentity==n.entityIdentity&&m.entityID==n.entityID&&
        m.sceneNameHash==n.sceneNameHash&&m.cameraRefHash==n.cameraRefHash&&m.slotNameHash==n.slotNameHash&&m.virtualCameraNameHash==n.virtualCameraNameHash&&
        m.publicationSerial==n.publicationSerial&&m.publishedAtMs==n.publishedAtMs&&
        a.frame.key==b.frame.key&&a.frame.nativeFrame==b.frame.nativeFrame&&a.frame.eye==b.frame.eye&&a.frame.sourceSequence==b.frame.sourceSequence&&
        a.source==b.source&&a.nativeContext==b.nativeContext&&a.nativeDestination==b.nativeDestination&&a.sourceDescriptor==b.sourceDescriptor&&
        a.baseDescriptor==b.baseDescriptor&&a.eyeDescriptor==b.eyeDescriptor&&a.localPoseRevision==b.localPoseRevision&&a.editorControlRevision==b.editorControlRevision&&
        v(x.pose.position,y.pose.position)&&q(x.pose.orientation,y.pose.orientation)&&
        f(x.fov.angleLeft,y.fov.angleLeft)&&f(x.fov.angleRight,y.fov.angleRight)&&f(x.fov.angleUp,y.fov.angleUp)&&f(x.fov.angleDown,y.fov.angleDown)&&
        x.origin==y.origin&&x.valid==y.valid&&v(x.orbit.pivot,y.orbit.pivot)&&q(x.orbit.inverseRotation,y.orbit.inverseRotation)&&
        x.orbit.controlRevision==y.orbit.controlRevision&&x.orbit.origin==y.orbit.origin&&f(x.orbit.angle,y.orbit.angle)&&x.orbit.valid==y.orbit.valid;
}
inline std::atomic<std::shared_ptr<const CameraApplication>> cameraApplication;
inline std::mutex cameraApplicationMutex;
struct EditorNativeApplicationState {
    Metadata common{};uint64_t generation{};
    std::array<NativePreviewSource,2> bindings{};
    std::array<std::shared_ptr<const CameraApplication>,2> applications{};
    bool configured{};
};
inline EditorNativeApplicationState editorNativeApplications;
inline bool SameEditorApplicationScope(const Metadata& a,const Metadata& b)noexcept{
    return a.screen==b.screen&&a.screenEpoch==b.screenEpoch&&a.setupID==b.setupID&&
        a.controllerIdentity==b.controllerIdentity&&a.componentIdentity==b.componentIdentity&&
        a.entityIdentity==b.entityIdentity&&a.entityID==b.entityID&&a.sceneNameHash==b.sceneNameHash&&
        a.cameraRefHash==b.cameraRefHash&&a.slotNameHash==b.slotNameHash&&a.virtualCameraNameHash==b.virtualCameraNameHash;
}
inline bool ConfigureEditorNativeApplications(const Metadata& common,uint64_t generation,
    const NativePreviewSource& left,const NativePreviewSource& right){
    if(common.screen!=2||!common.screenEpoch||!common.controllerIdentity||!common.entityIdentity||!common.entityID||
       !generation||!left.component||!right.component||left.component==right.component||
       !left.cameraKey||!right.cameraKey||left.cameraKey==right.cameraKey||
       left.eye!=0||right.eye!=1||left.mode!=NativePreviewMode::NativeEditorDual||right.mode!=left.mode||
       left.bindingGeneration!=generation||right.bindingGeneration!=generation||
       common.componentIdentity!=left.component||common.virtualCameraNameHash!=left.cameraKey)return false;
    std::lock_guard lock(cameraApplicationMutex);auto& state=editorNativeApplications;
    const bool sameLifetime=state.configured&&state.common.screenEpoch==common.screenEpoch&&
        state.common.controllerIdentity==common.controllerIdentity&&state.common.entityIdentity==common.entityIdentity&&state.common.entityID==common.entityID;
    if(sameLifetime&&(generation<state.generation||(generation==state.generation&&
       (!SameEditorApplicationScope(state.common,common)||state.bindings[0]!=left||state.bindings[1]!=right))))return false;
    if(!state.configured||state.generation!=generation||!SameEditorApplicationScope(state.common,common)||
       state.bindings[0]!=left||state.bindings[1]!=right){state={};state.configured=true;state.generation=generation;state.bindings={left,right};}
    state.common=common;return true;
}
inline bool EditorNativeApplicationsConfigured(){std::lock_guard lock(cameraApplicationMutex);return editorNativeApplications.configured;}
inline bool EditorNativeSourceRegistered(const Metadata& common,const NativePreviewSource& source){
    if(source.mode!=NativePreviewMode::NativeEditorDual||source.eye>1)return false;
    std::lock_guard lock(cameraApplicationMutex);const auto& state=editorNativeApplications;
    return state.configured&&SameEditorApplicationScope(common,state.common)&&source==state.bindings[source.eye];
}
inline bool PublishEditorNativeApplication(std::shared_ptr<const CameraApplication> value){
    if(!value||value->source.mode!=NativePreviewMode::NativeEditorDual||value->source.eye>1||!value->nativeContext||!value->frame.sourceSequence)return false;
    std::lock_guard lock(cameraApplicationMutex);auto& state=editorNativeApplications;const auto eye=value->source.eye;
    const auto& m=value->metadata;const PreviewFrameKey key{m.screenEpoch,m.setupID,m.componentIdentity,m.virtualCameraNameHash};
    if(!state.configured||value->source!=state.bindings[eye]||!SameEditorApplicationScope(m,state.common)||
       value->frame.key!=key||value->frame.eye!=eye||!FreshPublication(m.publishedAtMs,PublicationNowMs())||
       (state.applications[eye^1u]&&state.applications[eye^1u]->nativeContext==value->nativeContext))return false;
    auto& previous=state.applications[eye];
    if(previous&&previous->frame.sourceSequence>value->frame.sourceSequence)return false;
    if(previous&&SameCameraApplication(*previous,*value))return true;
    previous=std::move(value);return true;
}
inline void PublishCameraApplication(std::shared_ptr<const CameraApplication> value){
    if(!value)return;
    std::lock_guard lock(cameraApplicationMutex);
    const auto previous=cameraApplication.load(std::memory_order_acquire);
    if(previous && previous->frame.sourceSequence>value->frame.sourceSequence){if(EditorFreshnessEnabled())NoteEditorPublication(previous.get(),value.get(),false,true);return;}
    if(previous && SameCameraApplication(*previous,*value)){if(EditorFreshnessEnabled())NoteEditorPublication(previous.get(),value.get(),true,false);return;}
    if(EditorFreshnessEnabled())NoteEditorPublication(previous.get(),value.get(),false,false);
    cameraApplication.store(std::move(value),std::memory_order_release);
}
inline std::shared_ptr<const CameraApplication> ReadCameraApplication(){return cameraApplication.load(std::memory_order_acquire);}
inline uint64_t CameraApplicationSourceKey(const CameraApplication& app)noexcept{return app.source.mode==NativePreviewMode::NativeEditorDual?app.source.cameraKey:app.metadata.virtualCameraNameHash;}
inline uintptr_t CameraApplicationSourceComponent(const CameraApplication& app)noexcept{return app.source.mode==NativePreviewMode::NativeEditorDual?app.source.component:app.metadata.componentIdentity;}
inline std::shared_ptr<const CameraApplication> ReadCameraApplicationForNode(uintptr_t context,uint64_t key,bool clockKnown,uint32_t frame){
    if(!context||!key||!clockKnown)return {};
    std::lock_guard lock(cameraApplicationMutex);const auto& state=editorNativeApplications;
    if(state.configured){for(unsigned eye=0;eye<2;++eye)if(state.bindings[eye].cameraKey==key){const auto& app=state.applications[eye];
        if(app&&app->nativeContext==context&&app->frame.nativeFrame==frame&&FreshPublication(app->metadata.publishedAtMs,PublicationNowMs()))return app;return {};}return {};}
    const auto app=ReadCameraApplication();return app&&app->nativeContext==context&&CameraApplicationSourceKey(*app)==key&&
        app->frame.nativeFrame==frame?app:nullptr;
}
inline std::shared_ptr<const CameraApplication> ReadCurrentApplicationForSource(const CameraApplication& value){
    std::lock_guard lock(cameraApplicationMutex);const auto& state=editorNativeApplications;
    if(state.configured){if(value.source.mode!=NativePreviewMode::NativeEditorDual||value.source.eye>1||
        value.source!=state.bindings[value.source.eye]||!SameEditorApplicationScope(value.metadata,state.common))return {};
        return state.applications[value.source.eye];}
    return value.source.mode==NativePreviewMode::NativeEditorDual?nullptr:ReadCameraApplication();
}
inline bool IsCurrentCameraApplication(const std::shared_ptr<const CameraApplication>& app){return app&&ReadCurrentApplicationForSource(*app)==app;}
inline bool HasCurrentPreviewApplications(){std::lock_guard lock(cameraApplicationMutex);const auto& s=editorNativeApplications;
    return s.configured?bool(s.applications[0]||s.applications[1]):bool(ReadCameraApplication());}
inline std::shared_ptr<const CameraApplication> ReadPreviewScopeApplication(){std::lock_guard lock(cameraApplicationMutex);const auto& s=editorNativeApplications;
    return s.configured?(s.applications[0]?s.applications[0]:s.applications[1]):ReadCameraApplication();}
inline bool CameraApplicationUiSource(const CameraApplication& app)noexcept{return app.source.mode!=NativePreviewMode::NativeEditorDual||app.source.eye==0;}
inline bool ClearCameraApplicationForSource(const NativePreviewSource& source){
    if(source.mode!=NativePreviewMode::NativeEditorDual||source.eye>1)return false;
    std::lock_guard lock(cameraApplicationMutex);auto& state=editorNativeApplications;
    if(!state.configured||source!=state.bindings[source.eye])return false;
    state.applications[source.eye].reset();return true;
}
inline void ClearCameraApplication(){std::lock_guard lock(cameraApplicationMutex);cameraApplication.store({},std::memory_order_release);editorNativeApplications={};}
// Native adapter serializes this admission with Begin/Clear/Invalidate.
bool PublishCameraApplicationIfCurrent(std::shared_ptr<const CameraApplication> value);
}
