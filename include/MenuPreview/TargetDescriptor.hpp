#pragma once
#include <cstdint>
#include <limits>
namespace cvr::menu_preview {
struct TargetDescriptor {uintptr_t expectedHandle{},manager{};uint32_t textureID{};bool known{};};
// Diagnostic rejection bound, NOT a claim about the engine's allocation capacity.
inline constexpr uint32_t TargetDescriptorDiagnosticIDLimit=65536;
inline bool DefaultTargetSlot(uintptr_t manager,uint32_t id,uintptr_t& address)noexcept{
    address=0;
    if(manager<0x10000 || !id || id>TargetDescriptorDiagnosticIDLimit)return false;
    const uint64_t offset=0x2F1F0ull+uint64_t(id-1)*0xB0ull;
    if(offset>std::numeric_limits<uintptr_t>::max()-manager)return false;
    address=manager+uintptr_t(offset);return true;
}
// Called only while CopyToTexture owns the exact validated view/backing.
// Copies values; does not retain or invoke an engine object/accessor.
TargetDescriptor ResolvePreviewDefaultTarget(uintptr_t colorBacking)noexcept;
}
