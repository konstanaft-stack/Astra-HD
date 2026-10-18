#pragma once
#include "MenuPreview/InitialEditorFraming.hpp"
#include "MenuPreview/SpatialEditorPolicy.hpp"

namespace cvr::menu_preview {
inline bool SameEditorCameraLifetime(const Metadata& a,const Metadata& b)noexcept {
    return a.screen==2&&b.screen==2&&a.screenEpoch&&a.controllerIdentity&&a.entityIdentity&&a.entityID&&
        a.screenEpoch==b.screenEpoch&&a.controllerIdentity==b.controllerIdentity&&
        a.entityIdentity==b.entityIdentity&&a.entityID==b.entityID;
}
// Caller owns synchronization and commits this value only with a successful
// common native frame. A category/binding is not an editor camera lifetime.
class EditorCameraLifetime {
public:
    template<class Prepare>
    bool Resolve(const Metadata& scope,uint64_t origin,const XrPosef& head,Prepare&& prepare){
        if(!SameEditorCameraLifetime(scope,scope)||!origin||!ValidHead(head))return false;
        if(ready_&&SameEditorCameraLifetime(scope_,scope)){
            if(origin_!=origin){anchor_=head;origin_=origin;}
            return true;
        }
        cvr::camera::ExternalViewPose camera;InitialEditorFraming framing;
        if(!prepare(camera,framing)||!framing.latched||!framing.oriented||
           framing.epoch!=scope.screenEpoch||framing.controller!=scope.controllerIdentity||
           framing.owner!=scope.entityIdentity)return false;
        auto rotation=camera.rotation;if(!cvr::camera::NormalizeExternal(rotation))return false;
        camera_=camera;framing_=framing;anchor_=head;scope_=scope;origin_=origin;ready_=true;return true;
    }
    void Invalidate(bool epochOpen)noexcept{if(!epochOpen)Clear();}
    void Clear()noexcept{*this={};}
    uint32_t Epoch()const noexcept{return ready_?scope_.screenEpoch:0;}
    const cvr::camera::ExternalViewPose& Camera()const noexcept{return camera_;}
    const InitialEditorFraming& Framing()const noexcept{return framing_;}
    const XrPosef& Anchor()const noexcept{return anchor_;}
private:
    static bool ValidHead(const XrPosef& head)noexcept{
        const float values[]{head.position.x,head.position.y,head.position.z,head.orientation.x,
            head.orientation.y,head.orientation.z,head.orientation.w};
        for(float v:values)if(!std::isfinite(v))return false;
        const auto& q=head.orientation;const double norm=double(q.x)*q.x+double(q.y)*q.y+double(q.z)*q.z+double(q.w)*q.w;
        return norm>=.999&&norm<=1.001;
    }
    Metadata scope_{};cvr::camera::ExternalViewPose camera_{};InitialEditorFraming framing_{};
    XrPosef anchor_{{0,0,0,1},{0,0,0}};uint64_t origin_{};bool ready_{};
};
}
