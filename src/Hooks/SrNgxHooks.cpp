#include "Framegen/SrNativeApi.hpp"
#include "Framegen/SrNativeDiagnostics.hpp"
#include "Framegen/NativeFgObservation.hpp"
#include "Framegen/CpuStageTiming.hpp"
#include "Hooks/Hook.hpp"
#include <MinHook.h>
#include <sl_core_types.h>
#include <array>
#include <atomic>
#include <cwchar>
#include <mutex>
#include <string_view>
#ifdef CVR_SR_NGX_TESTING
#include "SrNgxHookTestApi.hpp"
#endif
namespace cvr::framegen {bool InstallInputHooks();}
extern "C" {
__declspec(dllexport) std::atomic<uint64_t> CyberpunkVR_SrNativeHookStatusSeq{};
__declspec(dllexport) cvr::framegen::SrNativeHookStatus CyberpunkVR_SrNativeHookStatus;
__declspec(dllexport) std::atomic<uint64_t> CyberpunkVR_SrNativeLifecycleStatusSeq{};
__declspec(dllexport) cvr::framegen::SrNativeLifecycleStatus CyberpunkVR_SrNativeLifecycleStatus;
}
namespace cvr::framegen {
namespace {
using SlInit=sl::Result(*)(const sl::Preferences&,uint64_t);
using SlDevice=sl::Result(*)(void*);
using SlAllocate=sl::Result(*)(void*,sl::Feature,const sl::ViewportHandle&);
using SlShutdown=sl::Result(*)();
#ifdef CVR_SR_NGX_TESTING
using Module=sr_test::Module;using Platform=sr_test::Platform;
#else
struct Module {uintptr_t base{};void* targets[4]{};};
struct Platform {
    bool(*sl)(Module&);bool(*native)(Module&);bool(*pin)(uintptr_t);
    int(*create)(void*,void*,void**);int(*queueEnable)(void*);int(*apply)();int(*disable)(void*);int(*remove)(void*);
};
#endif
struct Bundle {
    explicit Bundle(uint32_t n):count(n){}
    uint32_t count;std::array<void*,4> targets{};std::array<std::atomic<void*>,4> originals{};
    uint32_t created{};std::atomic<uint32_t> enabled{};std::atomic<bool> ready{};
    bool everPublished=false;uintptr_t module{};
};
Bundle slBundle{4},nativeBundle{3};
std::mutex installMutex,registryMutex,retirementMutex;
std::atomic<std::shared_ptr<SrNativeHandler>> handler;
std::atomic<SrNativeLifecycleFactory> lifecycleFactory{};
std::shared_ptr<SrNativeHandler> retirementOwner;
std::atomic<bool> quiescing{},modulePinned{},slInitObserved{},slDeviceObserved{},slInitialized{};
std::atomic<bool> rearmPending{},rearmInProgress{};
std::atomic<uintptr_t> coreModule{};
std::atomic<uint64_t> moduleGeneration{},lifecycleGeneration{1},createOrdinal{},firstCreateOrdinal{};
std::atomic<uint32_t> slInitResult{},slDeviceResult{};
std::array<std::atomic<uint64_t>,16> counters{};
std::atomic_flag statusWriter=ATOMIC_FLAG_INIT;
std::atomic<uint64_t> droppedStatus{};
std::atomic_flag lifecycleWriter=ATOMIC_FLAG_INIT;
std::atomic<uint64_t> lifecycleDropped{};
thread_local uint32_t callbackDepth{},shutdownDepth{};
struct Entry {NVSDK_NGX_Handle* handle{};uint32_t feature{};uint64_t module{},life{},ordinal{};};
std::array<Entry,64> registry{};
void Count(SrHookCounter counter){counters[uint32_t(counter)].fetch_add(1,std::memory_order_relaxed);}
void Lifecycle(SrLifecycleEvent event,uint32_t result=0,bool known=false,
    SrNativeRetireResult retired=SrNativeRetireResult::NotRegistered,uint32_t feature=UINT32_MAX,uint64_t ordinal=0){
    if(lifecycleWriter.test_and_set(std::memory_order_acquire)){++lifecycleDropped;return;}
    LARGE_INTEGER qpc{};QueryPerformanceCounter(&qpc);auto& wire=CyberpunkVR_SrNativeLifecycleStatus;
    SrLifecycleRecord record;record.serial=wire.publishedEvents+1;record.qpc=uint64_t(qpc.QuadPart);
    record.lifecycleGeneration=lifecycleGeneration.load();record.moduleGeneration=moduleGeneration.load();record.createOrdinal=ordinal;
    record.event=uint32_t(event);record.result=result;record.retirement=uint32_t(retired);record.feature=feature;
    record.flags=(known?SrLifecycleResultKnown:0u)|(handler.load()?SrLifecycleHandler:0u)|
        (quiescing.load()?SrLifecycleQuiescing:0u)|(slInitialized.load()?SrLifecycleSlInitialized:0u)|
        (rearmPending.load()?SrLifecycleRearmPending:0u)|(lifecycleFactory.load()?SrLifecycleFactory:0u)|
        (rearmInProgress.load()?SrLifecycleRearming:0u);
    CyberpunkVR_SrNativeLifecycleStatusSeq.fetch_add(1,std::memory_order_acq_rel);
    wire.events[(record.serial-1)%wire.Capacity]=record;wire.publishedEvents=record.serial;wire.droppedEvents=lifecycleDropped.load();
    CyberpunkVR_SrNativeLifecycleStatusSeq.fetch_add(1,std::memory_order_release);lifecycleWriter.clear(std::memory_order_release);
}
void RearmLifecycle();
void Publish(uint32_t stage=0,uint32_t feature=UINT32_MAX,const void* handleValue=nullptr,const void* list=nullptr,
    uint32_t result=0,bool resultKnown=false,SrNativeRetireResult retire=SrNativeRetireResult::NotRegistered){
    if(statusWriter.test_and_set(std::memory_order_acquire)){droppedStatus.fetch_add(1,std::memory_order_relaxed);return;}
    SrNativeHookStatus next;
    for(unsigned i=0;i<counters.size();++i)next.counters[i]=counters[i].load(std::memory_order_relaxed);
    next.coreModule=coreModule.load();next.moduleGeneration=moduleGeneration.load();next.firstCreateOrdinal=firstCreateOrdinal.load();
    next.lastHandle=uintptr_t(handleValue);next.lastCommandList=uintptr_t(list);
    // SDK handle is opaque: never label an unproved first word as its ID.
    next.slReady=slBundle.ready.load();next.nativeReady=nativeBundle.ready.load();next.modulePinned=modulePinned.load();
    next.lastStage=stage;next.lastFeature=feature;next.lastResult=result;next.resultKnown=resultKnown;next.retirement=uint32_t(retire);
    next.slInitResult=slInitResult.load();next.slDeviceResult=slDeviceResult.load();next.slInitObserved=slInitObserved.load();next.slDeviceObserved=slDeviceObserved.load();
    next.slHookMask=slBundle.enabled.load();next.nativeHookMask=nativeBundle.enabled.load();next.droppedLastRecords=droppedStatus.load();
    CyberpunkVR_SrNativeHookStatusSeq.fetch_add(1,std::memory_order_acq_rel);CyberpunkVR_SrNativeHookStatus=next;
    CyberpunkVR_SrNativeHookStatusSeq.fetch_add(1,std::memory_order_release);statusWriter.clear(std::memory_order_release);
}
bool EqualNoCase(std::wstring_view a,std::wstring_view b){
    if(a.size()!=b.size())return false;
    for(size_t i=0;i<a.size();++i){auto lower=[](wchar_t c){return c>=L'A'&&c<=L'Z'?wchar_t(c+(L'a'-L'A')):c;};if(lower(a[i])!=lower(b[i]))return false;}
    return true;
}
bool IsDriverStorePath(std::wstring_view path,std::wstring_view windows){
    constexpr std::wstring_view suffix=L"\\System32\\DriverStore\\FileRepository\\";
    if(windows.empty()||path.size()<=windows.size()+suffix.size()||!EqualNoCase(path.substr(0,windows.size()),windows)||
       !EqualNoCase(path.substr(windows.size(),suffix.size()),suffix))return false;
    const auto rest=path.substr(windows.size()+suffix.size());const auto split=rest.find(L'\\');
    return split!=std::wstring_view::npos&&split>0&&rest.substr(0,split)!=L"."&&rest.substr(0,split)!=L".."&&
        EqualNoCase(rest.substr(split+1),L"_nvngx.dll")&&rest.find(L'/')==std::wstring_view::npos;
}
#ifndef CVR_SR_NGX_TESTING
bool ImageSize(HMODULE module,size_t* size){
    __try{
        const auto* bytes=reinterpret_cast<const unsigned char*>(module);const auto* dos=reinterpret_cast<const IMAGE_DOS_HEADER*>(bytes);
        if(dos->e_magic!=IMAGE_DOS_SIGNATURE||dos->e_lfanew<0||dos->e_lfanew>0x100000)return false;
        const auto* nt=reinterpret_cast<const IMAGE_NT_HEADERS64*>(bytes+dos->e_lfanew);
        if(nt->Signature!=IMAGE_NT_SIGNATURE||nt->OptionalHeader.Magic!=IMAGE_NT_OPTIONAL_HDR64_MAGIC)return false;
        *size=nt->OptionalHeader.SizeOfImage;return *size>4096&&*size<512u*1024u*1024u;
    }__except(EXCEPTION_EXECUTE_HANDLER){return false;}
}
bool Resolve(HMODULE module,const char*const* names,unsigned count,Module& out){
    if(!module)return false;size_t size{};if(!ImageSize(module,&size))return false;out.base=reinterpret_cast<uintptr_t>(module);
    for(unsigned i=0;i<count;++i){out.targets[i]=reinterpret_cast<void*>(GetProcAddress(module,names[i]));
        const auto address=reinterpret_cast<uintptr_t>(out.targets[i]);if(address<out.base||address-out.base>=size)return false;}
    return true;
}
bool ResolveSl(Module& out){
    const char* names[]{"slInit","slSetD3DDevice","slAllocateResources","slShutdown"};return Resolve(GetModuleHandleW(L"sl.interposer.dll"),names,4,out);
}
bool ResolveNative(Module& out){
    const auto module=GetModuleHandleW(L"_nvngx.dll");if(!module)return false;
    wchar_t path[MAX_PATH]{},windows[MAX_PATH]{};const auto n=GetModuleFileNameW(module,path,MAX_PATH);const auto w=GetWindowsDirectoryW(windows,MAX_PATH);
    if(!n||n>=MAX_PATH||!w||w>=MAX_PATH||!IsDriverStorePath({path,n},{windows,w}))return false;
    const char* names[]{"NVSDK_NGX_D3D12_CreateFeature","NVSDK_NGX_D3D12_EvaluateFeature","NVSDK_NGX_D3D12_ReleaseFeature"};return Resolve(module,names,3,out);
}
bool Pin(uintptr_t base){
    HMODULE module{},self{};
    return GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_PIN,reinterpret_cast<LPCWSTR>(base),&module)&&
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_PIN,reinterpret_cast<LPCWSTR>(&Pin),&self);
}
int CreateHook(void* target,void* detour,void** original){return int(MH_CreateHook(target,detour,original));}
int QueueHook(void* target){return int(MH_QueueEnableHook(target));}
int ApplyHooks(){return int(MH_ApplyQueued());}
int DisableHook(void* target){const auto queued=MH_QueueDisableHook(target);const auto disabled=MH_DisableHook(target);return int(queued!=MH_OK?queued:disabled);}
int RemoveHook(void* target){return int(MH_RemoveHook(target));}
Platform platform{ResolveSl,ResolveNative,Pin,CreateHook,QueueHook,ApplyHooks,DisableHook,RemoveHook};
#else
Platform platform{};
#endif
bool Install(Bundle& bundle,const Module& candidate,const std::array<void*,4>& detours){
    if(bundle.module&&bundle.module!=candidate.base)return false;
    if(!platform.pin(candidate.base))return false;
    bundle.module=candidate.base;bundle.targets={candidate.targets[0],candidate.targets[1],candidate.targets[2],candidate.targets[3]};
    bool okay=true;
    for(unsigned i=0;i<bundle.count;++i)if(!(bundle.created&(1u<<i))){void* original{};
        if(platform.create(bundle.targets[i],detours[i],&original)!=MH_OK||!original){okay=false;break;}
        bundle.originals[i].store(original,std::memory_order_release);bundle.created|=1u<<i;
    }
    if(okay)for(unsigned i=0;i<bundle.count;++i)if(platform.queueEnable(bundle.targets[i])!=MH_OK){okay=false;break;}
    if(okay){
        // Once a detour could become reachable, its trampoline is retained for
        // process lifetime even after rollback. A callback may have entered just
        // before its first C++ instruction; an active counter cannot close that
        // race. Retrying reuses these disabled hooks, never frees live code.
        bundle.everPublished=true;
        if(platform.apply()!=MH_OK)okay=false;
    }
    if(okay){bundle.enabled.store((1u<<bundle.count)-1);bundle.ready.store(true,std::memory_order_release);return true;}
    uint32_t remaining=0;
    for(unsigned i=0;i<bundle.count;++i)if(bundle.created&(1u<<i)){
        const auto disabled=platform.disable(bundle.targets[i]);if(disabled!=MH_OK&&disabled!=MH_ERROR_DISABLED)remaining|=1u<<i;
    }
    bundle.enabled.store(remaining);bundle.ready.store(false);
    if(!bundle.everPublished&&!remaining){
        for(unsigned i=0;i<bundle.count;++i)if(bundle.created&(1u<<i)){
            if(platform.remove(bundle.targets[i])==MH_OK){bundle.created&=~(1u<<i);bundle.originals[i].store(nullptr);}
        }
    }
    return false;
}
SrNativeApiContext Context(uint32_t feature=UINT32_MAX,bool known=false){
    SrNativeApiContext context;context.originals={reinterpret_cast<SrNativeOriginals::Create>(nativeBundle.originals[0].load(std::memory_order_acquire)),
        reinterpret_cast<SrNativeOriginals::Evaluate>(nativeBundle.originals[1].load(std::memory_order_acquire)),reinterpret_cast<SrNativeOriginals::Release>(nativeBundle.originals[2].load(std::memory_order_acquire))};
    context.coreModule=coreModule.load();context.moduleGeneration=moduleGeneration.load();context.lifecycleGeneration=lifecycleGeneration.load();
    context.featureId=feature;context.featureKnown=known;context.modulePinned=modulePinned.load();return context;
}
Entry Find(const NVSDK_NGX_Handle* handleValue){
    std::lock_guard lock(registryMutex);for(const auto& item:registry)if(item.handle==handleValue&&item.module==moduleGeneration.load()&&item.life==lifecycleGeneration.load())return item;return {};
}
bool OutputHandle(NVSDK_NGX_Handle** pointer,NVSDK_NGX_Handle** value){
    __try{if(!pointer)return false;*value=*pointer;return *value!=nullptr;}__except(EXCEPTION_EXECUTE_HANDLER){return false;}
}
void Remember(const Entry& value){
    std::lock_guard lock(registryMutex);if(value.module!=moduleGeneration.load()||value.life!=lifecycleGeneration.load())return;
    for(auto& item:registry)if(item.handle==value.handle){item=value;return;}
    for(auto& item:registry)if(!item.handle){item=value;return;}Count(SrHookCounter::RegistryFull);
}
void Forget(const Entry& value){
    std::lock_guard lock(registryMutex);for(auto& item:registry)if(item.handle==value.handle&&item.ordinal==value.ordinal&&item.module==value.module&&item.life==value.life){item={};return;}
}
struct CallbackScope {CallbackScope(){++callbackDepth;}~CallbackScope(){--callbackDepth;}};
bool Selected(uint32_t feature){return feature==1||feature==13||(feature==11&&NativeFgStereoRequested());}
std::shared_ptr<SrNativeHandler> Callback(bool release=false){
    if(!slBundle.ready.load(std::memory_order_acquire)||!nativeBundle.ready.load(std::memory_order_acquire)||callbackDepth||(!release&&quiescing.load()))return {};
    return handler.load(std::memory_order_acquire);
}
NVSDK_NGX_Result NVSDK_CONV NativeCreate(ID3D12GraphicsCommandList* list,NVSDK_NGX_Feature feature,NVSDK_NGX_Parameter* params,NVSDK_NGX_Handle** output){
    // slInit can itself perform Shutdown and then Create before returning.
    // This is an actual Create boundary, never reconstruction from Evaluate.
    if(!callbackDepth)RearmLifecycle();
    Count(SrHookCounter::Create);auto context=Context(uint32_t(feature),true);context.createOrdinal=createOrdinal.fetch_add(1)+1;
    Lifecycle(SrLifecycleEvent::CreateBegin,0,false,SrNativeRetireResult::NotRegistered,uint32_t(feature),context.createOrdinal);
    uint64_t empty=0;firstCreateOrdinal.compare_exchange_strong(empty,context.createOrdinal);Publish(1,uint32_t(feature),nullptr,list);
    auto owner=Selected(uint32_t(feature))?Callback():nullptr;if(uint32_t(feature)==11)Count(SrHookCounter::Feature11Passthrough);
    CallbackScope scope;
    const auto result=owner?owner->Create(context,list,feature,params,output):context.originals.create(list,feature,params,output);
    NVSDK_NGX_Handle* created{};
    if(NVSDK_NGX_SUCCEED(result)&&OutputHandle(output,&created))Remember({created,uint32_t(feature),context.moduleGeneration,context.lifecycleGeneration,context.createOrdinal});
    Publish(1,uint32_t(feature),created,list,uint32_t(result),true);
    Lifecycle(SrLifecycleEvent::CreateEnd,uint32_t(result),true,SrNativeRetireResult::NotRegistered,uint32_t(feature),context.createOrdinal);return result;
}
NVSDK_NGX_Result NVSDK_CONV NativeEvaluate(ID3D12GraphicsCommandList* list,const NVSDK_NGX_Handle* handleValue,const NVSDK_NGX_Parameter* params,PFN_NVSDK_NGX_ProgressCallback progress){
    Count(SrHookCounter::Evaluate);const auto item=Find(handleValue);auto context=Context(item.handle?item.feature:UINT32_MAX,item.handle!=nullptr);context.createOrdinal=item.ordinal;
    if(!item.handle)Count(SrHookCounter::UnknownEvaluate);else if(item.feature==11)Count(SrHookCounter::Feature11Passthrough);
    Publish(2,context.featureId,handleValue,list);auto owner=item.handle&&Selected(item.feature)?Callback():nullptr;CallbackScope scope;
    cpu_timing::Scope totalTiming(cpu_timing::Stage::Ngx11Total,item.handle&&item.feature==11);
    NativeFgObservationTicket fgObservation;
    {cpu_timing::Scope timing(cpu_timing::Stage::NgxObserveBegin,item.handle&&item.feature==11);
        fgObservation=item.handle&&item.feature==11?BeginNativeFgObservation(list,handleValue,params):NativeFgObservationTicket{};}
    const auto result=owner?owner->Evaluate(context,list,handleValue,params,progress):context.originals.evaluate(list,handleValue,params,progress);
    {cpu_timing::Scope timing(cpu_timing::Stage::NgxObserveEnd,item.handle&&item.feature==11);
        EndNativeFgObservation(fgObservation,list,handleValue,params,result);}
    Publish(2,context.featureId,handleValue,list,uint32_t(result),true);return result;
}
NVSDK_NGX_Result NVSDK_CONV NativeRelease(NVSDK_NGX_Handle* handleValue){
    Count(SrHookCounter::Release);const auto item=Find(handleValue);auto context=Context(item.handle?item.feature:UINT32_MAX,item.handle!=nullptr);context.createOrdinal=item.ordinal;
    if(!item.handle)Count(SrHookCounter::UnknownRelease);else if(item.feature==11)Count(SrHookCounter::Feature11Passthrough);
    Lifecycle(SrLifecycleEvent::ReleaseBegin,0,false,SrNativeRetireResult::NotRegistered,context.featureId,item.ordinal);
    Publish(3,context.featureId,handleValue);auto owner=item.handle&&Selected(item.feature)?Callback(true):nullptr;CallbackScope scope;
#if defined(CVR_SR_NGX_TESTING) && defined(CVR_SR_NGX_NEGATIVE_RELEASE)
    // Finite negative control only: recreate the forbidden early-erase policy.
    // Production builds cannot enter this branch.
    if(item.handle)Forget(item);
#endif
    const auto result=owner?owner->Release(context,handleValue):context.originals.release(handleValue);
    if(NVSDK_NGX_SUCCEED(result)&&item.handle)Forget(item);Publish(3,context.featureId,handleValue,nullptr,uint32_t(result),true);
    Lifecycle(SrLifecycleEvent::ReleaseEnd,uint32_t(result),true,SrNativeRetireResult::NotRegistered,context.featureId,item.ordinal);return result;
}
void RearmLifecycle(){
    if(!rearmPending.load(std::memory_order_acquire)||callbackDepth)return;
    SrNativeLifecycleFactory factory{};uint64_t generation{};std::shared_ptr<SrNativeHandler> previous;
    {std::lock_guard lock(retirementMutex);
        if(!rearmPending.load()||rearmInProgress.load()||retirementOwner||!quiescing.load())return;
        factory=lifecycleFactory.load(std::memory_order_acquire);if(!factory){
            Lifecycle(SrLifecycleEvent::RearmEnd,uint32_t(SrRearmResult::NoFactory),true);return;}
        generation=lifecycleGeneration.load();previous=handler.load();rearmInProgress=true;
    }
    Lifecycle(SrLifecycleEvent::RearmBegin);std::shared_ptr<SrNativeHandler> next;
    try{next=factory(generation);}catch(...){/* Our factory failed; original SL API still runs exactly once. */}
    SrRearmResult result=SrRearmResult::FactoryFailed;
    {std::lock_guard lock(retirementMutex);
        if(!rearmPending.load()||retirementOwner||lifecycleGeneration.load()!=generation||handler.load()!=previous)result=SrRearmResult::ChangedLifecycle;
        else if(next&&next!=previous){handler.store(next,std::memory_order_release);rearmPending=false;quiescing=false;result=SrRearmResult::Installed;}
        else if(next)result=SrRearmResult::SameOwner;
        rearmInProgress=false;
    }
    // Both old and rejected-new owner destruction occur outside metadata locks.
    Lifecycle(SrLifecycleEvent::RearmEnd,uint32_t(result),true);Publish();
}
void Retry(){InstallInputHooks();EnsureSrNativeHooks();}
sl::Result SlInitHook(const sl::Preferences& prefs,uint64_t version){
    Lifecycle(SrLifecycleEvent::InitBegin);EnsureSrNativeHooks();RearmLifecycle();
    Publish(4);
    const auto original=reinterpret_cast<SlInit>(slBundle.originals[0].load(std::memory_order_acquire));const auto result=original(prefs,version);
    // A Create can occur inside a forwarded lifecycle API. Do not invalidate
    // that just-observed receipt on return; successful Shutdown advances epoch.
    slInitObserved=true;slInitResult=uint32_t(result);if(result==sl::Result::eOk)slInitialized=true;
    Retry();Publish(4,UINT32_MAX,nullptr,nullptr,uint32_t(result),true);Lifecycle(SrLifecycleEvent::InitEnd,uint32_t(result),true);return result;
}
sl::Result SlDeviceHook(void* device){
    Lifecycle(SrLifecycleEvent::DeviceBegin);Retry();RearmLifecycle();Publish(5);const auto original=reinterpret_cast<SlDevice>(slBundle.originals[1].load(std::memory_order_acquire));const auto result=original(device);
    slDeviceObserved=true;slDeviceResult=uint32_t(result);Retry();Publish(5,UINT32_MAX,nullptr,nullptr,uint32_t(result),true);Lifecycle(SrLifecycleEvent::DeviceEnd,uint32_t(result),true);return result;
}
sl::Result SlAllocateHook(void* list,sl::Feature feature,const sl::ViewportHandle& viewport){
    Lifecycle(SrLifecycleEvent::AllocateBegin,0,false,SrNativeRetireResult::NotRegistered,uint32_t(feature));Retry();RearmLifecycle();Publish(6,uint32_t(feature),nullptr,list);const auto original=reinterpret_cast<SlAllocate>(slBundle.originals[2].load(std::memory_order_acquire));const auto result=original(list,feature,viewport);
    Retry();Publish(6,uint32_t(feature),nullptr,list,uint32_t(result),true);Lifecycle(SrLifecycleEvent::AllocateEnd,uint32_t(result),true,SrNativeRetireResult::NotRegistered,uint32_t(feature));return result;
}
sl::Result SlShutdownHook(){
    Count(SrHookCounter::Shutdown);Lifecycle(SrLifecycleEvent::ShutdownBegin);
    if(shutdownDepth||rearmInProgress.load()){Count(SrHookCounter::NotReady);Publish(7,UINT32_MAX,nullptr,nullptr,uint32_t(sl::Result::eErrorInvalidState),true,SrNativeRetireResult::ReentrantDenied);
        Lifecycle(SrLifecycleEvent::ShutdownRefused,uint32_t(sl::Result::eErrorInvalidState),true,SrNativeRetireResult::ReentrantDenied);return sl::Result::eErrorInvalidState;}
    struct Depth {Depth(){++shutdownDepth;}~Depth(){--shutdownDepth;}} depth;
    std::shared_ptr<SrNativeHandler> owner;
    {std::lock_guard lock(retirementMutex);rearmPending=false;owner=retirementOwner;if(!owner){owner=handler.load();if(owner){retirementOwner=owner;quiescing=true;}}}
    auto retired=SrNativeRetireResult::NotRegistered;
    Publish(7,UINT32_MAX,nullptr,nullptr,0,false,owner?SrNativeRetireResult::InProgress:SrNativeRetireResult::NotRegistered);
    if(owner){
        Lifecycle(SrLifecycleEvent::RetirementBegin,0,false,SrNativeRetireResult::InProgress);
        retired=owner->BeforeHostShutdown(); // Never under metadata/installer lock.
        Lifecycle(SrLifecycleEvent::RetirementEnd,0,false,retired);
        if(retired!=SrNativeRetireResult::Retired){Count(SrHookCounter::NotReady);Publish(7,UINT32_MAX,nullptr,nullptr,uint32_t(sl::Result::eErrorInvalidState),true,retired);
            Lifecycle(SrLifecycleEvent::ShutdownRefused,uint32_t(sl::Result::eErrorInvalidState),true,retired);return sl::Result::eErrorInvalidState;}
        Count(SrHookCounter::Retired);
    }else Count(SrHookCounter::NotRegistered);
    const auto original=reinterpret_cast<SlShutdown>(slBundle.originals[3].load(std::memory_order_acquire));
    Lifecycle(SrLifecycleEvent::ShutdownOriginalBegin);
    const auto result=original(); // Native exceptions/results preserved.
    if(result==sl::Result::eOk){
        slInitialized=false;++lifecycleGeneration;{std::lock_guard lock(registryMutex);registry={};}
        std::shared_ptr<SrNativeHandler> release;{std::lock_guard lock(retirementMutex);release=std::move(retirementOwner);rearmPending=retired==SrNativeRetireResult::Retired;}
    }
    Publish(7,UINT32_MAX,nullptr,nullptr,uint32_t(result),true,retired);Lifecycle(SrLifecycleEvent::ShutdownOriginalEnd,uint32_t(result),true,retired);return result;
}
[[maybe_unused]] bool InstallEarly(){const auto sl=InstallSrSlHooks();const auto inputs=InstallInputHooks();EnsureSrNativeHooks();return sl&&inputs;}
}
bool SetSrNativeHandler(std::shared_ptr<SrNativeHandler> next){
    std::shared_ptr<SrNativeHandler> previous;
    {std::lock_guard lock(retirementMutex);if(rearmInProgress.load()||(retirementOwner&&retirementOwner!=next)||(rearmPending.load()&&next))return false;
        previous=handler.exchange(std::move(next),std::memory_order_acq_rel);if(!retirementOwner&&!rearmPending.load())quiescing=false;}
    return true; // previous destructor occurs outside metadata locks
}
void SetSrNativeLifecycleFactory(SrNativeLifecycleFactory factory) noexcept {lifecycleFactory.store(factory,std::memory_order_release);}
SrNativeHookReadiness GetSrNativeHookReadiness(){
    return {slBundle.ready.load(),nativeBundle.ready.load(),modulePinned.load(),quiescing.load(),slInitObserved.load(),slDeviceObserved.load(),
        coreModule.load(),moduleGeneration.load(),lifecycleGeneration.load(),bool(handler.load())};
}
bool InstallSrSlHooks(){
    if(slBundle.ready.load(std::memory_order_acquire))return true;std::unique_lock lock(installMutex,std::try_to_lock);if(!lock)return false;
    if(slBundle.ready.load())return true;Count(SrHookCounter::SlInstall);Module module{};
    const std::array<void*,4> detours{reinterpret_cast<void*>(&SlInitHook),reinterpret_cast<void*>(&SlDeviceHook),reinterpret_cast<void*>(&SlAllocateHook),reinterpret_cast<void*>(&SlShutdownHook)};
    const auto okay=platform.sl(module)&&Install(slBundle,module,detours);if(!okay)Count(SrHookCounter::SlInstallFailed);Publish();return okay;
}
bool EnsureSrNativeHooks(){
    Count(SrHookCounter::Ensure);if(nativeBundle.ready.load(std::memory_order_acquire))return true;std::unique_lock lock(installMutex,std::try_to_lock);if(!lock)return false;
    if(nativeBundle.ready.load())return true;Module module{};if(!platform.native(module))return false;Count(SrHookCounter::NativeInstall);
    if(!coreModule.load()){coreModule=module.base;++moduleGeneration;}
    if(coreModule.load()!=module.base)return false;
    const std::array<void*,4> detours{reinterpret_cast<void*>(&NativeCreate),reinterpret_cast<void*>(&NativeEvaluate),reinterpret_cast<void*>(&NativeRelease),nullptr};
    const auto okay=Install(nativeBundle,module,detours);modulePinned=nativeBundle.module!=0;
    if(!okay)Count(SrHookCounter::NativeInstallFailed);Publish();return okay;
}
#ifndef CVR_SR_NGX_TESTING
CVR_HOOK("SrNgxEarly",::cvr::hooks::Stage::PreDevice,30,InstallEarly);
#else
namespace sr_test {
void Reset(const Platform& replacement){
    handler.store({});retirementOwner.reset();lifecycleFactory=nullptr;rearmPending=false;rearmInProgress=false;quiescing=false;modulePinned=false;slInitObserved=false;slDeviceObserved=false;slInitialized=false;
    coreModule=0;moduleGeneration=0;lifecycleGeneration=1;createOrdinal=0;firstCreateOrdinal=0;slInitResult=0;slDeviceResult=0;registry={};
    for(auto* bundle:{&slBundle,&nativeBundle}){bundle->targets={};for(auto& original:bundle->originals)original=nullptr;bundle->created=0;bundle->enabled=0;bundle->ready=false;bundle->everPublished=false;bundle->module=0;}
    for(auto& c:counters)c=0;droppedStatus=0;CyberpunkVR_SrNativeHookStatusSeq=0;CyberpunkVR_SrNativeHookStatus={};
    lifecycleDropped=0;lifecycleWriter.clear();CyberpunkVR_SrNativeLifecycleStatusSeq=0;CyberpunkVR_SrNativeLifecycleStatus={};platform=replacement;
}
bool DriverStorePath(const wchar_t* path,const wchar_t* windows){return path&&windows&&IsDriverStorePath(path,windows);}
NVSDK_NGX_Result Create(ID3D12GraphicsCommandList* a,NVSDK_NGX_Feature b,NVSDK_NGX_Parameter*c,NVSDK_NGX_Handle**d){return NativeCreate(a,b,c,d);}
NVSDK_NGX_Result Evaluate(ID3D12GraphicsCommandList*a,const NVSDK_NGX_Handle*b,const NVSDK_NGX_Parameter*c,PFN_NVSDK_NGX_ProgressCallback d){return NativeEvaluate(a,b,c,d);}
NVSDK_NGX_Result Release(NVSDK_NGX_Handle* h){return NativeRelease(h);}
sl::Result Init(const sl::Preferences&p,uint64_t v){return SlInitHook(p,v);}
sl::Result SetDevice(void* d){return SlDeviceHook(d);}
sl::Result Allocate(void* c,sl::Feature f,const sl::ViewportHandle&v){return SlAllocateHook(c,f,v);}
sl::Result Shutdown(){return SlShutdownHook();}
}
#endif
}
