#pragma once
#include "Framegen/Inputs.hpp"
#include <sl_consts.h>
#include <cstring>

namespace cvr::framegen {
// CP2077 2.31 EXE A7DE...60991, native emitter 0x78933C; constructor
// 0x1D4F700 at object+0x200. See audit/luke-eye-cycle-20261003/native-sl-78933c.
// Called inside NativeCamera's existing SEH read boundary. The caller runs
// BEFORE the native emitter, so its four dynamic SL matrices may still be old.
inline CameraConstants SnapshotNativeCameraState(const void* state,const CameraData& camera) {
    auto out=SnapshotNativeCamera(camera);
    if(!state)return out;
    const auto* bytes=static_cast<const unsigned char*>(state);
    const auto* header=reinterpret_cast<const sl::Constants*>(bytes+0x200);
    if(header->structType!=sl::Constants::s_structType || header->structVersion!=2)return out;
    out.version=2;
    auto copy=[&](unsigned target,unsigned offset,unsigned count) {
        std::memcpy(out.values.data()+target,bytes+offset,count*sizeof(float));
        for(unsigned i=target;i<target+count;++i)out.valuePresent.set(i);
    };
    copy(0,0xA0,16);copy(16,0xE0,16);
    copy(48,0x20,16);copy(64,0x60,16);
    copy(100,0x1D8,1); // Native field, retained raw; backend derives radians from projection.
    // These members persist in the actual SL object. Constructor defaults
    // include INVALID_FLOAT; preserve the sentinel, never mark an invented
    // lens identity or pinhole zero as measured native data.
    copy(32,0x2A0,16);copy(84,0x370,2);copy(102,0x3B8,1);copy(103,0x3C4,1);
    // The emitter explicitly writes these three flags false (0x789557/562).
    for(unsigned flag=4;flag<7;++flag) {out.flags[flag]=0;out.flagPresent.set(flag);}
    return out;
}
}
