#pragma once
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <type_traits>
namespace sl {struct Constants;struct FrameToken;struct ViewportHandle;struct BaseStructure;}
namespace cvr::framegen {
enum class SrTraceKind:uint32_t {Unknown,Constants,Tags,Evaluate};
enum class SrViewportStatus:uint32_t {Missing,Known,Malformed,Ambiguous,Limit,UnsupportedVersion,InvalidValue,Cycle};
enum SrTraceFlag:uint32_t {SrNodePoseKnown=1u<<0,SrNativeFrameKnown=1u<<1,SrSourceKnown=1u<<2,
    SrPhysicalEyeKnown=1u<<3,SrTokenKnown=1u<<4,SrViewportKnown=1u<<5,SrResultKnown=1u<<6,
    SrConstantsKnown=1u<<7,SrTagsTruncated=1u<<8,SrArgumentsMalformed=1u<<9,SrNodeKeyKnown=1u<<10};
enum SrTagFlag:uint32_t {SrTagKnown=1,SrResourceKnown=2,SrDescriptorKnown=4,SrTagMalformed=8};
struct SrTagWire {
    uint64_t native{},resourceStruct{};
    uint32_t type{},lifecycle{},left{},top{},width{},height{};
    uint64_t actualWidth{};
    uint32_t actualHeight{},format{},dimension{},sampleCount{},state{},resourceType{},flags{},depthOrArraySize{};
};
static_assert(sizeof(SrTagWire)==80);
static_assert(offsetof(SrTagWire,actualWidth)==40 && offsetof(SrTagWire,flags)==72);
struct SrConstantsWire {
    std::array<uint32_t,104> floatBits{};
    std::array<uint32_t,7> flags{};
    std::array<uint32_t,4> valuePresent{};
    uint32_t flagPresent{};
};
static_assert(sizeof(SrConstantsWire)==464);
static_assert(offsetof(SrConstantsWire,flags)==416 && offsetof(SrConstantsWire,valuePresent)==444 && offsetof(SrConstantsWire,flagPresent)==460);
struct SrTraceRecord {
    uint64_t qpcBegin{},qpcEnd{},commandList{},engineViewKey{};
    int64_t sourceId=-1;
    uint64_t poseId{},poseWriteId{},origin{},rendererEpoch{},gameWorldEpoch{},hmdWorldEpoch{},requestRevision{},latchRevision{},sceneOrdinal{};
    uint64_t sourceClockGeneration{},captureClockGeneration{};
    int64_t contentTimeNs{},posePredictionNs{};
    uint64_t ngxFeatureHandle{},nodeContext{}; // NGX handle UNKNOWN: no direct hook.
    uint32_t kind{},flags{},nativeFrame{},engineView{},physicalEye=UINT32_MAX,slFrameToken{},slViewport=UINT32_MAX,viewportStatus{};
    uint32_t feature=UINT32_MAX,apiResult=UINT32_MAX,inputCount{},capturedCount{},viewportsFound{},payloadKind{};
    uint64_t constantsVersion{},arguments{};
    uint32_t tagMalformedCount{},reserved0{},constantFlagCount{},constantFloatCount{};
    int64_t requestedPoseTimeNs{};
    std::array<uint32_t,320> payload{}; // ConstantsWire OR up to16 TagWire, copied as bytes.
};
static_assert(std::is_trivially_copyable_v<SrTraceRecord> && sizeof(SrTraceRecord)==1536);
static_assert(offsetof(SrTraceRecord,kind)==160 && offsetof(SrTraceRecord,payload)==256);
static_assert(offsetof(SrTraceRecord,slFrameToken)==180 && offsetof(SrTraceRecord,constantsVersion)==216 && offsetof(SrTraceRecord,requestedPoseTimeNs)==248);
struct SrTraceSlot {uint64_t sequence{};SrTraceRecord record;};
static_assert(sizeof(SrTraceSlot)==1544);
struct SrIdentityTraceStorage {
    static constexpr uint32_t Capacity=256,MaxTags=16,MaxInputs=16,MaxChain=8,MaxNodes=64;
    uint32_t version=1,bytes=sizeof(SrIdentityTraceStorage),recordBytes=sizeof(SrTraceRecord),slotBytes=sizeof(SrTraceSlot);
    uint32_t capacity=Capacity,slotsOffset{},headerBytes=104,reserved{};
    std::atomic<uint64_t> writeEpoch{},published{},attempted{},dropped{};
    std::atomic<uint32_t> writer{};uint32_t reserved2{};
    uint64_t qpcFrequency{};
    std::array<std::atomic<uint64_t>,3> calls{};
    std::array<SrTraceSlot,Capacity> slots{};
    SrIdentityTraceStorage() noexcept;
    bool Record(const SrTraceRecord&) noexcept; // Single try, no wait/heap/retention.
};
static_assert(offsetof(SrIdentityTraceStorage,writeEpoch)==32 && offsetof(SrIdentityTraceStorage,slots)==104);
static_assert(offsetof(SrIdentityTraceStorage,published)==40 && offsetof(SrIdentityTraceStorage,writer)==64 && offsetof(SrIdentityTraceStorage,calls)==80);
static_assert(sizeof(SrIdentityTraceStorage)==395368);
static_assert(std::atomic<uint64_t>::is_always_lock_free && std::atomic<uint32_t>::is_always_lock_free);
uint64_t SrTraceQpc() noexcept;
// Opt-in CPU observation only. Does not enable guide copies, SR ownership or FG.
bool SrIdentityTraceRequested() noexcept;
void SnapshotSrConstants(SrTraceRecord&,const sl::Constants*,const sl::FrameToken*,const sl::ViewportHandle*) noexcept;
void SnapshotSrTags(SrTraceRecord&,const void* viewport,const void* tags,uint32_t count,void* list) noexcept;
void SnapshotSrEvaluate(SrTraceRecord&,uint32_t feature,const sl::FrameToken*,const sl::BaseStructure* const*,uint32_t count,void* list) noexcept;
void RecordSrIdentity(const SrTraceRecord&) noexcept;
}
extern "C" {__declspec(dllexport) extern cvr::framegen::SrIdentityTraceStorage CyberpunkVR_SrIdentityTrace;}
