#pragma once
#include "Overlay/PpdCalibration.hpp"

namespace cvr::ppd {
struct PpdRenderedFov {
    float angleLeft{},angleRight{},angleUp{},angleDown{};
    friend bool operator==(const PpdRenderedFov&,const PpdRenderedFov&)=default;
};
struct ReferenceOptics {
    std::array<PpdRenderedFov,2> raw{},applied{};
    float pitch{},yaw{};
    bool valid{};
    friend bool operator==(const ReferenceOptics&,const ReferenceOptics&)=default;
};
inline TangentExtent FovTangents(const PpdRenderedFov& f){
    return {std::tan(f.angleUp),-std::tan(f.angleDown),-std::tan(f.angleLeft),std::tan(f.angleRight)};
}
inline bool ValidReferenceFov(const PpdRenderedFov& f){
    for(float a:{f.angleLeft,f.angleRight,f.angleUp,f.angleDown})
        if(!std::isfinite(a)||std::abs(a)>=1.57079632679f)return false;
    return f.angleLeft<0&&f.angleRight>0&&f.angleUp>0&&f.angleDown<0;
}
// Luke42DA 2DA3D0 -> 2C8AE0 -> 2CA2A0. Both optimizer policies are enabled,
// as measured in the reference profile. Keep the original float order and
// 0.017-radian threshold. A computed yaw pose is NOT proof that it was applied
// to the source camera: 2DCBB0 applies the common pitch to the queried head.
inline ReferenceOptics BuildReferenceOptics(std::array<PpdRenderedFov,2> raw){
    ReferenceOptics o;o.raw=raw;
    if(!ValidReferenceFov(raw[0])||!ValidReferenceFov(raw[1]))return o;
    auto l=FovTangents(raw[0]),r=FovTangents(raw[1]);
    const float ul=std::atan(l.up),ur=std::atan(r.up),dl=std::atan(l.down),dr=std::atan(r.down);
    const float ll=std::atan(l.left),lr=std::atan(r.left),rl=std::atan(l.right),rr=std::atan(r.right);
    constexpr float threshold=std::bit_cast<float>(uint32_t{0x3c8b4396});
    const float pitch=(((ul+ur)-dl)-dr)*.25f;
    const float yaw=(((rr+ll)-lr)-rl)*.25f;
    o.pitch=std::abs(pitch)>threshold?pitch:0;
    o.yaw=std::abs(yaw)>threshold?yaw:0;
    o.applied[0]={-std::atan(std::tan(ll-o.yaw)),std::atan(std::tan(rl+o.yaw)),
        std::atan(std::tan(ul-o.pitch)),-std::atan(std::tan(dl+o.pitch))};
    o.applied[1]={-std::atan(std::tan(lr+o.yaw)),std::atan(std::tan(rr-o.yaw)),
        std::atan(std::tan(ur-o.pitch)),-std::atan(std::tan(dr+o.pitch))};
    o.valid=ValidReferenceFov(o.applied[0])&&ValidReferenceFov(o.applied[1]);return o;
}
inline PpdCalibration CalibrateReferenceOptics(uint32_t w,uint32_t h,const ReferenceOptics& o){
    if(!o.valid||w>16384||h>16384)return {};
    return CalibratePpd(int(w),int(h),{}, {FovTangents(o.applied[0]),FovTangents(o.applied[1])});
}
inline PpdRenderedFov ReferenceSourceFov(uint32_t w,uint32_t h,const PpdCalibration& c){
    if(!c.valid||c.pixelsPerTangent<=0)return {};
    const float x=std::atan((float(w)*.5f)/c.pixelsPerTangent);
    const float y=std::atan((float(h)*.5f)/c.pixelsPerTangent);
    return {-x,x,y,-y};
}
// 29BD10 + roundf at4A3F40: nearest integer, ties away from zero. Width/height
// first, then clamp and round the origin. Bounds are right/bottom, not extents.
inline std::array<float,4> ReferenceEyeCrop(uint32_t w,uint32_t h,const PpdCalibration& c,
                                        const PpdRenderedFov& eye,SourceProjectionRegion center={}){
    if(!c.valid||!ValidReferenceFov(eye)||!w||!h||w>16384||h>16384)return {};
    const auto t=FovTangents(eye);
    constexpr float degrees=std::bit_cast<float>(uint32_t{0x42652ee1});
    const float focal=c.ppd*degrees;
    const float cw=std::round((t.left+t.right)*focal),ch=std::round((t.up+t.down)*focal);
    if(!(cw>=1&&cw<=float(w)&&ch>=1&&ch<=float(h)))return {};
    const float x=std::round(std::min(std::max(float(w)*center.centerX-t.left*focal,0.f),float(w)-cw));
    const float y=std::round(std::min(std::max(float(h)*center.centerY-t.up*focal,0.f),float(h)-ch));
    return {x,y,x+cw,y+ch};
}
}
