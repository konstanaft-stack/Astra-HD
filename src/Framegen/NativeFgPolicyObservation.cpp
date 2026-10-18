#include "Framegen/NativeFgObservation.hpp"
#include "Framegen/NativeFgPolicyTypes.hpp"
#include "Framegen/SrNativeApi.hpp"
#include "Framegen/CpuStageTiming.hpp"
#include "Framegen/NativeReflexTypes.hpp"
#include <MinHook.h>
#include <mutex>
#include <atomic>
#include <array>
#include <cstring>
#include <cstddef>
#include "Framegen/Framegen.hpp"
extern void Log(const char*,...);
extern "C" int32_t CyberpunkVR_VrcamDlssViewport;
namespace cvr::framegen {
namespace {
using GetFunction=sl::Result(*)(sl::Feature,const char*,void*&);
using Options=sl::Result(*)(const sl::ViewportHandle&,const sl::DLSSGOptions&);
using State=sl::Result(*)(const sl::ViewportHandle&,sl::DLSSGState&,const sl::DLSSGOptions*);
Options originalOptions{};State originalState{};
std::mutex install;std::atomic<bool> ready{};
std::array<std::atomic<unsigned>,2> modes{};
std::atomic<bool> everEnabled{};
NativePolicyStatusTracker policyStatus;
sl::PFun_slReflexSleep* originalReflexSleep{};
sl::PFun_slReflexSetOptions* originalReflexOptions{};
sl::PFun_slReflexGetState* originalReflexState{};
std::mutex reflexInstall;std::atomic<bool> reflexReady{};
sl::Result ObserveReflexSleep(const sl::FrameToken& frame){
    cpu_timing::Scope timing(cpu_timing::Stage::ReflexSleep);
    return originalReflexSleep(frame);
}
sl::Result ObserveReflexOptions(const sl::ReflexOptions& options){
    const auto result=originalReflexOptions(options);
    if(options.structVersion==1){
        const uint64_t value=(uint64_t(unsigned(options.mode))<<32)|options.frameLimitUs;
        static std::atomic<uint64_t> previous{UINT64_MAX};
        if(previous.exchange(value)!=value)Log("[FG-REFLEX-OPTIONS] version=%llu mode=%u frameLimitUs=%u markers=%d result=%u\n",
            options.structVersion,unsigned(options.mode),options.frameLimitUs,int(options.useMarkersToOptimize),unsigned(result));
    }
    return result;
}
sl::Result ObserveReflexState(sl::ReflexState& state){
    // GetState/SetOptions are NOT thread-safe in Streamline. Observe only the
    // game's existing invocation, never add a call from Present or another thread.
    const auto result=originalReflexState(state);
    static std::atomic<uint64_t> samples{};const auto n=++samples;
    if((n<=4||n%240==0)&&(state.structVersion==1||state.structVersion==2)){
        unsigned count{};uint64_t active{},span{},queue{},lastFrame{};
        if(result==sl::Result::eOk&&state.latencyReportAvailable){
            for(const auto& r:state.frameReport)if(r.frameID&&r.gpuFrameTimeUs){
                ++count;active+=r.gpuActiveRenderTimeUs;span+=r.gpuFrameTimeUs;lastFrame=(std::max)(lastFrame,r.frameID);
                if(r.osRenderQueueEndTime>=r.osRenderQueueStartTime)queue+=r.osRenderQueueEndTime-r.osRenderQueueStartTime;
            }
        }
        Log("[FG-REFLEX-REPORT] result=%u available=%d count=%u latestFrame=%llu gpuActiveUs=%.3f gpuFrameUs=%.3f osQueueUs=%.3f\n",
            unsigned(result),int(state.latencyReportAvailable),count,lastFrame,count?double(active)/count:0,count?double(span)/count:0,count?double(queue)/count:0);
    }
    return result;
}
void EnsureCpuReflexTiming(){
    if(!cpu_timing::Enabled()||reflexReady.load(std::memory_order_acquire))return;
    std::unique_lock lock(reflexInstall,std::try_to_lock);if(!lock||reflexReady.load())return;
    const auto module=GetModuleHandleW(L"sl.interposer.dll");if(!module)return;
    const auto get=reinterpret_cast<GetFunction>(GetProcAddress(module,"slGetFeatureFunction"));if(!get)return;
    void *sleep{},*options{},*state{};
    if(get(sl::kFeatureReflex,"slReflexSleep",sleep)!=sl::Result::eOk||!sleep||
       get(sl::kFeatureReflex,"slReflexSetOptions",options)!=sl::Result::eOk||!options||
       get(sl::kFeatureReflex,"slReflexGetState",state)!=sl::Result::eOk||!state)return;
    for(auto address:{sleep,options,state}){HMODULE held{};if(!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_PIN,
        reinterpret_cast<LPCWSTR>(address),&held))return;}
    if(MH_CreateHook(sleep,reinterpret_cast<void*>(&ObserveReflexSleep),reinterpret_cast<void**>(&originalReflexSleep))!=MH_OK)return;
    if(MH_CreateHook(options,reinterpret_cast<void*>(&ObserveReflexOptions),reinterpret_cast<void**>(&originalReflexOptions))!=MH_OK){MH_RemoveHook(sleep);return;}
    if(MH_CreateHook(state,reinterpret_cast<void*>(&ObserveReflexState),reinterpret_cast<void**>(&originalReflexState))!=MH_OK){MH_RemoveHook(sleep);MH_RemoveHook(options);return;}
    if(MH_QueueEnableHook(sleep)!=MH_OK||MH_QueueEnableHook(options)!=MH_OK||MH_QueueEnableHook(state)!=MH_OK||MH_ApplyQueued()!=MH_OK){
        MH_DisableHook(sleep);MH_DisableHook(options);MH_DisableHook(state);MH_RemoveHook(sleep);MH_RemoveHook(options);MH_RemoveHook(state);return;}
    reflexReady.store(true,std::memory_order_release);Log("[FG-REFLEX-TIMING] existing Sleep/Options/GetState observed; no extra API calls or changed values\n");
}
bool HostViewOnly(){static const bool enabled=[] {char v[8]{};return GetEnvironmentVariableA("CPVR_NATIVE_FG_HOST_VIEW_ONLY",v,sizeof(v))==1&&v[0]=='1';}();return enabled;}
sl::Result ObserveOptions(const sl::ViewportHandle&view,const sl::DLSSGOptions&o){
    const auto viewport=unsigned(view);
    if(viewport==0&&o.mode!=sl::DLSSGMode::eOff)everEnabled.store(true,std::memory_order_release);
    sl::DLSSGOptions translated;const sl::DLSSGOptions* applied=&o;
    const bool knownVersion=o.structVersion>=1&&o.structVersion<=5;
    const bool disableCompanion=HostViewOnly()&&!GetSettings().aerV2&&viewport==uint32_t(CyberpunkVR_VrcamDlssViewport);
    // The game requests RetainResourcesWhenOff even for a disabled viewport.
    // Let Streamline retire that viewport's own allocations on OFF. Otherwise
    // toggles/secondary views can keep FG memory while no frames are generated.
    // Preserve ON options, caller storage, the versioned ABI and SDK result.
    // A policy-only observer must remain observational.
    const bool releaseIdle=knownVersion&&NativeFgStereoRequested()&&
        (disableCompanion||o.mode==sl::DLSSGMode::eOff)&&
        (unsigned(o.flags)&unsigned(sl::DLSSGFlags::eRetainResourcesWhenOff));
    if(knownVersion&&(disableCompanion||releaseIdle)){
        const size_t bytes=o.structVersion==1?offsetof(sl::DLSSGOptions,bReserved15):
            o.structVersion==2?offsetof(sl::DLSSGOptions,queueParallelismMode):
            o.structVersion==3?offsetof(sl::DLSSGOptions,enableUserInterfaceRecomposition):
            o.structVersion==4?offsetof(sl::DLSSGOptions,dynamicTargetFrameRate):sizeof(sl::DLSSGOptions);
        std::memcpy(&translated,&o,bytes);
        if(disableCompanion)translated.mode=sl::DLSSGMode::eOff;
        if(releaseIdle)translated.flags=sl::DLSSGFlags(unsigned(translated.flags)&~unsigned(sl::DLSSGFlags::eRetainResourcesWhenOff));
        applied=&translated;
    }
    const auto result=originalOptions(view,*applied);static std::atomic<uint64_t> calls{};const auto n=++calls;
    policyStatus.Observe(viewport,result==sl::Result::eOk,applied->mode!=sl::DLSSGMode::eOff,applied->numFramesToGenerate);
    if(viewport<2 && result==sl::Result::eOk)modes[viewport].store(unsigned(applied->mode),std::memory_order_release);
    if(n<=12||n%240==0)Log("[NATIVE-FG-OPTIONS] call=%llu viewport=%u version=%llu mode=%u count=%u flags=%u result=%u appliedMode=%u appliedFlags=%u\n",
        n,unsigned(view),o.structVersion,unsigned(o.mode),o.numFramesToGenerate,unsigned(o.flags),unsigned(result),unsigned(applied->mode),unsigned(applied->flags));
    return result;
}
sl::Result ObserveState(const sl::ViewportHandle&view,sl::DLSSGState&s,const sl::DLSSGOptions*o){
    // Observe the game's existing read; an additional GetState would consume
    // its frames-presented counter. Forward once and never modify the result.
    const auto result=originalState(view,s,o);static std::atomic<uint64_t> calls{};const auto n=++calls;
    if(n<=12||n%240==0)Log("[NATIVE-FG-STATE-API] call=%llu viewport=%u version=%llu status=%u presented=%u minSize=%u result=%u\n",
        n,unsigned(view),s.structVersion,unsigned(s.status),s.numFramesActuallyPresented,s.minWidthOrHeight,unsigned(result));
    return result;
}
}
bool NativeFgGameEnabled() noexcept {return modes[0].load(std::memory_order_acquire)!=0;}
NativePolicyStatus ReadNativeFgPolicyStatus() noexcept {return policyStatus.Read();}
bool NativeFgGameWasEnabled() noexcept {return everEnabled.load(std::memory_order_acquire);}
void EnsureNativeFgPolicyObservation() noexcept {
    EnsureCpuReflexTiming();
    if(!NativeFgPolicyObservationRequested())return;
    // GetState is not thread-safe and consumes its presented-frame interval.
    // ObserveState already sees the game's calls on their original thread;
    // do not add another reader from the Present hook, even once per viewport.
    if(ready.load(std::memory_order_acquire))return;
    std::unique_lock lock(install,std::try_to_lock);if(!lock||ready.load())return;
    const auto module=GetModuleHandleW(L"sl.interposer.dll");if(!module)return;
    const auto get=reinterpret_cast<GetFunction>(GetProcAddress(module,"slGetFeatureFunction"));if(!get)return;
    void *options{},*state{};
    if(get(sl::kFeatureDLSS_G,"slDLSSGSetOptions",options)!=sl::Result::eOk||!options||
       get(sl::kFeatureDLSS_G,"slDLSSGGetState",state)!=sl::Result::eOk||!state)return;
    HMODULE holdOptions{},holdState{};
    if(!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_PIN,reinterpret_cast<LPCWSTR>(options),&holdOptions)||
       !GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_PIN,reinterpret_cast<LPCWSTR>(state),&holdState))return;
    if(MH_CreateHook(options,reinterpret_cast<void*>(&ObserveOptions),reinterpret_cast<void**>(&originalOptions))!=MH_OK)return;
    if(MH_CreateHook(state,reinterpret_cast<void*>(&ObserveState),reinterpret_cast<void**>(&originalState))!=MH_OK){MH_RemoveHook(options);return;}
    if(MH_QueueEnableHook(options)!=MH_OK||MH_QueueEnableHook(state)!=MH_OK||MH_ApplyQueued()!=MH_OK){
        MH_DisableHook(options);MH_DisableHook(state);MH_RemoveHook(options);MH_RemoveHook(state);return;}
    ready.store(true,std::memory_order_release);Log("[NATIVE-FG-POLICY] original options/state observer installed\n");
}
bool NativeFgPolicyObservationRequested() noexcept {
    static const bool policyOnly=[] {char flag[8]{};return GetEnvironmentVariableA("CPVR_NATIVE_FG_POLICY_ONLY",flag,sizeof(flag))==1&&flag[0]=='1';}();
    return NativeFgStereoRequested()||policyOnly;
}
}
