#include "Runtimes/WorldFrameReplay.hpp"
#include <cstdio>
#include <cstdlib>
using namespace cvr::xr;
unsigned checks{};
void Check(bool value,const char* text){++checks;if(!value){std::fprintf(stderr,"FAIL: %s\n",text);std::exit(1);}}
template<class T>T Handle(uintptr_t value){return reinterpret_cast<T>(value);}
int main(){
    WorldFrameReplay cache;
    WorldFrameKey key{Handle<XrSession>(1),Handle<XrSpace>(2),3,4,5,{Handle<XrSwapchain>(6),Handle<XrSwapchain>(7)}};
    XrCompositionLayerProjectionView views[2]{{XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW},{XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW}};
    for(unsigned eye=0;eye<2;++eye){views[eye].pose.orientation.w=1;views[eye].pose.position.x=eye?0.032f:-0.032f;
        views[eye].fov={-0.8f,0.8f,0.8f,-0.8f};views[eye].subImage.swapchain=key.eyes[eye];views[eye].subImage.imageRect.extent={1280,1400};}
    XrCompositionLayerProjection projection{XR_TYPE_COMPOSITION_LAYER_PROJECTION};projection.space=key.space;projection.viewCount=2;projection.views=views;
    XrCompositionLayerQuad quad{XR_TYPE_COMPOSITION_LAYER_QUAD};quad.space=key.space;quad.eyeVisibility=XR_EYE_VISIBILITY_BOTH;
    quad.pose.orientation.w=1;quad.pose.position.z=-1;quad.size={1,0.5f};quad.subImage.swapchain=Handle<XrSwapchain>(8);quad.subImage.imageRect.extent={200,100};
    const XrCompositionLayerBaseHeader* layers[]={reinterpret_cast<const XrCompositionLayerBaseHeader*>(&projection),reinterpret_cast<const XrCompositionLayerBaseHeader*>(&quad)};
    XrFrameEndInfo submitted{XR_TYPE_FRAME_END_INFO};submitted.displayTime=100;submitted.environmentBlendMode=XR_ENVIRONMENT_BLEND_MODE_OPAQUE;submitted.layerCount=2;submitted.layers=layers;
    Check(!cache.Prepare(key,101),"nothing may replay before an accepted frame");
    Check(cache.Capture(key,80,submitted),"capture accepted stereo frame");
    const auto* repeat=cache.Prepare(key,200);
    Check(repeat&&repeat->layerCount==2,"GPU-busy frame must retain the accepted stereo layers, not clear the display");
    Check(repeat->displayTime==200&&cache.Serial()==80,"repeat uses new display time but keeps source serial");
    auto* saved=reinterpret_cast<const XrCompositionLayerProjection*>(repeat->layers[0]);
    auto* savedQuad=reinterpret_cast<const XrCompositionLayerQuad*>(repeat->layers[1]);
    Check(saved->views!=views&&saved->views[0].pose.position.x==-0.032f&&saved->views[1].pose.position.x==0.032f,"own two distinct eye poses");
    views[0].pose.position.x=10;quad.pose.position.z=-10;
    Check(saved->views[0].pose.position.x==-0.032f&&savedQuad->pose.position.z==-1,"stack/overlay updates cannot rewrite cached frame");
    for(unsigned kind=0;kind<6;++kind){Check(cache.Capture(key,81,submitted),"refresh snapshot");auto changed=key;
        switch(kind){case 0:changed.world++;break;case 1:changed.origin++;break;case 2:changed.resources++;break;
        case 3:changed.session=Handle<XrSession>(10);break;case 4:changed.eyes[0]=Handle<XrSwapchain>(11);break;case 5:changed.space=Handle<XrSpace>(12);break;}
        Check(!cache.Prepare(changed,300),"world/origin/resize/session change rejects old layers");
        Check(!cache.Prepare(key,301),"rejected scope invalidates the old snapshot");}
    Check(cache.Capture(key,82,submitted),"refresh before acquire");cache.Invalidate();
    Check(!cache.Prepare(key,302),"acquiring new XR images invalidates the last-released-image snapshot");
    Check(cache.Capture(key,82,submitted),"capture before session stop/start");
    auto restarted=key;++restarted.sessionRun;
    Check(!cache.Prepare(restarted,303),"session stop/start with identical handles rejects previous-run images");
    auto menuKey=key;menuKey.menu=true;menuKey.world=0; // Pregame has no attached world.
    auto menuQuad=quad;menuQuad.subImage.swapchain=key.eyes[0];menuQuad.eyeVisibility=XR_EYE_VISIBILITY_BOTH;
    const XrCompositionLayerBaseHeader* menuLayers[]={reinterpret_cast<const XrCompositionLayerBaseHeader*>(&menuQuad)};
    auto menuFrame=submitted;menuFrame.layerCount=1;menuFrame.layers=menuLayers;
    Check(cache.Capture(menuKey,86,menuFrame),"accepted mono menu must survive allocator back-pressure without a projection layer");
    Check(cache.Prepare(menuKey,350)&&cache.Prepare(menuKey,351)->layerCount==1,"repeat the owned menu quad");
    Check(!cache.Prepare(key,352),"leaving menu must invalidate the old menu image");
    menuQuad.eyeVisibility=XR_EYE_VISIBILITY_LEFT;
    Check(!cache.Capture(menuKey,87,menuFrame),"one-eye-only menu cannot be cached");
    menuQuad.eyeVisibility=XR_EYE_VISIBILITY_BOTH;menuQuad.space=Handle<XrSpace>(99);
    Check(!cache.Capture(menuKey,87,menuFrame),"foreign primary menu space rejected");
    menuQuad.space=key.space;menuQuad.subImage.swapchain=Handle<XrSwapchain>(99);
    Check(!cache.Capture(menuKey,87,menuFrame),"unrelated HUD quad is not a complete menu image");
    menuQuad.subImage.swapchain=key.eyes[0];menuQuad.eyeVisibility=XR_EYE_VISIBILITY_LEFT;
    auto rightMenuQuad=menuQuad;rightMenuQuad.subImage.swapchain=key.eyes[1];rightMenuQuad.eyeVisibility=XR_EYE_VISIBILITY_RIGHT;
    const XrCompositionLayerBaseHeader* stereoMenuLayers[]={reinterpret_cast<const XrCompositionLayerBaseHeader*>(&menuQuad),reinterpret_cast<const XrCompositionLayerBaseHeader*>(&rightMenuQuad)};
    menuFrame.layerCount=2;menuFrame.layers=stereoMenuLayers;
    Check(cache.Capture(menuKey,88,menuFrame),"accepted separate left/right menu quads may be retained");
    rightMenuQuad.pose.position.x=99;
    const auto* stereoRepeat=cache.Prepare(menuKey,360);
    Check(stereoRepeat&&reinterpret_cast<const XrCompositionLayerQuad*>(stereoRepeat->layers[1])->pose.position.x!=99,"stereo menu owns both quad descriptions");
    for(unsigned kind=0;kind<6;++kind){auto bad=submitted;auto badProjection=projection;auto badQuad=quad;
        const XrCompositionLayerBaseHeader* badLayers[]={reinterpret_cast<const XrCompositionLayerBaseHeader*>(&badProjection),reinterpret_cast<const XrCompositionLayerBaseHeader*>(&badQuad)};bad.layers=badLayers;
        switch(kind){case 0:badProjection.viewCount=1;break;case 1:badProjection.next=&key;break;case 2:badQuad.next=&key;break;
        case 3:badQuad.type=XR_TYPE_UNKNOWN;break;case 4:bad.next=&key;break;case 5:bad.layers=nullptr;break;}
        Check(!cache.Capture(key,83,bad),"unsupported borrowed pointers/types must not be cached");}
    Check(!cache.Capture(key,0,submitted),"missing source serial rejected");
    Check(cache.Capture(key,84,submitted),"refresh before clock regression");
    Check(!cache.Prepare(key,99),"earlier display time cannot replay a future image");
    Check(cache.Capture(key,85,submitted),"new accepted frame replaces invalidated state");
    for(int time=100;time<500;++time)Check(cache.Prepare(key,time)!=nullptr,"repeated presentation does not acquire or render another image");
    std::printf("PASS: %u world-frame replay lifetime/scope/pointer/display-time checks; no GPU or runtime invoked\n",checks);
}
