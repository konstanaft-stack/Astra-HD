#pragma once
// Actual original-SL Constants from run tofu-sr-inputs-20261004-1628, PID18572.
// DLL677A; binary SHA40AED6DFFBC2A33AB279E17CD028704217BC795E67FCC5552054A627777B10F6.
// EyeProof, NGX evaluation sizes/scales and owner resource/history/pair/config keys
// were NOT captured here and deliberately stay unknown. Positive tests label
// every added proof/owner parameter as SYNTHETIC, never as measured evidence.
#include "Framegen/SrCameraAdapter.hpp"
#include <bit>
inline std::array<cvr::framegen::sr::CameraSample,3> MeasuredSr120_122() {
    using namespace cvr::framegen;using namespace cvr::framegen::sr;
    std::array<CameraSample,3> samples{};
    {auto& s=samples[0];
     // wire sequence7915, raw byte offset12092608
     const std::array<uint32_t,104> bits{
        0x3F382874u,0x00000000u,0x00000000u,0x00000000u,0x00000000u,0x3F382874u,0x00000000u,0x00000000u,
        0x00000000u,0x00000000u,0xB5A00000u,0x3F800000u,0x00000000u,0x00000000u,0x3CA3D717u,0x00000000u,
        0x3FB1EF25u,0x80000000u,0x80000000u,0x00000000u,0x80000000u,0x3FB1EF25u,0x00000000u,0x80000000u,
        0x80000000u,0x00000000u,0x80000000u,0x4247FFF1u,0x00000000u,0x80000000u,0x3F800000u,0x3879FFEDu,
        0x7F7FFFFFu,0x7F7FFFFFu,0x7F7FFFFFu,0x7F7FFFFFu,0x7F7FFFFFu,0x7F7FFFFFu,0x7F7FFFFFu,0x7F7FFFFFu,
        0x7F7FFFFFu,0x7F7FFFFFu,0x7F7FFFFFu,0x7F7FFFFFu,0x7F7FFFFFu,0x7F7FFFFFu,0x7F7FFFFFu,0x7F7FFFFFu,
        0x3F7FFFFEu,0x35372B94u,0xB000B3F9u,0x39CDECC1u,0xB539595Eu,0x3F800000u,0xAEA91348u,0x388742A0u,
        0xC00764C9u,0xBC333A09u,0x3F7FFFF6u,0x3F02C336u,0xB957C186u,0xB80C0CDDu,0xAB38A2F8u,0x3F800005u,
        0x3F800001u,0xB53AB079u,0x3000B3F9u,0xB9CDECC1u,0x3535D493u,0x3F800000u,0x2EA91378u,0xB88742C5u,
        0x40076316u,0x3C32EC5Cu,0x3F800006u,0xBF02F9B6u,0x3957C182u,0x380C0CB1u,0x2B547A13u,0x3F7FFFF6u,
        0x3CBD0000u,0x3E8B1A00u,0x3F800000u,0x3F800000u,0x7F7FFFFFu,0x7F7FFFFFu,0xC49B5FBDu,0xC479B7CEu,
        0x415F8468u,0x3B650238u,0xBD026E45u,0x3F7FDE5Du,0x3EDA012Cu,0x3F67875Eu,0x3CDFDA09u,0xBF67A1F8u,
        0x3ED9D802u,0x3C88F643u,0x3CA3D70Au,0x467A0000u,0x42D9147Au,0x3F800000u,0x7F7FFFFFu,0x42200000u,
     };
     for(size_t j=0;j<bits.size();++j)s.original.values[j]=std::bit_cast<float>(bits[j]);
     s.original.valuePresent.set();s.original.flagPresent.set();s.original.source=CameraConstantsSource::Streamline;s.original.version=2;
     s.original.flags={1,1,0,0,0,0,0};
     s.key.source={2,180,3,{1,1,1},120,AerV2Eye::Left,9,155806149499628LL,false};
     s.key.nativeFrame=5943;s.key.slToken=5942;s.key.viewport=0;s.key.engineView=1;
     s.key.nativeFrameKnown=s.key.slTokenKnown=s.key.viewportKnown=true;
     s.key.origin=1;s.key.captureClockGeneration=3;s.key.poseId=4611686018427388024ULL;s.key.writeId=238ULL;
     s.key.locatedPoseTimeNs=155806149499628LL;
    }
    {auto& s=samples[1];
     // wire sequence7922, raw byte offset12103416
     const std::array<uint32_t,104> bits{
        0x3F382874u,0x00000000u,0x00000000u,0x00000000u,0x00000000u,0x3F382874u,0x00000000u,0x00000000u,
        0x00000000u,0x00000000u,0xB5A00000u,0x3F800000u,0x00000000u,0x00000000u,0x3CA3D717u,0x00000000u,
        0x3FB1EF25u,0x80000000u,0x80000000u,0x00000000u,0x80000000u,0x3FB1EF25u,0x00000000u,0x80000000u,
        0x80000000u,0x00000000u,0x80000000u,0x4247FFF1u,0x00000000u,0x80000000u,0x3F800000u,0x3879FFEDu,
        0x7F7FFFFFu,0x7F7FFFFFu,0x7F7FFFFFu,0x7F7FFFFFu,0x7F7FFFFFu,0x7F7FFFFFu,0x7F7FFFFFu,0x7F7FFFFFu,
        0x7F7FFFFFu,0x7F7FFFFFu,0x7F7FFFFFu,0x7F7FFFFFu,0x7F7FFFFFu,0x7F7FFFFFu,0x7F7FFFFFu,0x7F7FFFFFu,
        0x3F800000u,0xB76DF34Eu,0xB0005707u,0x39CD580Bu,0x376DBD5Au,0x3F800000u,0xAE8520E7u,0x38550171u,
        0x401F9F23u,0xBC407238u,0x3F7FFFF6u,0x3F02FF2Eu,0xB95170C8u,0xB7DC8AADu,0xAB442EF8u,0x3F800005u,
        0x3F7FFFFEu,0x376DC718u,0x30005715u,0xB9CD5822u,0xB76DE98Fu,0x3F800000u,0x2E851D2Du,0xB854FB7Au,
        0xC01FA0D6u,0x3C3FA58Eu,0x3F800005u,0xBF02BF32u,0x395170A4u,0x37DC90B7u,0x2B5EE18Cu,0x3F7FFFF4u,
        0xBE636000u,0xBE98A800u,0x3F800000u,0x3F800000u,0x7F7FFFFFu,0x7F7FFFFFu,0xC49B5F17u,0xC479B383u,
        0x415F8B88u,0x3B633CE8u,0xBD025088u,0x3F7FDE6Eu,0x3ED9DFBEu,0x3F678F40u,0x3CDFC630u,0xBF67A9D6u,
        0x3ED9B6C2u,0x3C88998Cu,0x3CA3D70Au,0x467A0000u,0x42D9147Au,0x3F800000u,0x7F7FFFFFu,0x42200000u,
     };
     for(size_t j=0;j<bits.size();++j)s.original.values[j]=std::bit_cast<float>(bits[j]);
     s.original.valuePresent.set();s.original.flagPresent.set();s.original.source=CameraConstantsSource::Streamline;s.original.version=2;
     s.original.flags={1,1,0,0,0,0,0};
     s.key.source={2,181,3,{1,1,1},121,AerV2Eye::Right,9,155806164851306LL,false};
     s.key.nativeFrame=5944;s.key.slToken=5943;s.key.viewport=0;s.key.engineView=1;
     s.key.nativeFrameKnown=s.key.slTokenKnown=s.key.viewportKnown=true;
     s.key.origin=1;s.key.captureClockGeneration=3;s.key.poseId=4611686018427388025ULL;s.key.writeId=239ULL;
     s.key.locatedPoseTimeNs=155806164851306LL;
    }
    {auto& s=samples[2];
     // wire sequence7929, raw byte offset12114224
     const std::array<uint32_t,104> bits{
        0x3F382874u,0x00000000u,0x00000000u,0x00000000u,0x00000000u,0x3F382874u,0x00000000u,0x00000000u,
        0x00000000u,0x00000000u,0xB5A00000u,0x3F800000u,0x00000000u,0x00000000u,0x3CA3D717u,0x00000000u,
        0x3FB1EF25u,0x80000000u,0x80000000u,0x00000000u,0x80000000u,0x3FB1EF25u,0x00000000u,0x80000000u,
        0x80000000u,0x00000000u,0x80000000u,0x4247FFF1u,0x00000000u,0x80000000u,0x3F800000u,0x3879FFEDu,
        0x7F7FFFFFu,0x7F7FFFFFu,0x7F7FFFFFu,0x7F7FFFFFu,0x7F7FFFFFu,0x7F7FFFFFu,0x7F7FFFFFu,0x7F7FFFFFu,
        0x7F7FFFFFu,0x7F7FFFFFu,0x7F7FFFFFu,0x7F7FFFFFu,0x7F7FFFFFu,0x7F7FFFFFu,0x7F7FFFFFu,0x7F7FFFFFu,
        0x3F7FFFFEu,0xB836D839u,0xB038D615u,0x3A13DE77u,0x3836C697u,0x3F7FFFFFu,0xAE8863CFu,0x385A394Cu,
        0xC0013625u,0xBC845C71u,0x3F7FFFF1u,0x3F403405u,0xB99A4A29u,0xB7E1E89Eu,0xAB77FD02u,0x3F800006u,
        0x3F800000u,0x3836C7EDu,0x3038D648u,0xBA13DE9Fu,0xB836D6E6u,0x3F800001u,0x2E885350u,0xB85A1EE8u,
        0x4001328Au,0x3C84EA90u,0x3F800007u,0xBF407EB7u,0x399A49F9u,0x37E20420u,0x2B981444u,0x3F7FFFF2u,
        0xBEEF3000u,0x3E072C00u,0x3F800000u,0x3F800000u,0x7F7FFFFFu,0x7F7FFFFFu,0xC49B604Au,0xC479B65Au,
        0x415F8470u,0x3B623BCCu,0xBD021683u,0x3F7FDE8Du,0x3ED9AF98u,0x3F679AA5u,0x3CDF7900u,0xBF67B529u,
        0x3ED986CEu,0x3C8833E3u,0x3CA3D70Au,0x467A0000u,0x42D9147Au,0x3F800000u,0x7F7FFFFFu,0x42200000u,
     };
     for(size_t j=0;j<bits.size();++j)s.original.values[j]=std::bit_cast<float>(bits[j]);
     s.original.valuePresent.set();s.original.flagPresent.set();s.original.source=CameraConstantsSource::Streamline;s.original.version=2;
     s.original.flags={1,1,0,0,0,0,0};
     s.key.source={2,182,3,{1,1,1},122,AerV2Eye::Left,9,155806180202984LL,false};
     s.key.nativeFrame=5945;s.key.slToken=5944;s.key.viewport=0;s.key.engineView=1;
     s.key.nativeFrameKnown=s.key.slTokenKnown=s.key.viewportKnown=true;
     s.key.origin=1;s.key.captureClockGeneration=3;s.key.poseId=4611686018427388026ULL;s.key.writeId=240ULL;
     s.key.locatedPoseTimeNs=155806180202984LL;
    }
    return samples;
}

