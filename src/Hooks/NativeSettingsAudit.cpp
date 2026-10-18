#include "Hooks/Hook.hpp"
#include "Hooks/NativeSettingsPreflight.hpp"
#include "Hooks/NativeSettingsAdmission.hpp"
#include "Hooks/CetLifecycle.hpp"
#include "Render/NativeSettingsTransaction.hpp"
#include "Overlay/PpdNativeApply.hpp"
#include "Core/VrCoreShared.hpp"
#include <Windows.h>
#include <MinHook.h>
#include <atomic>
#include <cstring>
#include <chrono>

extern void Log(const char*,...);
extern "C" UINT GetForcedWindowWidth();
extern "C" UINT GetForcedWindowHeight();
extern "C" {
__declspec(dllexport) std::atomic<uint64_t> CyberpunkVR_SettingsApplyAuditHits{0};
__declspec(dllexport) std::atomic<uint64_t> CyberpunkVR_SettingsResizeAuditHits{0};
__declspec(dllexport) std::atomic<uint64_t> CyberpunkVR_SettingsResizeUnexpected{0};
__declspec(dllexport) std::atomic<uint64_t> CyberpunkVR_SettingsResizeUnowned{0};
__declspec(dllexport) std::atomic<uint64_t> CyberpunkVR_SettingsSnapshotFailures{0};
__declspec(dllexport) std::atomic<uint64_t> CyberpunkVR_SettingsApplyDeferred{0};
__declspec(dllexport) std::atomic<uint64_t> CyberpunkVR_SettingsOriginalApplyCalls{0};
__declspec(dllexport) std::atomic<uint32_t> CyberpunkVR_SettingsLastDeferredReason{0};
__declspec(dllexport) std::atomic<uint32_t> CyberpunkVR_SettingsLastAdmission{0};
__declspec(dllexport) std::atomic<uint32_t> CyberpunkVR_SettingsLastResize{0};
__declspec(dllexport) std::atomic<uint32_t> CyberpunkVR_SettingsDeferredDrainObserved{0};
__declspec(dllexport) std::atomic<uint32_t> CyberpunkVR_SettingsDeferredDrainQueues{0};
__declspec(dllexport) std::atomic<uint32_t> CyberpunkVR_SettingsDeferredDrainFailed{0};
__declspec(dllexport) std::atomic<uint32_t> CyberpunkVR_SettingsDeferredDrainInitialized{255};
__declspec(dllexport) std::atomic<uint32_t> CyberpunkVR_SettingsDeferredDrainNoReferences{0};
extern std::atomic<uint32_t> CyberpunkVR_CetDrainInitialized,CyberpunkVR_CetDrainNoReferences;
extern std::atomic<uint32_t> CyberpunkVR_ResizeDrainQueueCount,CyberpunkVR_ResizeDrainRegistryFailed;
}

