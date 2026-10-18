// CPU fixture: the runner extracts the current production publisher, copy and
// blend functions verbatim. XR clock/pose resolution and engine memory are the
// only fixture services; the receipt latch and address ledger are production.
#include <Windows.h>
#include "Camera/PoseAddressLedger.hpp"
#include "Camera/MainEyeWriteDiagnostics.hpp"
#include "Camera/MainEyeScenePose.hpp"
#include "Framegen/MainEyeSourceLatch.hpp"
#include <atomic>
#include <bit>
#include <cstring>
#include <iostream>
#include <limits>
#include <optional>
#include <stdexcept>
#include <vector>
#if __has_include("Camera/AppliedEyeReference.hpp")
#include "Camera/AppliedEyeReference.hpp"
#define HAS_APPLIED_EYE_REFERENCE 1
#else
// RED-only observation slot: old production functions do not write this field.
// No replacement publisher/copy/blend behavior is provided by this scaffold.
#define HAS_APPLIED_EYE_REFERENCE 0
namespace cvr::camera {
enum class AppliedEyeRoute : uint8_t {Unknown,Component,Scene};
enum class AppliedEyeSpace : uint8_t {Unknown,EngineWorldFixed17AndCameraQuaternion};
struct AppliedEyeReference {
    std::array<int32_t,3> centreFixed{};PoseFingerprint writtenEye{};
    AppliedEyeSpace space{};AppliedEyeRoute route{};bool known{};
    bool operator==(const AppliedEyeReference&)const=default;
};
}
#endif
struct XrQuaternionf {float x{},y{},z{},w=1;};
struct XrVector3f {float x{},y{},z{};};
struct XrPosef {XrQuaternionf orientation;XrVector3f position;};
struct OpenXRHeadPose {float posX{},posY{},posZ{},oriX{},oriY{},oriZ{},oriW=1;bool valid=true;uint64_t originSerial=30;int64_t locatedTime=5000;};
struct FixtureTime {int64_t qpc=100,xrTime=200;uint64_t clockGeneration=300;};
struct OpenXRManager {
    bool resolving=true,reset=false;uint64_t origin=30;
    static OpenXRManager& Get(){static OpenXRManager value;return value;}
    bool ResolveRenderedPose(const OpenXRHeadPose& h,XrPosef* p)const {
        if(!resolving||!h.valid||h.originSerial!=origin)return false;
        *p={{h.oriX,h.oriY,h.oriZ,h.oriW},{h.posX,h.posY,h.posZ}};return true;
    }
    bool ExternalPoseResetPending()const{return reset;}
    uint64_t GetTrackingOriginSerial()const{return origin;}
    std::optional<FixtureTime> SampleCameraApplicationTime(uint64_t)const{return FixtureTime{};}
};
#define CVR_DIAGNOSTIC(...) ((void)0)
namespace cvr::camera {
#include "applied_eye_types.inc"
PoseAddressLedger<PoseIdentity> s_addresses;
std::atomic<uint64_t> s_writeId{};
framegen::MainEyeSourceLatch s_latch;
struct Telemetry {
    uint64_t publishRejected{},published{},writeId{},captureClockGeneration{},publishClockMissing{};
    int64_t lastPublishSourceId{};uintptr_t publishedSetup{};
};
template<class F>void PoseTelemetry(F f){Telemetry d;f(d);}
template<class F>void TransportTelemetry(F f){Telemetry d;f(d);}
bool ValidateMainEyeSourceReceipt(const framegen::MainEyeSourceReceipt& r){return s_latch.Usable(r);}
#include "applied_eye_functions.inc"
}
using namespace cvr::camera;
using namespace cvr::framegen;
static unsigned checks;
static void Check(bool value,const char* why){++checks;if(!value)throw std::runtime_error(why);}
struct Setup {
    alignas(16) std::array<std::byte,64> bytes{};
    uintptr_t Address()const{return reinterpret_cast<uintptr_t>(bytes.data());}
    void Set(const PoseFingerprint& f){std::memcpy(bytes.data(),f.position.data(),12);std::memcpy(bytes.data()+16,f.rotation.data(),16);}
};
static PoseFingerprint Eye(std::array<int32_t,3> p={4000,-5000,6000},std::array<float,4> q={0,0,0,1}) {
    PoseFingerprint f;f.position=p;for(unsigned i=0;i<4;++i)f.rotation[i]=std::bit_cast<uint32_t>(q[i]);return f;
}
static AppliedEyeReference Proof(const PoseFingerprint& f=Eye(),std::array<int32_t,3> centre={3000,-5100,5900},AppliedEyeRoute route=AppliedEyeRoute::Component) {
    AppliedEyeReference v;v.centreFixed=centre;v.writtenEye=f;v.space=AppliedEyeSpace::EngineWorldFixed17AndCameraQuaternion;v.route=route;v.known=true;return v;
}
static MainEyeSourceReceipt BeginSource() {
    s_latch={};const auto revision=s_latch.Request(true);
    Check(s_latch.OnSceneFrame(revision,{10,1,20,30,true},0,AerV2Eye::Left,{revision,10,true},{{20,10,30},40,{},5000}),"source fixture admission");
    return *s_latch.ForMainWrite(30);
}
static PoseReceipt Publish(Setup& setup,const MainEyeSourceReceipt& source,const AppliedEyeReference& proof,
    bool external=false,uint64_t poseId=17,const OpenXRHeadPose& head={}) {
#if HAS_APPLIED_EYE_REFERENCE
    PublishCameraSetupPose(setup.Address(),0xABCD,1,poseId,head,external,source,proof);
#else
    (void)proof;PublishCameraSetupPose(setup.Address(),0xABCD,1,poseId,head,external,source);
#endif
    return CapturePoseAddress(setup.Address());
}
static void PublishedActualProof() {
    const auto source=BeginSource();Setup setup;setup.Set(Eye());const auto proof=Proof();
    auto first=Publish(setup,source,proof);
    Check(bool(first)&&first.payload.mainEyeSource==source,"valid AER pose retained");
    Check(first.payload.appliedEyeReference.known,"actual successful MAIN writer must publish applied eye proof");
    Check(first.payload.appliedEyeReference==proof,"raw position/quaternion must be exact, not recomputed");
    auto second=Publish(setup,source,proof);
    Check(second.payload.writeId>first.payload.writeId&&second.payload.poseId==first.payload.poseId,"same tracked pose receives a fresh camera write identity");
    Check(second.payload.appliedEyeReference==proof,"new write retains its own proof");
    Check(!TransferPoseAddress(first,setup.Address()),"stale receipt invalid after identical object rewrite");
}
static void PublisherRechecksSourceAndReadback() {
    const auto source=BeginSource();Setup setup;setup.Set(Eye());auto proof=Proof();
    auto wrong=proof;++wrong.writtenEye.position[0];auto value=Publish(setup,source,wrong);
    Check(bool(value)&&value.payload.mainEyeSource==source&&!value.payload.appliedEyeReference.known,"wrong position proof must not alter old AER pose policy");
    wrong=proof;wrong.writtenEye.rotation[2]^=1;value=Publish(setup,source,wrong);
    Check(bool(value)&&!value.payload.appliedEyeReference.known,"wrong quaternion readback accepted");
    value=Publish(setup,{},proof);Check(bool(value)&&!value.payload.appliedEyeReference.known,"legacy source manufactured an AER eye proof");
    value=Publish(setup,source,Proof(Eye(),{3000,-5100,5900},AppliedEyeRoute::Scene));
    Check(bool(value)&&!value.payload.appliedEyeReference.known,"wrong writer route accepted");
    value=Publish(setup,source,Proof(Eye(),{3000,-5100,5900},AppliedEyeRoute::Scene),true);
    Check(bool(value)&&value.payload.appliedEyeReference.known&&value.payload.externalView,"scene proof missing");
    auto stale=source;++stale.latchRevision;Check(!Publish(setup,stale,proof),"stale source publication accepted");
    auto head=OpenXRHeadPose{};++head.locatedTime;Check(!Publish(setup,source,proof,false,17,head),"mismatched source-pose time accepted");
    s_latch.Request(false);Check(!Publish(setup,source,proof),"revoked source publication accepted");
}
static void CopyAndTransformBoundary() {
    const auto source=BeginSource();Setup a,b,c;const auto eye=Eye();a.Set(eye);b.Set(eye);c.Set(eye);
    auto original=Publish(a,source,Proof());PublishSerializedPose(b.Address(),original);auto copied=CapturePoseAddress(b.Address());
    Check(bool(copied)&&copied.payload.appliedEyeReference==original.payload.appliedEyeReference,"serialized exact copy loses eye proof");
    Check(copied.payload.writeId==original.payload.writeId&&copied.payload.mainEyeSource==source,"copy invented a source/write identity");
    auto moved=eye;++moved.position[1];c.Set(moved);Check(TransferPoseAddress(copied,c.Address()),"legacy labelled native transform must still transfer");
    const auto transformed=CapturePoseAddress(c.Address());
    Check(bool(transformed)&&transformed.payload.mainEyeSource==source&&!transformed.payload.appliedEyeReference.known,"changed base must clear only applied eye proof");
    c.Set(eye);PublishSerializedPose(c.Address(),copied);Check(CapturePoseAddress(c.Address()).payload.appliedEyeReference.known,"exact serialization after transform lost fresh proof");
    a.Set(moved);Check(!TransferPoseAddress(original,b.Address()),"changed source memory accepted");
}
static void BlendBoundary() {
    const auto source=BeginSource();Setup a,b,out;auto eye=Eye();a.Set(eye);b.Set(eye);out.Set(eye);
    auto pa=Publish(a,source,Proof()),pb=Publish(b,source,Proof());
    std::array<WeightedPoseReceipt,2> inputs{{{pa,.25f},{pb,.75f}}};
    Check(PublishBlendedPose(out.Address(),inputs),"identical blend head sample rejected");
    Check(CapturePoseAddress(out.Address()).payload.appliedEyeReference.known,"identical proof blend lost metadata");
    auto different=Proof();--different.centreFixed[0];pb=Publish(b,source,different);inputs[1].source=pb;
    Check(PublishBlendedPose(out.Address(),inputs),"different offset rejected existing head label");
    Check(!CapturePoseAddress(out.Address()).payload.appliedEyeReference.known,"last positive contributor silently supplied a different offset");
    b.Set(Eye({4500,-5000,6000}));pb=Publish(b,source,Proof(Eye({4500,-5000,6000})));inputs[1].source=pb;
    Check(PublishBlendedPose(out.Address(),inputs),"different base rejected existing head label");
    Check(!CapturePoseAddress(out.Address()).payload.appliedEyeReference.known,"transformed base retained unproved centre blend");
    inputs[1].weight=0;Check(PublishBlendedPose(out.Address(),std::span<const WeightedPoseReceipt>(inputs.data(),1)),"single contributor rejected");
    Check(CapturePoseAddress(out.Address()).payload.appliedEyeReference.known,"single fingerprint-identical contributor lost proof");
    auto changed=eye;changed.rotation[0]=std::bit_cast<uint32_t>(.01f);out.Set(changed);
    Check(PublishBlendedPose(out.Address(),std::span<const WeightedPoseReceipt>(inputs.data(),1)),"transformed output changed old head acceptance");
    Check(!CapturePoseAddress(out.Address()).payload.appliedEyeReference.known,"rotated output retained writer-local proof");
}
static void ExactUnitsAndWriteGates() {
#if HAS_APPLIED_EYE_REFERENCE
    const int32_t centre[3]={(std::numeric_limits<int32_t>::min)(),1000,-1000};
    const int32_t eye[3]={(std::numeric_limits<int32_t>::max)(),1001,-999};const float q[4]={.1f,.2f,.3f,.9f};
    const auto value=MakeAppliedEyeReference(centre,eye,q,AppliedEyeRoute::Component,true,true);
    Check(value.known&&value.DeltaFixed()[0]==4294967295LL,"fixed subtraction overflowed int32");
    Check(value.DeltaFixed()[1]==1&&value.DeltaFixed()[2]==1,"one fixed unit lost before conversion");
    Check(value.DeltaMetres()[0]==4294967295.0/131072.0&&value.DeltaMetres()[1]==0x1p-17,"fixed position units not exactly 2^-17 metre");
    Check(value.writtenEye.rotation[0]==std::bit_cast<uint32_t>(q[0]),"writer quaternion was normalized/recomputed");
    for(unsigned gate=0;gate<3;++gate){
        const auto absent=MakeAppliedEyeReference(centre,eye,q,AppliedEyeRoute::Scene,(gate&1)!=0,(gate&2)!=0);
        Check(!absent.known,"partial write claims applied geometry");
    }
    const auto scene=MakeAppliedEyeReference(centre,eye,q,AppliedEyeRoute::Scene,true,true);Check(scene.known,"complete scene write missing proof");
    Check(!MakeAppliedEyeReference(nullptr,eye,q,AppliedEyeRoute::Component,true,true).known,"missing centre accepted");
    Check(!MakeAppliedEyeReference(centre,eye,q,AppliedEyeRoute::Unknown,true,true).known,"unknown route accepted");
    const float invalid[4]={0,0,0,(std::numeric_limits<float>::quiet_NaN)()};
    Check(!MakeAppliedEyeReference(centre,eye,invalid,AppliedEyeRoute::Component,true,true).known,"nonfinite quaternion accepted");
    const float zero[4]{};Check(!MakeAppliedEyeReference(centre,eye,zero,AppliedEyeRoute::Component,true,true).known,"zero quaternion accepted");
#endif
}
struct AuditStub {template<class T>void Set(T,bool){}};
static PoseReceipt PatchWriter(Setup& setup,const MainEyeSourceReceipt& source,bool positionWritten,
    bool orientationWritten,bool translationLabelKnown,bool mainEyeSourceApplied) {
    InvalidatePoseAddress(setup.Address());
    const uintptr_t owner=setup.Address()-0xE0;const unsigned camKind=1;const uint64_t writtenPoseId=17;
    const OpenXRHeadPose writtenPose{};const std::optional<MainEyeSourceReceipt> mainEyeSource=source;
    const int32_t centreBeforeIpd[3]={3000,-5100,5900};const int32_t p[3]={4000,-5000,6000};
    const float quat[4]={0,0,0,1};AuditStub writeAudit;
#include "applied_eye_patch_call.inc"
    return CapturePoseAddress(setup.Address());
}
static PoseReceipt SceneWriter(Setup& setup,const MainEyeSourceReceipt& source,bool sceneOrientationWritten,
    bool sceneTranslationWritten,bool sceneEyeWritten) {
    InvalidatePoseAddress(setup.Address());
    const std::optional<MainEyeSourceReceipt> aerSceneSource=source;const OpenXRHeadPose xrPose{};
    const auto rbxPtr=reinterpret_cast<void*>(setup.Address());const uintptr_t serializedComponent=0x12345;const uint64_t cameraPoseSequence=17;
    const int32_t renderedPosFP[3]={3000,-5100,5900};const int32_t posFP[3]={4000,-5000,6000};const float quat[4]={0,0,0,1};
#include "applied_eye_scene_call.inc"
    return CapturePoseAddress(setup.Address());
}
static void ExactWriterCallSites() {
    const auto source=BeginSource();Setup setup;setup.Set(Eye());
    auto patch=PatchWriter(setup,source,true,true,true,true);
    Check(bool(patch)&&patch.payload.appliedEyeReference==Proof(),"actual PatchCamera publication arguments lost centre/eye proof");
    auto scene=SceneWriter(setup,source,true,true,true);
    Check(bool(scene)&&scene.payload.appliedEyeReference==Proof(Eye(),{3000,-5100,5900},AppliedEyeRoute::Scene),"actual LocateCamera publication arguments lost scene centre/eye proof");
    for(unsigned missing=0;missing<3;++missing){
        Check(!PatchWriter(setup,source,missing!=0,missing!=1,missing!=2,true),"actual PatchCamera missing-write guard weakened");
        Check(!SceneWriter(setup,source,missing!=0,missing!=1,missing!=2),"actual LocateCamera missing-write guard weakened");
    }
    patch=PatchWriter(setup,source,true,true,true,false);
    Check(bool(patch)&&!patch.payload.mainEyeSource&&!patch.payload.appliedEyeReference.known,"inactive source changed legacy camera publication");
}
int main(){
    unsigned pass=0,fail=0;const std::pair<const char*,void(*)()> cases[]={
        {"published_actual_proof",PublishedActualProof},{"source_and_readback",PublisherRechecksSourceAndReadback},
        {"copy_transform_boundary",CopyAndTransformBoundary},{"blend_boundary",BlendBoundary},{"units_write_gates",ExactUnitsAndWriteGates},
        {"exact_writer_callsites",ExactWriterCallSites}};
    for(const auto&[name,run]:cases)try{run();++pass;std::cout<<"PASS "<<name<<'\n';}catch(const std::exception&e){++fail;std::cerr<<"FAIL "<<name<<": "<<e.what()<<'\n';}
    std::cout<<"RESULT "<<pass<<" passed, "<<fail<<" failed; "<<checks<<" checks; extracted production functions\n";return fail?1:0;
}
