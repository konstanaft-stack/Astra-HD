#pragma once
#include "MenuPreview/CameraApplication.hpp"
#include <string_view>
namespace cvr::menu_preview {
enum class CameraTestMode {Off,Zero,Left,Right,Alternate};
inline CameraTestMode ParseCameraTestMode(std::string_view v){
    if(v=="zero")return CameraTestMode::Zero;
    if(v=="left")return CameraTestMode::Left;if(v=="right")return CameraTestMode::Right;
    if(v=="1"||v=="alternate")return CameraTestMode::Alternate;return CameraTestMode::Off;
}
inline bool SamePreview(const Metadata& a,const Metadata& b){return a.screenEpoch==b.screenEpoch && a.setupID==b.setupID && a.componentIdentity==b.componentIdentity && a.virtualCameraNameHash==b.virtualCameraNameHash;}
inline bool SameDescriptorCamera(const Descriptor& a,const Descriptor& b){
    // Native build descriptors carry independently authored ancillary fields.
    // Pose and own optics are the exact registered-camera fingerprint.
    return !std::memcmp(a.data(),b.data(),28) && !std::memcmp(a.data()+0x38,b.data()+0x38,16);
}
struct AppliedProducer {Metadata metadata{};alignas(16) Descriptor base{},applied{};PreviewFrameEye source{};};
inline bool MatchesAppliedProducer(const AppliedProducer& p,const Metadata& m,const Descriptor& native){return p.source && SamePreview(p.metadata,m) && SameDescriptorCamera(p.applied,native);}
class ConsumingFrameOwner {
public:
    PreviewFrameEye Resolve(const Metadata& m,bool clockKnown,uint32_t frame,uint32_t actualEye){
        if(actualEye>1)return {};auto next=owner_.Resolve(m,m.virtualCameraNameHash,clockKnown,frame);
        if(!next)return {};
        if(!next.changed && actualEye!=lastEye_)return {}; // one frame cannot own two eyes
        next.eye=actualEye;lastEye_=actualEye;return next;
    }
private:PreviewFrameEyeOwner owner_;uint32_t lastEye_{};
};
class DirtyRequestBudget {
public: bool Take(){if(count_>=96)return false;++count_;return true;}uint32_t Count()const{return count_;}
private:uint32_t count_{};
};
}
