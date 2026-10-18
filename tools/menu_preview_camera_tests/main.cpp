#include "MenuPreview/CameraApplication.hpp"
#include "MenuPreview/ProducerRoute.hpp"
#include <iostream>
#include <stdexcept>
// This standalone policy test does not link the OpenXR runtime adapter.
// Adapter ON/OFF behavior is checked separately with the actual adapter.
namespace cvr::menu_preview {
bool EditorFreshnessEnabled()noexcept{return false;}
void NoteEditorPublication(const CameraApplication*,const CameraApplication*,bool,bool)noexcept{}
}
using namespace cvr::menu_preview;
void Check(bool value,const char* reason){if(!value)throw std::runtime_error(reason);}
int main(){try{
    Metadata m{};m.screen=1;m.screenEpoch=1;m.setupID=0;m.componentIdentity=200;m.virtualCameraNameHash=300;
    PreviewFrameEyeOwner owner;
    Check(!owner.Resolve(m,300,false,100),"missing clock fails closed");
    Check(!owner.Resolve(m,301,true,100),"other view key rejected");
    auto first=owner.Resolve(m,300,true,100);Check(first&&first.eye==0&&first.changed,"first left");
    auto repeat=owner.Resolve(m,300,true,100);Check(repeat.sourceSequence==first.sourceSequence&&repeat.eye==0&&!repeat.changed,"same frame pinned");
    auto next=owner.Resolve(m,300,true,101);Check(next.eye==1&&next.sourceSequence==first.sourceSequence+1,"new native frame alternates");
    Check(!owner.Resolve(m,300,true,100),"late older native frame rejected");
    Check(!owner.Resolve(m,300,false,102),"missing clock cannot advance eye");
    Check(owner.Resolve(m,300,true,102).eye==0,"next valid frame after missing clock");
    m.setupID=8;auto setup=owner.Resolve(m,300,true,102);Check(setup.eye==0&&setup.changed,"setup resets even same frame");
    ++m.screenEpoch;Check(owner.Resolve(m,300,true,102).eye==0,"epoch resets");
    m.componentIdentity=201;Check(owner.Resolve(m,300,true,102).eye==0,"component resets");
    m.screen=99;Check(!owner.Resolve(m,300,true,103),"unsupported screen rejected");m.screen=1;
    owner.Reset();auto wrap=owner.Resolve(m,300,true,UINT32_MAX);Check(wrap.eye==0,"reset");Check(owner.Resolve(m,300,true,0).eye==1,"native clock wrap advances");
    Descriptor base{};const float q[4]{0,0,0,1};std::memcpy(base.data(),q,16);
    const int32_t pos[3]{123,456,789};std::memcpy(base.data()+0x10,pos,12);
    const float optics[4]{32,1,.1f,1000};std::memcpy(base.data()+0x38,optics,16);
    Descriptor left{},right{};Check(MakeEyeDescriptor(base,-.032f,left)&&MakeEyeDescriptor(base,.032f,right),"private eye pair");
    Check(!std::memcmp(left.data()+0x38,optics,16)&&!std::memcmp(right.data()+0x38,optics,16),"preview optics preserved");
    int32_t l{},r{};std::memcpy(&l,left.data()+0x10,4);std::memcpy(&r,right.data()+0x10,4);Check(l==123-4194&&r==123+4194,"fixed-point opposite eyes");
    ClearCameraApplication();auto receipt=std::make_shared<CameraApplication>();receipt->frame=next;receipt->eyeDescriptor=right;
    PublishCameraApplication(receipt);auto retained=ReadCameraApplication();auto older=std::make_shared<CameraApplication>();older->frame=first;
    PublishCameraApplication(older);Check(ReadCameraApplication()->frame.sourceSequence==next.sourceSequence,"out of order receipt cannot replace newer");
    auto duplicate=std::make_shared<CameraApplication>(*retained);
    PublishCameraApplication(duplicate);
    Check(ReadCameraApplication()==retained,"identical repeat retains the application owned by an accepted writer");
    auto changedView=std::make_shared<CameraApplication>(*retained);changedView->eyeDescriptor[0]^=1;
    PublishCameraApplication(changedView);
    Check(ReadCameraApplication()==changedView,"same source with changed rendered camera must publish its actual application");
    auto changedOrigin=std::make_shared<CameraApplication>(*changedView);++changedOrigin->projection.origin;
    PublishCameraApplication(changedOrigin);
    Check(ReadCameraApplication()==changedOrigin,"tracking origin cannot be collapsed under the same source identity");
    auto changedDestination=std::make_shared<CameraApplication>(*changedOrigin);++changedDestination->nativeDestination;
    PublishCameraApplication(changedDestination);
    Check(ReadCameraApplication()==changedDestination,"native destination association remains exact");
    ClearCameraApplication();Check(!ReadCameraApplication()&&retained->eyeDescriptor==right,"clear retains reader immutable snapshot");
    PreviewFrameEyeOwner callbackOwner;
    const auto callbackFirst=callbackOwner.Resolve(m,m.virtualCameraNameHash,true,123);
    const auto callbackRepeat=callbackOwner.Resolve(m,m.virtualCameraNameHash,true,123);
    Check(callbackFirst.changed&&!callbackRepeat.changed&&callbackFirst.sourceSequence==callbackRepeat.sourceSequence,"actual callback repeat keeps source identity");
    auto callbackApp=std::make_shared<CameraApplication>();callbackApp->metadata=m;callbackApp->frame=callbackFirst;callbackApp->eyeDescriptor=right;
    PublishCameraApplication(callbackApp);
    auto repeatApp=std::make_shared<CameraApplication>(*callbackApp);repeatApp->frame=callbackRepeat;
    PublishCameraApplication(repeatApp);
    Check(ReadCameraApplication()==callbackApp,"first repeat bookkeeping cannot orphan a rendered application");
    ClearCameraApplication();
    Check(ParseCameraTestMode("left")==CameraTestMode::Left&&ParseCameraTestMode("right")==CameraTestMode::Right&&ParseCameraTestMode("1")==CameraTestMode::Alternate&&ParseCameraTestMode("alternate")==CameraTestMode::Alternate&&ParseCameraTestMode("off")==CameraTestMode::Off,"explicit modes");
    Check(ParseCameraTestMode("zero")==CameraTestMode::Zero,"zero diagnostic explicit mode");
    Descriptor zero{};Check(MakeEyeDescriptor(base,0.f,zero)&&zero==base,"zero private copy byte identical");
    AppliedProducer p{};p.metadata=m;p.source=wrap;p.base=base;p.applied=left;
    Check(MatchesAppliedProducer(p,m,left)&&!MatchesAppliedProducer(p,m,right),"only actual applied fingerprint admitted");
    auto changed=m;++changed.setupID;Check(!MatchesAppliedProducer(p,changed,left),"late prior setup rejected");
    ConsumingFrameOwner consumed;auto c1=consumed.Resolve(m,true,10,0);auto c2=consumed.Resolve(m,true,11,0);
    Check(c1.eye==0&&c2.eye==0&&c2.sourceSequence==c1.sourceSequence+1,"cached constant left does not invent alternation");
    Check(consumed.Resolve(m,true,11,0).sourceSequence==c2.sourceSequence&&!consumed.Resolve(m,true,11,1),"same consuming frame pinned to actual eye");
    Check(consumed.Resolve(m,true,12,1).eye==1,"actual right producer changes consuming eye");
    Check(!consumed.Resolve(m,false,13,0),"missing consuming clock fails closed");
    DirtyRequestBudget budget;for(int i=0;i<96;++i)Check(budget.Take(),"bounded dirty allowed");Check(!budget.Take()&&budget.Count()==96,"request limit stable");
    std::cout<<"PASS preview frame ownership/reuse/reset/wrap/rejection/optics/immutable receipts/early producer match/modes/cached-eye/dirty limit\n";
}catch(const std::exception& error){std::cerr<<"FAIL "<<error.what()<<'\n';return 1;}}
