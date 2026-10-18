#pragma once
#include "MenuPreview/Producer.hpp"
#include <span>

namespace cvr::menu_preview {
inline constexpr size_t RegisteredEditorCameraMinimumBytes=0x48;

namespace registered_editor_detail {
inline float ReadFloat(const uint8_t* source)noexcept {
    float value;std::memcpy(&value,source,sizeof(value));return value;
}
inline bool Valid(const Descriptor& descriptor,float nativeFar)noexcept {
    float quaternion[4]{};std::memcpy(quaternion,descriptor.data(),sizeof(quaternion));
    double norm=0;
    for(const float value:quaternion){if(!std::isfinite(value))return false;norm+=double(value)*value;}
    const float fov=ReadFloat(descriptor.data()+0x38),aspect=ReadFloat(descriptor.data()+0x3C);
    const float nearPlane=ReadFloat(descriptor.data()+0x40);
    // XYZ are native int32 fixed-point values, not floating-point bit patterns.
    // FOV is degrees, matching the existing editor projection policy.
    return norm>=.999&&norm<=1.001&&std::isfinite(fov)&&fov>0&&fov<179&&
        std::isfinite(aspect)&&aspect>0&&std::isfinite(nearPlane)&&nearPlane>0&&
        std::isfinite(nativeFar)&&nearPlane<nativeFar;
}
}

// Only for caller-owned local native bytes. No native constructor, derived
// matrix update, origin280/R8 or refs388 access occurs here. The registered copy
// owns those fields. Validate every mapped value before the first mutation.
inline bool OverlayRegisteredEditorCamera(std::span<uint8_t> nativeBytes,const Descriptor& desired)noexcept {
    if(nativeBytes.size()<RegisteredEditorCameraMinimumBytes)return false;
    const Descriptor staged=desired; // Also permits source/destination aliasing.
    if(!registered_editor_detail::Valid(staged,registered_editor_detail::ReadFloat(nativeBytes.data()+0x44)))return false;
    std::memcpy(nativeBytes.data(),staged.data()+0x10,12);
    std::memcpy(nativeBytes.data()+0x10,staged.data(),16);
    std::memcpy(nativeBytes.data()+0x20,staged.data()+0x38,4);
    std::memcpy(nativeBytes.data()+0x28,staged.data()+0x3C,4);
    std::memcpy(nativeBytes.data()+0x40,staged.data()+0x40,4);
    return true;
}

// Reconstruct only the proven pose/optics mapping, including read-only far44.
// Every other descriptor byte is inherited exactly from baseline. Invalid input
// leaves output untouched, including when output aliases baseline/nativeBytes.
inline bool ReadRegisteredEditorDescriptor(std::span<const uint8_t> nativeBytes,
    const Descriptor& baseline,Descriptor& output)noexcept {
    if(nativeBytes.size()<RegisteredEditorCameraMinimumBytes)return false;
    Descriptor result=baseline;
    std::memcpy(result.data()+0x10,nativeBytes.data(),12);
    std::memcpy(result.data(),nativeBytes.data()+0x10,16);
    std::memcpy(result.data()+0x38,nativeBytes.data()+0x20,4);
    std::memcpy(result.data()+0x3C,nativeBytes.data()+0x28,4);
    std::memcpy(result.data()+0x40,nativeBytes.data()+0x40,4);
    std::memcpy(result.data()+0x44,nativeBytes.data()+0x44,4);
    if(!registered_editor_detail::Valid(result,registered_editor_detail::ReadFloat(result.data()+0x44)))return false;
    output=result;return true;
}
}
