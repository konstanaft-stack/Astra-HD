#include "MenuPreview/EditorNativeScenePairPolicy.hpp"
#include "MenuPreview/EditorScenePolicy.hpp"
#include <cstdio>
#include <stdexcept>
using namespace cvr::menu_preview;
namespace cvr::menu_preview {bool EditorFreshnessEnabled()noexcept{return false;}void NoteEditorPublication(const CameraApplication*,const CameraApplication*,bool,bool)noexcept{}}
static void Check(bool value,const char* why){if(!value)throw std::runtime_error(why);}
static const uint64_t Now=PublicationNowMs();
static Metadata Common(){Metadata m{};m.screen=2;m.screenEpoch=7;m.setupID=6;m.controllerIdentity=0x10;m.entityIdentity=0x20;m.entityID=0x21;m.componentIdentity=0x30;m.virtualCameraNameHash=0x40;m.publishedAtMs=Now;return m;}
static NativePreviewSource Source(unsigned eye,uint64_t generation=3){return {uintptr_t(0x30+eye),uint64_t(0x40+eye),eye,NativePreviewMode::NativeEditorDual,generation};}
struct Fixture{
 Metadata common=Common();
 Fixture(){ClearCameraApplication();Check(ConfigureEditorNativeApplications(common,3,Source(0),Source(1)),"configure source membership");}
 ~Fixture(){ClearCameraApplication();}
 EditorNativeSceneProof Image(unsigned eye,uint32_t frame,uint64_t serial){
  auto a=std::make_shared<CameraApplication>();a->metadata=common;a->source=Source(eye);a->nativeContext=0x100+eye;
  a->frame={{7,6,0x30,0x40},frame,eye,serial,true};a->localPoseRevision=serial;a->editorControlRevision=4;
  a->projection.valid=true;a->projection.origin=5;a->projection.pose.position.x=eye?.032f:-.032f;
  // Literals exercise admission policy only; they do not claim GPU execution.
  return {a,uintptr_t(0x200+serial),uintptr_t(0x300+serial),0x400,0x500,serial,serial,serial,serial,Now};
 }
};
static EditorNativeSceneProof Copy(const EditorNativeSceneProof& p){auto q=p;q.application=std::make_shared<CameraApplication>(*p.application);return q;}
static CameraApplication& App(EditorNativeSceneProof& p){return *std::const_pointer_cast<CameraApplication>(p.application);}
static bool Pair(const EditorNativeSceneProof& l,const EditorNativeSceneProof& r,const Metadata& m){return EditorNativeScenesCoherent(l,r,m,Now);}
static void ExecutionIsNotUiConsumption(){
 Check(EditorSceneExecutionAllowsFinalize(NativePreviewMode::NativeEditorDual,true,0),"actual native Execute with UI0 cannot finalize raw");
 Check(!EditorSceneExecutionAllowsFinalize(NativePreviewMode::NativeEditorDual,false,77),"UI submission substituted for native recording proof");
 Check(!EditorSceneExecutionAllowsFinalize(NativePreviewMode::LegacyOneView,true,0),"legacy raw stopped requiring actual UI submission");
 Check(EditorSceneExecutionAllowsFinalize(NativePreviewMode::LegacyOneView,false,77),"legacy UI route changed");
 Check(!EditorSceneExecutionAllowsFinalize(NativePreviewMode(99),true,77),"unknown source mode admitted");
}
static void CoherentRealSourceMetadata(){Fixture f;auto l=f.Image(0,100,10),r=f.Image(1,100,11);Check(Pair(l,r,f.common),"matching complete native pair rejected");
 Check(l.application->projection.pose.position.x<0&&r.application->projection.pose.position.x>0,"fixture lost independent per-eye labels");
 auto bad=Copy(r);App(bad).frame.nativeFrame=99;Check(!Pair(l,bad,f.common),"native frames100/99 accepted");
 bad=Copy(r);App(bad).editorControlRevision=6;Check(!Pair(l,bad,f.common),"different camera control revision accepted");
 bad=Copy(r);App(bad).projection.origin=7;Check(!Pair(l,bad,f.common),"different tracking origin accepted");
 Check(!Pair(r,l,f.common),"raw source eyes swapped");
}
static void NewestCompleteFrameAndOneEyeLate(){Fixture f;auto l=f.Image(0,100,10),r=f.Image(1,100,11),nextL=f.Image(0,101,12),nextR=f.Image(1,101,13);
 std::array<EditorNativeSceneProof,4> cache{l,r,nextL,{}};auto selected=SelectEditorNativeScenePair(cache,f.common,Now);
 Check(selected&&(*selected)[0]==0&&(*selected)[1]==1,"one-eye late replaced prior complete frame");
 cache[3]=nextR;cache[3].completedValue=12;selected=SelectEditorNativeScenePair(cache,f.common,Now);
 Check(selected&&(*selected)[0]==0&&(*selected)[1]==1,"pending right GPU copy replaced complete frame");
 cache[3].completedValue=13;selected=SelectEditorNativeScenePair(cache,f.common,Now);
 Check(selected&&(*selected)[0]==2&&(*selected)[1]==3,"newest complete native frame not selected");
 std::array<EditorNativeSceneProof,1> lone{nextL};Check(!SelectEditorNativeScenePair(lone,f.common,Now),"single eye manufactured a stereo pair");
}
static void MembershipAndActualSources(){Fixture f;auto l=f.Image(0,100,10),r=f.Image(1,100,11);auto bad=Copy(r);App(bad).source.bindingGeneration=2;Check(!Pair(l,bad,f.common),"stale binding generation accepted");
 bad=Copy(r);App(bad).source.mode=NativePreviewMode::LegacyOneView;Check(!Pair(l,bad,f.common),"legacy application entered native pair");
 bad=Copy(r);App(bad).source.cameraKey=0x99;Check(!Pair(l,bad,f.common),"unregistered camera key accepted");
 bad=Copy(r);App(bad).source.component=0x99;Check(!Pair(l,bad,f.common),"unregistered component accepted");
 bad=Copy(r);App(bad).nativeContext=l.application->nativeContext;Check(!Pair(l,bad,f.common),"same native context passed as two views");
 bad=Copy(r);bad.texture=l.texture;Check(!Pair(l,bad,f.common),"same pixels passed as two views");
 bad=Copy(r);App(bad).metadata.setupID=9;Check(!Pair(l,bad,f.common),"different canonical setup accepted");
}
static void CompletedCopyAndLifetimeEvidence(){Fixture f;auto l=f.Image(0,100,10),r=f.Image(1,100,11);
 Check(l.fence==r.fence&&Pair(l,r,f.common),"recycled healthy fence handle with different values rejected");
 for(unsigned n=0;n<8;++n){auto bad=r;switch(n){case 0:bad.completedValue=10;break;case 1:bad.completedValue=UINT64_MAX;break;case 2:bad.producerValue=UINT64_MAX;break;case 3:bad.producerValue=0;break;case 4:bad.lifetime=0;break;case 5:bad.queue=0;break;case 6:bad.rawExecutionSerial=0;break;case 7:bad.fence=0;break;}
  Check(!Pair(l,bad,f.common),"missing/removed/pending GPU publication evidence admitted");}
}
static void FreshnessBound(){Fixture f;auto l=f.Image(0,100,10),r=f.Image(1,100,11);auto bad=r;bad.publishedAtMs=Now-501;Check(!Pair(l,bad,f.common),"stale raw publication admitted");
 bad=r;bad.publishedAtMs=Now+1;Check(!Pair(l,bad,f.common),"future raw publication admitted");
 auto common=f.common;common.publishedAtMs=Now-501;Check(!Pair(l,r,common),"stale current scope admitted");
 bad=Copy(r);App(bad).metadata.publishedAtMs=Now-501;Check(!Pair(l,bad,f.common),"stale camera application admitted");
}
static void LateGpuFrameDoesNotNeedLatestAppPointer(){Fixture f;auto l=f.Image(0,100,10),r=f.Image(1,100,11),next=f.Image(0,101,12);
 Check(PublishEditorNativeApplication(next.application),"publish frameN+1 app");
 Check(!IsCurrentCameraApplication(l.application)&&Pair(l,r,f.common),"completed frameN required latest exact camera pointer");
 auto common=f.common;common.setupID=9;Check(ConfigureEditorNativeApplications(common,4,Source(0,4),Source(1,4)),"new source scope configure");
 Check(!Pair(l,r,f.common),"old generation raw survived reconfiguration");
}
static void NativeFrameWrap(){Fixture f;std::array<EditorNativeSceneProof,4> cache{f.Image(0,UINT32_MAX,10),f.Image(1,UINT32_MAX,11),f.Image(0,0,12),f.Image(1,0,13)};
 auto selected=SelectEditorNativeScenePair(cache,f.common,Now);Check(selected&&(*selected)[0]==2&&(*selected)[1]==3,"newest complete pair did not handle native frame wrap");
}
int main(){unsigned failed{};auto run=[&](const char* n,auto test){try{test();std::printf("PASS %s\n",n);}catch(const std::exception& e){++failed;std::fprintf(stderr,"FAIL %s: %s\n",n,e.what());}};
 run("real-execute-vs-ui-contract",ExecutionIsNotUiConsumption);run("same-frame-control-origin-pose-labels",CoherentRealSourceMetadata);
 run("newest-complete-pair-and-one-eye-late",NewestCompleteFrameAndOneEyeLate);run("registered-distinct-native-sources",MembershipAndActualSources);
 run("completed-copy-fence-and-lease-evidence",CompletedCopyAndLifetimeEvidence);run("existing500ms-freshness",FreshnessBound);
 run("gpu-frameN-after-camera-frameNplus1",LateGpuFrameDoesNotNeedLatestAppPointer);run("native-frame-wrap",NativeFrameWrap);return failed?1:0;}