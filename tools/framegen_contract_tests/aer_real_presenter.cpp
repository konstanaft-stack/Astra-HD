#include "Framegen/AerRealPresenter.hpp"
#include "Framegen/PairedDisplayHistory.hpp"
#include "Framegen/CapturedHeadPolicy.hpp"
#include "Framegen/NativeGenerationBridge.hpp"
#include "Utils/XrMath.hpp"
#include "CpuD3d.hpp"
#include "Render/NativeObjectIdentity.hpp"
#include <iostream>
#include <limits>
#include <map>
#include <stdexcept>
using namespace cvr::framegen;
using Microsoft::WRL::ComPtr;
static unsigned checks=0;
static void Check(bool value,const char* message){++checks;if(!value)throw std::runtime_error(message);}
struct Fixture {
    ComPtr<CpuDevice> device;ComPtr<CpuFence> fence;ComPtr<CpuQueue> queue;
    std::vector<ComPtr<CpuResource>> resources;
    std::map<ID3D12Resource*,unsigned> pins;
    Fixture(){device.Attach(new CpuDevice);fence.Attach(new CpuFence(device.Get()));queue.Attach(new CpuQueue(device.Get()));fence->completed=8;}
    unsigned Pins()const{unsigned n=0;for(const auto&[r,count]:pins){(void)r;n+=count;}return n;}
    Frame Make(int64_t id){
        const auto eye=unsigned(id&1);Frame f;ComPtr<CpuResource> resource;resource.Attach(new CpuResource(device.Get()));
        auto& d=resource->descriptor;d.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;d.Width=200;d.Height=100;d.DepthOrArraySize=1;d.MipLevels=1;d.SampleDesc.Count=1;d.Format=DXGI_FORMAT_R8G8B8A8_UNORM;
        f.color[eye]=resource.Get();resources.push_back(resource);f.engineView[eye]=1;
        f.identity.serial=uint64_t(id)+100;f.identity.origin=3;f.identity.pose[eye]=uint64_t(id)+2000;
        f.nativeStamp.resourceEpoch=7;f.nativeStamp.transfer=CaptureTransfer::SrgbEncoded;
        auto& s=f.nativeStamp.eyes[eye];s.sourceSerial=f.identity.serial;s.poseId=f.identity.pose[eye];s.poseWriteId=uint64_t(id)+3000;s.spaceEpoch=3;s.clockGeneration=11;s.nativeFrameKnown=true;s.nativeFrame=uint32_t(id)+700;s.engineView=1;
        s.cameraApplicationQpc=id+5000;s.contentTimeNs=id*100+100000;s.posePredictionNs=id*1000+1000000;
        s.mainEyeSource={4,uint64_t(id)+1,6,{1,2,3},id,AerV2Eye(eye),23,s.posePredictionNs,false};
        const float angle=.01f*float(id);XrPosef head{{0,std::sin(angle*.5f),0,std::cos(angle*.5f)},{float(id)*.001f,1.6f,0}};
        const auto offset=RotateVector(head.orientation,{eye?.032f:-.032f,0,0});f.pose[eye]={head.orientation,{head.position.x+offset.x,head.position.y+offset.y,head.position.z+offset.z}};
        s.capturedHead={head.orientation.x,head.orientation.y,head.orientation.z,head.orientation.w,head.position.x,head.position.y,head.position.z};s.capturedHeadValid=true;
        f.fov[eye]={-.2f,.2f,.2f,-.2f}; // Deliberately wrong runtime/fallback FOV.
        auto input=std::make_shared<Inputs>();input->view=1;input->frameId=s.nativeFrame;input->poseId=s.poseId;input->poseWriteId=s.poseWriteId;input->origin=3;input->clockGeneration=11;input->cameraApplicationQpc=s.cameraApplicationQpc;input->contentTimeNs=s.contentTimeNs;input->posePredictionNs=s.posePredictionNs;input->queue=queue.Get();input->width=100;input->height=50;
        auto& c=input->camera.constants;c.source=CameraConstantsSource::Streamline;c.version=2;for(unsigned i=0;i<16;++i)c.valuePresent.set(i);c.flagPresent.set(4);c.values[0]=1;c.values[5]=2;c.values[8]=.2f;c.values[9]=-.1f;c.values[10]=.001f;c.values[11]=1;c.values[14]=.1f;
        f.inputs[eye]=input;f.colorCompletion=CaptureCompletion::AfterSignal(fence.Get(),8,S_OK,{},0);
        f.lease=std::make_shared<ReadLease>();f.lease->owner=this;f.lease->resources[eye]=f.color[eye];++pins[f.color[eye]];f.color[eye]->AddRef();
        f.lease->release=[](void* owner,ID3D12Resource*r,uint64_t){auto* f=static_cast<Fixture*>(owner);Check(f->pins[r]>0,"double unpin");--f->pins[r];r->Release();};
        return f;
    }
};
static bool Near(float a,float b){return std::abs(a-b)<.00001f;}
static std::shared_ptr<Inputs> Edit(Frame&f,unsigned eye){auto p=std::make_shared<Inputs>(*f.inputs[eye]);f.inputs[eye]=p;return p;}
static void FactoryGuards(){
 Fixture f;
 {auto c=AerRealPresenter::FromMainFrame(f.Make(0),AerV2Eye::Left);Check(bool(c),"valid completed MAIN source rejected");
  Check(Near(c.candidate->geometry.renderedFov.angleLeft,-std::atan(1.2f)),"runtime FOV substituted for SL projection");
  Check(Near(c.candidate->frame.fov[0].angleRight,std::atan(.8f)),"native bridge did not receive full source FOV");
  Check(c.candidate->frame.nativeStamp.eyes[0].clockGeneration==11&&c.candidate->receipt.clockGeneration==23,"different clock domains conflated");}
 Check(f.Pins()==0,"factory owner leaked");
 {auto c=AerRealPresenter::FromMainFrame(f.Make(0),AerV2Eye::Left,std::array<float,4>{20,10,180,90});Check(bool(c),"valid crop rejected");
  const auto&t=c.candidate->geometry.tangents;Check(Near(t[0],.45f)&&Near(t[1],.35f)&&Near(t[2],1)&&Near(t[3],.6f),"cropped source rays wrong");}
 for(unsigned which=0;which<12;++which){auto frame=f.Make(0);AerRealCode expected=AerRealCode::InvalidInput;
  switch(which){
  case 0:frame.nativeStamp.eyes[0].capturedHeadValid=false;expected=AerRealCode::MissingCapturedHead;break;
  case 1:frame.inputs[0].reset();expected=AerRealCode::UnsupportedProjection;break;
  case 2:Edit(frame,0)->camera.constants.values[15]=1;expected=AerRealCode::UnsupportedProjection;break;
  case 3:Edit(frame,0)->camera.constants.valuePresent.reset(0);expected=AerRealCode::UnsupportedProjection;break;
  case 4:Edit(frame,0)->poseWriteId++;expected=AerRealCode::UnsupportedProjection;break;
  case 5:frame.pose[0].orientation={0,std::sin(.03f),0,std::cos(.03f)};expected=AerRealCode::UnsupportedCant;break;
  case 6:frame.nativeStamp.eyes[0].mainEyeSource.sourceId=1;break;
  case 7:frame.engineView[0]=2;break;
  case 8:frame.identity.origin=4;break;
  case 9:frame.color[1]=frame.color[0];break;
  case 10:frame.lease.reset();expected=AerRealCode::MissingSourceLease;break;
  case 11:Edit(frame,0)->camera.constants.flags[4]=1;expected=AerRealCode::UnsupportedProjection;break;
  }
  auto result=AerRealPresenter::FromMainFrame(std::move(frame),AerV2Eye::Left);
  Check(!result&&result.code==expected,"invalid capture/geometry guard failed");Check(f.Pins()==0,"rejected candidate pinned the pool");
 }
 f.fence->completed=7;{auto result=AerRealPresenter::FromMainFrame(f.Make(0),AerV2Eye::Left);Check(result.code==AerRealCode::CapturePending,"uncompleted source accepted");}Check(f.Pins()==0,"pending source retained");
 f.fence->completed=UINT64_MAX;{auto result=AerRealPresenter::FromMainFrame(f.Make(0),AerV2Eye::Left);Check(result.code==AerRealCode::DeviceRemoved,"device removal accepted as completion");}Check(f.Pins()==0,"removed candidate retained");
 Check(f.fence->unexpected==0&&f.queue->unexpected==0,"presenter issued wait/execute/event calls");
}
static AerRealSelection Admit(Fixture&f,AerRealPresenter&p,AerRealTick&tick,int64_t id){
 auto candidate=AerRealPresenter::FromMainFrame(f.Make(id),AerV2Eye(unsigned(id&1)));Check(bool(candidate),"fixture capture rejected");
 ++tick.frame;return p.Step(tick,std::move(candidate.candidate));
}
static std::shared_ptr<NativeSelectionLease> Native(Fixture&f,const AerRealSelection&baseline,bool generatedLeft){
 auto lease=std::make_shared<NativeSelectionLease>();lease->device=f.device->Native();auto pair=std::make_shared<cpvr::StereoFrame>();
 pair->pair_id=41;pair->epoch=9001;pair->space_epoch=3;pair->cadence=cpvr::Cadence::AlternateEyes;
 for(unsigned eye=0;eye<2;++eye){const auto&base=*baseline.eyes[eye];auto value=std::make_shared<cpvr::EyeFrame>();
  value->eye=cpvr::Eye(eye);value->epoch=9001;value->space_epoch=3;value->source_id=uint64_t(base.receipt.sourceId)+1;
  value->content_time_ns=base.frame.nativeStamp.eyes[eye].contentTimeNs;value->generated=generatedLeft&&eye==0;
  const auto&pose=base.frame.pose[eye];value->render_pose={{pose.orientation.x,pose.orientation.y,pose.orientation.z,pose.orientation.w},{pose.position.x,pose.position.y,pose.position.z}};
  const auto&view=base.geometry.renderedFov;value->fov={view.angleLeft,view.angleRight,view.angleUp,view.angleDown};
  value->color={base.colorOwner,base.frame.colorCompletion.fence,8,CaptureColorReadState,baseline.OwnerForEye(eye)};
  value->color.source_format=DXGI_FORMAT_R8G8B8A8_UNORM;value->color.transfer=cpvr::ColorTransfer::SrgbEncoded;
  auto&origin=lease->selected.origins[eye];origin={false,value->source_id,value->source_id,0,0,value->content_time_ns,value->content_time_ns,0,0};
  if(value->generated){
   origin={true,value->source_id-2,value->source_id,0,0,value->content_time_ns-200,value->content_time_ns,1,1};
   value->content_time_ns-=100;value->color.resting_state=D3D12_RESOURCE_STATE_COMMON;
   ComPtr<CpuResource> privateColor;privateColor.Attach(new CpuResource(f.device.Get()));privateColor->descriptor=base.colorOwner->GetDesc();
   value->color.texture=privateColor;value->color.lifetime=std::make_shared<int>(1);f.resources.push_back(privateColor);
   // This product's actual head is far from the latest baseline reference.
   const XrPosef productHead{{0,std::sin(.015f),0,std::cos(.015f)},{.75f,1.6f,.2f}};
   auto offset=RotateVector(productHead.orientation,base.geometry.eyeFromHead.position);
   value->render_pose={{productHead.orientation.x,productHead.orientation.y,productHead.orientation.z,productHead.orientation.w},
    {productHead.position.x+offset.x,productHead.position.y+offset.y,productHead.position.z+offset.z}};
  }
  pair->eyes[eye]=value;
 }
 lease->selected.frame=pair;return lease;
}
static void NativeGuards(Fixture&f,const AerRealSelection&baseline){
 auto mixed=Native(f,baseline,true);auto output=BuildAerNativePair(baseline,mixed,9001);
 Check(output.has_value()&&!output->allReal&&output->generated[0]&&!output->generated[1],"mixed native pair rejected/mislabelled");
 Check(output->aerSourceIds==std::array<int64_t,2>{-1,-1},"generated pair invented integer source-clock samples");
 Check(output->sourcePositions==std::array<double,2>{3,5},"continuous source positions did not preserve mixed R/G ordinals");
 const XrPosef identity{{0,0,0,1},{0,0,0}};
 auto selectedHead=RelativePose(baseline.eyes[0]->geometry.eyeFromHead,identity);
 auto outputOffset=RotateVector(output->submitPose[0].orientation,selectedHead.position);
 Check(Near(output->submitPose[0].position.x+outputOffset.x,.75f)&&Near(output->submitPose[0].position.z+outputOffset.z,.2f),"generated source head translated to latest REAL reference");
 Check(Near(output->warp[0].reference[1],baseline.referenceHead.orientation.y),"native orientation reference changed");
 Check(output->bridgeEpoch==9001&&baseline.resourceEpoch==7,"bridge/capture epoch conflated");
 auto real=BuildAerNativePair(baseline,Native(f,baseline,false),9001);Check(real&&real->allReal&&real->aerSourceIds==baseline.sourceIds,"all-real +1 source IDs lost");
 Check(!BuildAerNativePair(baseline,mixed,7),"capture epoch accepted as bridge epoch");
 for(unsigned count=1;count<=5;++count)for(unsigned index=1;index<=count;++index){
  auto fractional=Native(f,baseline,true);auto pair=std::make_shared<cpvr::StereoFrame>(*fractional->selected.frame);
  auto eye=std::make_shared<cpvr::EyeFrame>(*pair->eyes[0]);pair->eyes[0]=eye;fractional->selected.frame=pair;
  auto&origin=fractional->selected.origins[0];origin.index=index;origin.count=count;
  const long double phase=static_cast<long double>(index)/(count+1);
  eye->content_time_ns=origin.previous_time_ns+std::llround(static_cast<long double>(origin.current_time_ns-origin.previous_time_ns)*phase);
  const auto mapped=BuildAerNativePair(baseline,fractional,9001);
  const double expected=double(static_cast<long double>(origin.previous_source_id-1)+2*phase);
  Check(mapped&&std::abs(mapped->sourcePositions[0]-expected)<1e-12,"noninteger native source position rounded or wrong");
 }
 for(unsigned which=0;which<10;++which){auto bad=Native(f,baseline,true);auto pair=std::make_shared<cpvr::StereoFrame>(*bad->selected.frame);auto eye=std::make_shared<cpvr::EyeFrame>(*pair->eyes[0]);pair->eyes[0]=eye;bad->selected.frame=pair;
  switch(which){
  case 0:bad->selected.origins[0]={};break;
  case 1:eye->fov.left+=.02f;break;
  case 2:pair->space_epoch=4;break;
  case 3:eye->color.ready_value=9;break;
  case 4:eye->color.lifetime.reset();break;
  case 5:eye->color.transfer=cpvr::ColorTransfer::Unknown;break;
  case 6:bad->selected.origins[0].index=0;break;
  case 7:eye->color.texture=baseline.eyes[0]->colorOwner;break;
  case 8:bad->selected.origins[0].current_time_ns++;break;
  case 9:eye->content_time_ns++;break;
  }
  Check(!BuildAerNativePair(baseline,bad,9001),"unknown native origin/geometry/readiness accepted");
 }
 auto cropped=baseline;auto owner=std::make_shared<AerRealEye>(*baseline.eyes[0]);owner->geometry.crop[0]=1;cropped.eyes[0]=owner;
 Check(!BuildAerNativePair(cropped,mixed,9001),"cropped calibration silently used for native full image");
}
static void Calibration(){
 Fixture f;AerRealGeometry base;
 {auto result=AerRealPresenter::FromMainFrame(f.Make(0),AerV2Eye::Left);base=result.candidate->geometry;}
 float maxNoise=0;
 for(unsigned i=0;i<2000;++i){
  const float angle=float(i)*.0123f;XrPosef head{{0,std::sin(angle*.5f),0,std::cos(angle*.5f)},
      {4*std::sin(angle),1.6f+std::sin(angle*.1f),3*std::cos(angle)}};
  auto eye=head;const auto offset=RotateVector(head.orientation,base.eyeFromHead.position);
  eye.position={head.position.x+offset.x,head.position.y+offset.y,head.position.z+offset.z};
  auto moved=base;moved.capturedHead=head;moved.eyeFromHead=RelativePose(head,eye);
  const auto&a=moved.eyeFromHead.position;const auto&b=base.eyeFromHead.position;
  maxNoise=std::max(maxNoise,std::sqrt((a.x-b.x)*(a.x-b.x)+(a.y-b.y)*(a.y-b.y)+(a.z-b.z)*(a.z-b.z)));
  Check(AerRealPresenter::SameCalibration(base,moved),"ordinary head/world motion changed calibration");
 }
 auto changed=base;changed.renderedFov.angleLeft+=.001f;Check(!AerRealPresenter::SameCalibration(base,changed),"material projection change ignored");
 changed=base;changed.crop[0]+=.01f;Check(!AerRealPresenter::SameCalibration(base,changed),"material crop change ignored");
 changed=base;changed.width++;Check(!AerRealPresenter::SameCalibration(base,changed),"extent change ignored");
 changed=base;changed.eyeFromHead.position.x+=.001f;Check(!AerRealPresenter::SameCalibration(base,changed),"static IPD change ignored");
 changed=base;changed.eyeFromHead.orientation.w=-changed.eyeFromHead.orientation.w;
 Check(AerRealPresenter::SameCalibration(base,changed),"quaternion sign changed calibration");
 AerRealPresenter presenter;AerRealTick tick{-1,0,-1,8,{1,2,3}};
 auto selected=Admit(f,presenter,tick,0);selected=Admit(f,presenter,tick,1);
 const auto initialRevision=selected.calibrationRevision,initialSourceGeneration=selected.sourceGeneration;
 for(int64_t id=2;id<102;++id){auto frame=f.Make(id);const unsigned eye=unsigned(id&1);
  Edit(frame,eye)->camera.constants.values[0]+=(id&1?1.f:-1.f)*.000001f;
  frame.pose[eye].position.x+=(id&1?1.f:-1.f)*.000001f;
  auto c=AerRealPresenter::FromMainFrame(std::move(frame),AerV2Eye(eye));Check(bool(c),"noise sample invalid");
  ++tick.frame;selected=presenter.Step(tick,std::move(c.candidate));
  Check(selected&&selected.calibrationRevision==initialRevision&&selected.sourceGeneration==initialSourceGeneration,"float noise reset history");
 }
 const auto offer=[&](int64_t id,bool rightIpd,bool newProjection){
  auto frame=f.Make(id);const unsigned eye=unsigned(id&1);
  if(rightIpd){const auto offset=RotateVector(frame.pose[eye].orientation,{.001f,0,0});
   frame.pose[eye].position.x+=offset.x;frame.pose[eye].position.y+=offset.y;frame.pose[eye].position.z+=offset.z;}
  if(newProjection)Edit(frame,eye)->camera.constants.values[0]=1.1f;
  auto c=AerRealPresenter::FromMainFrame(std::move(frame),AerV2Eye(eye));Check(bool(c),"changed calibration sample invalid");
  ++tick.frame;return presenter.Step(tick,std::move(c.candidate));
 };
 selected=offer(103,true,false);Check(!selected&&presenter.HeldEyeCount()==1&&selected.calibrationRevision>initialRevision&&selected.sourceGeneration!=initialSourceGeneration,"changed rig mixed with old opposite eye");
 const auto changedRevision=selected.calibrationRevision;
 selected=offer(103,true,false);Check(!selected&&selected.calibrationRevision==changedRevision,"repeat publication reset calibration again");
 selected=offer(104,false,false);Check(selected&&selected.sourceIds==std::array<int64_t,2>{104,103},"fresh compatible opposite eye did not restore pair");
 selected=offer(106,false,true);Check(!selected&&presenter.HeldEyeCount()==0&&selected.calibrationRevision>changedRevision,"new projection retained old pair");
 const auto pendingRevision=selected.calibrationRevision,pendingGeneration=selected.sourceGeneration;
 selected=offer(106,false,true);Check(!selected&&selected.calibrationRevision==pendingRevision&&selected.sourceGeneration==pendingGeneration,"ineligible changed source caused endless resets");
 selected=offer(107,true,false);Check(selected&&selected.sourceIds==std::array<int64_t,2>{106,107},"fresh queued left did not join the new compatible right");
 selected=offer(108,false,true);Check(selected&&selected.sourceIds==std::array<int64_t,2>{108,107},"new geometry did not require two fresh eyes");
 presenter.Reset();selected={};Check(f.Pins()==0,"calibration signatures retained pixel leases");
 std::cout<<"calibration_motion_sweeps=2000 max_position_noise_m="<<maxNoise
          <<" position_tolerance_m="<<AerRealPresenter::CalibrationPositionTolerance
          <<" angular_tolerance_rad="<<AerRealPresenter::CalibrationAngularTolerance<<'\n';
}
static void NativeCroppedX2Geometry(){
 Fixture f;AerRealPresenter presenter;AerRealTick tick{-1,0,-1,5,{1,2,3}};
 const std::array<std::array<float,4>,2> crops{{{20,10,180,90},{0,10,160,90}}};
 AerRealSelection baseline;
 for(int64_t id=1;id<=4;++id){
  auto candidate=AerRealPresenter::FromMainFrame(f.Make(id),AerV2Eye(unsigned(id&1)),crops[unsigned(id&1)]);
  Check(bool(candidate),"cropped X2 baseline source rejected");
  baseline=presenter.Step({++tick.frame,0,-1,5,{1,2,3}},std::move(candidate.candidate));
 }
 Check(baseline&&baseline.sourceIds==std::array<int64_t,2>{4,3},"cropped X2 baseline pair missing");
 auto native=Native(f,baseline,true);
 // L generated at the midpoint of sources2/4 and real R3 describe one content
 // time. Give this fixture their matching head, rather than NativeGuards' far
 // away head used to detect accidental translation to the latest real pose.
 auto pair=std::make_shared<cpvr::StereoFrame>(*native->selected.frame);
 auto left=std::make_shared<cpvr::EyeFrame>(*pair->eyes[0]);
 const XrPosef head{{0,std::sin(.015f),0,std::cos(.015f)},{.003f,1.6f,0}};
 const auto offset=RotateVector(head.orientation,baseline.eyes[0]->geometry.eyeFromHead.position);
 left->render_pose={{head.orientation.x,head.orientation.y,head.orientation.z,head.orientation.w},
     {head.position.x+offset.x,head.position.y+offset.y,head.position.z+offset.z}};
 pair->eyes[0]=left;native->selected.frame=pair;
 auto output=BuildAerNativePair(baseline,native,9001);
 Check(output.has_value(),"X2 cropped-eye product rejected by full-source-only output gate");
 Check(output->sourcePositions==std::array<double,2>{3,3}&&output->contentTimeNs[0]==output->contentTimeNs[1],"X2 content-time pair changed");
 for(unsigned eye=0;eye<2;++eye){
  Check(output->warp[eye].crop==crops[eye]&&output->warp[eye].tangents==baseline.eyes[eye]->geometry.tangents,"NVIDIA output lost this eye's crop/rays");
  const auto&a=output->fov[eye];const auto&b=baseline.fov[eye];
  Check(Near(a.angleLeft,b.angleLeft)&&Near(a.angleRight,b.angleRight)&&Near(a.angleUp,b.angleUp)&&Near(a.angleDown,b.angleDown),"NVIDIA output submitted full-source FOV for a cropped eye");
 }
 const auto&a=output->submitPose[0].position;const auto&b=output->submitPose[1].position;
 Check(Near(std::sqrt((a.x-b.x)*(a.x-b.x)+(a.y-b.y)*(a.y-b.y)+(a.z-b.z)*(a.z-b.z)),.064f),"co-timed cropped X2 pair lost its stereo baseline");
 output.reset();native.reset();pair.reset();left.reset();baseline={};presenter.Reset();
 Check(f.Pins()==0,"cropped X2 fixture leaked capture ownership");
}
static void AdmissionAndLeases(){
 Fixture f;AerRealPresenter p;AerRealTick tick{-1,0,-1,5,{1,2,3}};auto selected=Admit(f,p,tick,0);
 Check(!selected&&selected.sourceIds==std::array<int64_t,2>{0,-1}&&p.HeldEyeCount()==1,"missing eye was fabricated");
 selected=Admit(f,p,tick,1);Check(bool(selected)&&selected.referenceReceipt.sourceId==1&&selected.newlyAccepted[1],"right admission failed");
 selected=Admit(f,p,tick,2);Check(bool(selected)&&selected.sourceIds==std::array<int64_t,2>{2,1},"left window failed");
 selected=Admit(f,p,tick,4);Check(selected.candidateCode==AerRealCode::Ineligible&&f.Pins()==3&&p.QueuedEyeCount(0)==1,"future left was not retained within the original queue capacity");
 selected=Admit(f,p,tick,5);Check(selected.sourceIds==std::array<int64_t,2>{4,5},"right-first lower bound did not admit the retained left source");
 selected=Admit(f,p,tick,4);Check(selected.referenceReceipt.sourceId==4&&selected.sourceIds==std::array<int64_t,2>{4,5},"reference used largest ID instead of last admission");
 Check(Near(selected.referenceHead.position.x,.004f)&&selected.sourceClockGeneration==23&&selected.captureClockGeneration==11,"reference/clock provenance lost");
 const auto offset=RotateVector(selected.referenceHead.orientation,selected.eyes[1]->geometry.eyeFromHead.position);
 Check(Near(selected.submitPose[1].position.x,selected.referenceHead.position.x+offset.x),"REAL D1 did not reproduce full reference XYZ");
 selected=Admit(f,p,tick,4);Check(selected.candidateCode==AerRealCode::Duplicate&&!selected.newlyAccepted[0]&&!selected.newlyAccepted[1]&&f.Pins()==2,"duplicate was new admission/retained");
 NativeGuards(f,selected);
 auto gpuOwner=selected.OwnerForEye(0);p.Reset();selected={};Check(f.Pins()==1,"reset released an external GPU source owner");gpuOwner.reset();Check(f.Pins()==0,"GPU source owner failed to release pooled lease");
 selected=Admit(f,p,tick,0);auto candidate=AerRealPresenter::FromMainFrame(f.Make(1),AerV2Eye::Right);++tick.frame;tick.epochs.gameWorld=9;
 selected=p.Step(tick,std::move(candidate.candidate));Check(!selected&&selected.candidateCode==AerRealCode::EpochMismatch&&p.HeldEyeCount()==0,"foreign world mixed into old selection");
 auto forged=AerRealPresenter::FromMainFrame(f.Make(1),AerV2Eye::Right);forged.candidate->receipt.epochs=tick.epochs;++tick.frame;
 selected=p.Step(tick,std::move(forged.candidate));Check(!selected&&selected.candidateCode==AerRealCode::EpochMismatch&&p.HeldEyeCount()==0,"handcrafted receipt hid the frame's actual old epoch");
 selected={};p.Reset();Check(f.Pins()==0,"epoch reset retained source pins");Check(f.fence->unexpected==0&&f.queue->unexpected==0,"metadata presenter touched GPU commands/waits");
}
static void CameraRouteHistory(){
 Fixture f;AerRealPresenter p;AerRealTick tick{-1,0,-1,5,{1,2,3}};
 auto offer=[&](int64_t id,uint64_t latch){
  auto frame=f.Make(id);frame.nativeStamp.eyes[unsigned(id&1)].mainEyeSource.latchRevision=latch;
  auto candidate=AerRealPresenter::FromMainFrame(std::move(frame),AerV2Eye(unsigned(id&1)));
  Check(bool(candidate),"route fixture rejected before presenter");
  ++tick.frame;return p.Step(tick,std::move(candidate.candidate));
 };
 auto selected=offer(0,6);selected=offer(1,6);
 Check(bool(selected),"FPP pair did not initialize");
 auto gpuOwner=selected.OwnerForEye(0);const auto fppGeneration=selected.sourceGeneration;
 selected=offer(2,7);
 Check(!selected&&p.HeldEyeCount()==0&&selected.sourceGeneration!=fppGeneration,"scene reset retained FPP history or admitted left before right");
 Check(f.Pins()==2&&p.QueuedEyeCount(0)==1,"camera route reset lost GPU reader/new pending source or retained old eyes");
 selected=offer(3,7);Check(selected&&selected.sourceIds==std::array<int64_t,2>{2,3},"scene right eye did not join its freshly queued left");
 selected=offer(4,7);Check(bool(selected)&&selected.sourceIds==std::array<int64_t,2>{4,3},"scene pair requires two fresh eyes");
 const auto sceneGeneration=selected.sourceGeneration;
 selected=offer(6,6);Check(selected.candidateCode==AerRealCode::EpochMismatch&&selected.sourceGeneration==sceneGeneration,"late FPP receipt reentered scene history");
 selected=offer(5,8);Check(!selected&&p.HeldEyeCount()==1&&selected.sourceGeneration!=sceneGeneration,"return to FPP retained a scene eye");
 selected=offer(6,8);Check(bool(selected)&&selected.sourceIds==std::array<int64_t,2>{6,5},"return to FPP lacks a coherent fresh pair");
 p.Reset();selected={};Check(f.Pins()==1,"route transitions corrupted outstanding GPU ownership");
 gpuOwner.reset();Check(f.Pins()==0,"route test retained a resource");
}
static void SceneCapturedHeadProof(){
 Fixture f;auto frame=f.Make(1);const auto& receipt=frame.nativeStamp.eyes[1].mainEyeSource;
 auto volatile gate=&CapturedHeadTrusted;
 Check(gate(false,false,{},0,0),"legacy component capture changed");
 Check(gate(true,true,receipt,3,receipt.requestedPoseTimeNs),"proved scene tracking head rejected");
 Check(!gate(true,false,receipt,3,receipt.requestedPoseTimeNs),"unvalidated external source trusted");
 Check(!gate(true,true,{},3,receipt.requestedPoseTimeNs),"missing scene receipt trusted");
 Check(!gate(true,true,receipt,4,receipt.requestedPoseTimeNs),"foreign scene tracking origin trusted");
 Check(!gate(true,true,receipt,3,receipt.requestedPoseTimeNs+1),"different scene sample time trusted");
}
static void ReferenceOpticsIngress(){
 Fixture f;
 const auto make=[&](){
  auto v=f.Make(0);auto* resource=static_cast<CpuResource*>(v.color[0]);resource->descriptor.Width=1632;resource->descriptor.Height=1744;
  auto& optics=v.nativeStamp.eyes[0].referenceOptics;
  optics=cvr::ppd::BuildReferenceOptics({cvr::ppd::PpdRenderedFov{-.9424777627f,.6981316805f,.767595768f,-.9471901655f},
    cvr::ppd::PpdRenderedFov{-.6981316805f,.9424777627f,.767595768f,-.9471901655f}});
  auto input=Edit(v,0);auto& c=input->camera.constants;c.values[0]=1/1.0812467336654663f;c.values[5]=1/1.1554498672485352f;c.values[8]=c.values[9]=0;
  return v;
 };
 auto v=make();
 for(unsigned eye=0;eye<2;++eye){
  AerRealGeometry paired;paired.width=1632;paired.height=1744;
  const auto& optics=v.nativeStamp.eyes[0].referenceOptics;
  Check(AerRealPresenter::ResolveCapturedGeometry(v.inputs[0]->camera.constants,optics,eye,paired),"paired native optics rejected");
  const auto expected=cvr::ppd::ReferenceEyeCrop(1632,1744,cvr::ppd::CalibrateReferenceOptics(1632,1744,optics),optics.applied[eye]);
  Check(paired.crop==expected,"paired crop differs from reference image path");
  Check(Near(paired.effectiveFov.angleLeft,optics.applied[eye].angleLeft),"paired output FOV differs from reference");
  auto wrong=v.inputs[0]->camera.constants;wrong.values[0]=.5f;
  Check(!AerRealPresenter::ResolveCapturedGeometry(wrong,optics,eye,paired),"paired path accepts wrong source projection");
 }
 auto result=AerRealPresenter::FromMainFrame(std::move(v),AerV2Eye::Left);
 Check(bool(result),"valid reference optics source rejected");
 Check(result.candidate->geometry.crop==std::array<float,4>{7,0,1626,1744},"reference crop omitted at real image ingress");
 Check(Near(result.candidate->geometry.effectiveFov.angleRight,.8203047513961792f),"submit uses full-game FOV instead of applied eye FOV");
 v=make();Edit(v,0)->camera.constants.values[0]=.5f;
 Check(!AerRealPresenter::FromMainFrame(std::move(v),AerV2Eye::Left),"different rendered projection accepted as original optics");
}
static void PairedZoomDisplayRegression(){
 Fixture f;
 const auto optics=cvr::ppd::BuildReferenceOptics({
  cvr::ppd::PpdRenderedFov{-.9424777627f,.6981316805f,.767595768f,-.9471901655f},
  cvr::ppd::PpdRenderedFov{-.6981316805f,.9424777627f,.767595768f,-.9471901655f}});
 const auto fit=cvr::ppd::CalibrateReferenceOptics(1920,1920,optics);
 const auto source=cvr::ppd::ReferenceSourceFov(1920,1920,fit);
 const float px=1/std::tan(source.angleRight),py=1/std::tan(source.angleUp);
 const auto make=[&](float zoom){
  auto pair=f.Make(0);auto right=f.Make(1);
  pair.color[1]=right.color[1];pair.pose[1]=right.pose[1];pair.identity.pose[1]=right.identity.pose[1];
  pair.nativeStamp.eyes[1]=right.nativeStamp.eyes[1];pair.inputs[1]=right.inputs[1];
  pair.lease->resources[1]=right.lease->resources[1];right.lease->resources[1]=nullptr;
  pair.identity.stereo=true;pair.identity.nativeFrame=700;
  for(unsigned eye=0;eye<2;++eye){
   auto* resource=static_cast<CpuResource*>(pair.color[eye]);resource->descriptor.Width=resource->descriptor.Height=1920;
   auto& stamp=pair.nativeStamp.eyes[eye];stamp.sourceSerial=pair.identity.serial;stamp.nativeFrame=700;
   stamp.engineView=pair.engineView[eye]=eye+1;stamp.renderedWidth=stamp.renderedHeight=1920;stamp.referenceOptics=optics;
   auto input=Edit(pair,eye);input->view=stamp.engineView;input->frameId=stamp.nativeFrame;
   input->camera.constants.values[0]=px*zoom;input->camera.constants.values[5]=py*zoom;
   input->camera.constants.values[8]=input->camera.constants.values[9]=0;
   pair.projection[eye]=SnapshotFrameProjection(*input);
  }
  return pair;
 };
 std::array<AerRealGeometry,2> baseline{};bool calibrated=false;
 {auto pair=make(1);Check(AerRealPresenter::ResolvePairedDisplayGeometry(pair,baseline,calibrated)&&calibrated,
    "ordinary paired frame no longer satisfies calibration");}
 {auto pair=make(1);ComPtr<CpuDevice> wrapper;wrapper.Attach(new CpuDevice(f.device.Get()));
  Check(cvr::gpu::RegisterNativeObject(f.device.Get()),"register native fixture device");
  for(unsigned eye=0;eye<2;++eye)static_cast<CpuResource*>(pair.color[eye])->device=wrapper.Get();
  std::array<AerRealGeometry,2> geometry{};
  Check(AerRealPresenter::ResolvePairedDisplayGeometry(pair,geometry,calibrated),"same native device returned through resource wrapper rejected");
  ComPtr<CpuDevice> unrelated;unrelated.Attach(new CpuDevice);
  Check(cvr::gpu::RegisterNativeObject(unrelated.Get()),"register different fixture device");
  static_cast<CpuResource*>(pair.color[1])->device=unrelated.Get();
  Check(!AerRealPresenter::ResolvePairedDisplayGeometry(pair,geometry,calibrated),"different native device must remain rejected");}
 // The live black-HMD capture had both current eyes and projections, but
 // p00/p11 changed from .951442003 to .951578915 / 1.04619598.
 for(float zoom:{1.0001439f,1.099588f,2.f,4.f}){
  auto pair=make(zoom);std::array<AerRealGeometry,2> geometry{};
  Check(AerRealPresenter::ResolvePairedDisplayGeometry(pair,geometry,calibrated),
    "paired display rejects the measured native scanner/zoom projection");
  Check(!calibrated,"zoom must not be admitted as a new PPD calibration");
  for(unsigned eye=0;eye<2;++eye){
   Check(geometry[eye].crop==baseline[eye].crop,"zoom changed the source-pixel crop and cancelled magnification");
   Check(Near(geometry[eye].effectiveFov.angleLeft,baseline[eye].effectiveFov.angleLeft)&&
     Near(geometry[eye].effectiveFov.angleUp,baseline[eye].effectiveFov.angleUp),"zoom narrowed the physical HMD field of view");
   Check(geometry[eye].renderedFov.angleRight<baseline[eye].renderedFov.angleRight,
     "native source projection was replaced by a guessed baseline");
   auto strict=geometry[eye];
   Check(!AerRealPresenter::ResolveCapturedGeometry(pair.projection[eye]->constants,optics,eye,strict),
     "strict AER/PPD calibration was relaxed to accept native zoom");
  }
 }
 {auto pair=make(1.099588f);pair.projection[1]->constants.values[0]*=1.00047f;pair.projection[1]->constants.values[5]*=1.00047f;
  std::array<AerRealGeometry,2> geometry{};
  Check(AerRealPresenter::ResolvePairedDisplayGeometry(pair,geometry,calibrated),"measured inter-eye transition residual rejected");}
 // Reject malformed optics and stale identities even though the image itself
 // is available. The display contract must not become a generic bypass.
 for(unsigned defect=0;defect<20;++defect){
  auto pair=make(1.099588f);
  switch(defect){
  case 0:pair.projection[1]->constants.values[0]*=1.08f;break;
  case 1:pair.projection[1]->constants.values[0]*=1.08f;pair.projection[1]->constants.values[5]*=1.08f;break;
  case 2:pair.projection[1]->constants.values[1]=.01f;break;
  case 3:pair.projection[0]->constants.values[8]=.1f;pair.projection[1]->constants.values[8]=.1f;break;
  case 4:pair.projection[1]->constants.values[5]=std::numeric_limits<float>::quiet_NaN();break;
  case 5:pair.projection[1].reset();break;
  case 6:++pair.projection[1]->frameId;break;
  case 7:++pair.projection[1]->poseId;break;
  case 8:++pair.projection[1]->poseWriteId;break;
  case 9:++pair.projection[1]->origin;break;
  case 10:++pair.projection[1]->clockGeneration;break;
  case 11:++pair.projection[1]->cameraApplicationQpc;break;
  case 12:++pair.projection[1]->contentTimeNs;break;
  case 13:++pair.projection[1]->posePredictionNs;break;
  case 14:pair.projection[1]->view=1;break;
  case 15:pair.nativeStamp.eyes[1].referenceOptics.valid=false;break;
  case 16:pair.lease.reset();break;
  case 17:pair.color[1]=pair.color[0];break;
  case 18:pair.projection[1]->queue.Reset();break;
  case 19:++pair.nativeStamp.eyes[1].sourceSerial;break;
  }
  std::array<AerRealGeometry,2> geometry{};
  Check(!AerRealPresenter::ResolvePairedDisplayGeometry(pair,geometry,calibrated),"invalid/stale paired zoom accepted");
 }
 {auto pair=make(1.1f);for(unsigned eye=0;eye<2;++eye){pair.engineView[eye]=2-eye;pair.nativeStamp.eyes[eye].engineView=2-eye;pair.projection[eye]->view=2-eye;}
  std::array<AerRealGeometry,2> geometry{};Check(AerRealPresenter::ResolvePairedDisplayGeometry(pair,geometry,calibrated),"MAIN on right eye is unsupported");}
 Check(f.Pins()==0,"paired display validation retained a capture resource");
 Check(f.fence->unexpected==0&&f.queue->unexpected==0,"paired validation waited for GPU or submitted work");
}
static void PairedHistoryRecovery(){
 PairedDisplayHistory history;
 Check(!history.Update(true,1000000000),"ordinary view unnecessarily disables generation");
 Check(history.Update(false,1010000000),"changed projection retained generation history");
 for(int64_t t=1020000000;t<2020000000;t+=10000000){
  const bool baseline=(t/10000000)%2==0;
  Check(history.Update(baseline,t),"small FOV residuals oscillate generation resets");
 }
 Check(history.Update(true,2100000000),"first restored frame prematurely resumed generation");
 Check(history.Update(true,2349999999),"generation resumed before baseline settled");
 Check(!history.Update(true,2350000000),"generation did not resume after stable baseline");
 Check(history.Update(false,2400000000),"next scanner entry did not discard old history");
 Check(history.Update(true,2500000000),"recovery lost its settling window");
 Check(history.Update(true,1000000000),"clock reset accidentally accepted history");
 Check(!history.Update(true,1250000000),"history did not recover after clock reset");
}
int main(){try{
 PairedHistoryRecovery();
 PairedZoomDisplayRegression();
 NativeCroppedX2Geometry();
 ReferenceOpticsIngress();
 FactoryGuards();AdmissionAndLeases();Calibration();CameraRouteHistory();SceneCapturedHeadProof();
 std::cout<<"PASS AerRealPresenter production checks="<<checks<<" gpu_execution=0 source_clock_writes=0\n";return 0;
}catch(const std::exception&e){std::cerr<<"FAIL "<<e.what()<<'\n';return 1;}}
