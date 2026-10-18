#pragma once
#include "MenuPreview/UiConsumer.hpp"
#include <cstring>

namespace cvr::xr {
// A stale, unaccepted body cache must not monopolize the managed five-cell
// pool. Caller swaps this state under its lock, then releases BOTH the retired
// publisher and its local old-state snapshot outside that lock. Actual display
// readers keep their independent shared packet leases.
template<class State>
std::shared_ptr<State> EditorNativeUiStateWithoutSupersededPacket(const std::shared_ptr<State>& current,
    uint64_t leftRaw,uint64_t rightRaw){
    if(!current||!current->packet||(current->packet->rawSceneSerial[0]==leftRaw&&current->packet->rawSceneSerial[1]==rightRaw))return current;
    auto replacement=std::make_shared<State>();replacement->application=current->application;replacement->presentSerial=current->presentSerial;
    return replacement;
}
// UI provenance remains the application actually consumed by native UI, even
// after another camera application is published. No right-eye UI is invented.
inline bool EditorNativeUiReceiptProven(const cvr::menu_preview::UiFrameReceipt& receipt,
    const cvr::menu_preview::Metadata& common,uint64_t controls,uint64_t origin,uint64_t generation,uint64_t now){
    using namespace cvr::menu_preview;
    const auto& app=receipt.application;
    if(!app||!receipt.uiSubmission||!receipt.queue||!receipt.backBuffer||receipt.eye!=0||
       app->source.mode!=NativePreviewMode::NativeEditorDual||app->source.eye!=0||app->frame.eye!=0||
       !EditorNativeSourceRegistered(common,app->source)||app->source.bindingGeneration!=generation||
       !controls||app->editorControlRevision!=controls||!origin||app->projection.origin!=origin||
       !app->projection.valid||!receipt.projection.valid||receipt.projection.origin!=origin||
       !SameEditorApplicationScope(receipt.scope,common)||!SameEditorApplicationScope(app->metadata,common)||
       !FreshPublication(common.publishedAtMs,now)||!FreshPublication(receipt.scope.publishedAtMs,now)||
       !FreshPublication(app->metadata.publishedAtMs,now))return false;
    return app->metadata.publicationSerial==receipt.scope.publicationSerial&&app->metadata.publishedAtMs==receipt.scope.publishedAtMs&&
        app->frame.nativeFrame==receipt.nativeFrame&&app->frame.sourceSequence==receipt.sourceSequence&&
        app->localPoseRevision&&app->localPoseRevision==receipt.cameraRevision&&
        !std::memcmp(&app->projection.pose,&receipt.projection.pose,sizeof(XrPosef))&&
        !std::memcmp(&app->projection.fov,&receipt.projection.fov,sizeof(XrFovf));
}
}
