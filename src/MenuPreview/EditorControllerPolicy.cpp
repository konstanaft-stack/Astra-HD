#include "MenuPreview/EditorControllerPolicy.hpp"
#include <limits>

namespace cvr::menu_preview {
EditorControllerStep EditorControllerPolicy::Update(const EditorControllerSample& sample)noexcept{
    if(!sample.available||sample.blocked||sample.timeNs<=0||!sample.origin||
       !std::isfinite(sample.grip)||!std::isfinite(sample.stickX)||!std::isfinite(sample.stickY)||
       !std::isfinite(sample.deadzone)) {Reset();return {};}
    const bool down=held_?sample.grip>.55f:sample.grip>=.7f;
    if(!down){Reset();return {};}
    const bool continuous=held_&&origin_==sample.origin&&sample.timeNs>time_&&sample.timeNs-time_<=250000000;
    const float seconds=continuous?std::min(float(double(sample.timeNs-time_)*1e-9),.05f):0.f;
    EditorControllerStep step;step.claimed=true;step.reset=sample.stickClick&&!click_;
    held_=true;click_=sample.stickClick;time_=sample.timeNs;origin_=sample.origin;
    if(step.reset||sample.stickClick||!continuous)return step;
    const float x=std::clamp(sample.stickX,-1.f,1.f),y=std::clamp(sample.stickY,-1.f,1.f);
    const float magnitude=std::hypot(x,y),deadzone=std::clamp(sample.deadzone,0.f,.8f);
    // Independent right X remains active when the left stick is neutral.
    if(std::isfinite(sample.orbitX)&&std::isfinite(sample.orbitDeadzone)){
        const float right=std::clamp(sample.orbitX,-1.f,1.f),dz=std::clamp(sample.orbitDeadzone,0.f,.8f);
        if(std::abs(right)>dz)step.orbit=std::copysign((std::abs(right)-dz)/(1.f-dz),right)*1.0471975511965976f*seconds;
    }
    if(magnitude<=deadzone||magnitude<=0)return step;
    const float shaped=(std::min(magnitude,1.f)-deadzone)/(1.f-deadzone);
    step.height=(y/magnitude)*shaped*.5f*seconds;
    step.distance=(x/magnitude)*shaped*.6f*seconds;
    return step;
}
EditorControlLatch::Value EditorControlLatch::Resolve(uint64_t source,uint32_t eye,float height,float distance,
    uint64_t presentedRevision,uint64_t& revisionCounter,float orbit)noexcept{
    if(!source||eye>1||source<source_||!std::isfinite(height)||!std::isfinite(distance)||
       std::abs(height)>5||std::abs(distance)>5||!std::isfinite(orbit)||std::abs(orbit)>3.141593f)return {};
    if(source==source_)return value_;
    source_=source;
    if(!value_.revision||(eye==0&&presentedRevision==value_.revision&&
        (height!=value_.height||distance!=value_.distance||orbit!=value_.orbit))){
        if(revisionCounter==std::numeric_limits<uint64_t>::max())return {};
        value_={height,distance,++revisionCounter,orbit};
    }
    return value_;
}
}