namespace {
using namespace cvr::hooks;
using NativeApply=void(*)(void*);
NativeApply originalApply{},originalResizeHelper{};
struct Frame {
    void* settings{};
    uint64_t serial{};
    NativeSettingsSnapshot before;
    NativeSettingsDecision decision;
    NativeDimensionWrite planned;
    bool valid{},requiresAdmission{};
    uint64_t revision{};
    cvr::gpu::NativeSettingsTransaction::Pass* transition{};
    unsigned helpers{};
    bool drainObserved{};uint32_t drainQueues{},drainFailed{},drainInitialized{255},drainNoReferences{};
    Frame* previous{};
};
thread_local Frame* current{};
cvr::gpu::NativeSettingsTransaction transactions;
auto& deferred=CyberpunkVR_SettingsApplyDeferred;
std::atomic<uint64_t> completed{};

bool ReadSnapshot(void* settings,NativeSettingsSnapshot* out) noexcept {
    if(!settings||!out)return false;
    __try {std::memcpy(out->bytes.data(),settings,out->bytes.size());return true;}
    __except(EXCEPTION_EXECUTE_HANDLER){return false;}
}

NativeDimensionWrite PlannedWrite(uint64_t& revision) {
    if(SettingsResolutionOverrideSuspended())return {};
    if(const auto request=cvr::ppd::ReadNativeRequestedSize()) {
        if(request->stage==cvr::ppd::NativeApplyStage::Error)return {};
        revision=request->revision;
        return {NativeDimensionWrite::Kind::Pending,{request->size.width,request->size.height}};
    }
    const auto width=GetForcedWindowWidth(),height=GetForcedWindowHeight();
    if(width&&height)return {NativeDimensionWrite::Kind::Author,{width,height}};
    return {};
}

unsigned ChangedMask(const NativeSettingsDecision& decision) {
    unsigned mask=0;for(size_t i=0;i<decision.changed.size();++i)if(decision.changed[i])mask|=1u<<i;return mask;
}

void Apply(void* settings) {
    Frame frame;frame.settings=settings;frame.previous=current;
    frame.serial=CyberpunkVR_SettingsApplyAuditHits.fetch_add(1,std::memory_order_relaxed)+1;
    frame.valid=ReadSnapshot(settings,&frame.before);
    if(frame.valid){frame.decision=InspectNativeSettings(frame.before);frame.planned=PlannedWrite(frame.revision);
        frame.requiresAdmission=RequiresNativeSettingsAdmission(frame.before,frame.planned);}
    else CyberpunkVR_SettingsSnapshotFailures.fetch_add(1,std::memory_order_relaxed);
    CyberpunkVR_SettingsLastAdmission.store(frame.requiresAdmission,std::memory_order_relaxed);
    CyberpunkVR_SettingsLastResize.store(frame.decision.willResize,std::memory_order_relaxed);
    struct Restore {Frame* previous;~Restore(){current=previous;}}restore{current};
    current=&frame;
    if(cvr::cet::LifecycleRequested()&&cvr::cet::ProducerHooksInstalled()) {
        // This is before the native apply's first mutation. A busy recorder or
        // failed GPU fence leaves the complete native operation for a later tick.
        if(!frame.valid){CyberpunkVR_SettingsLastDeferredReason.store(1000);++deferred;return;}
        using namespace std::chrono_literals;
        const cvr::gpu::NativeSettingsTransaction::Plan plan{
            reinterpret_cast<uintptr_t>(settings),frame.revision,frame.planned.size.width,frame.planned.size.height};
        const auto result=transactions.Run(plan,frame.requiresAdmission,frame.decision.willResize,
            [&]{return cvr::cet::Lifetime().ReserveBeforeDimensions(50ms,[&]{
                const auto drained=cvr::cet::DrainGraphics(2000);frame.drainObserved=true;
                frame.drainQueues=CyberpunkVR_ResizeDrainQueueCount.load();frame.drainFailed=CyberpunkVR_ResizeDrainRegistryFailed.load();
                frame.drainInitialized=CyberpunkVR_CetDrainInitialized.load();frame.drainNoReferences=CyberpunkVR_CetDrainNoReferences.load();return drained;});},
            [&](auto& pass){frame.transition=&pass;cvr::cet::ControlledResetScope owned;++CyberpunkVR_SettingsOriginalApplyCalls;originalApply(settings);});
        if(!result.ran){CyberpunkVR_SettingsLastDeferredReason.store(unsigned(result.status));
            CyberpunkVR_SettingsDeferredDrainObserved.store(frame.drainObserved);CyberpunkVR_SettingsDeferredDrainQueues.store(frame.drainQueues);
            CyberpunkVR_SettingsDeferredDrainFailed.store(frame.drainFailed);CyberpunkVR_SettingsDeferredDrainInitialized.store(frame.drainInitialized);CyberpunkVR_SettingsDeferredDrainNoReferences.store(frame.drainNoReferences);
            const auto n=++deferred;if(n<=8||n%120==0)
            Log("[SETTINGS-LIFETIME] deferred apply=%llu revision=%llu reason=%u pending=%d\n",frame.serial,frame.revision,unsigned(result.status),int(result.pending));return;}
        if(frame.helpers){++completed;Log("[SETTINGS-LIFETIME] completed apply=%llu revision=%llu helpers=%u pending=%d\n",frame.serial,frame.revision,frame.helpers,int(result.pending));}
    }else {
        // The first native apply creates the device and installs PreDevice
        // hooks. Never block that bootstrap; live PPD needs proven coverage.
        if(cvr::cet::LifecycleRequested()&&(!cvr::cet::ProducerBootstrapPending()||frame.planned.kind==NativeDimensionWrite::Kind::Pending)){
            CyberpunkVR_SettingsLastDeferredReason.store(1001);
            const auto n=++deferred;if(n<=8||n%120==0)Log("[SETTINGS-LIFETIME] producer coverage unavailable; apply deferred\n");return;}
        ++CyberpunkVR_SettingsOriginalApplyCalls;originalApply(settings);
    }
    if(frame.serial<=4||frame.helpers||frame.serial%600==0) {
        NativeSettingsSnapshot after;const bool afterValid=ReadSnapshot(settings,&after);
        Log("[SETTINGS-APPLY-AUDIT] call=%llu settings=%p read=%d index=%u mode=%u dirty=%u rawResize=%d planned=%u:%ux%u admission=%d mask=%03X helpers=%u active=%ux%u pending=%ux%u afterRead=%d after=%ux%u/%ux%u unexpected=%llu unowned=%llu\n",
            frame.serial,settings,int(frame.valid),frame.before.U32(0x38),unsigned(frame.before.U8(0x20)),unsigned(frame.before.U8(0x10f)),
            int(frame.decision.willResize),unsigned(frame.planned.kind),frame.planned.size.width,frame.planned.size.height,int(frame.requiresAdmission),
            ChangedMask(frame.decision),frame.helpers,frame.before.U32(0x18),frame.before.U32(0x1c),frame.before.U32(0x84),frame.before.U32(0x88),int(afterValid),
            after.U32(0x18),after.U32(0x1c),after.U32(0x84),after.U32(0x88),
            CyberpunkVR_SettingsResizeUnexpected.load(std::memory_order_relaxed),CyberpunkVR_SettingsResizeUnowned.load(std::memory_order_relaxed));
    }
}

void ResizeHelper(void* settings) {
    const auto call=CyberpunkVR_SettingsResizeAuditHits.fetch_add(1,std::memory_order_relaxed)+1;
    if(current&&current->settings==settings) {
        ++current->helpers;
        if(!current->valid||!current->requiresAdmission) {
            const auto unexpected=CyberpunkVR_SettingsResizeUnexpected.fetch_add(1,std::memory_order_relaxed)+1;
            if(unexpected<=8||unexpected%60==0)Log("[SETTINGS-APPLY-AUDIT] UNEXPECTED resize=%llu apply=%llu settings=%p valid=%d\n",call,current->serial,settings,int(current->valid));
        }
    }else{
        const auto unowned=CyberpunkVR_SettingsResizeUnowned.fetch_add(1,std::memory_order_relaxed)+1;
        if(unowned<=8||unowned%60==0)Log("[SETTINGS-APPLY-AUDIT] UNOWNED resize=%llu settings=%p\n",call,settings);
    }
    originalResizeHelper(settings);
    if(current&&current->settings==settings&&current->transition)
        current->transition->NativeResizeCompleted();
}

bool Enabled(){char value[8]{};return cvr::cet::LifecycleRequested()||(GetEnvironmentVariableA("CPVR_SETTINGS_APPLY_AUDIT",value,sizeof(value))==1&&value[0]=='1');}
bool Install(){
    const auto base=reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));if(!base)return false;
    const auto*dos=reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    const auto*nt=reinterpret_cast<const IMAGE_NT_HEADERS64*>(base+dos->e_lfanew);
    if(dos->e_magic!=IMAGE_DOS_SIGNATURE||nt->Signature!=IMAGE_NT_SIGNATURE||
       nt->FileHeader.Machine!=IMAGE_FILE_MACHINE_AMD64||nt->FileHeader.TimeDateStamp!=0x68af45ea||
       nt->OptionalHeader.SizeOfImage!=0x4efc000)return false;
    struct Hook{uintptr_t rva;const char*signature;void*detour;void**original;};
    const Hook hooks[]{
        {0x21bf9c,"\x48\x89\x5c\x24\x18\x55\x56\x57\x41\x54\x41\x55\x41\x56\x41\x57\x48\x83\xec\x20\x48\x8d\x71\x38",reinterpret_cast<void*>(&Apply),reinterpret_cast<void**>(&originalApply)},
        {0x87320c,"\x40\x55\x53\x56\x57\x41\x54\x41\x55\x41\x56\x41\x57\x48\x8b\xec\x48\x83\xec\x78\x80\xb9\x90\x00",reinterpret_cast<void*>(&ResizeHelper),reinterpret_cast<void**>(&originalResizeHelper)}
    };
    for(const auto&h:hooks)if(std::memcmp(reinterpret_cast<const void*>(base+h.rva),h.signature,24)){
        Log("[SETTINGS-APPLY-AUDIT] signature mismatch rva=%llX; no hooks enabled\n",static_cast<unsigned long long>(h.rva));return false;}
    size_t made=0;
    for(const auto&h:hooks){if(MH_CreateHook(reinterpret_cast<void*>(base+h.rva),h.detour,h.original)!=MH_OK)break;++made;}
    bool ok=made==std::size(hooks);
    if(ok)for(const auto&h:hooks)if(MH_QueueEnableHook(reinterpret_cast<void*>(base+h.rva))!=MH_OK){ok=false;break;}
    if(ok)ok=MH_ApplyQueued()==MH_OK;
    if(!ok){for(size_t i=0;i<made;++i){MH_DisableHook(reinterpret_cast<void*>(base+hooks[i].rva));MH_RemoveHook(reinterpret_cast<void*>(base+hooks[i].rva));}return false;}
    Log("[SETTINGS-APPLY-AUDIT] installed native apply21BF9C/helper87320C, lifecycle=%d\n",int(cvr::cet::LifecycleRequested()));return true;
}

