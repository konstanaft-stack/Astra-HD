#pragma once
#include "Camera/PoseAddressLedger.hpp"
#include <bit>
#include <cmath>

namespace cvr::camera {
enum class AppliedEyeRoute : uint8_t {Unknown,Component,Scene};
enum class AppliedEyeSpace : uint8_t {Unknown,EngineWorldFixed17AndCameraQuaternion};

// Raw evidence from one successful camera write. It is deliberately not an XR
// pose or an SL camera position. The containing PoseIdentity owns source/write
// provenance; consumers must establish their own camera-local basis mapping.
struct AppliedEyeReference {
    std::array<int32_t,3> centreFixed{};
    PoseFingerprint writtenEye{};
    AppliedEyeSpace space{};
    AppliedEyeRoute route{};
    bool known{};
    bool operator==(const AppliedEyeReference&) const=default;
    explicit operator bool() const noexcept {
        if(!known || space!=AppliedEyeSpace::EngineWorldFixed17AndCameraQuaternion ||
           (route!=AppliedEyeRoute::Component && route!=AppliedEyeRoute::Scene))return false;
        double norm=0;
        for(const auto bits:writtenEye.rotation){
            const float q=std::bit_cast<float>(bits);
            if(!std::isfinite(q))return false;
            norm+=double(q)*double(q);
        }
        return norm>0 && std::isfinite(norm);
    }
    bool MatchesWrittenFingerprint(const PoseFingerprint& fingerprint) const noexcept {
        return bool(*this) && writtenEye==fingerprint;
    }
    std::array<int64_t,3> DeltaFixed() const noexcept {
        return {int64_t(writtenEye.position[0])-int64_t(centreFixed[0]),
                int64_t(writtenEye.position[1])-int64_t(centreFixed[1]),
                int64_t(writtenEye.position[2])-int64_t(centreFixed[2])};
    }
    std::array<double,3> DeltaMetres() const noexcept {
        const auto delta=DeltaFixed();
        return {double(delta[0])*0x1p-17,double(delta[1])*0x1p-17,double(delta[2])*0x1p-17};
    }
};

// No normalization, additional IPD, rotation or head/roomscale application.
// Publisher subsequently validates the supplied eye/quaternion against the
// actual object fingerprint and binds it to a usable MAIN source receipt.
inline AppliedEyeReference MakeAppliedEyeReference(const int32_t* centre,const int32_t* eye,
    const float* quaternion,AppliedEyeRoute route,bool positionWritten,bool orientationWritten) noexcept {
    if(!centre || !eye || !quaternion || !positionWritten || !orientationWritten)return {};
    AppliedEyeReference out;
    for(unsigned i=0;i<3;++i){out.centreFixed[i]=centre[i];out.writtenEye.position[i]=eye[i];}
    for(unsigned i=0;i<4;++i)out.writtenEye.rotation[i]=std::bit_cast<uint32_t>(quaternion[i]);
    out.space=AppliedEyeSpace::EngineWorldFixed17AndCameraQuaternion;out.route=route;out.known=true;
    return out ? out:AppliedEyeReference{};
}
}
