#pragma once
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace cvr::hooks {
// CP2077 2.31, pinned image A7DE8294...A60991. The runtime adapter must obtain
// this complete snapshot safely; no live engine pointer is retained here.
struct NativeSettingsSnapshot {
    std::array<uint8_t,0x16c> bytes{};
    uint8_t U8(size_t offset) const noexcept { return bytes[offset]; }
    uint32_t U32(size_t offset) const noexcept {
        uint32_t value;std::memcpy(&value,bytes.data()+offset,sizeof(value));return value;
    }
    void PutU32(size_t offset,uint32_t value) noexcept { std::memcpy(bytes.data()+offset,&value,sizeof(value)); }
};

struct NativeSettingsDecision {
    // Exact order: W, M, nine common predicates, J. Matches the reviewed native
    // entry table and is useful for auditing the reason, not just a Boolean.
    std::array<bool,12> changed{};
    bool validDevice{},willResize{};
};

inline bool NativeSettingsFloatChanged(uint32_t left,uint32_t right) noexcept {
    // Native UCOMISS+JNE regards unordered as unchanged. Test NaN using integer
    // bits first so preflight does not add an invalid FP exception for sNaN.
    if((left&0x7fffffffu)>0x7f800000u || (right&0x7fffffffu)>0x7f800000u)return false;
    return std::bit_cast<float>(left)!=std::bit_cast<float>(right);
}

inline NativeSettingsDecision InspectNativeSettings(const NativeSettingsSnapshot& s) noexcept {
    NativeSettingsDecision result;
    auto& p=result.changed;
    const auto integer=[&](size_t left,size_t right){return s.U32(left)!=s.U32(right);};
    const auto byte=[&](size_t left,size_t right){return s.U8(left)!=s.U8(right);};
    const auto floating=[&](size_t left,size_t right){return NativeSettingsFloatChanged(s.U32(left),s.U32(right));};
    p[0]=integer(0x18,0x84)||integer(0x1c,0x88)||byte(0x28,0x91); //21C584 W
    p[1]=byte(0x3f,0x93)||byte(0x3e,0x109);                    //21C520 M
    p[2]=integer(0xd0,0x154)||floating(0xd4,0x158);             //21C53C
    p[3]=floating(0xd8,0x114);                                  //21C508
    p[4]=integer(0xc4,0x11c)||floating(0xdc,0x124);              //21C4C0
    p[5]=integer(0xe0,0x130)||floating(0xe4,0x138);              //21C478
    p[6]=integer(0xe8,0x13c)||floating(0xec,0x144);              //1D56D90
    p[7]=integer(0xf0,0x148)||floating(0xf4,0x150);              //1D56DE0
    p[8]=byte(0xc2,0x118);                                     //21C3E4
    p[9]=byte(0xf8,0x15c)||integer(0x100,0x160)||
         integer(0x104,0x164)||integer(0xfc,0x168);             //21C438
    p[10]=byte(0xc0,0x111)||(s.U8(0x111)==1&&integer(0xc8,0x128)); //1D56E70
    p[11]=byte(0xc1,0x112)||(s.U8(0x112)==1&&integer(0xcc,0x12c)); //1D56E30 J
    result.validDevice=uint32_t(s.U32(0x38)-1u)<=0x7ffffffeu;
    if(!result.validDevice)return result;
    bool common=false;for(size_t i=2;i<=10;++i)common|=p[i];
    const bool base=p[0]||s.U8(0x10f)!=0||p[1]||common;
    result.willResize=s.U8(0x20)==3 ? base : base||byte(0x92,0x3c)||
        byte(0x90,0x20)||integer(0x8c,0x78)||p[11];
    return result;
}

struct NativeDimensionWrite {
    enum class Kind {None,Pending,Author} kind=Kind::None;
    struct Size {uint32_t width{},height{};} size;
};

inline bool RequiresNativeSettingsAdmission(const NativeSettingsSnapshot& s,NativeDimensionWrite write) noexcept {
    if(InspectNativeSettings(s).willResize)return true;
    if(write.kind==NativeDimensionWrite::Kind::None)return false;
    // A later callback changes fields after the native predicate has loaded EAX.
    // Merely projecting both new active/pending values can incorrectly predict
    // equality. Conservatively admit any real field mutation, but not repeated
    // identical writes. The runtime write callback must still revalidate this
    // plan/revision and refuse a newly introduced write on an unreserved pass.
    if(s.U32(0x84)!=write.size.width||s.U32(0x88)!=write.size.height)return true;
    return write.kind==NativeDimensionWrite::Kind::Author&&
        (s.U32(0x18)!=write.size.width||s.U32(0x1c)!=write.size.height);
}
}
