#include "Framegen/SrHistoryOwner.hpp"
#include <iostream>
#include <stdexcept>
#include <vector>
#include <cstring>
#include <limits>
#include "SrOwnerNativeFixture.hpp"
#include "../../src/Framegen/SrHistoryNativeParams.hpp"
using namespace cvr::framegen;
void Log(const char*,...){}
static unsigned checks{};
static void Check(bool ok,const char* message){++checks;if(!ok)throw std::runtime_error(message);}
struct Parameters final:NVSDK_NGX_Parameter {
    void Set(const char*,unsigned long long)override{}void Set(const char*,float)override{}void Set(const char*,double)override{}
    void Set(const char*,unsigned int)override{}void Set(const char*,int)override{}void Set(const char*,ID3D11Resource*)override{}
    void Set(const char*,ID3D12Resource*)override{}void Set(const char*,void*)override{}void Reset()override{}
    NVSDK_NGX_Result Get(const char*,unsigned long long*)const override{return NVSDK_NGX_Result_FAIL_InvalidParameter;}
    NVSDK_NGX_Result Get(const char*,float*)const override{return NVSDK_NGX_Result_FAIL_InvalidParameter;}
    NVSDK_NGX_Result Get(const char*,double*)const override{return NVSDK_NGX_Result_FAIL_InvalidParameter;}
    NVSDK_NGX_Result Get(const char*,unsigned int*v)const override{*v=64;return NVSDK_NGX_Result_Success;}
    NVSDK_NGX_Result Get(const char*,int*v)const override{*v=NVSDK_NGX_DLSS_Feature_Flags_MVLowRes|NVSDK_NGX_DLSS_Feature_Flags_DepthInverted;return NVSDK_NGX_Result_Success;}
    NVSDK_NGX_Result Get(const char*,ID3D11Resource**)const override{return NVSDK_NGX_Result_FAIL_InvalidParameter;}
    NVSDK_NGX_Result Get(const char*,ID3D12Resource**)const override{return NVSDK_NGX_Result_FAIL_InvalidParameter;}
    NVSDK_NGX_Result Get(const char*,void**)const override{return NVSDK_NGX_Result_FAIL_InvalidParameter;}
};
struct Calls {
    SrHistoryOwner* owner{};unsigned creates{},evaluations{},releases{};bool failOriginal{},failClone{},aliasClone{},failRelease{};
    bool sequentialHandles{},failCompanionRelease{},throwReleaseOnce{};std::function<void(unsigned)> onCreate;
    NVSDK_NGX_Parameter* borrowed{};std::vector<const NVSDK_NGX_Handle*> released;
} calls;
static NVSDK_NGX_Result NVSDK_CONV Create(ID3D12GraphicsCommandList*,NVSDK_NGX_Feature,NVSDK_NGX_Parameter*p,NVSDK_NGX_Handle**out){
    const auto callNumber=++calls.creates;Check(p==calls.borrowed,"companion did not borrow the identical Create parameter object");
    if(calls.owner)(void)calls.owner->Status(); // no metadata mutex across native call
    if((calls.creates==1&&calls.failOriginal)||(calls.creates==2&&calls.failClone))return NVSDK_NGX_Result_FAIL_FeatureNotSupported;
    if(calls.onCreate)calls.onCreate(callNumber);
    *out=reinterpret_cast<NVSDK_NGX_Handle*>(calls.sequentialHandles?uintptr_t(0x1000+callNumber*16):uintptr_t(callNumber==1||calls.aliasClone?0x1110:0x2220));return NVSDK_NGX_Result_Success;
}
static NVSDK_NGX_Result NVSDK_CONV Evaluate(ID3D12GraphicsCommandList*,const NVSDK_NGX_Handle*,const NVSDK_NGX_Parameter*,PFN_NVSDK_NGX_ProgressCallback){
    ++calls.evaluations;if(calls.owner)(void)calls.owner->Status();return NVSDK_NGX_Result_FAIL_UnsupportedInputFormat;
}
static NVSDK_NGX_Result NVSDK_CONV Release(NVSDK_NGX_Handle*h){++calls.releases;calls.released.push_back(h);if(calls.owner)(void)calls.owner->Status();
    if(calls.throwReleaseOnce){calls.throwReleaseOnce=false;throw std::runtime_error("injected release exception before effect");}
    return calls.failRelease||(calls.failCompanionRelease&&h==reinterpret_cast<NVSDK_NGX_Handle*>(0x2220))?NVSDK_NGX_Result_FAIL_PlatformError:NVSDK_NGX_Result_Success;}
