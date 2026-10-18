#include "Framegen/PairedGeometryDiagnostics.hpp"
#include "Framegen/ProducerContract.hpp"
#include "Overlay/ReferenceOptics.hpp"
#include <d3d12.h>
#include <array>
#include <memory>
#include <optional>
#include <cassert>
#include <cstdio>
#include <functional>
namespace cvr::framegen {
struct Fov {float angleLeft{},angleRight{},angleUp{},angleDown{};friend bool operator==(const Fov&,const Fov&)=default;};
struct AerRealGeometry {uint32_t width{},height{};std::array<float,4> crop{},tangents{};Fov renderedFov,effectiveFov;friend bool operator==(const AerRealGeometry&,const AerRealGeometry&)=default;};
struct Calls {unsigned devices{},descriptors{},scheduled{};friend bool operator==(const Calls&,const Calls&)=default;};inline Calls calls;
struct DeviceObject {unsigned device{};};
struct Resource:DeviceObject {D3D12_RESOURCE_DESC desc{};D3D12_RESOURCE_DESC GetDesc(){++calls.descriptors;return desc;}};
struct Ref {DeviceObject* value{};DeviceObject* Get()const{return value;}explicit operator bool()const{return value!=nullptr;}};
inline bool SameDevice(DeviceObject* a,DeviceObject* b){++calls.devices;return a&&b&&a->device==b->device;}
enum class CaptureTransfer {Unknown,SrgbEncoded,Linear};constexpr unsigned CaptureColorReadState=7;
struct ProjectionData {CameraConstants constants;Ref queue;uint64_t poseId{},poseWriteId{},origin{},clockGeneration{};int64_t cameraApplicationQpc{},contentTimeNs{},posePredictionNs{};uint32_t view{},frameId{};};
struct EyeStamp {uint64_t sourceSerial{},poseId{},poseWriteId{},spaceEpoch{},clockGeneration{};int64_t cameraApplicationQpc{},contentTimeNs{},posePredictionNs{};uint32_t nativeFrame{},engineView{};bool nativeFrameKnown{};cvr::ppd::ReferenceOptics referenceOptics;};
struct Frame {struct {uint64_t serial{},origin{};bool stereo{};std::array<uint64_t,2>pose{};}identity;struct {uint64_t resourceEpoch{};CaptureTransfer transfer=CaptureTransfer::SrgbEncoded;std::array<EyeStamp,2>eyes;}nativeStamp;std::array<Resource*,2>color{};struct Lease{void* owner{};bool release{};std::array<Resource*,2>resources{};};std::shared_ptr<Lease>lease;struct {bool scheduled=true;unsigned colorState=CaptureColorReadState;Ref fence;bool HasScheduledCopySource()const{++calls.scheduled;return scheduled;}}colorCompletion;std::array<std::shared_ptr<ProjectionData>,2>projection;std::array<uint32_t,2>engineView{};};
class AerRealPresenter {public:static constexpr float CalibrationAngularTolerance=.00002f;static bool ResolvePairedDisplayGeometry(const Frame&,std::array<AerRealGeometry,2>&,bool&,PairedGeometryDiagnostic* =nullptr);};
}
#include "PairedGeometryProduction.hpp"
using namespace cvr::framegen;
struct Environment {Resource color[2];DeviceObject queue,fence;};
static Frame Make(Environment& env){
 Frame f;f.identity.serial=42;f.identity.origin=2;f.identity.stereo=true;f.nativeStamp.resourceEpoch=1;
 f.lease=std::make_shared<Frame::Lease>();f.lease->owner=&env;f.lease->release=true;f.colorCompletion.fence.value=&env.fence;
 const cvr::ppd::PpdRenderedFov raw{-.7f,.7f,.7f,-.7f};const auto optics=cvr::ppd::BuildReferenceOptics({raw,raw});
 for(unsigned eye=0;eye<2;++eye){auto& r=env.color[eye];r.desc.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;r.desc.Width=r.desc.Height=1920;r.desc.DepthOrArraySize=r.desc.MipLevels=r.desc.SampleDesc.Count=1;r.desc.Format=DXGI_FORMAT_R8G8B8A8_UNORM;
  f.color[eye]=f.lease->resources[eye]=&r;f.engineView[eye]=eye+1;f.identity.pose[eye]=10+eye;
  auto& s=f.nativeStamp.eyes[eye];s.sourceSerial=42;s.poseId=10+eye;s.poseWriteId=20+eye;s.spaceEpoch=2;s.clockGeneration=3;s.cameraApplicationQpc=100;s.contentTimeNs=200;s.posePredictionNs=300;s.nativeFrame=123;s.nativeFrameKnown=true;s.engineView=eye+1;s.referenceOptics=optics;
  auto p=std::make_shared<ProjectionData>();p->queue.value=&env.queue;p->poseId=s.poseId;p->poseWriteId=s.poseWriteId;p->origin=s.spaceEpoch;p->clockGeneration=s.clockGeneration;p->cameraApplicationQpc=s.cameraApplicationQpc;p->contentTimeNs=s.contentTimeNs;p->posePredictionNs=s.posePredictionNs;p->view=s.engineView;p->frameId=s.nativeFrame;
  auto& c=p->constants;c.source=CameraConstantsSource::NativeStaging;c.version=1;c.flagPresent.set(4);for(unsigned i=0;i<16;++i)c.valuePresent.set(i);
  const auto fit=cvr::ppd::CalibrateReferenceOptics(1920,1920,optics);const auto expected=cvr::ppd::ReferenceSourceFov(1920,1920,fit);c.values[0]=1.f/std::tan(expected.angleRight);c.values[5]=1.f/std::tan(expected.angleUp);c.values[10]=c.values[11]=1;c.values[14]=-.1f;f.projection[eye]=p;
 }return f;
}
static unsigned tested;
static void Case(PairedGeometryReason expected,unsigned eye,std::function<void(Frame&,Environment&)> mutate={}){
 Environment env;auto frame=Make(env);if(mutate)mutate(frame,env);
 std::array<AerRealGeometry,2> oldOutput{},output{},without{};oldOutput[0].width=output[0].width=without[0].width=777;
 bool oldBaseline=true,baseline=true,noDiagBaseline=true;PairedGeometryDiagnostic diagnostic;
 calls={};const bool old=LegacyPairedDisplayGeometry(frame,oldOutput,oldBaseline);const auto oldCalls=calls;
 calls={};const bool now=AerRealPresenter::ResolvePairedDisplayGeometry(frame,output,baseline,&diagnostic);const auto nowCalls=calls;
 calls={};const bool noDiag=AerRealPresenter::ResolvePairedDisplayGeometry(frame,without,noDiagBaseline,nullptr);
 assert(old==now&&now==noDiag&&oldOutput==output&&output==without&&oldBaseline==baseline&&baseline==noDiagBaseline);
 assert(oldCalls==nowCalls&&oldCalls==calls);assert(diagnostic.reason==expected&&diagnostic.eye==eye);
 if(expected!=PairedGeometryReason::Ready)assert(output[0].width==777);
 if(expected==PairedGeometryReason::SerialMissing)assert(diagnostic.knownBits==0&&calls.devices==0&&calls.descriptors==0);
 if(expected==PairedGeometryReason::SourceSerialMismatch)assert(diagnostic.eyes[eye].knownBits==0&&calls.descriptors==0);
 if(expected==PairedGeometryReason::AnisotropicScale||expected==PairedGeometryReason::CenterX)assert((diagnostic.eyes[eye].knownBits&32)!=0&&diagnostic.eyes[eye].crop[2]==0);
 ++tested;
}
int main(){
 static_assert(std::is_trivially_copyable_v<PairedGeometryDiagnostic>);
 constexpr unsigned global=UINT32_MAX;
 Case(PairedGeometryReason::Ready,global);
 Case(PairedGeometryReason::SerialMissing,global,[](auto& f,auto&){f.identity.serial=f.identity.origin=0;});
 Case(PairedGeometryReason::LeaseMissing,global,[](auto& f,auto&){f.lease.reset();});
 Case(PairedGeometryReason::CopyNotScheduled,global,[](auto& f,auto&){f.colorCompletion.scheduled=false;});
 Case(PairedGeometryReason::ColorState,global,[](auto& f,auto&){f.colorCompletion.colorState=0;});
 Case(PairedGeometryReason::ColorDevice,global,[](auto&,auto& e){e.color[1].device=1;});
 Case(PairedGeometryReason::ProjectionMissing,0,[](auto& f,auto&){f.projection[0].reset();f.nativeStamp.eyes[0].sourceSerial=0;});
 Case(PairedGeometryReason::SourceSerialMismatch,0,[](auto& f,auto&){f.nativeStamp.eyes[0].sourceSerial=40;f.projection[0]->origin=99;});
 Case(PairedGeometryReason::PoseWriteMissing,0,[](auto& f,auto&){f.nativeStamp.eyes[0].poseWriteId=0;});
 Case(PairedGeometryReason::ProjectionContentTime,1,[](auto& f,auto&){f.projection[1]->contentTimeNs++;});
 Case(PairedGeometryReason::ProjectionColorDevice,0,[](auto&,auto& e){e.queue.device=1;});
 Case(PairedGeometryReason::ProjectionFenceDevice,0,[](auto&,auto& e){e.fence.device=1;});
 Case(PairedGeometryReason::PairNativeFrame,1,[](auto& f,auto&){f.nativeStamp.eyes[1].nativeFrame++;f.projection[1]->frameId++;});
 Case(PairedGeometryReason::TextureDimension,0,[](auto&,auto& e){e.color[0].desc.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;});
 Case(PairedGeometryReason::SampleCount,0,[](auto&,auto& e){e.color[0].desc.SampleDesc.Count=2;});
 Case(PairedGeometryReason::ColorFormat,0,[](auto&,auto& e){e.color[0].desc.Format=DXGI_FORMAT_UNKNOWN;});
 Case(PairedGeometryReason::ProjectionInvalid,0,[](auto& f,auto&){f.projection[0]->constants.values[0]=0;});
 Case(PairedGeometryReason::ReferenceFit,0,[](auto&,auto& e){e.color[0].desc.Width=20000;});
 Case(PairedGeometryReason::ScaleXMinimum,0,[](auto& f,auto&){f.projection[0]->constants.values[0]*=.1f;f.projection[0]->constants.values[5]*=.1f;});
 Case(PairedGeometryReason::ScaleYMaximum,0,[](auto& f,auto&){f.projection[0]->constants.values[5]*=20.f;});
 Case(PairedGeometryReason::AnisotropicScale,0,[](auto& f,auto&){f.projection[0]->constants.values[0]*=1.002f;});
 Case(PairedGeometryReason::CenterX,0,[](auto& f,auto&){f.projection[0]->constants.values[8]=.002f;});
 Case(PairedGeometryReason::PairScale,global,[](auto& f,auto&){f.projection[1]->constants.values[0]*=1.002f;f.projection[1]->constants.values[5]*=1.002f;});
 Case(PairedGeometryReason::VrcamViewMissing,global,[](auto& f,auto&){f.nativeStamp.eyes[1].engineView=f.engineView[1]=f.projection[1]->view=1;});
 std::printf("PASS %u exact production paired geometry cases: legacy/current/nullptr results, outputs, first conjunct and device/descriptor call counts\n",tested);
}
