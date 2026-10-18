#include "MenuPreview/EditorCameraLifetime.hpp"
#include "MenuPreview/EditorNativeSpatialFrame.hpp"
#include "MenuPreview/EditorControllerPolicy.hpp"
#include "../src/MenuPreview/EditorControllerPolicy.cpp"
#include <cstdio>
#include <cstdlib>
using namespace cvr::menu_preview;
namespace {
void require(bool v,const char* message){if(!v){std::fprintf(stderr,"FAIL: %s\n",message);std::exit(1);}}
Metadata scope(){Metadata m{};m.screen=2;m.screenEpoch=1;m.setupID=6;m.controllerIdentity=3;m.componentIdentity=4;m.virtualCameraNameHash=5;m.entityIdentity=6;m.entityID=7;return m;}
XrPosef head(float x=0){return {{0,0,0,1},{x,0,0}};}
bool same(const cvr::camera::ExternalViewPose& a,const cvr::camera::ExternalViewPose& b){return a.position==b.position&&std::memcmp(&a.rotation,&b.rotation,sizeof(a.rotation))==0;}
InitialEditorFraming framing(const Metadata& m){InitialEditorFraming f;f.epoch=m.screenEpoch;f.controller=m.controllerIdentity;f.owner=m.entityIdentity;f.actor=10;f.actorID=11;f.latched=true;f.centre={-207,808,-194};return f;}
cvr::camera::ExternalViewPose typed(float distance=0){cvr::camera::ExternalViewPose p;p.position={int32_t(-207.2*131072),int32_t((807.6+distance)*131072),int32_t(-194.2*131072)};return p;}
auto prepare(const Metadata& m,const cvr::camera::ExternalViewPose& input,unsigned& calls){return [&,m,input](auto& mapped,auto& f){++calls;if(!MapEditorCamera(input,mapped))return false;f=framing(m);std::array<double,3> p{double(mapped.position[0])/131072.,double(mapped.position[1])/131072.,double(mapped.position[2])/131072.};std::array<double,4> q{mapped.rotation.x,mapped.rotation.y,mapped.rotation.z,mapped.rotation.w};if(!f.Apply(m.screenEpoch,m.controllerIdentity,m.entityIdentity,p,q))return false;for(unsigned i=0;i<3;++i)mapped.position[i]=int32_t(std::round(p[i]*131072.));mapped.rotation={float(q[0]),float(q[1]),float(q[2]),float(q[3])};return true;};}
void categoryDoesNotMoveCameraOrHeadAnchor(){
    EditorCameraLifetime life;auto m=scope();unsigned calls=0;
    require(life.Resolve(m,1,head(),prepare(m,typed(),calls)),"first settled mapped/framed typed camera is accepted");
    const auto base=life.Camera();const auto initialFraming=life.Framing();
    m.setupID=9;m.componentIdentity=44;m.virtualCameraNameHash=55;m.cameraRefHash=66;m.slotNameHash=77;m.publicationSerial=88;m.publishedAtMs=99;
    require(life.Resolve(m,1,head(.25f),prepare(m,typed(4),calls)),"new category/binding may prepare a fresh strict render frame");
    require(same(life.Camera(),base)&&calls==1,"category selection must not remap/reframe the lifetime base camera");
    require(life.Anchor().position.x==0&&life.Framing().delta==initialFraming.delta&&life.Framing().centre==initialFraming.centre,"category selection must not re-anchor head or puppet framing");
    EditorEye before,after;
    require(BuildEditorEye(base,head(),head(.25f),head(),1,.0315f,0,94,1280,1400,before,EditorImageRoute::FullRawImage)&&BuildEditorEye(life.Camera(),life.Anchor(),head(.25f),head(),1,.0315f,0,94,1280,1400,after,EditorImageRoute::FullRawImage),"real current head/IPD math remains valid after category");
    require(same(before.camera,after.camera),"category must preserve the actual current-head camera result");
    EditorEye moved;require(BuildEditorEye(life.Camera(),life.Anchor(),head(.5f),head(),1,.0315f,0,94,1280,1400,moved,EditorImageRoute::FullRawImage)&&!same(after.camera,moved.camera),"head tracking remains fresh after the base is frozen");
    life.Invalidate(true);require(life.Resolve(m,1,head(.5f),prepare(m,typed(5),calls))&&same(life.Camera(),base)&&calls==1,"open-epoch category gap preserves the base and head anchor");
    require(life.Resolve(m,2,head(.75f),prepare(m,typed(6),calls))&&same(life.Camera(),base)&&calls==1&&life.Anchor().position.x==.75f,"origin/recenter resets only head reference, not base camera");
    std::puts("PASS category/setup/binding/gap retain framed base and anchor; current head updates; origin resets anchor only");
}
void manualControlsAndTrueLifetimeReset(){
    EditorCameraLifetime life;auto m=scope();unsigned calls=0;require(life.Resolve(m,1,head(),prepare(m,typed(),calls)),"manual fixture initialized");const auto base=life.Camera();
    EditorControllerPolicy controller;EditorControllerSample sample{};sample.available=true;sample.origin=1;sample.timeNs=1000000000;sample.grip=1;sample.stickY=.5f;sample.stickX=.25f;sample.orbitX=.4f;
    require(controller.Update(sample).claimed,"grip is initially claimed without a time step");sample.timeNs+=50000000;
    const auto step=controller.Update(sample);require(step.claimed&&!step.reset&&(step.height!=0||step.distance!=0)&&step.orbit!=0,"grip/sticks still produce manual height/distance/orbit input");
    cvr::camera::ExternalViewPose controlled;require(OffsetEditorCamera(base,step.height,step.distance,1,controlled)&&!same(controlled,base),"manual offset still changes actual camera");
    std::array<double,3> p{double(controlled.position[0])/131072.,double(controlled.position[1])/131072.,double(controlled.position[2])/131072.};const auto prior=p;std::array<double,4> q{controlled.rotation.x,controlled.rotation.y,controlled.rotation.z,controlled.rotation.w};require(life.Framing().Orbit(m.screenEpoch,m.controllerIdentity,m.entityIdentity,step.orbit,p,q)&&p!=prior,"manual orbit remains functional around preserved framing centre");
    sample.stickClick=true;require(controller.Update(sample).reset,"left grip + L3 reset remains a manual reset");
    for(unsigned field=0;field<4;++field){auto next=m;if(field==0)++next.screenEpoch;if(field==1)++next.controllerIdentity;if(field==2)++next.entityIdentity;if(field==3)++next.entityID;require(!SameEditorCameraLifetime(m,next),"every actual lifetime identity change differs");require(life.Resolve(next,1,head(2),prepare(next,typed(2),calls))&&life.Anchor().position.x==2&&!same(life.Camera(),base),"new editor/controller/owner/entity-ID obtains its own camera and head reference");require(life.Resolve(m,1,head(),prepare(m,typed(),calls)),"restore fixture identity");}
    const auto before=calls;life.Invalidate(false);require(!life.Epoch()&&life.Resolve(m,1,head(3),prepare(m,typed(3),calls))&&calls==before+1&&life.Anchor().position.x==3,"true editor close/reopen resets camera even when entity ID is reused");
    std::puts("PASS actual manual height/zoom/orbit/reset; epoch/controller/entity lifetime replacement and true close/reopen");
}
void invalidFirstCameraDoesNotLatch(){
    EditorCameraLifetime life;auto m=scope();unsigned calls=0;auto bad=typed();bad.position={};require(!life.Resolve(m,1,head(),prepare(m,bad,calls))&&!life.Epoch(),"unsettled typed camera cannot freeze the lifetime");require(life.Resolve(m,1,head(),prepare(m,typed(),calls))&&calls==2,"first later valid camera may initialize");
    auto invalid=m;invalid.entityID=0;const auto base=life.Camera();require(!life.Resolve(invalid,1,head(),prepare(invalid,typed(2),calls))&&same(base,life.Camera()),"invalid lifetime identity cannot replace an established base");
    std::puts("PASS invalid initial typed camera and invalid identity fail without latching/replacing the base");
}
void firstEyeFailureCannotCommitLifetime(){
    struct Common:EditorNativeSpatialCommon {EditorCameraLifetime lifetime;};
    EditorNativeSpatialFrameCache<Common> cache;EditorCameraLifetime committed;auto m=scope();unsigned calls=0;float horizontal=0;auto input=typed();
    const auto source=[](uint32_t eye){return NativePreviewSource{4+eye,5+eye,eye,NativePreviewMode::NativeEditorDual,11};};
    const auto prepareFrame=[&](const EditorNativeFrameSample& sample,Common& c){c.lifetime=committed;if(!c.lifetime.Resolve(m,sample.origin,sample.headPose,prepare(m,input,calls)))return false;c.camera=c.lifetime.Camera();c.anchor=c.lifetime.Anchor();c.head=sample.headPose;c.recenter=head();c.origin=sample.origin;c.scale=1;c.halfIpd=.0315f;c.horizontal=horizontal;c.width=1280;c.height=1400;c.imageRoute=EditorImageRoute::FullRawImage;return true;};
    const auto compose=[](const Common& c,const auto& sample,const auto& scope,const auto& binding,const auto& native,auto& app){return BuildEditorNativeSpatialEye(c,sample,scope,binding,native,app);};
    Descriptor native{};CameraApplication app;bool first=false;
    require(!cache.Resolve(m,source(1),100,1,head(),native,prepareFrame,compose,app,&first)&&!first&&!committed.Epoch()&&!cache.CommonState(),"failed real first-eye FOV math must not commit a staged camera lifetime");
    input=typed(3);horizontal=94;
    require(cache.Resolve(m,source(1),101,1,head(.25f),native,prepareFrame,compose,app,&first)&&first,"next valid native frame can prepare a different initial camera");
    committed=cache.CommonState()->lifetime;const auto base=committed.Camera();require(calls==2&&committed.Anchor().position.x==.25f,"only successful common frame commits base and reference head");
    ++m.setupID;input=typed(5);
    require(!cache.Resolve(m,source(0),101,1,head(.5f),native,prepareFrame,compose,app),"lifetime retention must not relax exact current-frame fullscope guards");
    require(cache.Resolve(m,source(0),102,1,head(.5f),native,prepareFrame,compose,app,&first)&&first,"next category native frame still requires fresh strict frame acceptance");
    committed=cache.CommonState()->lifetime;require(calls==2&&same(committed.Camera(),base)&&committed.Anchor().position.x==.25f,"accepted category frame retains first committed camera/reference without rerunning native mapping");
    std::puts("PASS actual common-frame transaction: first-eye failure cannot commit; strict category frame scope retained");
}
}
int main(){categoryDoesNotMoveCameraOrHeadAnchor();manualControlsAndTrueLifetimeReset();invalidFirstCameraDoesNotLatch();firstEyeFailureCannotCommitLifetime();std::puts("PASS editor lifetime camera policy; CPU evidence only");}
