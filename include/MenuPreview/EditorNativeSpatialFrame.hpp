#pragma once
#include "MenuPreview/EditorNativeFrame.hpp"
#include "MenuPreview/SpatialEditorPolicy.hpp"
#include <utility>

namespace cvr::menu_preview {
struct EditorNativeSpatialCommon {
    cvr::camera::ExternalViewPose camera;
    XrPosef anchor{{0,0,0,1},{0,0,0}},head{{0,0,0,1},{0,0,0}},recenter{{0,0,0,1},{0,0,0}};
    float scale{},halfIpd{},horizontal{};
    uint32_t width{},height{};
    uint64_t origin{},controlRevision{};
    EditorImageRoute imageRoute=EditorImageRoute::LegacyCroppedQuad;
    EditorOrbitView orbit{};
};
inline bool BuildEditorNativeSpatialEye(const EditorNativeSpatialCommon& common,
    const EditorNativeFrameSample& sample,const Metadata& scope,const NativePreviewSource& source,
    const Descriptor& native,CameraApplication& output)noexcept {
    if(!sample||source.mode!=NativePreviewMode::NativeEditorDual||source.eye!=sample.frame.eye||
       source.bindingGeneration!=sample.bindingGeneration||common.origin!=sample.origin)return false;
    EditorEye eye;
    if(!BuildEditorEye(common.camera,common.anchor,common.head,common.recenter,common.scale,
        common.halfIpd,source.eye,common.horizontal,common.width,common.height,eye,common.imageRoute))return false;
    CameraApplication result;result.metadata=scope;result.source=source;result.frame=sample.frame;
    result.baseDescriptor=native;result.eyeDescriptor=native;result.localPoseRevision=sample.frame.sourceSequence;
    result.editorControlRevision=common.controlRevision;
    std::memcpy(result.eyeDescriptor.data(),&eye.camera.rotation,16);
    std::memcpy(result.eyeDescriptor.data()+16,eye.camera.position.data(),12);
    std::memcpy(result.eyeDescriptor.data()+0x38,&eye.cameraVerticalFov,4);
    std::memcpy(result.eyeDescriptor.data()+0x3C,&eye.cameraAspect,4);
    std::memcpy(result.eyeDescriptor.data()+0x40,&eye.nearPlane,4);
    result.projection={eye.localEye,eye.projection,common.origin,true};result.projection.orbit=common.orbit;
    output=std::move(result);return true;
}

// Pure transaction around the real math callbacks. A new native frame revokes
// the preceding common state immediately; prepare AND first-eye math must
// succeed before either eye may reuse it. No retries can revive a partial frame.
template<class Common> class EditorNativeSpatialFrameCache {
public:
    template<class Prepare,class Compose>
    bool Resolve(const Metadata& scope,const NativePreviewSource& source,uint32_t frame,uint64_t origin,
        const XrPosef& head,const Descriptor& native,Prepare&& prepare,Compose&& compose,
        CameraApplication& output,bool* firstCommitted=nullptr) {
        if(firstCommitted)*firstCommitted=false;
        if(source.mode!=NativePreviewMode::NativeEditorDual||source.eye>1||!source.component||!source.cameraKey)return false;
        if(hasFrame_&&frame==frame_&&!SameEditorApplicationScope(scope,scope_))return false;
        const auto sample=latch_.Resolve(scope,source.bindingGeneration,frame,source.eye,origin,head);
        if(!sample)return false;
        if(sample.firstInFrame){valid_=false;ready_={};common_={};scope_=scope;frame_=frame;hasFrame_=true;}
        if(!sample.firstInFrame&&!valid_)return false;
        if(ready_[source.eye]){
            if(applications_[source.eye].source!=source)return false;
            output=applications_[source.eye];output.frame.changed=sample.frame.changed;return true;
        }
        Common candidate;
        const Common* prepared=&common_;
        if(sample.firstInFrame){if(!prepare(sample,candidate))return false;prepared=&candidate;}
        CameraApplication application;
        if(!compose(*prepared,sample,scope_,source,native,application))return false;
        if(sample.firstInFrame){common_=std::move(candidate);valid_=true;if(firstCommitted)*firstCommitted=true;}
        applications_[source.eye]=std::move(application);ready_[source.eye]=true;
        output=applications_[source.eye];return true;
    }
    const Common* CommonState()const noexcept{return valid_?&common_:nullptr;}
    void Clear()noexcept{latch_.Clear();valid_=hasFrame_=false;ready_={};common_={};scope_={};frame_=0;applications_={};}
private:
    EditorNativeFrameLatch latch_;
    Common common_{};
    Metadata scope_{};
    uint32_t frame_{};
    std::array<CameraApplication,2> applications_{};
    std::array<bool,2> ready_{};
    bool valid_{},hasFrame_{};
};
}