// TEST-ONLY synthetic writer quaternion reconstructed from the fixed, code-
// proved [right,forward,up] convention. These are NOT captured EyeProof values.
inline constexpr std::array<std::array<uint32_t,4>,3> SyntheticWriterQuaternionBits{{
    {0x3C6B9E80u,0xBBE740B7u,0x3F092891u,0x3F581E0Fu},
    {0x3C6B49B9u,0xBBE7719Au,0x3F09305Cu,0x3F581922u},
    {0x3C6AD07Du,0xBBE743D5u,0x3F093B98u,0x3F581209u},
}};
inline constexpr std::array<double,16> ActualT0Shader{
    0.99999988079071045,4.3577132601058111e-05,0.00040574194547780769,-0.05613077534706544,
    -4.3593554437393323e-05,0.99999994039535522,3.7409472429517212e-05,-0.00044921087513175083,
    -0.0004057774375264972,-3.7427680437043115e-05,0.99999982023983591,-0.015015892945469012,
    -4.9916072187757423e-16,1.2479018013490221e-16,1.475407164678107e-12,1.00000000094623,
};
inline constexpr std::array<double,16> ActualT2Shader{
    1,1.4170387657941319e-05,0.00028179105332968321,0.069341239650539135,
    -1.4182949598762207e-05,1,3.6527402059127945e-05,-0.00032656464620127565,
    -0.00028174913923217221,-3.6532706734382532e-05,0.99999998604508278,-0.010234136822394158,
    -2.495803621056498e-16,-9.3592635245313495e-17,1.5382608333771299e-13,1.0000000139549174,
};