CVR_HOOK_IF("NativeSettingsAudit",cvr::hooks::Stage::Boot,84,Install,Enabled);
}

bool cvr::hooks::AdmitNativePpdWrite(const cvr::ppd::NativeApplyStatus& state,void* settings) noexcept {
    if(!cvr::cet::LifecycleRequested())return true;
    if(!cvr::cet::ProducerHooksInstalled()||!current||current->settings!=settings||!current->transition)return false;
    auto& pass=*current->transition;
    if(pass.plan.revision!=state.revision||pass.plan.width!=state.requested.width||pass.plan.height!=state.requested.height)return false;
    if(!pass.Reserved())return state.observedActive==state.requested&&state.observedPending==state.requested;
    return pass.Arm();
}
cvr::hooks::AuthorSizeAdmission cvr::hooks::AdmitAuthorSizeWrite(uintptr_t settings,uint32_t width,uint32_t height) noexcept {
    if(!cvr::cet::LifecycleRequested()||cvr::cet::ProducerBootstrapPending())return AuthorSizeAdmission::Bootstrap;
    // Rejected/retired coverage is never bootstrap. Even a nominal no-op
    // author store must run under this native apply, not a cached-pointer
    // background writer racing a different apply's coherent snapshot.
    if(!cvr::cet::ProducerHooksInstalled()||!current||reinterpret_cast<uintptr_t>(current->settings)!=settings||!current->transition)return AuthorSizeAdmission::Denied;
    NativeSettingsSnapshot snapshot;
    if(!ReadSnapshot(reinterpret_cast<void*>(settings),&snapshot))return AuthorSizeAdmission::Denied;
    if(snapshot.U32(0x18)==width&&snapshot.U32(0x1c)==height&&snapshot.U32(0x84)==width&&snapshot.U32(0x88)==height)return AuthorSizeAdmission::PendingOnly;
    auto& pass=*current->transition;
    return pass.plan.width==width&&pass.plan.height==height&&pass.Reserved()&&pass.Arm() ? AuthorSizeAdmission::PendingOnly:AuthorSizeAdmission::Denied;
}
