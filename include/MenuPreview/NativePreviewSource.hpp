#pragma once
#include <cstdint>

namespace cvr::menu_preview {
enum class NativePreviewMode : uint32_t { LegacyOneView, NativeEditorDual };
// Native source facts, separate from the editor's canonical public Metadata.
struct NativePreviewSource {
    uintptr_t component{};
    uint64_t cameraKey{};
    uint32_t eye{};
    NativePreviewMode mode=NativePreviewMode::LegacyOneView;
    uint64_t bindingGeneration{};
    bool operator==(const NativePreviewSource&)const=default;
};
}
