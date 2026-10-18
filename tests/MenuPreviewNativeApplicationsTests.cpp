// Pure production ownership policy tests; no GPU, game, SDK membership or UI proof.
// Same CPU-test dependencies as MenuPreviewProducerTests.cpp / ImageLineageTests.cpp.
#include "MenuPreview/ImageLineagePolicy.hpp"
#include <cstdio>
#include <stdexcept>
#include <string_view>
#include <type_traits>
using namespace cvr::menu_preview;

// Metrics-only stubs: no store, identity, lifetime, or matching behavior is mocked.
namespace cvr::menu_preview {
bool EditorFreshnessEnabled() noexcept { return false; }
void NoteEditorPublication(const CameraApplication*,const CameraApplication*,bool,bool) noexcept {}
}
static void Require(bool value,const char* why){if(!value)throw std::runtime_error(why);}
struct ResetStore {~ResetStore(){ClearCameraApplication();}};
static constexpr uintptr_t LeftContext=0x101000,RightContext=0x202000;
static constexpr uint64_t LeftKey=0x12340001,RightKey=0x12340002;
static constexpr uintptr_t LeftComponent=0x301000,RightComponent=0x302000;
static_assert(std::is_trivially_copyable_v<NativePreviewSource> && std::is_standard_layout_v<NativePreviewSource>);
static std::shared_ptr<CameraApplication> App(uint32_t eye,uint64_t sequence,uint32_t frame){
 auto a=std::make_shared<CameraApplication>();auto& m=a->metadata;
 m.screen=2;m.screenEpoch=7;m.setupID=6;m.controllerIdentity=0x400000;
 m.entityIdentity=0x500000;m.entityID=0xA001;m.sceneNameHash=0x71;m.cameraRefHash=0x72;m.slotNameHash=0x73;
 // Current production represents the native source in Metadata. The proposed
 // design moves these two source facts into internal per-view identity while
 // keeping one canonical Metadata/frame.key for the public editor lifetime.
 m.componentIdentity=eye?RightComponent:LeftComponent;
 m.virtualCameraNameHash=eye?RightKey:LeftKey;m.publishedAtMs=PublicationNowMs();m.publicationSerial=sequence;
 a->frame.key={m.screenEpoch,m.setupID,m.componentIdentity,m.virtualCameraNameHash};
 a->frame.eye=eye;a->frame.nativeFrame=frame;a->frame.sourceSequence=sequence;
 a->nativeContext=eye?RightContext:LeftContext;a->nativeDestination=a->nativeContext+0x70;
 a->localPoseRevision=sequence;a->editorControlRevision=1;a->projection.valid=true;a->projection.origin=9;
 a->eyeDescriptor[0]=uint8_t(eye+1);return a;
}
static NativePreviewSource Source(uint32_t eye,uint64_t generation=5){
 return {eye?RightComponent:LeftComponent,eye?RightKey:LeftKey,eye,NativePreviewMode::NativeEditorDual,generation};
}
static Metadata Common(){return App(0,1,90)->metadata;}
static void Configure(uint64_t generation=5){Require(ConfigureEditorNativeApplications(Common(),generation,Source(0,generation),Source(1,generation)),"valid native bindings rejected");}
static std::shared_ptr<CameraApplication> NativeApp(uint32_t eye,uint64_t sequence,uint32_t frame,uint64_t generation=5){
 auto a=App(eye,sequence,frame);a->metadata=Common();a->frame.key={a->metadata.screenEpoch,a->metadata.setupID,a->metadata.componentIdentity,a->metadata.virtualCameraNameHash};
 a->source=Source(eye,generation);return a;
}
static std::shared_ptr<const CameraApplication> ReadForNode(uintptr_t context,uint64_t key,uint32_t frame){
 auto a=ReadCameraApplicationForNode(context,key,true,frame);
 return MatchPreviewNode(a,context,key,true,frame,PublicationNowMs())?a:nullptr;
}
static void LegacySingleViewControl(){
 ClearCameraApplication();ResetStore reset;auto left=App(0,10,90);PublishCameraApplication(left);
 Require(ReadForNode(LeftContext,LeftKey,90)==left,"legacy current view not found");
 Require(!ReadForNode(RightContext,RightKey,90),"unknown native source was relabeled as current");
 auto retained=ReadCameraApplication();ClearCameraApplication();
 Require(!ReadCameraApplication()&&retained==left&&retained->eyeDescriptor[0]==1,"immutable retained reader lost on clear");
}
static void RedSameFrameTwoNativeApplications(){
 ClearCameraApplication();ResetStore reset;Configure();auto left=NativeApp(0,10,90),right=NativeApp(1,10,90);
 Require(PublishEditorNativeApplication(left),"left publish failed");auto alreadyRetained=ReadForNode(LeftContext,LeftKey,90);Require(PublishEditorNativeApplication(right),"right publish failed");
 const auto l=ReadForNode(LeftContext,LeftKey,90),r=ReadForNode(RightContext,RightKey,90);
 Require(alreadyRetained==left,"fixture lost its immutable held left snapshot");
 std::fprintf(stdout,"same-frame exact lookups: left=%u right=%u heldLeft=%u\n",unsigned(bool(l)),unsigned(bool(r)),unsigned(alreadyRetained==left));
 Require(l==left&&r==right,"second native application evicts first from exact source lookup, despite same editor epoch/frame");
 Require(IsCurrentCameraApplication(left)&&IsCurrentCameraApplication(right),"each raw writer must remain current in its own slot");
 Require(CameraApplicationUiSource(*left)&&!CameraApplicationUiSource(*right),"offscreen right must not qualify as UI source");
}
static void RedSequenceIsPerNativeSource(){
 ClearCameraApplication();ResetStore reset;Configure();auto left=NativeApp(0,100,90),right=NativeApp(1,9,90);
 Require(PublishEditorNativeApplication(left)&&PublishEditorNativeApplication(right),"independent lane counters rejected");
 const auto l=ReadForNode(LeftContext,LeftKey,90),r=ReadForNode(RightContext,RightKey,90);
 std::fprintf(stdout,"cross-lane counters: left=%u right=%u\n",unsigned(bool(l)),unsigned(bool(r)));
 Require(l==left&&r==right,"global sourceSequence older-reject discards a different native source's first application");
 auto older=NativeApp(1,8,89);Require(!PublishEditorNativeApplication(older)&&IsCurrentCameraApplication(right),"older same-source app must not replace its slot");
 auto newer=NativeApp(1,10,91);Require(PublishEditorNativeApplication(newer)&&IsCurrentCameraApplication(left)&&!IsCurrentCameraApplication(right),"new right must replace only right slot");
}
static void SourceAndGenerationAdmission(){
 ClearCameraApplication();ResetStore reset;Configure();Require(EditorNativeApplicationsConfigured()&&!HasCurrentPreviewApplications(),"configuration must not invent rendered apps");
 auto left=NativeApp(0,10,90),right=NativeApp(1,10,90);
 Require(PublishEditorNativeApplication(left)&&PublishEditorNativeApplication(right),"fixture publication");
 Configure();Require(IsCurrentCameraApplication(left)&&IsCurrentCameraApplication(right),"same scope/generation refresh must preserve slots");
 auto bad=*right;bad.source.cameraKey=0xBAD;Require(!PublishEditorNativeApplication(std::make_shared<CameraApplication>(bad)),"unknown source key accepted");
 bad=*right;bad.source.component=LeftComponent;Require(!PublishEditorNativeApplication(std::make_shared<CameraApplication>(bad)),"foreign source component accepted");
 bad=*right;bad.source.bindingGeneration=4;Require(!PublishEditorNativeApplication(std::make_shared<CameraApplication>(bad)),"stale source generation accepted");
 bad=*right;bad.nativeContext=LeftContext;Require(!PublishEditorNativeApplication(std::make_shared<CameraApplication>(bad)),"two native sources share one context");
 bad=*right;bad.frame.eye=0;Require(!PublishEditorNativeApplication(std::make_shared<CameraApplication>(bad)),"source eye and rendered eye mismatch accepted");
 bad=*right;bad.metadata.controllerIdentity++;Require(!PublishEditorNativeApplication(std::make_shared<CameraApplication>(bad)),"foreign editor owner accepted");
 bad=*right;bad.metadata.publishedAtMs=1;Require(!PublishEditorNativeApplication(std::make_shared<CameraApplication>(bad)),"stale camera application accepted");
 Require(!ReadForNode(LeftContext,RightKey,90)&&!ReadForNode(RightContext,0xBAD,90)&&!ReadForNode(RightContext,RightKey,89),"unknown context/key/frame matched another slot");
 Require(!ReadCameraApplicationForNode(RightContext,RightKey,false,90),"unknown clock admitted");
 auto legacy=App(0,999,90);PublishCameraApplication(legacy);Require(ReadForNode(LeftContext,LeftKey,90)==left,"dual lookup fell back to legacy singleton");
 auto changed=*left;changed.source.bindingGeneration++;Require(!SameCameraApplication(*left,changed),"immutable equality collapsed source generation");
 Require(HasCurrentPreviewApplications(),"published pair not visible to observation gate");
}
static void ScopeAndReset(){
 ClearCameraApplication();ResetStore reset;Configure();auto left=NativeApp(0,10,90);Require(PublishEditorNativeApplication(left),"fixture publication");
 auto retained=ReadForNode(LeftContext,LeftKey,90);auto common=Common();common.setupID++;
 Require(ConfigureEditorNativeApplications(common,6,Source(0,6),Source(1,6)),"new strict setup rejected");
 Require(!HasCurrentPreviewApplications()&&!IsCurrentCameraApplication(left)&&retained==left,"scope change did not clear lookup/preserve held snapshot");
 Require(!PublishEditorNativeApplication(left),"old setup/generation revived");
 Require(!ConfigureEditorNativeApplications(Common(),5,Source(0,5),Source(1,5)),"stale generation reconfigured the same editor lifetime");
 ClearCameraApplication();Require(!EditorNativeApplicationsConfigured()&&!HasCurrentPreviewApplications(),"authoritative clear leaves dual active");
 auto legacy=App(0,10,90);PublishCameraApplication(legacy);Require(ReadForNode(LeftContext,LeftKey,90)==legacy,"clear did not restore legacy one-view behavior");
}
static void InvalidBindingsDoNotReplaceCurrent(){
 ClearCameraApplication();ResetStore reset;Configure();auto left=NativeApp(0,10,90);Require(PublishEditorNativeApplication(left),"fixture publication");
 const auto common=Common();auto l=Source(0),r=Source(1);r.component=l.component;
 Require(!ConfigureEditorNativeApplications(common,5,l,r),"same component admitted");r=Source(1);r.cameraKey=l.cameraKey;
 Require(!ConfigureEditorNativeApplications(common,5,l,r),"same camera key admitted");r=Source(1);r.eye=0;
 Require(!ConfigureEditorNativeApplications(common,5,l,r),"wrong binding ordinal admitted");r=Source(1);r.bindingGeneration=4;
 Require(!ConfigureEditorNativeApplications(common,5,l,r),"stale binding generation admitted");r=Source(1);r.mode=NativePreviewMode::LegacyOneView;
 Require(!ConfigureEditorNativeApplications(common,5,l,r),"legacy source smuggled into dual pair");r=Source(1);auto wrongCommon=common;wrongCommon.componentIdentity++;
 Require(!ConfigureEditorNativeApplications(wrongCommon,5,l,r)&&IsCurrentCameraApplication(left),"invalid primary match mutated active source");
}
static bool ClearFailedSource(const NativePreviewSource& source){return ClearCameraApplicationForSource(source);}
static void SourceLocalFailurePreservesOtherEye(){
 ClearCameraApplication();ResetStore reset;Configure();auto left=NativeApp(0,10,90),right=NativeApp(1,10,90);
 Require(PublishEditorNativeApplication(left)&&PublishEditorNativeApplication(right),"fixture publication");auto retained=right;
 Require(ClearFailedSource(Source(1)),"registered failed source not handled");
 Require(IsCurrentCameraApplication(left)&&!IsCurrentCameraApplication(right)&&EditorNativeApplicationsConfigured(),"one source failure cleared the other native eye or its binding");
 Require(retained->eyeDescriptor[0]==2,"failed source destroyed immutable held pixels/app");
 Require(ClearFailedSource(Source(1))&&IsCurrentCameraApplication(left),"source-local clear not idempotent");
 Require(!ClearFailedSource(Source(0,4))&&IsCurrentCameraApplication(left),"stale failed source cleared current generation");
 Require(!ClearFailedSource({})&&IsCurrentCameraApplication(left),"legacy/unknown clear entered dual slot");
}
static void CanonicalScopeLedgerControl(){
 auto left=App(0,10,90),right=App(1,10,90);
 // This control tests only published image grouping: same common Metadata/key,
 // different actual nativeContext and resource. It does NOT invent admission.
 right->metadata=left->metadata;right->frame.key=left->frame.key;
 ImageLineageStore ledger;ledger.ObserveScope(left);
 ledger.Record(0x610000,0x710000,left);ledger.Record(0x620000,0x720000,right);
 const uintptr_t lists[]{0x610000,0x620000};auto prepared=ledger.Prepare(0x810000,std::span(lists));
 ledger.Commit(0x810000,std::move(prepared),PublicationNowMs());
 const auto l=ledger.Read(0x710000),r=ledger.Read(0x720000);
 Require(l&&r&&l.pose==left&&r.pose==right,"common scope ledger cannot retain distinct raw source resources");
}
int main(int argc,char** argv){
 unsigned failures=0;
 auto run=[&](const char* name,auto test){try{test();std::printf("PASS %s\n",name);}catch(const std::exception& e){++failures;std::fprintf(stderr,"FAIL %s: %s\n",name,e.what());}};
 run("legacy-single-view-control",LegacySingleViewControl);
 run("canonical-common-scope-image-ledger-control",CanonicalScopeLedgerControl);
 if(argc<2||std::string_view(argv[1])!="--legacy-controls-only"){
  run("two-native-applications-same-epoch-and-frame",RedSameFrameTwoNativeApplications);
  run("monotonic-sequence-is-per-native-source",RedSequenceIsPerNativeSource);
  run("source-generation-context-and-ui-admission",SourceAndGenerationAdmission);
  run("scope-reset-and-legacy-reactivation",ScopeAndReset);
  run("invalid-binding-configuration-is-non-mutating",InvalidBindingsDoNotReplaceCurrent);
  run("source-local-failure-preserves-other-eye",SourceLocalFailurePreservesOtherEye);
 }
 return failures?1:0;
}
