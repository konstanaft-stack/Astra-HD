#pragma once
#include <array>
#include <cstdint>
namespace cvr::framegen {
struct PairedImageEyeIdentity {
    uintptr_t color{};
    uint64_t serial{},pose{},write{},origin{};
    uint32_t view{},nativeFrame{};
    bool nativeFrameKnown{};
};
struct PairedImageIdentity {
    std::array<PairedImageEyeIdentity,2> eyes;
    uint64_t serial{},origin{},resourceEpoch{};
    bool producerSignalKnown{};
};
enum class PairedImageAdmission {Ready,MissingImage,SameImage,MissingIdentity,WrongView,WrongSerial,WrongOrigin,WrongFrame,MissingCompletion};
inline PairedImageAdmission ValidatePairedImage(const PairedImageIdentity& pair) noexcept {
    const auto& a=pair.eyes[0];const auto& b=pair.eyes[1];
    if(!a.color||!b.color)return PairedImageAdmission::MissingImage;
    if(a.color==b.color)return PairedImageAdmission::SameImage;
    if(!pair.serial||!pair.origin||!pair.resourceEpoch||!a.pose||!b.pose||!a.write||!b.write)return PairedImageAdmission::MissingIdentity;
    if(!((a.view==1&&b.view==2)||(a.view==2&&b.view==1)))return PairedImageAdmission::WrongView;
    if(a.serial!=pair.serial||b.serial!=pair.serial)return PairedImageAdmission::WrongSerial;
    if(a.origin!=pair.origin||b.origin!=pair.origin)return PairedImageAdmission::WrongOrigin;
    if(!a.nativeFrameKnown||!b.nativeFrameKnown||a.nativeFrame!=b.nativeFrame)return PairedImageAdmission::WrongFrame;
    if(!pair.producerSignalKnown)return PairedImageAdmission::MissingCompletion;
    return PairedImageAdmission::Ready;
}
}
