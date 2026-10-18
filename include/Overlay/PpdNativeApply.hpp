#pragma once
#include <cstdint>
#include <optional>

namespace cvr::ppd {
struct NativeSize {
    uint32_t width{},height{};
    friend bool operator==(const NativeSize&,const NativeSize&)=default;
};
enum class NativeApplyStage {Idle,Requested,Dispatched,ObservedNative,Error};
enum class NativeApplyError {None,ReadFailed,InvalidNativeSize,PendingWriteFailed,CounterExhausted};
struct NativeApplyStatus {
    uint64_t revision{};
    NativeApplyStage stage=NativeApplyStage::Idle;
    NativeApplyError error=NativeApplyError::None;
    NativeSize requested{},observedActive{},observedPending{};
    // Hook visits are diagnostic ordinals, never game/source frame IDs.
    uint64_t hookVisits{},dispatchVisit{},observationVisit{};
    uint64_t requestedAtMs{};
    bool OwnsDimensions() const noexcept{return stage!=NativeApplyStage::Idle;}
};
struct NativeSizeRequest {
    uint64_t revision{};
    NativeSize size{};
    NativeApplyStage stage=NativeApplyStage::Idle;
};
// Luke42DA AF5332-AF53D5: one second after the request, cap only the
// physical client height to1024 and truncate its aspect-derived width.
inline std::optional<NativeSize> ReferencePpdClientSize(const NativeApplyStatus& s,uint64_t now){
    if(!s.revision||s.stage!=NativeApplyStage::ObservedNative||s.requested!=s.observedActive||
       now<s.requestedAtMs||now-s.requestedAtMs<1000||s.requested.height<=1024||
       !s.requested.width||s.requested.width>16384||s.requested.height>16384)return {};
    const auto width=uint32_t(float(s.requested.width)/float(s.requested.height)*1024.f)&0xffffu;
    return width?std::optional<NativeSize>{NativeSize{width,1024}}:std::nullopt;
}
// Read-only diagnostic export layout. Read sequence/packet/sequence and accept
// only equal even sequence values. Visits here are owner-helper visits at the
// last state change; the separate SettingsHookVisits export counts real hooks.
struct NativeApplyTelemetryPacket {
    uint32_t version=1,bytes=72;
    uint64_t revision{};
    uint32_t stage{},error{};
    uint32_t requestedWidth{},requestedHeight{},activeWidth{},activeHeight{},pendingWidth{},pendingHeight{};
    uint64_t processVisits{},dispatchVisit{},observationVisit{};
};
static_assert(sizeof(NativeApplyTelemetryPacket)==72);

// UI/owner API: no engine pointer is accepted or stored. Invalid dimensions or
// exhausted revision domain return zero without modifying the existing request.
uint64_t RequestNativeSize(uint32_t width,uint32_t height);
bool CancelNativeSize(uint64_t revision);
NativeApplyStatus ReadNativeApplyStatus();
std::optional<NativeSizeRequest> ReadNativeRequestedSize();

// Actual SettingsRes hook only. True means this owner excludes the author-mode
// override, even while map dispatch is deferred or an error awaits owner action.
// A later ObservedNative is only an engine-field read, not DXGI/GPU/XR acceptance.
// Called under the short owner lock immediately before either pending store.
// It must not wait, call the owner API, or retain the status reference. Prepare
// expensive CPU/GPU quiescence outside this function; this callback only checks
// the exact request and arms the prepared transaction before the first store.
using NativeWriteAdmission = bool (*)(const NativeApplyStatus&, void*) noexcept;
bool ProcessNativeSizeAtSettingsHook(uintptr_t settings,bool applyAllowed,
                                    NativeWriteAdmission admission=nullptr,void* context=nullptr);

// Existing startup/background author path. The same short lock excludes these
// four legacy stores while a live request owns dimensions. No pointer is cached.
bool WriteAuthorNativeSize(uintptr_t settings,uint32_t width,uint32_t height,bool pendingOnly=false);
}
