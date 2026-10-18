#pragma once
#include "MenuPreview/Scope.hpp"
#include <array>
#include <cmath>
#include <cstring>

namespace cvr::menu_preview {
using Descriptor = std::array<uint8_t,0x60>;
// A private descriptor only. This does not schedule or modify a live view.
inline bool MakeEyeDescriptor(const Descriptor& source,float signedHalfIpd,Descriptor& output) {
    if(!std::isfinite(signedHalfIpd)||std::abs(signedHalfIpd)>.15f)return false;
    float q[4]{};std::memcpy(q,source.data(),sizeof(q));
    float norm=0;for(float v:q){if(!std::isfinite(v))return false;norm+=v*v;}
    if(norm<.999f||norm>1.001f)return false;
    const float right[]{1-2*(q[1]*q[1]+q[2]*q[2]),2*(q[0]*q[1]+q[3]*q[2]),2*(q[0]*q[2]-q[3]*q[1])};
    Descriptor result=source;
    for(unsigned i=0;i<3;++i){
        int32_t position{};std::memcpy(&position,source.data()+0x10+i*4,4);
        // Same truncation and fixed-point scale as the native camera writer.
        volatile float half=right[i]*signedHalfIpd;
        volatile float fixed=half*131072.0f;
        const auto shifted=int64_t(position)+int64_t(int32_t(fixed));
        if(shifted<std::numeric_limits<int32_t>::min()||shifted>std::numeric_limits<int32_t>::max())return false;
        position=int32_t(shifted);std::memcpy(result.data()+0x10+i*4,&position,4);
    }
    output=result;return true;
}
inline bool MatchesProducerIdentity(const Metadata& m,uintptr_t component,uint64_t now){
    return component && component==m.componentIdentity && SupportedPreviewScreen(m.screen) && m.screenEpoch &&
        m.virtualCameraNameHash && FreshPublication(m.publishedAtMs,now);
}
// Save/restore also clears an invalid nested source for the duration of its call.
template<class T> class ScopedProducer {
public:
    ScopedProducer(T& slot,T next):slot_(slot),previous_(std::move(slot)){slot_=std::move(next);}
    ~ScopedProducer(){slot_=std::move(previous_);}
    ScopedProducer(const ScopedProducer&)=delete;
    ScopedProducer& operator=(const ScopedProducer&)=delete;
private:T& slot_;T previous_;
};
}
