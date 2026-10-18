#pragma once
#include <atomic>
#include <cstddef>
#include <cstdint>
namespace cvr::framegen {
enum class SrHookCounter:uint32_t {Ensure,SlInstall,SlInstallFailed,NativeInstall,NativeInstallFailed,
    Create,Evaluate,Release,UnknownEvaluate,UnknownRelease,Feature11Passthrough,RegistryFull,
    Shutdown,Retired,NotReady,NotRegistered,Count};
struct SrNativeHookStatus {
    uint32_t version=1,bytes=256;
    uint64_t counters[16]{};
    uint64_t coreModule{},moduleGeneration{},firstCreateOrdinal{},lastHandle{},lastHandleId{},lastCommandList{};
    uint32_t slReady{},nativeReady{},modulePinned{},hostNgxInitProven{};
    uint32_t lastStage{},lastFeature=UINT32_MAX,lastResult{},resultKnown{};
    uint32_t handleIdKnown{},retirement{},slInitResult{},slDeviceResult{};
    uint32_t slInitObserved{},slDeviceObserved{},slHookMask{},nativeHookMask{};
    uint64_t droppedLastRecords{};
};
static_assert(sizeof(SrNativeHookStatus)==256 && offsetof(SrNativeHookStatus,coreModule)==136);
static_assert(offsetof(SrNativeHookStatus,slReady)==184 && offsetof(SrNativeHookStatus,droppedLastRecords)==248);
enum class SrLifecycleEvent:uint32_t {
    InitBegin=1,InitEnd,DeviceBegin,DeviceEnd,AllocateBegin,AllocateEnd,
    ShutdownBegin,ShutdownRefused,RetirementBegin,RetirementEnd,
    ShutdownOriginalBegin,ShutdownOriginalEnd,RearmBegin,RearmEnd,
    CreateBegin,CreateEnd,ReleaseBegin,ReleaseEnd
};
enum class SrRearmResult:uint32_t {Installed=1,NoFactory,FactoryFailed,SameOwner,ChangedLifecycle};
enum SrLifecycleFlag:uint32_t {SrLifecycleResultKnown=1,SrLifecycleHandler=2,
    SrLifecycleQuiescing=4,SrLifecycleSlInitialized=8,SrLifecycleRearmPending=16,
    SrLifecycleFactory=32,SrLifecycleRearming=64};
struct SrLifecycleRecord {
    uint64_t serial{},qpc{},lifecycleGeneration{},moduleGeneration{},createOrdinal{};
    uint32_t event{},result{},flags{},feature=UINT32_MAX,retirement{},reserved{};
};
struct SrNativeLifecycleStatus {
    static constexpr uint32_t Capacity=64;
    uint32_t version=1,bytes=4128,capacity=Capacity,recordBytes=64;
    uint64_t publishedEvents{},droppedEvents{};
    SrLifecycleRecord events[Capacity]{};
};
static_assert(sizeof(SrLifecycleRecord)==64&&offsetof(SrLifecycleRecord,event)==40);
static_assert(sizeof(SrNativeLifecycleStatus)==4128&&offsetof(SrNativeLifecycleStatus,events)==32);
}
extern "C" {
__declspec(dllexport) extern std::atomic<uint64_t> CyberpunkVR_SrNativeHookStatusSeq;
__declspec(dllexport) extern cvr::framegen::SrNativeHookStatus CyberpunkVR_SrNativeHookStatus;
__declspec(dllexport) extern std::atomic<uint64_t> CyberpunkVR_SrNativeLifecycleStatusSeq;
__declspec(dllexport) extern cvr::framegen::SrNativeLifecycleStatus CyberpunkVR_SrNativeLifecycleStatus;
}
