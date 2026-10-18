#include "MenuPreview/UiPairPolicy.hpp"
#include <cassert>
#include <iostream>
#include "Runtimes/CapturePoolPolicy.hpp"
#include "Runtimes/EditorUiLayerPolicy.hpp"
#include "Runtimes/WorldFrameReplay.hpp"
using namespace cvr::menu_preview;
int main(){UiPairPolicy pair;const UiPairScope scope{1,0,2,3,4,5};assert(!pair.Read(scope));
 UiPairCell left{scope,100,0,1,10,20,0,30},right{scope,200,0,2,11,21,1,31};assert(!pair.Publish(left)[0]);assert(!pair.Read(scope));assert(!pair.Publish(right)[1]);assert(pair.Read(scope));
 cvr::capture::BufferLeases leases;assert(leases.Pin(100));assert(leases.Pin(100)); // publisher + actual consumer hand-off
 leases.Unpin(100);assert(!leases.BeginWrite(100)); // old publisher dropped, reader still protects
 leases.Unpin(100);assert(leases.BeginWrite(100));leases.EndWrite(100);
 std::array<uintptr_t,4> pool{100,200,300,400};assert(leases.Pin(100));assert(leases.Pin(200));
 const auto slot=cvr::capture::ReserveMainWrite(pool,300,2,leases);assert(slot&&*slot==3);leases.EndWrite(400); // opposite-eye pubs + original mono fallback remain protected

 auto next=left;next.resource=300;next.sourceSequence=3;next.uiSubmission=12;auto old=pair.Publish(next);assert(old[0]==100&&pair.Read(scope));assert(leases.Readers(100)==1);
 assert(pair.HasSource(scope,0,1));assert(!pair.Publish(left)[0]);assert(pair.Read(scope)->at(0).resource==300);
 auto changed=right;changed.scope.epoch=2;changed.sourceSequence=4;old=pair.Publish(changed);assert(old[0]==300&&old[1]==200);assert(!pair.Read(scope)&&!pair.Read(changed.scope));
 changed.eye=0;changed.resource=400;changed.sourceSequence=5;pair.Publish(changed);assert(pair.Read(changed.scope));
 auto resize=changed;resize.scope.pool=6;resize.sourceSequence=6;old=pair.Publish(resize);assert(old[0]==400&&old[1]==200);assert(!pair.Read(changed.scope));
 auto wrongOwner=resize;wrongOwner.scope.owner=9;wrongOwner.eye=1;wrongOwner.resource=500;wrongOwner.sourceSequence=7;pair.Publish(wrongOwner);assert(!pair.Read(resize.scope));
 pair.Clear();assert(!pair.Read(wrongOwner.scope));UiPairCell invalid=left;invalid.uiSubmission=0;pair.Publish(invalid);assert(!pair.Cells()[0]);
 pair.Publish(left);right.resource=100;pair.Publish(right);assert(!pair.Read(scope)); // same resource cannot masquerade as two images
 struct Attachment {Metadata scope;uint32_t eye{},nativeFrame{};uint64_t sourceSequence{},cameraRevision{};};
 Metadata editor{};editor.screen=2;editor.screenEpoch=3;editor.controllerIdentity=100;editor.componentIdentity=200;editor.setupID=6;editor.virtualCameraNameHash=300;
 Attachment receipt{editor,1,44,50,60},ui=receipt;
 assert(cvr::xr::EditorUiAttachmentMatches(ui,receipt));
 ui.sourceSequence++;assert(!cvr::xr::EditorUiAttachmentMatches(ui,receipt));ui=receipt;
 ui.scope.controllerIdentity++;assert(!cvr::xr::EditorUiAttachmentMatches(ui,receipt));ui=receipt;
 ui.scope.setupID++;assert(!cvr::xr::EditorUiAttachmentMatches(ui,receipt));ui=receipt;
 ui.cameraRevision++;assert(!cvr::xr::EditorUiAttachmentMatches(ui,receipt));ui=receipt;
 ui.eye^=1;assert(!cvr::xr::EditorUiAttachmentMatches(ui,receipt));ui=receipt;
 ui.nativeFrame++;assert(!cvr::xr::EditorUiAttachmentMatches(ui,receipt));
 cvr::xr::EditorUiAnchor anchor;XrPosef initial{{0,0,0,1},{0,1.6f,0}};
 const auto fixed=anchor.Pose(editor,1,initial);assert(fixed&&fixed->position.z==-1&&fixed->position.y==1.6f);
 XrPosef turned{{0,.70710678f,0,.70710678f},{8,9,10}};
 editor.setupID++;editor.componentIdentity++;editor.virtualCameraNameHash++;
 const auto category=anchor.Pose(editor,1,turned);assert(category&&category->position.z==fixed->position.z&&category->position.x==fixed->position.x&&category->orientation.w==1);
 const auto recentered=anchor.Pose(editor,2,turned);assert(recentered&&recentered->position.x>6.9f&&recentered->position.x<7.1f);
 editor.controllerIdentity++;assert(anchor.Pose(editor,2,initial)->position.z==-1);
 anchor.Reset();assert(!anchor.Valid());assert(anchor.Pose(editor,2,turned)->position.x>6.9f);
 assert(!cvr::xr::EditorUiSize(0)&&!cvr::xr::EditorUiSize(NAN));assert(cvr::xr::EditorUiSize(.9f)->height==2.8f);
 assert(cvr::xr::EditorUiPairRequirement(false,false)==false&&cvr::xr::EditorUiPairRequirement(true,true)==true);
 assert(!cvr::xr::EditorUiPairRequirement(true,false));
 assert(cvr::xr::EditorUiRetainedAllowed(editor,2,2,true,nullptr)); // Explicit last-accepted display, not fresh scope.
 auto changedCategory=editor;changedCategory.componentIdentity++;changedCategory.setupID++;
 assert(cvr::xr::EditorUiRetainedAllowed(editor,2,2,true,&changedCategory));
 assert(!cvr::xr::EditorUiRetainedAllowed(editor,2,2,false,nullptr)); // Actual controller close.
 assert(!cvr::xr::EditorUiRetainedAllowed(editor,2,3,true,nullptr)); // Origin changed.
 changedCategory.controllerIdentity++;assert(!cvr::xr::EditorUiRetainedAllowed(editor,2,2,true,&changedCategory));
 cvr::capture::BufferLeases displayLeases;displayLeases.Pin(100);displayLeases.Pin(200);displayLeases.Pin(400);
 const std::array<uintptr_t,cvr::xr::EditorUiMinimumPool(true)> displayPool{100,200,300,400,500};
 assert(!cvr::capture::ReserveMainWrite(std::span(displayPool).first(4),300,3,displayLeases));
 const auto nextEye=cvr::capture::ReserveMainWrite(displayPool,300,3,displayLeases);assert(nextEye&&displayPool[*nextEye]==500);displayLeases.EndWrite(500);
 cvr::xr::WorldFrameReplay replay;
 cvr::xr::WorldFrameKey key{reinterpret_cast<XrSession>(1),reinterpret_cast<XrSpace>(2),0,1,1,
     {reinterpret_cast<XrSwapchain>(3),reinterpret_cast<XrSwapchain>(4)},1,true,reinterpret_cast<XrSwapchain>(5)};
 XrCompositionLayerProjectionView views[2]{{XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW},{XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW}};
 for(unsigned eye=0;eye<2;++eye)views[eye].subImage={key.eyes[eye],{{0,0},{32,32}},0};
 XrCompositionLayerProjection projection{XR_TYPE_COMPOSITION_LAYER_PROJECTION};projection.space=key.space;projection.viewCount=2;projection.views=views;
 XrCompositionLayerQuad panel{XR_TYPE_COMPOSITION_LAYER_QUAD};panel.space=key.space;panel.eyeVisibility=XR_EYE_VISIBILITY_BOTH;
 panel.pose={{0,0,0,1},{0,1.6f,-1}};panel.size={2,2.8f};panel.layerFlags=3;panel.subImage={key.editorUi,{{0,0},{32,32}},0};
 const XrCompositionLayerBaseHeader* layers[]{reinterpret_cast<const XrCompositionLayerBaseHeader*>(&projection),reinterpret_cast<const XrCompositionLayerBaseHeader*>(&panel)};
 XrFrameEndInfo end{XR_TYPE_FRAME_END_INFO};end.displayTime=100;end.layerCount=2;end.layers=layers;
 assert(replay.Capture(key,1,end)); // Already accepted scene projection + independent UI must survive allocator-busy ticks.
 panel.pose.position.x=99;const auto retained=replay.Prepare(key,101);assert(retained&&reinterpret_cast<const XrCompositionLayerQuad*>(retained->layers[1])->pose.position.x==0);
 panel.pose.position.x=0;panel.space=reinterpret_cast<XrSpace>(99);assert(!replay.Capture(key,2,end));panel.space=key.space;
 panel.subImage.swapchain=key.eyes[0];assert(!replay.Capture(key,2,end));panel.subImage.swapchain=key.editorUi;
 panel.eyeVisibility=XR_EYE_VISIBILITY_LEFT;assert(!replay.Capture(key,2,end));panel.eyeVisibility=XR_EYE_VISIBILITY_BOTH;
 panel.subImage.imageArrayIndex=1;assert(!replay.Capture(key,2,end));panel.subImage.imageArrayIndex=0;
 end.layerCount=1;assert(!replay.Capture(key,2,end));end.layerCount=2;
 assert(replay.Capture(key,3,end));auto changedOrigin=key;changedOrigin.origin++;assert(!replay.Prepare(changedOrigin,102));
 auto worldKey=key;worldKey.world=9;worldKey.menu=false;worldKey.editorUi=XR_NULL_HANDLE;
 end.layerCount=1;assert(replay.Capture(worldKey,4,end)); // Existing projection path unchanged.
 auto menuKey=key;menuKey.editorUi=XR_NULL_HANDLE;panel.subImage.swapchain=key.eyes[0];
 const XrCompositionLayerBaseHeader* mixed[]{reinterpret_cast<const XrCompositionLayerBaseHeader*>(&panel)};
 end.layers=mixed;assert(replay.Capture(menuKey,5,end)); // Existing whole-menu quad path unchanged.
 std::cout<<"PASS UI pair leases/attachment/anchor/retention and strict accepted editor projection+quad replay\n";
}
