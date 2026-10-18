#pragma once
#include "MenuPreview/Scope.hpp"
#include "MenuPreview/CameraApplication.hpp"
#include "Camera/ExternalViewMath.hpp"
#include "Utils/XrMath.hpp"
#include <optional>

namespace cvr::xr {
// Same game↔XR basis as ComposeExternal. No camera/actor ownership retained.
inline bool BuildEditorOrbitView(const cvr::camera::ExternalViewPose& unorbited,
    const std::array<double,3>& actor,const XrPosef& reference,const XrPosef& recenter,
    float scale,float angle,uint64_t origin,uint64_t revision,cvr::menu_preview::EditorOrbitView& out){
    out={};if(angle==0)return true;
    if(!origin||!revision||!std::isfinite(angle)||std::abs(angle)>3.141593f||!std::isfinite(scale)||scale<=0||scale>100)return false;
    auto native=unorbited.rotation;auto ref=reference,base=recenter;
    if(!cvr::camera::NormalizeExternal(native)||!cvr::camera::NormalizeExternal(ref.orientation)||!cvr::camera::NormalizeExternal(base.orientation)||
       std::abs(native.x)>.0001f||std::abs(native.y)>.0001f)return false; // Current qualified native yaw-only staging cameras.
    for(double v:actor)if(!std::isfinite(v))return false;
    const double yaw=std::atan2(2*(native.w*native.z+native.x*native.y),1-2*(native.y*native.y+native.z*native.z));
    const double dx=actor[0]-double(unorbited.position[0])/131072.,dy=actor[1]-double(unorbited.position[1])/131072.,dz=actor[2]-double(unorbited.position[2])/131072.;
    const XrVector3f local{float((std::cos(yaw)*dx+std::sin(yaw)*dy)/scale),float(dz/scale),float((std::sin(yaw)*dx-std::cos(yaw)*dy)/scale)};
    const auto refRotation=MultiplyQuat(base.orientation,ref.orientation);const auto refPosition=RotateVector(base.orientation,ref.position);
    const auto offset=RotateVector(refRotation,local);
    cvr::menu_preview::EditorOrbitView result;result.pivot={base.position.x+refPosition.x+offset.x,base.position.y+refPosition.y+offset.y,base.position.z+refPosition.z+offset.z};
    // XR+Y maps game+Z; inverse viewer transform is XR−Y, conjugated by reference/recenter.
    result.inverseRotation=MultiplyQuat(MultiplyQuat(refRotation,{0,-std::sin(angle*.5f),0,std::cos(angle*.5f)}),ConjugateQuat(refRotation));
    if(!cvr::camera::NormalizeExternal(result.inverseRotation))return false;
    if(!std::isfinite(result.pivot.x)||!std::isfinite(result.pivot.y)||!std::isfinite(result.pivot.z))return false;
    result.controlRevision=revision;result.origin=origin;result.angle=angle;result.valid=true;out=result;return true;
}
inline bool EditorOrbitViewValid(const cvr::menu_preview::EditorOrbitView& v,uint64_t origin,uint64_t revision){
    auto q=v.inverseRotation;return v.valid&&origin&&revision&&v.origin==origin&&v.controlRevision==revision&&
        std::isfinite(v.angle)&&std::abs(v.angle)<=3.141593f&&std::isfinite(v.pivot.x)&&std::isfinite(v.pivot.y)&&std::isfinite(v.pivot.z)&&cvr::camera::NormalizeExternal(q);
}
inline bool EditorOrbitViewsMatch(const cvr::menu_preview::EditorOrbitView& a,const cvr::menu_preview::EditorOrbitView& b,uint64_t origin,uint64_t revision){
    if(a.angle==0&&b.angle==0)return true;
    return EditorOrbitViewValid(a,origin,revision)&&EditorOrbitViewValid(b,origin,revision)&&a.angle==b.angle&&
        a.pivot.x==b.pivot.x&&a.pivot.y==b.pivot.y&&a.pivot.z==b.pivot.z&&a.inverseRotation.x==b.inverseRotation.x&&
        a.inverseRotation.y==b.inverseRotation.y&&a.inverseRotation.z==b.inverseRotation.z&&a.inverseRotation.w==b.inverseRotation.w;
}
class EditorOrbitViewLatch {
    cvr::menu_preview::EditorOrbitView view_{};cvr::menu_preview::PreviewFrameKey key_{};uint64_t source_{};
public:
    void Reset(){view_={};key_={};source_=0;}
    bool NeedsLeft(uint64_t source,uint32_t eye)const{return eye==0&&source!=source_;}
    bool Resolve(const cvr::menu_preview::PreviewFrameKey& key,uint64_t source,uint32_t eye,uint64_t origin,uint64_t revision,float angle,
                 const cvr::menu_preview::EditorOrbitView* prepared,cvr::menu_preview::EditorOrbitView& out){
        if(!source||eye>1||source<source_)return false;
        if(NeedsLeft(source,eye)){
            if(!prepared||!EditorOrbitViewValid(*prepared,origin,revision)||prepared->angle!=angle)return false;
            view_=*prepared;key_=key;source_=source;
        }
        if(key_!=key||!EditorOrbitViewValid(view_,origin,revision)||view_.angle!=angle)return false;
        out=view_;return true;
    }
};
inline bool ApplyEditorOrbitView(const cvr::menu_preview::EditorOrbitView& v,uint64_t origin,uint64_t revision,const XrPosef& initial,XrPosef& out){
    if(v.angle==0){out=initial;return true;}
    if(!EditorOrbitViewValid(v,origin,revision))return false;
    const auto d=RotateVector(v.inverseRotation,{initial.position.x-v.pivot.x,initial.position.y-v.pivot.y,initial.position.z-v.pivot.z});
    out={MultiplyQuat(v.inverseRotation,initial.orientation),{v.pivot.x+d.x,v.pivot.y+d.y,v.pivot.z+d.z}};return cvr::camera::NormalizeExternal(out.orientation);
}
inline constexpr uint32_t EditorUiMinimumPool(bool separated){return separated?5u:0u;}
inline bool EditorUiRetainedAllowed(const cvr::menu_preview::Metadata& saved,uint64_t savedOrigin,
    uint64_t currentOrigin,bool epochOpen,const cvr::menu_preview::Metadata* current){
    return epochOpen&&saved.screen==2&&saved.screenEpoch&&saved.controllerIdentity&&savedOrigin&&
        savedOrigin==currentOrigin&&(!current||(current->screen==2&&
        current->screenEpoch==saved.screenEpoch&&current->controllerIdentity==saved.controllerIdentity));
}
inline std::optional<bool> EditorUiPairRequirement(bool left,bool right){
    if(left!=right)return {};return left;
}
template<class Publication,class Receipt>
bool EditorUiAttachmentMatches(const Publication& ui,const Receipt& receipt) {
    const auto& a=ui.scope;const auto& b=receipt.scope;
    return a.screen==2&&b.screen==2&&a.screenEpoch&&a.screenEpoch==b.screenEpoch&&
        a.controllerIdentity&&a.controllerIdentity==b.controllerIdentity&&
        a.setupID==b.setupID&&a.componentIdentity&&a.componentIdentity==b.componentIdentity&&
        a.virtualCameraNameHash&&a.virtualCameraNameHash==b.virtualCameraNameHash&&
        ui.eye<2&&ui.eye==receipt.eye&&ui.nativeFrame==receipt.nativeFrame&&
        ui.sourceSequence&&ui.sourceSequence==receipt.sourceSequence&&
        ui.cameraRevision&&ui.cameraRevision==receipt.cameraRevision;
}
struct EditorUiAnchor {
    uint32_t epoch{};uint64_t owner{},origin{};
    XrPosef base{{0,0,0,1},{0,0,0}};
    void Reset(){epoch=0;owner=origin=0;}
    bool Valid()const{return epoch&&owner&&origin;}
    std::optional<XrPosef> Pose(const cvr::menu_preview::Metadata& m,uint64_t o,const XrPosef& b){
        if(m.screen!=2||!m.screenEpoch||!m.controllerIdentity||!o)return {};
        // Setup/component/camera and body-camera controls do not move this plane.
        if(epoch!=m.screenEpoch||owner!=m.controllerIdentity||origin!=o){
            epoch=m.screenEpoch;owner=m.controllerIdentity;origin=o;base=b;
        }
        const auto offset=RotateVector(base.orientation,{0,0,-1});
        return XrPosef{base.orientation,{base.position.x+offset.x,base.position.y+offset.y,base.position.z+offset.z}};
    }
};
inline std::optional<XrExtent2Df> EditorUiSize(float aspect){
    if(!std::isfinite(aspect)||aspect<=0||!std::isfinite(aspect*2.8f))return {};
    return XrExtent2Df{aspect*2.8f,2.8f};
}
}
