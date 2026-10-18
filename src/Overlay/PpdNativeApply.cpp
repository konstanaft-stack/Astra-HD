#include "Overlay/PpdNativeApply.hpp"
#include "Utils/MemorySafe.hpp"
#include <d3d12.h>
#include <limits>
#include <mutex>
#include <atomic>

extern "C" {
__declspec(dllexport) std::atomic<uint64_t> CyberpunkVR_PpdNativeStatusSeq{0};
__declspec(dllexport) cvr::ppd::NativeApplyTelemetryPacket CyberpunkVR_PpdNativeStatus{};
}

namespace cvr::ppd {
namespace {
std::mutex ownerMutex;
NativeApplyStatus state;
void PublishStatusLocked() noexcept {
    const NativeApplyTelemetryPacket next{1,72,state.revision,uint32_t(state.stage),uint32_t(state.error),
        state.requested.width,state.requested.height,state.observedActive.width,state.observedActive.height,
        state.observedPending.width,state.observedPending.height,state.hookVisits,state.dispatchVisit,state.observationVisit};
    const auto& previous=CyberpunkVR_PpdNativeStatus;
    if(next.revision==previous.revision && next.stage==previous.stage && next.error==previous.error &&
       next.requestedWidth==previous.requestedWidth && next.requestedHeight==previous.requestedHeight &&
       next.activeWidth==previous.activeWidth && next.activeHeight==previous.activeHeight &&
       next.pendingWidth==previous.pendingWidth && next.pendingHeight==previous.pendingHeight)return;
    CyberpunkVR_PpdNativeStatusSeq.fetch_add(1,std::memory_order_acq_rel);
    CyberpunkVR_PpdNativeStatus=next;
    CyberpunkVR_PpdNativeStatusSeq.fetch_add(1,std::memory_order_release);
}
bool WriteNativeU32(uintptr_t address,uint32_t value) noexcept {
    __try {*reinterpret_cast<uint32_t*>(address)=value;return true;}
    __except(EXCEPTION_EXECUTE_HANDLER){return false;}
}
bool ValidSize(NativeSize size) noexcept {
    return size.width && size.height &&
        size.width<=D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION &&
        size.height<=D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION;
}
bool ValidPointerRange(uintptr_t settings) noexcept {
    return settings>=0x10000 && settings<=(std::numeric_limits<uintptr_t>::max)()-0x8b;
}
void Fail(NativeApplyError error) noexcept {
    state.stage=NativeApplyStage::Error;state.error=error;state.observationVisit=0;
    PublishStatusLocked();
}
}
uint64_t RequestNativeSize(uint32_t width,uint32_t height) {
    const NativeSize wanted{width,height};if(!ValidSize(wanted))return 0;
    std::lock_guard lock(ownerMutex);
    if(state.revision==(std::numeric_limits<uint64_t>::max)())return 0;
    const auto revision=state.revision+1;state={};state.revision=revision;
    state.requestedAtMs=GetTickCount64();
    state.requested=wanted;state.stage=NativeApplyStage::Requested;PublishStatusLocked();return revision;
}
bool CancelNativeSize(uint64_t revision) {
    std::lock_guard lock(ownerMutex);
    if(!revision || revision!=state.revision || !state.OwnsDimensions())return false;
    state={};state.revision=revision;PublishStatusLocked();return true;
}
NativeApplyStatus ReadNativeApplyStatus() {
    std::lock_guard lock(ownerMutex);return state;
}
std::optional<NativeSizeRequest> ReadNativeRequestedSize() {
    std::lock_guard lock(ownerMutex);
    if(!state.OwnsDimensions())return {};
    return NativeSizeRequest{state.revision,state.requested,state.stage};
}
bool ProcessNativeSizeAtSettingsHook(uintptr_t settings,bool applyAllowed,NativeWriteAdmission admission,void* context) {
    std::lock_guard lock(ownerMutex);
    if(!state.OwnsDimensions())return false;
    if(state.stage==NativeApplyStage::Error)return true;
    if(state.hookVisits==(std::numeric_limits<uint64_t>::max)()) {
        Fail(NativeApplyError::CounterExhausted);return true;
    }
    ++state.hookVisits;
    if(!applyAllowed)return true;
    NativeSize active{},pending{};
    if(!ValidPointerRange(settings) || !ReadU32Safe(settings+0x18,&active.width) ||
       !ReadU32Safe(settings+0x1c,&active.height) || !ReadU32Safe(settings+0x84,&pending.width) ||
       !ReadU32Safe(settings+0x88,&pending.height)) {
        Fail(NativeApplyError::ReadFailed);return true;
    }
    state.observedActive=active;state.observedPending=pending;
    if(!ValidSize(active) || !ValidSize(pending)) {
        Fail(NativeApplyError::InvalidNativeSize);return true;
    }
    if(state.stage==NativeApplyStage::Requested || pending!=state.requested) {
        // Admission is a final nonblocking check of the SAME coherent request.
        // The GPU/CPU wait must already have happened outside ownerMutex.
        // Refusal leaves both native fields and the request stage unchanged.
        if(admission && !admission(state,context)) {
            PublishStatusLocked();
            return true;
        }
        // Only the live engine callback owns this pointer. Never copy requested
        // dimensions into active+18/+1C: the native changed predicate needs them.
        state.observationVisit=0;
        if(!WriteNativeU32(settings+0x84,state.requested.width) ||
           !WriteNativeU32(settings+0x88,state.requested.height)) {
            Fail(NativeApplyError::PendingWriteFailed);return true;
        }
        state.stage=NativeApplyStage::Dispatched;state.dispatchVisit=state.hookVisits;
        PublishStatusLocked();
        return true;
    }
    // This read is necessarily a later callback than the successful stores.
    // The engine copies active before DXGI success; this is not Applied PPD.
    state.stage=active==state.requested ? NativeApplyStage::ObservedNative : NativeApplyStage::Dispatched;
    state.observationVisit=active==state.requested ? state.hookVisits : 0;
    PublishStatusLocked();
    return true;
}
bool WriteAuthorNativeSize(uintptr_t settings,uint32_t width,uint32_t height,bool pendingOnly) {
    std::lock_guard lock(ownerMutex);
    if(state.OwnsDimensions() || !ValidPointerRange(settings) || !width || !height)return false;
    // Preserve the original startup/author behavior only outside live ownership.
    // Live CET admission must leave active dimensions to the original helper.
    // Otherwise an author height-only update erases the native predicate and
    // the armed reservation can never observe a real resize completion.
    const bool activeW=pendingOnly||WriteNativeU32(settings+0x18,width);
    const bool activeH=pendingOnly||WriteNativeU32(settings+0x1c,height);
    const bool pendingW=WriteNativeU32(settings+0x84,width);
    const bool pendingH=WriteNativeU32(settings+0x88,height);
    return activeW && activeH && pendingW && pendingH;
}
}
