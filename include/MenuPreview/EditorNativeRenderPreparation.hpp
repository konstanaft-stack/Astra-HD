#pragma once
#include "MenuPreview/EditorCameraTransport.hpp"
#include <utility>

namespace cvr::menu_preview {
inline bool EditorNativeRenderPreparationMatches(const CameraApplication& original,const Metadata& common,
    const NativePreviewSource& source,const Descriptor& incoming)noexcept {
    const PreviewFrameKey key{common.screenEpoch,common.setupID,common.componentIdentity,common.virtualCameraNameHash};
    return IsEditorNativePreparation(original)&&original.source==source&&
        SameEditorApplicationScope(original.metadata,common)&&original.frame.key==key&&
        EditorSetupFingerprint(original.eyeDescriptor)==EditorSetupFingerprint(incoming);
}
inline bool EditorNativeRenderFrameCurrent(const CameraApplication& app,uint32_t frame,uint64_t origin)noexcept {
    return app.projection.valid&&origin&&app.projection.origin==origin&&app.frame.nativeFrame==frame;
}
// Every actual native render constructs a fresh tracked application. The
// registered object/receipt remains original even if the engine copies it once.
template<class Compose>
bool BuildEditorNativeRenderApplication(const CameraApplication& original,const Metadata& common,
    const NativePreviewSource& source,const Descriptor& incoming,Compose&& compose,CameraApplication& output){
    if(!EditorNativeRenderPreparationMatches(original,common,source,incoming))return false;
    CameraApplication candidate;if(!compose(incoming,candidate))return false;
    output=std::move(candidate);return true;
}
}
