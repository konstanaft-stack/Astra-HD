#pragma once
#include <d3d12.h>
#include <nvsdk_ngx.h>
#include <cstdint>
#include <memory>

namespace cvr::framegen {
inline bool NativeFgStereoRequested(){static const bool enabled=[] {char flag[8]{};return GetEnvironmentVariableA("CPVR_NATIVE_FG_STEREO",flag,sizeof(flag))==1&&flag[0]=='1';}();return enabled;}
struct SrNativeOriginals {
    using Create=decltype(&NVSDK_NGX_D3D12_CreateFeature);
    using Evaluate=decltype(&NVSDK_NGX_D3D12_EvaluateFeature);
    using Release=decltype(&NVSDK_NGX_D3D12_ReleaseFeature);
    Create create{};Evaluate evaluate{};Release release{};
};
struct SrNativeApiContext {
    SrNativeOriginals originals; // native trampolines; private calls bypass hooks
    uintptr_t coreModule{};
    uint64_t moduleGeneration{},lifecycleGeneration{};
    uint32_t featureId=UINT32_MAX;
    bool featureKnown=false,handleIdKnown=false;
    uint64_t handleId{},createOrdinal{};
    // A pinned module proves code lifetime, NOT that host NGX Init succeeded.
    bool modulePinned=false;
};
enum class SrNativeRetireResult:uint32_t {NotRegistered,Retired,NotReady,ReentrantDenied,InProgress};
class SrNativeHandler {
public:
    virtual ~SrNativeHandler()=default;
    // Called for feature1/13, and opt-in native feature11 stereo. Default methods forward exactly once. The
    // owner may wrap originals to implement its controlled companion pair.
    // Parameters/list/context are borrowed for this call; never retain them.
    virtual NVSDK_NGX_Result Create(const SrNativeApiContext& context,
        ID3D12GraphicsCommandList* list,NVSDK_NGX_Feature feature,
        NVSDK_NGX_Parameter* parameters,NVSDK_NGX_Handle** handle) {
        return context.originals.create(list,feature,parameters,handle);
    }
    virtual NVSDK_NGX_Result Evaluate(const SrNativeApiContext& context,
        ID3D12GraphicsCommandList* list,const NVSDK_NGX_Handle* handle,
        const NVSDK_NGX_Parameter* parameters,PFN_NVSDK_NGX_ProgressCallback callback) {
        return context.originals.evaluate(list,handle,parameters,callback);
    }
    virtual NVSDK_NGX_Result Release(const SrNativeApiContext& context,NVSDK_NGX_Handle* handle) {
        return context.originals.release(handle);
    }
    // Required for every registered owner. Runs outside hook metadata locks.
    // NotReady quiesces handler admissions, keeps this owner pinned, and refuses
    // host slShutdown with eErrorInvalidState. Retired permits original shutdown.
    // A missing handler is diagnosed NotRegistered and forwards original.
    virtual SrNativeRetireResult BeforeHostShutdown()=0;
};
struct SrNativeHookReadiness {
    bool slReady=false,nativeReady=false,modulePinned=false,quiescing=false;
    bool slInitObserved=false,slDeviceObserved=false;
    // Native Init is intentionally NOT hooked/proven by module/SL observations.
    uintptr_t coreModule{};uint64_t moduleGeneration{},lifecycleGeneration{};
    bool handlerRegistered=false;
};
// Atomic strong callback lease. Replacement does not destroy an in-flight
// callback. A NotReady retirement owner cannot be replaced until retired.
bool SetSrNativeHandler(std::shared_ptr<SrNativeHandler>);
// Registered once by integration. After a registered owner reports Retired AND
// original slShutdown returns eOk, the next Init/Device/Allocate or actual native
// Create boundary invokes this factory BEFORE forwarding. Never invoked by
// Evaluate. Return a NEW configured owner and publish its integration-side
// references before returning; do not call SetSrNativeHandler from the factory.
// Null/throw/same retired owner keeps admission quiesced and permits a later
// lifecycle/Create retry. Callback runs serially outside hook metadata locks.
// SL lifecycle APIs are not thread-safe; the host must serialize them.
using SrNativeLifecycleFactory=std::shared_ptr<SrNativeHandler>(*)(uint64_t lifecycleGeneration);
void SetSrNativeLifecycleFactory(SrNativeLifecycleFactory) noexcept;
SrNativeHookReadiness GetSrNativeHookReadiness();
bool InstallSrSlHooks();
bool EnsureSrNativeHooks();
// Hook registry contains only observed HOST Create results. It registers after
// Create callback returns success and erases only after successful Release.
// Companion calls through context.originals bypass this registry: the paired
// history owner alone owns their identities. Feature11/unknown are passthrough.
}
