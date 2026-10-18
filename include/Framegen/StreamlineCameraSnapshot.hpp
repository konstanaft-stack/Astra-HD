#pragma once
#include "Framegen/ProducerContract.hpp"
#include <sl_consts.h>
#include <cstring>

namespace cvr::framegen {
inline CameraConstants SnapshotStreamlineCamera(const sl::Constants& input) {
    CameraConstants out;
    if(input.structType!=sl::Constants::s_structType || input.structVersion<1 || input.structVersion>2)return out;
    out.source=CameraConstantsSource::Streamline;out.version=uint32_t(input.structVersion);
    auto copy=[&](unsigned first,const void* source,unsigned count) {
        std::memcpy(out.values.data()+first,source,count*sizeof(float));
        for(unsigned i=first;i<first+count;++i)out.valuePresent.set(i);
    };
    static_assert(sizeof(sl::float4x4)==16*sizeof(float));
    static_assert(sizeof(sl::float2)==2*sizeof(float) && sizeof(sl::float3)==3*sizeof(float));
    copy(0,&input.cameraViewToClip,16);copy(16,&input.clipToCameraView,16);
    copy(32,&input.clipToLensClip,16);copy(48,&input.clipToPrevClip,16);copy(64,&input.prevClipToClip,16);
    copy(80,&input.jitterOffset,2);copy(82,&input.mvecScale,2);copy(84,&input.cameraPinholeOffset,2);
    copy(86,&input.cameraPos,3);copy(89,&input.cameraUp,3);copy(92,&input.cameraRight,3);copy(95,&input.cameraFwd,3);
    copy(98,&input.cameraNear,1);copy(99,&input.cameraFar,1);copy(100,&input.cameraFOV,1);
    copy(101,&input.cameraAspectRatio,1);copy(102,&input.motionVectorsInvalidValue,1);
    // A v1 allocation may end before this member. Do not read a v2 default
    // from its tail and report it as measured input.
    if(input.structVersion>=2)copy(103,&input.minRelativeLinearDepthObjectSeparation,1);
    out.flags={uint32_t(input.depthInverted),uint32_t(input.cameraMotionIncluded),uint32_t(input.motionVectors3D),
        uint32_t(input.reset),uint32_t(input.orthographicProjection),uint32_t(input.motionVectorsDilated),uint32_t(input.motionVectorsJittered)};
    out.flagPresent.set();return out;
}
}
