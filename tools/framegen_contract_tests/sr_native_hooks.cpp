#include "SrNgxHookTestApi.hpp"
#include "Framegen/SrNativeDiagnostics.hpp"
#include <MinHook.h>
#include <array>
#include <cstdio>
#include <functional>
#include <stdexcept>
#include <vector>
#include <memory>
// Test-only concrete handle. Production deliberately treats the SDK handle as opaque.
struct NVSDK_NGX_Handle {uint32_t fixtureWord{};};
using namespace cvr::framegen;
namespace test=cvr::framegen::sr_test;
static unsigned checks{},failures{},inputRetries{};
#define CHECK(x) do{++checks;if(!(x)){++failures;std::printf("FAIL %d: %s\n",__LINE__,#x);}}while(false)
namespace cvr::framegen {bool InstallInputHooks(){++inputRetries;return InstallSrSlHooks();}}
struct Params:NVSDK_NGX_Parameter {
    mutable unsigned touches{};uint64_t marker=0x123456789ABCDEF0;
#define SET(T) void Set(const char*,T)override{++touches;}
    SET(unsigned long long) SET(float) SET(double) SET(unsigned int) SET(int) SET(ID3D11Resource*) SET(ID3D12Resource*) SET(void*)
#undef SET
#define GET(T) NVSDK_NGX_Result Get(const char*,T*)const override{++touches;return NVSDK_NGX_Result_Fail;}
    GET(unsigned long long) GET(float) GET(double) GET(unsigned int) GET(int) GET(ID3D11Resource*) GET(ID3D12Resource*) GET(void*)
#undef GET
    void Reset()override{++touches;}
};
struct Mock {
    struct Hook {void* target{};void* detour{};bool queued{},enabled{},removed{};};
    std::vector<Hook> hooks;bool nativeAvailable=true,slAvailable=true,pinOkay=true;
    unsigned createHookCalls{},removeCalls{},disableCalls{},applyCalls{},pinCalls{};
    int failCreateAt=-1;bool failApply=false;
    unsigned creates{},evaluates{},releases{},inits{},devices{},allocates{},shutdowns{};
    NVSDK_NGX_Result createResult=NVSDK_NGX_Result_Success,evaluateResult=NVSDK_NGX_Result_Success,releaseResult=NVSDK_NGX_Result_Success;
    sl::Result slResult=sl::Result::eOk;
    bool throwCreate=false,throwEvaluate=false,throwShutdown=false;
    std::function<void()> duringEvaluate,duringInit,duringRelease,duringDevice,duringAllocate;
    std::array<NVSDK_NGX_Handle,256> handles{};unsigned nextHandle{};
    std::array<uintptr_t,5> args{};
    Mock(){for(unsigned i=0;i<handles.size();++i)handles[i].fixtureWord=100+i;}
} mock;
NVSDK_NGX_Result NVSDK_CONV CreateOriginal(ID3D12GraphicsCommandList* list,NVSDK_NGX_Feature feature,NVSDK_NGX_Parameter* p,NVSDK_NGX_Handle** h){
    ++mock.creates;mock.args={uintptr_t(list),uint32_t(feature),uintptr_t(p),uintptr_t(h),0};if(mock.throwCreate)throw std::runtime_error("original create");
    if(NVSDK_NGX_SUCCEED(mock.createResult)&&h)*h=&mock.handles[mock.nextHandle++];return mock.createResult;
}
NVSDK_NGX_Result NVSDK_CONV EvaluateOriginal(ID3D12GraphicsCommandList* l,const NVSDK_NGX_Handle* h,const NVSDK_NGX_Parameter* p,PFN_NVSDK_NGX_ProgressCallback c){
    ++mock.evaluates;mock.args={uintptr_t(l),uintptr_t(h),uintptr_t(p),reinterpret_cast<uintptr_t>(c),0};
    if(mock.duringEvaluate)mock.duringEvaluate();if(mock.throwEvaluate)throw std::runtime_error("original evaluate");return mock.evaluateResult;
}
NVSDK_NGX_Result NVSDK_CONV ReleaseOriginal(NVSDK_NGX_Handle* h){++mock.releases;mock.args={uintptr_t(h),0,0,0,0};if(mock.duringRelease)mock.duringRelease();return mock.releaseResult;}
sl::Result InitOriginal(const sl::Preferences& p,uint64_t v){++mock.inits;mock.args={uintptr_t(&p),v,0,0,0};if(mock.duringInit)mock.duringInit();return mock.slResult;}
sl::Result DeviceOriginal(void* p){++mock.devices;mock.args={uintptr_t(p),0,0,0,0};if(mock.duringDevice)mock.duringDevice();return mock.slResult;}
sl::Result AllocateOriginal(void* l,sl::Feature f,const sl::ViewportHandle& v){++mock.allocates;mock.args={uintptr_t(l),uint32_t(f),uintptr_t(&v),0,0};if(mock.duringAllocate)mock.duringAllocate();return mock.slResult;}
sl::Result ShutdownOriginal(){++mock.shutdowns;if(mock.throwShutdown)throw std::runtime_error("original shutdown");return mock.slResult;}
bool Sl(test::Module& m){m={0x10000,{reinterpret_cast<void*>(&InitOriginal),reinterpret_cast<void*>(&DeviceOriginal),reinterpret_cast<void*>(&AllocateOriginal),reinterpret_cast<void*>(&ShutdownOriginal)}};return mock.slAvailable;}
bool Native(test::Module& m){m={0x20000,{reinterpret_cast<void*>(&CreateOriginal),reinterpret_cast<void*>(&EvaluateOriginal),reinterpret_cast<void*>(&ReleaseOriginal),nullptr}};return mock.nativeAvailable;}
bool Pin(uintptr_t){++mock.pinCalls;return mock.pinOkay;}
Mock::Hook* Find(void* target){for(auto& h:mock.hooks)if(h.target==target&&!h.removed)return &h;return nullptr;}
int CreateHook(void* target,void* detour,void** original){
    const auto call=mock.createHookCalls++;if(int(call)==mock.failCreateAt)return MH_ERROR_MEMORY_ALLOC;if(Find(target))return MH_ERROR_ALREADY_CREATED;
    mock.hooks.push_back({target,detour});*original=target;return MH_OK;
}
int Queue(void* target){auto* h=Find(target);if(!h)return MH_ERROR_NOT_CREATED;h->queued=true;return MH_OK;}
int Apply(){++mock.applyCalls;for(auto& h:mock.hooks)if(!h.removed){h.enabled=h.queued;if(mock.failApply)return MH_ERROR_MEMORY_PROTECT;}return MH_OK;}
int Disable(void* target){++mock.disableCalls;auto* h=Find(target);if(!h)return MH_ERROR_NOT_CREATED;h->queued=false;h->enabled=false;return MH_OK;}
int Remove(void* target){++mock.removeCalls;auto* h=Find(target);if(!h)return MH_ERROR_NOT_CREATED;h->removed=true;return MH_OK;}
void Reset(){test::Reset({Sl,Native,Pin,CreateHook,Queue,Apply,Disable,Remove});mock=Mock{};inputRetries=0;}
void Ready(){CHECK(InstallSrSlHooks());CHECK(EnsureSrNativeHooks());CHECK(GetSrNativeHookReadiness().slReady&&GetSrNativeHookReadiness().nativeReady);}
struct Owner:SrNativeHandler {
    unsigned creates{},evaluates{},releases{},retires{};bool companion=false,reenterShutdown=false;
    SrNativeRetireResult retireResult=SrNativeRetireResult::Retired;
    SrNativeApiContext last;NVSDK_NGX_Handle* privateHandle{};unsigned* destroyed{};
    ~Owner(){if(destroyed)++*destroyed;}
    NVSDK_NGX_Result Create(const SrNativeApiContext& c,ID3D12GraphicsCommandList* l,NVSDK_NGX_Feature f,NVSDK_NGX_Parameter* p,NVSDK_NGX_Handle** h)override{
        ++creates;last=c;const auto result=SrNativeHandler::Create(c,l,f,p,h);
        if(companion&&NVSDK_NGX_SUCCEED(result))c.originals.create(l,f,p,&privateHandle);return result;
    }
    NVSDK_NGX_Result Evaluate(const SrNativeApiContext& c,ID3D12GraphicsCommandList* l,const NVSDK_NGX_Handle* h,const NVSDK_NGX_Parameter* p,PFN_NVSDK_NGX_ProgressCallback b)override{
        ++evaluates;last=c;return SrNativeHandler::Evaluate(c,l,h,p,b);
    }
    NVSDK_NGX_Result Release(const SrNativeApiContext& c,NVSDK_NGX_Handle* h)override{++releases;last=c;return SrNativeHandler::Release(c,h);}
    SrNativeRetireResult BeforeHostShutdown()override{
        ++retires;CHECK(GetSrNativeHookReadiness().quiescing);CHECK(EnsureSrNativeHooks());
        if(reenterShutdown)CHECK(test::Shutdown()==sl::Result::eErrorInvalidState);return retireResult;
    }
};
int main(){
    const auto* win=L"C:\\Windows";
    CHECK(test::DriverStorePath(L"C:\\WINDOWS\\System32\\DriverStore\\FileRepository\\nv_dispi.inf_amd64_x\\_nvngx.dll",win));
    for(const auto* p:{L"C:\\game\\_nvngx.dll",L"C:\\Windows\\System32\\DriverStore\\FileRepository\\..\\_nvngx.dll",L"C:\\Windows\\System32\\DriverStore\\FileRepository\\nv_x\\fake.dll",L"C:\\Windows\\System32\\DriverStore\\FileRepository\\nv_x\\..\\_nvngx.dll",L"C:\\WindowsFake\\System32\\DriverStore\\FileRepository\\nv_x\\_nvngx.dll"})CHECK(!test::DriverStorePath(p,win));
    Reset();mock.nativeAvailable=false;CHECK(!EnsureSrNativeHooks());CHECK(mock.createHookCalls==0&&!GetSrNativeHookReadiness().modulePinned);
    Reset();mock.pinOkay=false;CHECK(!EnsureSrNativeHooks());CHECK(mock.createHookCalls==0&&!GetSrNativeHookReadiness().modulePinned);
    Reset();mock.failCreateAt=1;CHECK(!EnsureSrNativeHooks());CHECK(mock.removeCalls==1);for(const auto& h:mock.hooks)CHECK(!h.enabled);
    mock.failCreateAt=-1;CHECK(EnsureSrNativeHooks());CHECK(GetSrNativeHookReadiness().moduleGeneration==1);
    Reset();mock.failApply=true;CHECK(!EnsureSrNativeHooks());CHECK(mock.removeCalls==0);for(const auto& h:mock.hooks)CHECK(!h.enabled&&!h.queued);
    const auto created=mock.createHookCalls;mock.failApply=false;CHECK(EnsureSrNativeHooks());CHECK(mock.createHookCalls==created);CHECK(GetSrNativeHookReadiness().modulePinned);
    CHECK(CyberpunkVR_SrNativeHookStatus.hostNgxInitProven==0);

    Reset();Ready();Params params;auto* list=reinterpret_cast<ID3D12GraphicsCommandList*>(0x1234);auto cb=reinterpret_cast<PFN_NVSDK_NGX_ProgressCallback>(uintptr_t(0x5678));NVSDK_NGX_Handle* handle{};
    CHECK(test::Create(list,NVSDK_NGX_Feature_SuperSampling,&params,&handle)==mock.createResult);CHECK(mock.creates==1&&handle);
    CHECK(mock.args[0]==uintptr_t(list)&&mock.args[1]==1&&mock.args[2]==uintptr_t(&params)&&mock.args[3]==uintptr_t(&handle));
    mock.evaluateResult=static_cast<NVSDK_NGX_Result>(0xBAD00005);CHECK(test::Evaluate(list,handle,&params,cb)==mock.evaluateResult);
    CHECK(mock.evaluates==1&&mock.args[1]==uintptr_t(handle)&&mock.args[3]==uintptr_t(cb));CHECK(params.touches==0&&params.marker==0x123456789ABCDEF0);
    auto owner=std::make_shared<Owner>();CHECK(SetSrNativeHandler(owner));
    mock.releaseResult=NVSDK_NGX_Result_Fail;CHECK(test::Release(handle)==NVSDK_NGX_Result_Fail);CHECK(owner->releases==1);
    test::Evaluate(list,handle,&params,cb);CHECK(owner->evaluates==1&&owner->last.featureId==1&&owner->last.featureKnown&&!owner->last.handleIdKnown);
    mock.releaseResult=NVSDK_NGX_Result_Success;test::Release(handle);test::Evaluate(list,handle,&params,cb);CHECK(owner->evaluates==1);CHECK(CyberpunkVR_SrNativeHookStatus.counters[uint32_t(SrHookCounter::UnknownEvaluate)]==1);
    const auto ownerCreates=owner->creates,ownerEvals=owner->evaluates,ownerReleases=owner->releases;
    test::Create(list,NVSDK_NGX_Feature_FrameGeneration,&params,&handle);test::Evaluate(list,handle,&params,cb);test::Release(handle);
    CHECK(owner->creates==ownerCreates&&owner->evaluates==ownerEvals&&owner->releases==ownerReleases);
    CHECK(CyberpunkVR_SrNativeHookStatus.counters[uint32_t(SrHookCounter::Feature11Passthrough)]==3);
    CHECK(params.touches==0);
    owner->companion=true;test::Create(list,NVSDK_NGX_Feature_RayReconstruction,&params,&handle);
    CHECK(owner->last.featureId==13&&owner->privateHandle!=handle&&owner->last.moduleGeneration==1);
    const auto count=owner->evaluates;test::Evaluate(list,owner->privateHandle,&params,cb);CHECK(owner->evaluates==count);
    test::Evaluate(list,handle,&params,cb);CHECK(owner->evaluates==count+1&&owner->last.createOrdinal!=0);

    // Shared callback ownership survives an exchange while original is running.
    unsigned destroyed=0;owner->destroyed=&destroyed;std::weak_ptr<Owner> weak=owner;owner.reset();
    mock.duringEvaluate=[&]{CHECK(SetSrNativeHandler({}));CHECK(!weak.expired());};
    test::Evaluate(list,handle,&params,cb);CHECK(weak.expired()&&destroyed==1);mock.duringEvaluate={};
    owner=std::make_shared<Owner>();owner->destroyed=&destroyed;CHECK(SetSrNativeHandler(owner));weak=owner;owner.reset();
    mock.duringEvaluate=[&]{CHECK(SetSrNativeHandler({}));CHECK(!weak.expired());};mock.throwEvaluate=true;
    bool threw=false;try{test::Evaluate(list,handle,&params,cb);}catch(const std::runtime_error& e){threw=std::string_view(e.what())=="original evaluate";}
    CHECK(threw&&weak.expired()&&destroyed==2);CHECK(CyberpunkVR_SrNativeHookStatus.resultKnown==0);mock.throwEvaluate=false;mock.duringEvaluate={};
    owner=std::make_shared<Owner>();CHECK(SetSrNativeHandler(owner));test::Evaluate(list,handle,&params,cb);CHECK(owner->evaluates==1); // depth unwound
    mock.throwCreate=true;const auto old=handle;threw=false;try{test::Create(list,NVSDK_NGX_Feature_SuperSampling,&params,&handle);}catch(const std::runtime_error&){threw=true;}
    CHECK(threw&&handle==old);mock.throwCreate=false;

    // Default lifecycle forwards exact arguments/results. Init retries after original.
    Reset();CHECK(InstallSrSlHooks());mock.nativeAvailable=false;sl::Preferences prefs;sl::ViewportHandle viewport(0);mock.duringInit=[] {CHECK(!GetSrNativeHookReadiness().nativeReady);mock.nativeAvailable=true;};
    CHECK(test::Init(prefs,789)==sl::Result::eOk);CHECK(mock.inits==1&&mock.args[0]==uintptr_t(&prefs)&&mock.args[1]==789);CHECK(GetSrNativeHookReadiness().nativeReady);
    CHECK(inputRetries==1&&GetSrNativeHookReadiness().slInitObserved);mock.slResult=static_cast<sl::Result>(77);
    CHECK(test::SetDevice(list)==mock.slResult);CHECK(mock.devices==1&&mock.args[0]==uintptr_t(list));CHECK(inputRetries==3);
    CHECK(test::Allocate(list,1001,viewport)==mock.slResult);CHECK(mock.allocates==1&&mock.args[1]==1001&&mock.args[2]==uintptr_t(&viewport));CHECK(inputRetries==5);
    CHECK(test::Shutdown()==mock.slResult&&mock.shutdowns==1);CHECK(CyberpunkVR_SrNativeHookStatus.retirement==uint32_t(SrNativeRetireResult::NotRegistered));
    mock.throwShutdown=true;threw=false;try{test::Shutdown();}catch(const std::runtime_error&){threw=true;}CHECK(threw);mock.throwShutdown=false;

    // Registered NotReady refuses the host; retry Retired calls original once.
    Reset();Ready();owner=std::make_shared<Owner>();owner->retireResult=SrNativeRetireResult::NotReady;owner->reenterShutdown=true;
    CHECK(SetSrNativeHandler(owner));weak=owner;CHECK(test::Shutdown()==sl::Result::eErrorInvalidState);CHECK(mock.shutdowns==0&&owner->retires==1);
    CHECK(GetSrNativeHookReadiness().quiescing&&!SetSrNativeHandler({}));owner.reset();CHECK(!weak.expired());
    test::Create(list,NVSDK_NGX_Feature_SuperSampling,&params,&handle);CHECK(weak.lock()->creates==0); // admission quiesced
    weak.lock()->retireResult=SrNativeRetireResult::Retired;const auto life=GetSrNativeHookReadiness().lifecycleGeneration;
    CHECK(test::Shutdown()==sl::Result::eOk&&mock.shutdowns==1);CHECK(GetSrNativeHookReadiness().lifecycleGeneration>life);
    CHECK(SetSrNativeHandler({}));CHECK(weak.expired());CHECK(CyberpunkVR_SrNativeHookStatus.hostNgxInitProven==0);

    Reset();Ready();for(unsigned i=0;i<65;++i)test::Create(list,NVSDK_NGX_Feature_SuperSampling,&params,&handle);
    CHECK(CyberpunkVR_SrNativeHookStatus.counters[uint32_t(SrHookCounter::RegistryFull)]==1);
    owner=std::make_shared<Owner>();CHECK(SetSrNativeHandler(owner));test::Evaluate(list,handle,&params,cb);CHECK(owner->evaluates==0);
    CHECK(CyberpunkVR_SrNativeHookStatus.counters[uint32_t(SrHookCounter::UnknownEvaluate)]==1);
    CHECK(!(CyberpunkVR_SrNativeHookStatusSeq.load()&1));CHECK(CyberpunkVR_SrNativeHookStatus.bytes==256);CHECK(params.touches==0);
    CHECK(SetSrNativeHandler({}));
    // Native alone is insufficient: no retirement boundary means no owner dispatch.
    Reset();CHECK(EnsureSrNativeHooks());owner=std::make_shared<Owner>();CHECK(SetSrNativeHandler(owner));
    CHECK(GetSrNativeHookReadiness().handlerRegistered&&!GetSrNativeHookReadiness().slReady);
    test::Create(list,NVSDK_NGX_Feature_SuperSampling,&params,&handle);CHECK(owner->creates==0&&mock.creates==1);
    CHECK(InstallSrSlHooks());test::Create(list,NVSDK_NGX_Feature_SuperSampling,&params,&handle);CHECK(owner->creates==1);
    // A real Create nested inside slInit remains a valid receipt after return.
    mock.duringInit=[&]{test::Create(list,NVSDK_NGX_Feature_SuperSampling,&params,&handle);};const auto epoch=GetSrNativeHookReadiness().lifecycleGeneration;
    test::Init(prefs,1);CHECK(GetSrNativeHookReadiness().lifecycleGeneration==epoch);test::Evaluate(list,handle,&params,cb);CHECK(owner->evaluates==1);
    // Release can re-enter the API and reuse a pointer. Erase the old ordinal only.
    Reset();Ready();owner=std::make_shared<Owner>();CHECK(SetSrNativeHandler(owner));test::Create(list,NVSDK_NGX_Feature_SuperSampling,&params,&handle);
    const auto originalOrdinal=owner->last.createOrdinal;NVSDK_NGX_Handle* reused{};
    mock.duringRelease=[&]{mock.nextHandle=0;test::Create(list,NVSDK_NGX_Feature_SuperSampling,&params,&reused);};
    CHECK(test::Release(handle)==NVSDK_NGX_Result_Success&&reused==handle);mock.duringRelease={};test::Evaluate(list,reused,&params,cb);
    CHECK(owner->evaluates==1&&owner->last.createOrdinal>originalOrdinal);CHECK(SetSrNativeHandler({}));
    std::printf("SR native hook CPU: checks=%u failures=%u game/GPU/actual-hooks=not-used\n",checks,failures);return failures?1:0;
}
