#pragma once
#include "MenuPreview/ProducerRoute.hpp"
namespace cvr::menu_preview {
struct LocalComponentPose {
    Metadata metadata{};uint32_t eye{UINT32_MAX};uint64_t publishedAtMs{},revision{};
    std::array<uint8_t,32> baseline{},applied{}; // native FP xyz, padding, xyzw
    bool verified{};
};
inline std::atomic<std::shared_ptr<const LocalComponentPose>> localComponentPose;
inline std::shared_ptr<const LocalComponentPose> ReadLocalComponentPose(){return localComponentPose.load(std::memory_order_acquire);}
inline void ClearLocalComponentPose(){localComponentPose.store({},std::memory_order_release);}
inline bool MatchesLocalComponentPose(const LocalComponentPose& p,const Metadata& m,const Descriptor& native){
    return p.verified && p.eye<=1 && SamePreview(p.metadata,m) &&
        !std::memcmp(native.data(),p.applied.data()+16,16) && !std::memcmp(native.data()+16,p.applied.data(),12);
}
inline bool VerifyLocalEyeDelta(const LocalComponentPose& p,float half){
    Descriptor base{},expected{};std::memcpy(base.data(),p.baseline.data()+16,16);std::memcpy(base.data()+16,p.baseline.data(),12);
    if(p.eye>1 || half<=0 || !MakeEyeDescriptor(base,p.eye?half:-half,expected))return false;
    if(std::memcmp(p.baseline.data()+16,p.applied.data()+16,16))return false;
    for(unsigned i=0;i<3;++i){int32_t a{},b{};std::memcpy(&a,expected.data()+16+i*4,4);std::memcpy(&b,p.applied.data()+i*4,4);
        // Local->world float update may quantise the native fixed-point value.
        if(std::abs(int64_t(a)-int64_t(b))>32)return false;
    }
    return true;
}
inline bool MatchesLocalRenderOrigin(const LocalComponentPose& p,const float origin[4]){
    if(!origin || !std::isfinite(origin[3]) || std::abs(origin[3]-1.f)>.0001f)return false;
    for(unsigned i=0;i<3;++i){int32_t position{};std::memcpy(&position,p.applied.data()+i*4,4);
        if(!std::isfinite(origin[i]) || std::abs(double(origin[i])-double(position)/131072.)>.0001)return false;
    }
    return true;
}
bool PublishLocalCameraApplicationIfCurrent(std::shared_ptr<const CameraApplication> value);
}
