#include "MenuPreview/CameraApplication.hpp"
#include "MenuPreview/ProducerRoute.hpp"
#include <iostream>
#include <stdexcept>
using namespace cvr::menu_preview;
void Check(bool value,const char* reason){if(!value)throw std::runtime_error(reason);}
void RunCameraTests(){
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
    m.screen=2;m.setupID=6;Check(owner.Resolve(m,300,true,103).eye==0,"appearance scope starts its own eye pair");
    m.screen=3;Check(!owner.Resolve(m,300,true,104),"unsupported screen rejected");m.screen=1;
    Check(ValidPublication(1,1,true,11,0,1)&&ValidPublication(2,1,true,11,6,1),"inventory and UI Skin admitted");
    Check(!ValidPublication(2,1,true,11,5,1)&&!ValidPublication(2,1,true,11,7,1)&&!ValidPublication(2,1,true,11,6,2),"appearance setup and unique RTT required");
    struct TestScope {Metadata metadata;};ScopeStore<TestScope> scopes;const auto editorEpoch=scopes.Begin();auto editorScope=std::make_shared<TestScope>();
    editorScope->metadata.screen=2;editorScope->metadata.screenEpoch=editorEpoch;Check(scopes.Publish(editorScope)&&scopes.Read()==editorScope,"appearance scope stored");
    Check(scopes.Clear(editorEpoch)&&!scopes.Read(),"appearance scope cleared");
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
    ClearCameraApplication();Check(!ReadCameraApplication()&&retained->eyeDescriptor==right,"clear retains reader immutable snapshot");
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
}

#include "MenuPreview/Producer.hpp"
#include <iostream>
#include <stdexcept>
using namespace cvr::menu_preview;
void PCheck(bool v,const char* why){if(!v)throw std::runtime_error(why);}
template<class T> void Write(Descriptor& d,size_t o,T v){std::memcpy(d.data()+o,&v,sizeof(v));}
int32_t Pos(const Descriptor& d,unsigned i){int32_t v;std::memcpy(&v,d.data()+0x10+i*4,4);return v;}
void RunDescriptorTests(){
    Descriptor d{};for(size_t i=0;i<d.size();++i)d[i]=uint8_t(i+1);
    for(unsigned i=0;i<4;++i)Write(d,i*4,i==3?1.f:0.f);
    for(unsigned i=0;i<3;++i)Write(d,0x10+i*4,int32_t(100000+i*200000));
    Descriptor l{},r{};PCheck(MakeEyeDescriptor(d,-.032f,l)&&MakeEyeDescriptor(d,.032f,r),"identity eyes");
    PCheck(Pos(l,0)==Pos(d,0)-4194&&Pos(r,0)==Pos(d,0)+4194,"native truncation");
    for(size_t i=0;i<d.size();++i)if(i<0x10||i>=0x1C)PCheck(l[i]==d[i]&&r[i]==d[i],"all optics/off-axis/scene bytes preserved");
    PCheck(Pos(l,1)==Pos(d,1)&&Pos(l,2)==Pos(d,2),"other positions preserved");
    const float h=std::sqrt(.5f);Write(d,8,h);Write(d,12,h);
    PCheck(MakeEyeDescriptor(d,.032f,r)&&std::abs(Pos(r,1)-Pos(d,1)-4194)<=1&&std::abs(Pos(r,0)-Pos(d,0))<=1,"90 degree yaw rotates right to Y");
    Write(d,8,0.f);Write(d,4,h);
    PCheck(MakeEyeDescriptor(d,.032f,r)&&std::abs(Pos(r,2)-Pos(d,2)+4194)<=1,"90 degree pitch rotates right to minus Z");
    Write(d,0,std::numeric_limits<float>::quiet_NaN());const auto previous=r;
    PCheck(!MakeEyeDescriptor(d,.032f,r)&&r==previous,"NaN rejected without output mutation");
    Write(d,0,0.f);PCheck(!MakeEyeDescriptor(d,.2f,r),"excess IPD rejected");
    for(unsigned i=0;i<4;++i)Write(d,i*4,i==3?1.f:0.f);
    Write(d,0x10,std::numeric_limits<int32_t>::max());PCheck(!MakeEyeDescriptor(d,.032f,r),"overflow rejected");
    Metadata m{};m.screen=1;m.screenEpoch=3;m.componentIdentity=100;m.virtualCameraNameHash=9;m.publishedAtMs=1000;
    PCheck(MatchesProducerIdentity(m,100,1500)&&!MatchesProducerIdentity(m,101,1000)&&!MatchesProducerIdentity(m,100,1501),"identity/staleness");
    int slot=1;{ScopedProducer outer(slot,2);PCheck(slot==2,"outer installed");{ScopedProducer nested(slot,0);PCheck(slot==0,"invalid nested cleared");}PCheck(slot==2,"nested restored");try{ScopedProducer nested(slot,3);throw 1;}catch(int){}PCheck(slot==2,"exception unwinds");}PCheck(slot==1,"outer restored");
    std::cout<<"PASS preview descriptor optics/axes/fixed-point/rejection and nested producer scope\n";
}

