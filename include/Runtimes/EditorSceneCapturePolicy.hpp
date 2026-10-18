#pragma once
#include "MenuPreview/EditorSceneImage.hpp"
#include "MenuPreview/UiConsumer.hpp"
#include "Render/AerReconstructionPass.hpp"
#include "Render/CommandResources.hpp"
#include "Framegen/ColorRead.hpp"
#include <cstring>
#include <utility>

namespace cvr::xr {
inline bool EditorSceneMatchesReceipt(const cvr::menu_preview::EditorSceneImage& scene,
    const cvr::menu_preview::UiFrameReceipt& receipt,uint64_t now){
    if(!scene.application||!scene.serial||!scene.queue||!scene.producerUiSubmission||
       scene.producerUiSubmission>receipt.uiSubmission)return false;
    const auto& app=*scene.application;const auto& a=app.metadata;const auto& b=receipt.scope;
    const cvr::menu_preview::PreviewFrameKey expected{a.screenEpoch,a.setupID,a.componentIdentity,a.virtualCameraNameHash};
    return a.screen==2&&b.screen==2&&a.screenEpoch&&a.screenEpoch==b.screenEpoch&&
        a.controllerIdentity&&a.controllerIdentity==b.controllerIdentity&&a.setupID==b.setupID&&
        a.componentIdentity&&a.componentIdentity==b.componentIdentity&&
        a.virtualCameraNameHash&&a.virtualCameraNameHash==b.virtualCameraNameHash&&
        cvr::menu_preview::FreshPublication(a.publishedAtMs,now)&&
        app.frame.key==expected&&app.frame.eye<2&&app.frame.eye==receipt.eye&&app.frame.sourceSequence&&
        app.frame.sourceSequence==receipt.sourceSequence&&app.frame.nativeFrame==receipt.nativeFrame&&
        app.editorControlRevision&&app.localPoseRevision&&app.localPoseRevision==receipt.cameraRevision&&
        app.projection.valid&&receipt.projection.valid&&app.projection.origin&&
        app.projection.origin==receipt.projection.origin&&
        !std::memcmp(&app.projection.pose,&receipt.projection.pose,sizeof(XrPosef))&&
        !std::memcmp(&app.projection.fov,&receipt.projection.fov,sizeof(XrFovf));
}
struct EditorSceneCaptureSource {
    ID3D12Resource* texture{};
    std::shared_ptr<const cvr::menu_preview::EditorSceneImage> scene;
    cvr::menu_preview::PreviewProjection projection{};
};
inline EditorSceneCaptureSource SelectEditorSceneCapture(bool required,ID3D12Resource* backbuffer,
    const cvr::menu_preview::UiFrameReceipt& receipt,
    std::shared_ptr<const cvr::menu_preview::EditorSceneImage> scene,uint64_t now){
    if(!required)return {backbuffer,{},receipt.projection};
    if(!scene||!EditorSceneMatchesReceipt(*scene,receipt,now)||!scene->pixels.texture||!scene->pixels.lifetime||
       !scene->producerFence||!scene->producerValue||scene->producerValue==UINT64_MAX||
       scene->restingState!=D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE||
       scene->producerFence->GetCompletedValue()==UINT64_MAX)return {};
    const auto desc=scene->pixels.texture->GetDesc();
    if(desc.Dimension!=D3D12_RESOURCE_DIMENSION_TEXTURE2D||!desc.Width||!desc.Height||
       desc.Width>UINT32_MAX||desc.MipLevels!=1||desc.DepthOrArraySize!=1||desc.SampleDesc.Count!=1||
       desc.SampleDesc.Quality||(desc.Flags&D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE))return {};
    return {scene->pixels.texture.Get(),scene,scene->application->projection};
}
inline cvr::render::AerColorRoute EditorSceneColorRoute(DXGI_FORMAT destination){
    using cvr::render::AerEncoding;
    return {AerEncoding::EncodedSrgb,
        destination==DXGI_FORMAT_R16G16B16A16_FLOAT||destination==DXGI_FORMAT_R32G32B32A32_FLOAT?
            AerEncoding::Linear:AerEncoding::EncodedSrgb};
}
// Existing colour pass only: full source, identity warp, zero filters. Source
// token and exact packet accompany the read; the pass never owns a naked RTT.
inline cvr::render::AerRecordTicket RecordEditorSceneCapture(cvr::render::AerReconstructionPass& pass,
    ID3D12GraphicsCommandList* list,const EditorSceneCaptureSource& source,
    ID3D12Resource* destination,ID3D12Fence* fence,uint64_t value){
    if(!source.scene||!source.texture||!destination)return {};
    if(!cvr::gpu::KeepCommandResources(list,{source.texture,destination,
        source.scene->pixels.lifetime.Get(),source.scene->producerFence.Get()}))return {};
    const auto sd=source.texture->GetDesc();
    cvr::render::AerRecordRequest request;request.source=source.texture;request.destination=destination;
    request.sourceOwner=std::shared_ptr<void>(source.scene,source.texture);
    request.completion={fence,value};request.eye=source.scene->application->frame.eye;
    request.sourceState=D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    request.warp.crop={0,0,float(sd.Width),float(sd.Height)};
    request.warp.sourceEpoch=request.warp.referenceEpoch=source.scene->application->metadata.screenEpoch;
    request.warp.sourceSpace=request.warp.referenceSpace=source.projection.origin;
    request.filter={0,0,0,0};request.color=EditorSceneColorRoute(destination->GetDesc().Format);
    D3D12_RESOURCE_BARRIER barriers[2]{};for(auto& b:barriers)b.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barriers[0].Transition={source.texture,D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
        source.scene->restingState,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE};
    barriers[1].Transition={destination,D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
        cvr::framegen::CaptureColorReadState,D3D12_RESOURCE_STATE_RENDER_TARGET};
    list->ResourceBarrier(2,barriers);const auto ticket=pass.Record(list,request);
    for(auto& b:barriers)std::swap(b.Transition.StateBefore,b.Transition.StateAfter);
    list->ResourceBarrier(2,barriers);return ticket;
}
}
