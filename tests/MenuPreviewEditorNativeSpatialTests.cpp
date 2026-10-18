#include <cstdio>
#include <cstdlib>
#include <cstring>
#if __has_include("MenuPreview/EditorNativeSpatialFrame.hpp")
#include "MenuPreview/EditorNativeSpatialFrame.hpp"
#include "MenuPreview/EditorControllerPolicy.hpp"
#include "../src/MenuPreview/EditorControllerPolicy.cpp"
using namespace cvr::menu_preview;
namespace {
void require(bool v,const char* m){if(!v){std::fprintf(stderr,"FAIL: %s\n",m);std::exit(1);}}
Metadata scope(){Metadata m{};m.screen=2;m.screenEpoch=1;m.setupID=6;m.controllerIdentity=3;m.componentIdentity=4;m.virtualCameraNameHash=5;m.entityIdentity=6;m.entityID=7;return m;}
NativePreviewSource binding(uint32_t eye){return {4+eye,5+eye,eye,NativePreviewMode::NativeEditorDual,11};}
XrPosef pose(float x=0){return {{0,0,0,1},{x,0,0}};}
struct Common:EditorNativeSpatialCommon {uint64_t marker{};};
bool compose(const Common& c,const EditorNativeFrameSample& sample,const Metadata& m,const NativePreviewSource& source,const Descriptor& native,CameraApplication& out){return BuildEditorNativeSpatialEye(c,sample,m,source,native,out);}
void frozenPair(){
    EditorNativeSpatialFrameCache<Common> cache;auto m=scope();Descriptor native{};Common input;
    input.camera.position={100*131072,200*131072,3*131072};input.camera.rotation={0,0,0,1};input.anchor=pose();input.recenter=pose();input.scale=1;input.halfIpd=.0315f;input.horizontal=94;input.width=1280;input.height=1400;input.imageRoute=EditorImageRoute::FullRawImage;input.controlRevision=9;input.marker=10;
    unsigned preparations=0;const auto prepare=[&](const EditorNativeFrameSample& s,Common& out){++preparations;out=input;out.head=s.headPose;out.origin=s.origin;return true;};
    CameraApplication right,left,repeat;bool first=false;
    require(cache.Resolve(m,binding(1),100,8,pose(1),native,prepare,compose,right,&first)&&first,"right-first frame prepares one complete common state");
    input.camera.position[0]+=10000;input.scale=2;input.halfIpd=.05f;input.horizontal=60;input.width=1920;input.height=1920;input.controlRevision=20;input.marker=99;input.anchor=pose(8);input.recenter=pose(7);
    std::memset(native.data(),0x22,native.size());
    require(cache.Resolve(m,binding(0),100,8,pose(2),native,prepare,compose,left,&first)&&!first,"other actual native eye uses frozen common state");
    require(preparations==1&&cache.CommonState()->marker==10,"whole camera/controls/scale/IPD/recenter/FOV/extents snapshot is prepared only once");
    require(right.editorControlRevision==9&&left.editorControlRevision==9&&right.projection.origin==left.projection.origin,"both eyes retain the common controls and origin");
    require(right.projection.fov.angleRight==left.projection.fov.angleRight&&right.projection.fov.angleUp==left.projection.fov.angleUp,"both eyes retain the same frozen optics despite live settings changes");
    require(cache.Resolve(m,binding(1),100,8,pose(3),native,prepare,compose,repeat)&&SameCameraApplication(right,repeat)&&!repeat.frame.changed,"same-eye repeat retains exact descriptor/projection/camera facts");
    require(!cache.Resolve(m,binding(1),100,9,pose(),native,prepare,compose,repeat),"mid-frame recenter/origin cannot enter cached state");
    require(cache.Resolve(m,binding(1),101,9,pose(4),native,prepare,compose,repeat,&first)&&first&&preparations==2&&cache.CommonState()->marker==99,"next native frame refreshes all common inputs");
    std::puts("PASS whole common-frame snapshot, right-first and immutable repeat application");
}
void failedPreparation(){
    EditorNativeSpatialFrameCache<Common> cache;const auto m=scope();Descriptor native{};bool succeed=false;unsigned prepared=0;
    const auto prepare=[&](const EditorNativeFrameSample& s,Common& out){++prepared;out.camera.position={100,200,300};out.camera.rotation={0,0,0,1};out.anchor=out.recenter=pose();out.head=s.headPose;out.origin=s.origin;out.scale=1;out.halfIpd=.03f;out.horizontal=94;out.width=1280;out.height=1400;return succeed;};
    CameraApplication app;
    require(!cache.Resolve(m,binding(0),1,1,pose(),native,prepare,compose,app)&&!cache.CommonState(),"failed preparation never commits a common state");
    succeed=true;require(!cache.Resolve(m,binding(1),1,1,pose(),native,prepare,compose,app)&&prepared==1,"second eye cannot revive a failed partial frame");
    require(cache.Resolve(m,binding(0),2,1,pose(),native,prepare,compose,app)&&prepared==2,"next frame can prepare after failure");
    succeed=false;require(!cache.Resolve(m,binding(0),3,1,pose(),native,prepare,compose,app)&&!cache.CommonState(),"new failed frame removes previous valid common state");
    require(!cache.Resolve(m,binding(1),2,1,pose(),native,prepare,compose,app),"old frame cannot recover the prior state after failure");
    succeed=true;const auto rejectCompose=[](const auto&,const auto&,const auto&,const auto&,const auto&,auto&){return false;};
    require(!cache.Resolve(m,binding(0),4,1,pose(),native,prepare,rejectCompose,app)&&!cache.CommonState(),"first eye math failure also prevents common commit");
    require(!cache.Resolve(m,binding(1),4,1,pose(),native,prepare,compose,app),"other eye cannot use a state whose first eye math failed");
    cache.Clear();require(cache.Resolve(m,binding(1),1,1,pose(),native,prepare,compose,app),"Clear permits new native clock baseline without a stale common state");
    std::puts("PASS failed common preparation/first-eye math fail closed and clear stale state");
}
void rightFirstControls(){
    EditorControlLatch controls;uint64_t counter=0;const auto first=controls.Resolve(1,0,1,2,0,counter,.2f);
    require(first.revision==1&&first.height==1&&first.distance==2&&first.orbit==.2f,"a right-first native frame starts controls with common eye-zero semantics");
    const auto held=controls.Resolve(3,0,3,4,0,counter,.4f);require(held.revision==first.revision&&held.height==1,"unpresented controls remain latched across frames");
    const auto changed=controls.Resolve(5,0,3,4,first.revision,counter,.4f);require(changed.revision==2&&changed.height==3&&changed.distance==4&&changed.orbit==.4f,"presented complete pair unlocks next common control revision regardless of first actual eye");
    std::puts("PASS existing control-presentation latch with common-frame semantics");
}
}
int main(){frozenPair();failedPreparation();rightFirstControls();std::puts("PASS editor native spatial common-state contract; CPU evidence only");}
#else
#include "MenuPreview/SpatialEditorPolicy.hpp"
int main(){using namespace cvr::menu_preview;cvr::camera::ExternalViewPose base;base.position={100*131072,200*131072,3*131072};const XrPosef a{{0,0,0,1},{0,0,0}},b{{0,0,0,1},{.1f,0,0}};EditorEye first,repeat;
    if(!BuildEditorEye(base,a,a,a,1,.0315f,0,94,1280,1400,first)||!BuildEditorEye(base,a,b,a,1,.0315f,0,94,1280,1400,repeat))return 2;
    if(first.camera.position!=repeat.camera.position){std::fprintf(stderr,"FAIL: old per-callback editor math changes one eye's camera when the head sample changes within the same intended native frame\n");return 1;}return 0;
}
#endif
