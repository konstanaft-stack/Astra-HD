/*
* Copyright (c) 2022-2023 NVIDIA CORPORATION. All rights reserved
*
* Permission is hereby granted, free of charge, to any person obtaining a copy
* of this software and associated documentation files (the "Software"), to deal
* in the Software without restriction, including without limitation the rights
* to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
* copies of the Software, and to permit persons to whom the Software is
* furnished to do so, subject to the following conditions:
*
* The above copyright notice and this permission notice shall be included in all
* copies or substantial portions of the Software.
*
* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
* IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
* FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
* AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
* LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
* OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
* SOFTWARE.
*/ 


#pragma once
// ABI declarations extracted unchanged from pinned Streamline v2.14.1 sl_reflex.h.
// Only includes/import helpers omitted; original header SHA256: 3B623A1189E04A686384D224A58C4AD9974C4E6E3204077676F6EC529475164C
#include <sl_core_types.h>
#include <sl_consts.h>
namespace sl {
enum ReflexMode
{
    eOff,
    eLowLatency,
    eLowLatencyWithBoost,

    // ReflexMode is a C-enum (rather than enum class) so we can't add an eCount value 
    // without polluting the global namespace (and conflicts with SMSCGMode::eCount in sl.dlss_g/defines.h)
    ReflexMode_eCount
};

// {F03AF81A-6D0B-4902-A651-C4965E215434}
SL_STRUCT_BEGIN(ReflexOptions, StructType({ 0xf03af81a, 0x6d0b, 0x4902, { 0xa6, 0x51, 0xc4, 0x96, 0x5e, 0x21, 0x54, 0x34 } }), kStructVersion1)
    //! Specifies which mode should be used
    ReflexMode mode = ReflexMode::eOff;
    //! Specifies if frame limiting (FPS cap) is enabled (0 to disable, microseconds otherwise).
    //! One benefit of using Reflex's FPS cap over other implementations is the driver would be aware and can provide better optimizations.
    //! This setting is independent of ReflexOptions::mode; it can even be used with mode == ReflexMode::eOff.
    //! The value is used each time you call slReflexSetOptions/slSetData, make sure to initialize when changing one of the other Reflex options during frame limiting.
    //! It is overridden (ignored) by frameLimitUs if set in sl.reflex.json in non-production builds.
    uint32_t frameLimitUs = 0;
    //! This should only be enabled in specific scenarios with subtle caveats.
    //! Most integrations should leave unset unless advised otherwise by the Reflex team
    bool useMarkersToOptimize = false;
    //! Specifies the hot-key which should be used instead of custom message for PC latency marker
    //! Possible values: VK_F13, VK_F14, VK_F15
    uint16_t virtualKey = 0;
    //! ThreadID for PCL Stats messages
    //! Most integrations should leave unset unless advised otherwise by the Reflex team
    uint32_t idThread = 0;

    //! IMPORTANT: New members go here or if optional can be chained in a new struct, see sl_struct.h for details
SL_STRUCT_END()

// {0D569B37-A1C8-4453-BE4D-40F4DE57952B}
SL_STRUCT_BEGIN(ReflexReport, StructType({ 0xd569b37, 0xa1c8, 0x4453, { 0xbe, 0x4d, 0x40, 0xf4, 0xde, 0x57, 0x95, 0x2b } }), kStructVersion1)
    //! Various latency related stats
    uint64_t frameID{};
    uint64_t inputSampleTime{};
    uint64_t simStartTime{};
    uint64_t simEndTime{};
    uint64_t renderSubmitStartTime{};
    uint64_t renderSubmitEndTime{};
    uint64_t presentStartTime{};
    uint64_t presentEndTime{};
    uint64_t driverStartTime{};
    uint64_t driverEndTime{};
    uint64_t osRenderQueueStartTime{};
    uint64_t osRenderQueueEndTime{};
    uint64_t gpuRenderStartTime{};
    uint64_t gpuRenderEndTime{};
    uint32_t gpuActiveRenderTimeUs{};
    uint32_t gpuFrameTimeUs{};
    //! IMPORTANT: This struct cannot have new members because it is arrayed by ReflexState.
SL_STRUCT_END()

// {68bb0632-5e1c-402b-899d-b49f633c56c2}
SL_STRUCT_BEGIN(ReflexReport2, StructType({ 0x68bb0632, 0x5e1c, 0x402b, { 0x89, 0x9d, 0xb4, 0x9f, 0x63, 0x3c, 0x56, 0xc2 } }), kStructVersion1)
    //! Various latency related stats
    uint64_t cameraConstructedTime{};
    uint32_t crossAdapterCopyTimeUs{};
    //! IMPORTANT: This struct cannot have new members because it is arrayed by ReflexState.
SL_STRUCT_END()

constexpr int kReflexFrameReportCount = 64;

// {F0BB5985-DAF9-4728-B2FD-AE80A2BD7989}
SL_STRUCT_BEGIN(ReflexState, StructType({ 0xf0bb5985, 0xdaf9, 0x4728, { 0xb2, 0xfd, 0xae, 0x80, 0xa2, 0xbd, 0x79, 0x89 } }), kStructVersion2)
    //! Specifies if low-latency mode is available or not
    bool lowLatencyAvailable = false;
    //! Specifies if the frameReport below contains valid data or not
    bool latencyReportAvailable = false;
    //! Specifies low latency Windows message id (if ReflexOptions::virtualKey is 0)
    uint32_t statsWindowMessage;
    //! Reflex report per frame
    ReflexReport frameReport[kReflexFrameReportCount];
    //! Specifies ownership of flash indicator toggle (true = driver, false = application)
    bool flashIndicatorDriverControlled = false;
    // kStructVersion2
    //! Reflex report per frame
    ReflexReport2 frameReport2[kReflexFrameReportCount];

    //! IMPORTANT: New members go here or if optional can be chained in a new struct, see sl_struct.h for details
SL_STRUCT_END()


using PFun_slReflexSleep=Result(const FrameToken&);
using PFun_slReflexSetOptions=Result(const ReflexOptions&);
using PFun_slReflexGetState=Result(ReflexState&);
}
