#pragma once
#include "MenuPreview/CameraApplication.hpp"
#include <algorithm>
#include <optional>
#include <span>

namespace cvr::menu_preview {
// Scalar evidence sampled from real immutable publications. A CPU test may
// exercise admission with literals; only the D3D adapter supplies GPU facts.
struct EditorNativeSceneProof {
    std::shared_ptr<const CameraApplication> application;
    uintptr_t texture{},lifetime{},fence{},queue{};
    uint64_t producerValue{},completedValue{},serial{},rawExecutionSerial{},publishedAtMs{};
};
inline bool EditorNativeSceneAdmitted(const EditorNativeSceneProof& image,const Metadata& common,uint64_t now){
    if(!image.application||!image.texture||!image.lifetime||!image.fence||!image.queue||!image.serial||!image.rawExecutionSerial||
       !image.producerValue||image.producerValue==UINT64_MAX||image.completedValue==UINT64_MAX||image.completedValue<image.producerValue||
       !FreshPublication(image.publishedAtMs,now)||!FreshPublication(common.publishedAtMs,now))return false;
    const auto& app=*image.application;const auto& m=app.metadata;
    const PreviewFrameKey key{common.screenEpoch,common.setupID,common.componentIdentity,common.virtualCameraNameHash};
    return SameEditorApplicationScope(m,common)&&FreshPublication(m.publishedAtMs,now)&&
        EditorNativeSourceRegistered(common,app.source)&&app.source.eye==app.frame.eye&&app.frame.key==key&&
        app.frame.sourceSequence&&app.localPoseRevision&&app.editorControlRevision&&app.nativeContext&&
        app.projection.valid&&app.projection.origin;
}
inline bool EditorNativeScenesCoherent(const EditorNativeSceneProof& left,const EditorNativeSceneProof& right,
    const Metadata& common,uint64_t now){
    if(!EditorNativeSceneAdmitted(left,common,now)||!EditorNativeSceneAdmitted(right,common,now))return false;
    const auto& a=*left.application;const auto& b=*right.application;
    return a.frame.eye==0&&b.frame.eye==1&&a.frame.key==b.frame.key&&a.frame.nativeFrame==b.frame.nativeFrame&&
        a.editorControlRevision==b.editorControlRevision&&a.projection.origin==b.projection.origin&&
        a.source.bindingGeneration==b.source.bindingGeneration&&a.source.component!=b.source.component&&
        a.source.cameraKey!=b.source.cameraKey&&a.nativeContext!=b.nativeContext&&left.texture!=right.texture;
}
inline std::optional<std::array<size_t,2>> SelectEditorNativeScenePair(std::span<const EditorNativeSceneProof> images,
    const Metadata& common,uint64_t now){
    std::optional<std::array<size_t,2>> result;uint32_t newestFrame{};uint64_t newestSerial{};
    for(size_t l=0;l<images.size();++l)for(size_t r=0;r<images.size();++r){
        if(!EditorNativeScenesCoherent(images[l],images[r],common,now))continue;
        const auto frame=images[l].application->frame.nativeFrame;const auto serial=std::max(images[l].serial,images[r].serial);
        if(!result||int32_t(frame-newestFrame)>0||(frame==newestFrame&&serial>newestSerial)){
            result=std::array<size_t,2>{l,r};newestFrame=frame;newestSerial=serial;
        }
    }
    return result;
}
}