static SrNativeApiContext Context(uint32_t feature=1){SrNativeApiContext c;c.originals={Create,Evaluate,Release};c.coreModule=0x123400;c.moduleGeneration=2;c.lifecycleGeneration=3;
    c.featureId=feature;c.featureKnown=c.modulePinned=true;c.createOrdinal=7;return c;}
static void CloneOnlyActualCreate(){Parameters parameters;calls={};calls.borrowed=&parameters;SrHistoryOwner owner({});calls.owner=&owner;owner.SetRequested(true);
    auto context=Context();NVSDK_NGX_Handle* original{};const auto result=owner.Create(context,nullptr,NVSDK_NGX_Feature_SuperSampling,&parameters,&original);
    Check(result==NVSDK_NGX_Result_Success&&original==reinterpret_cast<NVSDK_NGX_Handle*>(0x1110),"original Create result/handle changed");
    Check(calls.creates==2&&owner.Status().livePairs==1,"actual successful SR Create did not form one companion pair");
    const auto error=owner.Evaluate(context,nullptr,original,&parameters,nullptr);
    Check(error==NVSDK_NGX_Result_FAIL_UnsupportedInputFormat&&calls.evaluations==1&&calls.creates==2,"unknown SL scope swallowed native error or cloned on Evaluate");
    Check(owner.Release(context,original)==NVSDK_NGX_Result_Success,"original Release result changed");
    Check(calls.releases==2&&owner.Status().livePairs==0,"original/companion not released exactly once");
    Check(owner.BeforeHostShutdown()==SrNativeRetireResult::Retired,"drained companion owner blocks shutdown");
}
static void CloneFailureAndOpaqueAlias(){for(unsigned scenario=0;scenario<3;++scenario){Parameters parameters;calls={};calls.borrowed=&parameters;calls.failOriginal=scenario==0;calls.failClone=scenario==1;calls.aliasClone=scenario==2;
    SrHistoryOwner owner({});calls.owner=&owner;owner.SetRequested(true);auto context=Context();NVSDK_NGX_Handle* handle{};
    const auto result=owner.Create(context,nullptr,NVSDK_NGX_Feature_SuperSampling,&parameters,&handle);
    Check(result==(scenario==0?NVSDK_NGX_Result_FAIL_FeatureNotSupported:NVSDK_NGX_Result_Success),"companion failure replaced original Create result");
    Check(calls.creates==(scenario==0?1u:2u)&&owner.Status().livePairs==0,"failed/aliased clone was owned as a pair");
    if(scenario!=0){Check(owner.Release(context,handle)==NVSDK_NGX_Result_Success,"unpaired host release failed");Check(calls.releases==1,"aliased or failed companion released host handle twice");}
}}
static void NonSrAndLateHandlePassThrough(){Parameters parameters;calls={};calls.borrowed=&parameters;SrHistoryOwner owner({});calls.owner=&owner;owner.SetRequested(true);
    auto context=Context(11);NVSDK_NGX_Handle* handle{};Check(owner.Create(context,nullptr,static_cast<NVSDK_NGX_Feature>(11),&parameters,&handle)==NVSDK_NGX_Result_Success,"feature11 result changed");
    Check(calls.creates==1&&owner.Status().livePairs==0,"SR owner cloned feature11");context=Context();
    Check(owner.Evaluate(context,nullptr,reinterpret_cast<NVSDK_NGX_Handle*>(0x9990),&parameters,nullptr)==NVSDK_NGX_Result_FAIL_UnsupportedInputFormat,"late handle native result changed");
    Check(calls.creates==1&&calls.evaluations==1,"late observed handle was cloned from evaluation parameters");
    owner.Release(context,handle);
}
static void CompanionReleaseRetryDoesNotReleaseHostAgain(){Parameters parameters;calls={};calls.borrowed=&parameters;SrHistoryOwner owner({});calls.owner=&owner;owner.SetRequested(true);
    auto context=Context();NVSDK_NGX_Handle* handle{};owner.Create(context,nullptr,NVSDK_NGX_Feature_SuperSampling,&parameters,&handle);calls.failCompanionRelease=true;
    Check(owner.Release(context,handle)==NVSDK_NGX_Result_Success&&owner.Status().livePairs==1,"failed companion release erased ownership or changed host success");
    calls.failCompanionRelease=false;Check(owner.Release(context,handle)==NVSDK_NGX_Result_Success,"companion release retry failed");
    Check(std::count(calls.released.begin(),calls.released.end(),handle)==1&&calls.releases==3,"successful host release repeated during companion-only retry");
}
static void ReentrantLastPairSlotIsReserved(){Parameters parameters;calls={};calls.borrowed=&parameters;calls.sequentialHandles=true;SrHistoryOwner owner({});calls.owner=&owner;owner.SetRequested(true);
    for(unsigned i=0;i<63;++i){auto c=Context();c.createOrdinal=i+1;NVSDK_NGX_Handle* h{};owner.Create(c,nullptr,NVSDK_NGX_Feature_SuperSampling,&parameters,&h);}
    Check(owner.Status().livePairs==63,"registry setup failed");calls.onCreate=[&](unsigned n){if(n==128){auto nested=Context();nested.createOrdinal=1000;NVSDK_NGX_Handle*h{};
        Check(owner.Create(nested,nullptr,NVSDK_NGX_Feature_SuperSampling,&parameters,&h)==NVSDK_NGX_Result_Success,"nested original result changed");
        Check(calls.creates==129,"unreserved final slot allowed an extra nested companion");}};
    auto last=Context();last.createOrdinal=64;NVSDK_NGX_Handle*h{};owner.Create(last,nullptr,NVSDK_NGX_Feature_SuperSampling,&parameters,&h);
    Check(calls.creates==129&&owner.Status().livePairs==64,"reentrant clone stole the final reserved registry slot");
    calls.onCreate={};Check(owner.BeforeHostShutdown()==SrNativeRetireResult::Retired,"bounded companion registry did not retire");
}
static void ShutdownExceptionAndBoundedScopes(){Parameters parameters;calls={};calls.borrowed=&parameters;SrHistoryOwner owner({});calls.owner=&owner;owner.SetRequested(true);
    auto context=Context();NVSDK_NGX_Handle*h{};owner.Create(context,nullptr,NVSDK_NGX_Feature_SuperSampling,&parameters,&h);calls.throwReleaseOnce=true;bool threw=false;
    try{(void)owner.BeforeHostShutdown();}catch(const std::runtime_error&){threw=true;}
    Check(threw,"native release exception was swallowed");const auto releases=calls.releases;
    Check(owner.BeforeHostShutdown()==SrNativeRetireResult::NotReady&&calls.releases==releases,"unknown partial release effect was retried without proof");
    SrHistoryOwner scopes({});std::array<SrSlEvaluationToken,8> tokens;for(auto&t:tokens){t=scopes.BeginSlEvaluation({});Check(bool(t),"bounded scope missing");}
    Check(!scopes.BeginSlEvaluation({}),"scope overflow allocated unbounded storage");
    for(auto i=tokens.rbegin();i!=tokens.rend();++i)scopes.EndSlEvaluation(*i,0);
    Check(scopes.BeforeHostShutdown()==SrNativeRetireResult::Retired,"bounded TLS scope count leaked");
    {SrHistoryOwner destructor({});calls={};calls.borrowed=&parameters;calls.owner=&destructor;destructor.SetRequested(true);NVSDK_NGX_Handle*handle{};
        destructor.Create(context,nullptr,NVSDK_NGX_Feature_SuperSampling,&parameters,&handle);calls.throwReleaseOnce=true;}
    Check(!calls.throwReleaseOnce,"destructor did not encounter the injected release exception");
}
static void SuccessfullyRetiredCallbacksAreNoops(){unsigned resolves{};calls={};SrHistoryOwner owner([&](IUnknown*,REFIID,void**out){++resolves;*out=nullptr;return E_NOINTERFACE;});
    owner.SetRequested(true);Check(owner.BeforeHostShutdown()==SrNativeRetireResult::Retired,"empty owner did not retire");const auto before=owner.Status();
    auto* queue=reinterpret_cast<ID3D12CommandQueue*>(uintptr_t(0x1230));auto* list=reinterpret_cast<ID3D12GraphicsCommandList*>(uintptr_t(0x4560));
    ID3D12CommandList* lists[]{reinterpret_cast<ID3D12CommandList*>(list)};sr::Parameters configuration;configuration.key=41;configuration.known=configuration.effectiveFix=true;
    for(unsigned i=0;i<100;++i){owner.SetRequested(true);owner.SetConfiguration(configuration,{true,false,false});
        if(!i)Check(owner.Status().stage==SrHistoryStage::Retired&&!owner.Status().requested,"settings overwrote persistent retired state");
        Check(!owner.BeforeSubmit(queue,1,lists),"retired owner created submission claim");owner.Submitted({999},queue,1,lists);owner.ResetList(list);owner.Collect();
        Check(owner.BeforeHostShutdown()==SrNativeRetireResult::Retired,"successful retirement was not persistent");}
    const auto after=owner.Status();Check(resolves==0,"proven retired owner still called unavailable resolver");
    Check(after.stage==SrHistoryStage::Retired&&!after.requested&&after.quiescing,"SetRequested revived retired stage or admission");
    Check(after.submissionFailures==before.submissionFailures&&after.resetEvents==before.resetEvents&&after.historyEpoch==before.historyEpoch&&after.lastSubmissionResult==before.lastSubmissionResult,"late empty callbacks fabricated new failure/reset events");
    Check(calls.creates==0&&calls.evaluations==0&&calls.releases==0,"retired empty callbacks entered native API");
}
static void UnsuccessfulRetirementRetainsFaultSemantics(){Parameters parameters;calls={};calls.borrowed=&parameters;unsigned resolves{};
    SrHistoryOwner owner([&](IUnknown*,REFIID,void**out){++resolves;*out=nullptr;return E_NOINTERFACE;});calls.owner=&owner;owner.SetRequested(true);
    auto context=Context();NVSDK_NGX_Handle*handle{};owner.Create(context,nullptr,NVSDK_NGX_Feature_SuperSampling,&parameters,&handle);calls.failCompanionRelease=true;
    Check(owner.BeforeHostShutdown()==SrNativeRetireResult::NotReady,"unreleased owned companion falsely retired");
    auto*queue=reinterpret_cast<ID3D12CommandQueue*>(uintptr_t(0x1230));ID3D12CommandList*lists[]{reinterpret_cast<ID3D12CommandList*>(uintptr_t(0x4560))};
    owner.BeforeSubmit(queue,1,lists);Check(resolves==1&&owner.Status().submissionFailures==1,"NotReady owner incorrectly acquired retired no-op exemption");
    calls.failCompanionRelease=false;Check(owner.BeforeHostShutdown()==SrNativeRetireResult::Retired,"eventual release failed");
}
static void AbsentDeltaPreservesAbsence(){OwnerParameters bag;bag.color=reinterpret_cast<ID3D12Resource*>(0x1000);bag.depth=reinterpret_cast<ID3D12Resource*>(0x2000);
    bag.motion=reinterpret_cast<ID3D12Resource*>(0x3000);bag.output=reinterpret_cast<ID3D12Resource*>(0x4000);bag.deltaPresent=false;
    cvr::framegen::sr_owner::NativeParameters read;Check(read.Read(&bag),"optional FrameTimeDelta absence incorrectly blocks SR inputs");
    {cvr::framegen::sr_owner::ParameterOverride override(&bag,read);override.Delta(99.f);override.Reset(1);}
    Check(!bag.deltaPresent&&bag.deltaSets==0,"RAII synthesized an absent delta key which it cannot remove");
    Check(bag.reset==0,"absent delta changed ordinary reset restoration");
    Check(!read.deltaKnown&&read.missing==0,"optional absence reported as required-key failure");
    bag.deltaPresent=true;bag.delta=11;cvr::framegen::sr_owner::NativeParameters present;Check(present.Read(&bag)&&present.deltaKnown,"present delta not retained");
    {cvr::framegen::sr_owner::ParameterOverride override(&bag,present);override.Delta(24);Check(bag.delta==24,"present delta not overridden");}
    Check(bag.delta==11&&bag.deltaSets==2,"present delta not restored exactly once");
    for(float invalid:{-1.f,std::numeric_limits<float>::quiet_NaN(),std::numeric_limits<float>::infinity()}){
        bag.delta=invalid;cvr::framegen::sr_owner::NativeParameters rejected;Check(!rejected.Read(&bag)&&rejected.deltaKnown&&rejected.missing==0,"known invalid delta admitted as optional absence");}
}
int main(){try{AbsentDeltaPreservesAbsence();SuccessfullyRetiredCallbacksAreNoops();UnsuccessfulRetirementRetainsFaultSemantics();ShutdownExceptionAndBoundedScopes();CompanionReleaseRetryDoesNotReleaseHostAgain();ReentrantLastPairSlotIsReserved();CloneOnlyActualCreate();CloneFailureAndOpaqueAlias();NonSrAndLateHandlePassThrough();std::cout<<"PASS SR owner native CPU checks="<<checks<<'\n';return 0;}
catch(const std::exception&e){std::cerr<<"FAIL after "<<checks<<": "<<e.what()<<'\n';return 1;}}
