#include "Framegen/SrHistoryIntegration.hpp"
#include "Framegen/SrReferenceDefaults.hpp"
#include "Framegen/Framegen.hpp"
#include "Framegen/SrIdentityDiagnostics.hpp"
#include "Camera/PoseIdentity.hpp"
#include "Hooks/Hook.hpp"
#pragma warning(push)
#pragma warning(disable:4100)
#include "CpuD3d.hpp"
#pragma warning(pop)
#include <sl_core_types.h>
#include <sl_consts.h>
#include <cstdio>
#include <cstring>
#include <bit>
#include <vector>
#include <stdexcept>
using namespace cvr::framegen;
static unsigned checks{},failures{},before{},submitted{},resets{},parsed{},tagStarts{},tagEnds{},evalStarts{},evalEnds{},constantEnds{};
static uint32_t tagResult{},evalResult{};static bool tagResultKnown{},evalResultKnown{};static SrConstantsObservation seen;
static SrSlEvaluationObservation evaluation;static SrSourceStamp tagSource;static std::vector<SrSnapshotTag> tagValues;
static cvr::camera::PoseIdentity pose;static std::shared_ptr<SrNativeHandler> registered;
static bool requested;static sr::Parameters configuration;
static std::vector<uint64_t> completedSubmissions;
using TestLifecycleFactory=std::shared_ptr<SrNativeHandler>(*)(uint64_t);
static TestLifecycleFactory lifecycleFactory{};
#define CHECK(x) do{++checks;if(!(x)){++failures;std::printf("FAIL %d: %s\n",__LINE__,#x);}}while(false)
char g_liveControlPath[MAX_PATH]{};
void InitRuntimePaths(){}
void Log(const char*,...){}
namespace cvr::hooks {Hook::Hook(const char*,Stage,int,InstallFn,EnabledFn){}}
namespace cvr::camera {PoseIdentity CurrentNodePoseIdentity(){return pose;}}
namespace cvr::framegen {
Settings GetSettings(){Settings s;s.enabled=true;s.aerV2=false;return s;}
// The fixture verifies that the existing parser is invoked; parser semantics
// already have their separate production suite.
bool ParseSetting(const char* text,Settings& s){++parsed;int n;if(sscanf_s(text,"xr_aer_v2 = %d",&n)==1){s.aerV2=n!=0;return true;}return false;}
bool SetSrNativeHandler(std::shared_ptr<SrNativeHandler> p){registered=std::move(p);return true;}
void SetSrNativeLifecycleFactory(TestLifecycleFactory factory) noexcept {lifecycleFactory=factory;}
struct SrHistoryOwner::Impl{};
SrHistoryOwner::SrHistoryOwner(SrNativeResolver):m_impl(std::make_unique<Impl>()){}
SrHistoryOwner::~SrHistoryOwner()=default;
void SrHistoryOwner::SetRequested(bool value){requested=value;}
void SrHistoryOwner::SetConfiguration(const sr::Parameters& p,SrOptionalOverridePolicy o){configuration=p;CHECK(o.known&&!o.replaceColor&&!o.replaceSpecularMotion);}
void SrHistoryOwner::ObserveConstants(const SrConstantsObservation& s){seen=s;++constantEnds;}
SrTagCaptureToken SrHistoryOwner::BeginTags(ID3D12GraphicsCommandList*,uint32_t,bool,const SrSourceStamp& s,std::span<const SrSnapshotTag> t){tagSource=s;tagValues.assign(t.begin(),t.end());++tagStarts;return {91};}
void SrHistoryOwner::EndTags(SrTagCaptureToken t,uint32_t result,bool known){CHECK(t.value==91);tagResult=result;tagResultKnown=known;++tagEnds;}
SrSlEvaluationToken SrHistoryOwner::BeginSlEvaluation(const SrSlEvaluationObservation& s){evaluation=s;++evalStarts;return {92};}
void SrHistoryOwner::EndSlEvaluation(SrSlEvaluationToken t,uint32_t result,bool known){CHECK(t.value==92);evalResult=result;evalResultKnown=known;++evalEnds;}
SrSubmissionToken SrHistoryOwner::BeforeSubmit(ID3D12CommandQueue*,UINT,ID3D12CommandList*const*){return {100+(++before)};}
void SrHistoryOwner::Submitted(SrSubmissionToken token,ID3D12CommandQueue*,UINT,ID3D12CommandList*const*){completedSubmissions.push_back(token.value);++submitted;}
void SrHistoryOwner::ResetList(ID3D12GraphicsCommandList*){++resets;}
void SrHistoryOwner::Collect(){}
SrHistoryStatus SrHistoryOwner::Status()const{SrHistoryStatus s;s.requested=requested;s.correctedEvaluations=7;return s;}
NVSDK_NGX_Result SrHistoryOwner::Create(const SrNativeApiContext&,ID3D12GraphicsCommandList*,NVSDK_NGX_Feature,NVSDK_NGX_Parameter*,NVSDK_NGX_Handle**){return NVSDK_NGX_Result_Fail;}
NVSDK_NGX_Result SrHistoryOwner::Evaluate(const SrNativeApiContext&,ID3D12GraphicsCommandList*,const NVSDK_NGX_Handle*,const NVSDK_NGX_Parameter*,PFN_NVSDK_NGX_ProgressCallback){return NVSDK_NGX_Result_Fail;}
NVSDK_NGX_Result SrHistoryOwner::Release(const SrNativeApiContext&,NVSDK_NGX_Handle*){return NVSDK_NGX_Result_Fail;}
SrNativeRetireResult SrHistoryOwner::BeforeHostShutdown(){return SrNativeRetireResult::Retired;}
}
struct Token:sl::FrameToken{uint32_t value{};operator uint32_t()const override{return value;}};
int main(int argc,char**argv){
    if(argc!=2)return 2;strcpy_s(g_liveControlPath,argv[1]);
    CHECK(InstallSrHistoryIntegration());CHECK(registered!=nullptr);CHECK(requested);CHECK(parsed>=2);
    CHECK(configuration.known&&configuration.effectiveFix&&configuration.key==0x7B18232B987EB889ull);
    Token token;sl::ViewportHandle viewport{0};sl::Constants c;
    c.reset=sl::eTrue;c.cameraFOV=108.539993f;c.cameraNear=.02f;
    pose.poseId=31;pose.writeId=32;pose.head.valid=true;pose.head.originSerial=4;
    pose.hasRenderFrameId=true;pose.renderFrameId=0;pose.view=1;pose.posePredictionNs=123;
    pose.mainEyeSource.sourceId=71;pose.clockGeneration=8;
    auto call=BeginSrConstants(c,token,viewport);const unsigned oldEnds=constantEnds;
    pose.poseId=99;token.value=19;c.reset=sl::eFalse;
    EndSrConstants(call,27);CHECK(constantEnds==oldEnds+1);CHECK(seen.source.poseId==31);
    CHECK(seen.source.nativeFrameKnown&&seen.source.nativeFrame==0);CHECK(seen.source.locatedPoseTimeNs==123);
    CHECK(seen.viewportKnown&&seen.viewport==0&&seen.tokenKnown&&seen.slToken==0);
    CHECK(seen.resultKnown&&seen.originalResult==27);CHECK(seen.constants.flags[3]==uint32_t(sl::eTrue));
    CHECK(seen.constants.values[100]==108.539993f);CHECK(!call.owner);
    pose={};c.reset=sl::eTrue;auto unknown=BeginSrConstants(c,token,viewport);EndSrConstants(unknown,0);
    CHECK(!seen.source.poseKnown&&seen.constants.flags[3]==uint32_t(sl::eTrue));
    auto list=reinterpret_cast<ID3D12GraphicsCommandList*>(uintptr_t{0x1234});
    sl::ResourceTag tag(nullptr,sl::kBufferTypeDepth,sl::eValidUntilEvaluate);
    auto tags=BeginSrTags(&viewport,&tag,1,list);CHECK(tagStarts==1);CHECK(tagValues.size()==1);
    if(tagValues.size()==1)CHECK(tagValues[0].resource==nullptr&&tagValues[0].type==sl::kBufferTypeDepth);
    EndSrTags(tags,9);CHECK(tagEnds==1&&tagResult==9&&!tags.owner);
    auto invalid=BeginSrTags(&viewport,&tag,100,list);CHECK(!invalid.owner&&tagStarts==1);
    auto revoke=BeginSrTags(&viewport,&tag,1,nullptr);CHECK(tagStarts==2&&revoke.owner);
    EndSrTags(revoke,0);CHECK(tagEnds==2&&tagResult==0);
    {
        Microsoft::WRL::ComPtr<CpuDevice> device;device.Attach(new CpuDevice);
        Microsoft::WRL::ComPtr<CpuResource> buffer;buffer.Attach(new CpuResource(device.Get()));
        buffer->descriptor.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;buffer->descriptor.Width=256;
        sl::Resource extra(sl::ResourceType::eBuffer,buffer.Get(),uint32_t(D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE));
        sl::ResourceTag mixed[]={tag,sl::ResourceTag(&extra,sl::kBufferTypeExposure,sl::eValidUntilPresent)};
        const auto countBefore=tagStarts;
        auto mixedCall=BeginSrTags(&viewport,mixed,2,list);
        CHECK(mixedCall.owner&&tagStarts==countBefore+1&&tagValues.size()==2);
        if(tagValues.size()==2)CHECK(!tagValues[0].resource&&tagValues[1].resource==buffer.Get());
        EndSrTags(mixedCall,0);
    }
    const sl::BaseStructure* inputs[]={&viewport};pose.poseId=57;
    auto ev=BeginSrEvaluation(0,token,inputs,1,list);pose.poseId=80;
    CHECK(evalStarts==1&&evaluation.source.poseId==57);CHECK(evaluation.slToken==19&&evaluation.viewportKnown);
    CHECK(evaluation.stateContract==SrSlStateContract::StreamlineOriginalEvaluate);
    EndSrEvaluation(ev,13);CHECK(evalEnds==1&&evalResult==13&&evalResultKnown&&!ev.owner);
    unsigned originalCalls{};
    const auto tagsBefore=tagEnds;
    try {
        auto failed=BeginSrTags(&viewport,&tag,1,list);SrTagsCleanup cleanup(failed);
        throw std::runtime_error("pre-original helper allocation failure");
    }catch(const std::runtime_error&){}
    CHECK(originalCalls==0&&tagEnds==tagsBefore+1&&!tagResultKnown&&tagResult==UINT32_MAX);
    const auto evalsBefore=evalEnds;
    {auto failed=BeginSrEvaluation(0,token,inputs,1,list);SrEvaluationCleanup cleanup(failed);}
    CHECK(evalEnds==evalsBefore+1&&!evalResultKnown&&evalResult==UINT32_MAX);
    const auto constantsBefore=constantEnds;
    {auto failed=BeginSrConstants(c,token,viewport);SrConstantsCleanup cleanup(failed);}
    CHECK(constantEnds==constantsBefore+1&&!seen.resultKnown&&seen.originalResult==UINT32_MAX);
    const auto successfulBefore=evalEnds;
    {auto good=BeginSrEvaluation(0,token,inputs,1,list);SrEvaluationCleanup cleanup(good);++originalCalls;EndSrEvaluation(good,17);}
    CHECK(originalCalls==1&&evalEnds==successfulBefore+1&&evalResult==17&&evalResultKnown);
    SetSrHistoryRequested(false);CHECK(!requested);
    auto off=BeginSrConstants(c,token,viewport);CHECK(!off.owner);
    // Retirement submissions/reset still reach the owner after capture is off.
    auto outer=SrBeforeSubmit(nullptr,0,nullptr);SrResetList(list);
    auto inner=SrBeforeSubmit(nullptr,0,nullptr);SrSubmitted(inner,nullptr,0,nullptr);SrSubmitted(outer,nullptr,0,nullptr);
    SrSubmitted(outer,nullptr,0,nullptr); // A consumed claim is never submitted twice.
    CHECK(before==2&&submitted==2&&resets==1);
    CHECK((completedSubmissions==std::vector<uint64_t>{102,101}));CHECK(!inner.owner&&!outer.owner);
    CHECK(CyberpunkVR_SrHistoryStatus.correctedEvaluations==7&&CyberpunkVR_SrHistoryStatus.requested==0);
    CHECK(CyberpunkVR_SrHistoryStatusSeq.load()>0&&CyberpunkVR_SrHistoryStatusSeq.load()%2==0);
    CHECK(lifecycleFactory!=nullptr);
    if(lifecycleFactory){
        SetSrHistoryRequested(true);auto previous=registered;
        std::static_pointer_cast<SrHistoryOwner>(previous)->SetRequested(false);
        auto renewed=lifecycleFactory(2);CHECK(renewed&&renewed!=previous&&requested);
        auto reinitCall=BeginSrConstants(c,token,viewport);CHECK(reinitCall.owner.get()==renewed.get());EndSrConstants(reinitCall,0);
        SetSrHistoryRequested(false);auto disabled=lifecycleFactory(3);CHECK(disabled&&disabled!=renewed&&!requested);
        CHECK(!BeginSrConstants(c,token,viewport).owner);
    }
    std::printf("%u checks, %u failures\n",checks,failures);return failures?1:0;
}
