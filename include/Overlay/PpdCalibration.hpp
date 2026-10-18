#pragma once
#include "Overlay/PpdResolution.hpp"
#include <array>
#include <bit>
#include <cstdint>

namespace cvr::ppd {
// Positive tangent magnitudes, not angles or a headset model's nominal FOV.
struct TangentExtent {float up{},down{},left{},right{};};
struct SourceProjectionRegion {
    float centerX=.5f,centerY=.5f;
    float excludedRight{},excludedBottom{};
};
struct PpdCalibration {
    float ppd{},nominalWidth{},nominalHeight{},pixelsPerTangent{};
    bool valid{};
};
struct PpdAspectPolicy {
    float minimum=.1f,maximum=10,requested{};
    bool limit4096{};
};
inline constexpr float PpdRadiansPerDegree=std::bit_cast<float>(uint32_t{0x3c8efa35});

// Reconstructed density/fit rule: bounded by every side of BOTH eye frusta.
// Evidence: pinned reference 29B910; renderer+1F4 is copied to the PPD UI
// through the renderer+EC -> runtime+841FB8 packet. This function requires
// observed source projection and dimensions; it supplies no headset fallback.
inline PpdCalibration CalibratePpd(int width,int height,SourceProjectionRegion region,
                                  const std::array<TangentExtent,2>& eyes) {
    if(width<=0 || height<=0)return {};
    const float cx=region.centerX,cy=region.centerY;
    const float right=1-(region.excludedRight+cx);
    const float down=1-(region.excludedBottom+cy);
    if(!std::isfinite(cx)||!std::isfinite(cy)||!std::isfinite(region.excludedRight)||
       !std::isfinite(region.excludedBottom)||cx<=0||cx>=1||cy<=0||cy>=1||
       region.excludedRight<0||region.excludedBottom<0||right<=0||down<=0)return {};
    TangentExtent bounds{};
    for(const auto& eye:eyes) {
        for(float value:{eye.up,eye.down,eye.left,eye.right})
            if(!std::isfinite(value)||value<=0)return {};
        bounds.up=std::max(bounds.up,eye.up);bounds.down=std::max(bounds.down,eye.down);
        bounds.left=std::max(bounds.left,eye.left);bounds.right=std::max(bounds.right,eye.right);
    }
    const float w=static_cast<float>(width),h=static_cast<float>(height);
    const float horizontal=std::min((w*cx)/bounds.left,(right*w)/bounds.right);
    const float vertical=std::min((h*cy)/bounds.up,(down*h)/bounds.down);
    const float focal=std::min(horizontal,vertical);
    if(!std::isfinite(focal)||focal<=0)return {};
    const float fittedWidth=vertical<horizontal ? (w/horizontal)*vertical : w;
    const float fittedHeight=vertical<horizontal ? h : (h/vertical)*horizontal;
    const float ppd=focal*PpdRadiansPerDegree;
    if(!std::isfinite(ppd)||!std::isfinite(fittedWidth)||!std::isfinite(fittedHeight)||
       ppd<=0||fittedWidth<=0||fittedHeight<=0)return {};
    return {ppd,fittedWidth,fittedHeight,focal,true};
}

// Pinned 2B5580: extend to the aspect bounds, scale Target/reference, and
// round each positive dimension to the nearest16, with HALF-UP ties.
// Unknown calibration is an explicit error; it must not acknowledge Applied.
inline Resolution PreviewCalibratedPpd(const PpdCalibration& observed,float target,
                                      PpdAspectPolicy aspect={}) {
    if(!observed.valid||!std::isfinite(observed.ppd)||observed.ppd<=0||
       !std::isfinite(observed.nominalWidth)||observed.nominalWidth<=0||
       !std::isfinite(observed.nominalHeight)||observed.nominalHeight<=0||
       !std::isfinite(target)||target<=0||!std::isfinite(aspect.minimum)||
       !std::isfinite(aspect.maximum)||!std::isfinite(aspect.requested)||
       aspect.minimum<=0||aspect.maximum<aspect.minimum)return {};
    float w=observed.nominalWidth,h=observed.nominalHeight;
    float lo=aspect.minimum,hi=aspect.maximum;
    if(aspect.requested>=lo && aspect.requested<=hi)lo=hi=aspect.requested;
    const float ratio=w/h;
    if(ratio>hi)h=w/hi;
    if(ratio<lo)w=h*lo;
    float scale=target/observed.ppd;
    if(aspect.limit4096)scale=std::min(scale,4096.0f/std::max(w,h));
    // Keep original float operation order, and validate before integer casts.
    const float qw=scale*w*.0625f+.5f,qh=scale*h*.0625f+.5f;
    constexpr int maxTextureDimension=16384;
    if(!std::isfinite(qw)||!std::isfinite(qh)||qw<1||qh<1||
       qw>=maxTextureDimension/16+1.0f||qh>=maxTextureDimension/16+1.0f)return {};
    return {static_cast<int>(qw)*16,static_cast<int>(qh)*16,true};
}
}