#include "MenuPreview/LocalComponentPose.hpp"
#include "MenuPreview/ImageAliasObservation.hpp"
#include "MenuPreview/WidgetAliasObservation.hpp"
#include "MenuPreview/ReaderObservation.hpp"
void RunLocalComponentTests(){
    LocalComponentPose p;p.metadata.screen=1;p.metadata.screenEpoch=7;p.metadata.setupID=0;p.metadata.componentIdentity=55;p.metadata.virtualCameraNameHash=66;
    const int32_t position[3]{-13014500,80421911,-27960917};const float q[4]{0,0,0,1};
    std::memcpy(p.baseline.data(),position,12);std::memcpy(p.baseline.data()+16,q,16);
    Descriptor source{};std::memcpy(source.data(),q,16);std::memcpy(source.data()+16,position,12);
    for(uint32_t eye=0;eye<2;++eye){Descriptor applied{};Check(MakeEyeDescriptor(source,eye?.032f:-.032f,applied),"constant side descriptor");
        p.eye=eye;std::memcpy(p.applied.data(),applied.data()+16,12);std::memcpy(p.applied.data()+16,q,16);
        Check(VerifyLocalEyeDelta(p,.032f),"measured left/right native world delta");
        float origin[4]{0,0,0,1};for(unsigned i=0;i<3;++i){int32_t v{};std::memcpy(&v,p.applied.data()+i*4,4);origin[i]=float(v)/131072.f;}
        Check(MatchesLocalRenderOrigin(p,origin),"consumed R8 origin coherent");origin[0]+=.01f;
        Check(!MatchesLocalRenderOrigin(p,origin),"incoherent render origin rejected");
        Check(!MatchesLocalComponentPose(p,p.metadata,applied),"unverified baseline cannot label eye");
        p.verified=true;Check(MatchesLocalComponentPose(p,p.metadata,applied),"exact consumed component pose");
        auto changed=p.metadata;++changed.screenEpoch;Check(!MatchesLocalComponentPose(p,changed,applied),"stale epoch rejects consumed pose");
        auto original=source;Check(!MatchesLocalComponentPose(p,p.metadata,original),"unshifted consumer rejects applied eye");
        p.eye=eye^1u;Check(!VerifyLocalEyeDelta(p,.032f),"physical side cannot be guessed");p.verified=false;
    }
    p.eye=UINT32_MAX;Check(!VerifyLocalEyeDelta(p,.032f),"unlabelled diagnostic rejected");
    auto immutable=std::make_shared<LocalComponentPose>(p);localComponentPose.store(immutable);auto retained=ReadLocalComponentPose();ClearLocalComponentPose();
    Check(!ReadLocalComponentPose()&&retained->metadata.screenEpoch==7,"clear keeps immutable prior read lifetime");
    Check(ExactImageResourceAlias(100,100,true),"current exact UI CPU resource alias");
    Check(!ExactImageResourceAlias(100,100,false),"pre-scope candidate never ownership proof");
    Check(!ExactImageResourceAlias(100,101,true)&&!ExactImageResourceAlias(0,0,true),"mismatch/null CPU alias rejected");
    Check(ExactWidgetResourceAlias(100,100,true,true),"typed widget exact stable resource alias");
    Check(!ExactWidgetResourceAlias(100,100,false,true)&&!ExactWidgetResourceAlias(100,100,true,false),"current scope and stable snapshot mandatory");
    Check(!ExactWidgetResourceAlias(100,101,true,true)&&!ExactWidgetResourceAlias(0,0,true,true),"typed widget mismatch/null resource fails closed");
    WidgetAliasObservation alias;alias.metadata=p.metadata;alias.resource=100;alias.componentResource=100;alias.aliasMatch=true;alias.snapshotStable=true;
    Check(ExactReaderAlias(alias,p.metadata,100),"existing accessor exact typed alias");
    auto nextScope=p.metadata;++nextScope.screenEpoch;Check(!ExactReaderAlias(alias,nextScope,100),"old alias cannot admit new scope reader");
    Check(!ExactReaderAlias(alias,p.metadata,101),"unrelated DynamicTexture reader rejected");
    alias.aliasMatch=false;Check(!ExactReaderAlias(alias,p.metadata,100),"candidate reader not admitted");
    std::cout<<"PASS supported local component side/delta/consumed fingerprint/stale scope/unlabelled/immutable reset\n";
}
int main(){try{RunDescriptorTests();RunCameraTests();RunLocalComponentTests();return 0;}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
