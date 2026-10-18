#pragma once
#include "Framegen/SrHistoryOwner.hpp"
#include <atomic>
#include <vector>
namespace cvr::framegen {
struct Frame;
struct NativeFgReceipt {
    // Observed around the actual NGX evaluation, never reconstructed at selection.
    unsigned policyRevision{};
    SrSourceStamp previous,current;
    uint64_t moduleGeneration{},lifecycleGeneration{},createOrdinal{},historyEpoch{},resourceEpoch{};
    uint32_t index{},count{};
    bool reset{};
    // Paired cameras use real renderer IDs, never forged alternating receipts.
    bool paired{};
    uint32_t pairedEye{};
    uint64_t previousPairSerial{},currentPairSerial{},gameWorldEpoch{};
    std::shared_ptr<const Frame> pairedPreviousMetadata,pairedCurrentMetadata;
};
inline bool ValidNativeFgReceipt(const NativeFgReceipt&r){
    const auto& a=r.previous;const auto& b=r.current;
    if(r.paired)return !r.reset&&r.moduleGeneration&&r.lifecycleGeneration&&r.createOrdinal&&r.historyEpoch&&r.resourceEpoch&&
        r.count>=1&&r.count<=3&&r.index>=1&&r.index<=r.count&&r.pairedEye<2&&r.gameWorldEpoch&&
        r.previousPairSerial&&r.currentPairSerial>r.previousPairSerial&&
        a.poseKnown&&b.poseKnown&&a.poseId&&b.poseId&&a.writeId&&b.writeId&&a.nativeFrameKnown&&b.nativeFrameKnown&&
        !a.source&&!b.source&&a.engineView>=1&&a.engineView<=2&&a.engineView==b.engineView&&
        uint32_t(b.nativeFrame-a.nativeFrame)==1&&a.origin&&a.origin==b.origin&&
        a.captureClockGeneration&&a.captureClockGeneration==b.captureClockGeneration;
    return !r.reset&&r.moduleGeneration&&r.lifecycleGeneration&&r.createOrdinal&&r.historyEpoch&&r.resourceEpoch&&
        r.count>=1&&r.count<=3&&r.index>=1&&r.index<=r.count&&a.poseKnown&&b.poseKnown&&a.poseId&&b.poseId&&a.writeId&&b.writeId&&
        a.nativeFrameKnown&&b.nativeFrameKnown&&a.engineView==1&&b.engineView==1&&a.source&&b.source&&
        a.source.physicalEye==b.source.physicalEye&&b.source.sourceId>a.source.sourceId&&b.source.sourceId-a.source.sourceId==2&&
        a.source.epochs==b.source.epochs&&a.source.clockGeneration==b.source.clockGeneration&&a.source.requestRevision==b.source.requestRevision&&
        a.source.latchRevision==b.source.latchRevision&&a.origin==b.origin&&a.captureClockGeneration==b.captureClockGeneration;
}
struct NativeFgPacket {
    NativeFgReceipt receipt;
    Microsoft::WRL::ComPtr<ID3D12Resource> color,real;
    Microsoft::WRL::ComPtr<ID3D12Fence> fence;
    uint64_t value{},call{};
    bool interpolationAllowed{};
    std::shared_ptr<void> lifetime;
    std::shared_ptr<std::atomic<bool>> valid;
};
struct NativeFgOutputStatus {
    uint64_t recorded{},ready{},suppressed{},poolBusy{},unknown{},cancelled{};
    // Diagnostic counters under the existing output mutex; index zero is vendor phase 1.
    uint64_t phaseRecorded[2][3]{},phaseReady[2][3]{},phaseCancelled[2][3]{};
    uint64_t realCopies{},realReused{};
    uint64_t realDependencyDrops{};
};
void RecordNativeFgOutput(ID3D12GraphicsCommandList*,ID3D12Resource* real,D3D12_RESOURCE_STATES realState,ID3D12Resource* color,D3D12_RESOURCE_STATES,
    ID3D12Resource* disable,D3D12_RESOURCE_STATES,const NativeFgReceipt&,uint64_t call,uint64_t recording) noexcept;
void NativeFgOutputReset(ID3D12GraphicsCommandList*) noexcept;
void NativeFgOutputBeforeSubmit(ID3D12CommandQueue*,UINT,ID3D12CommandList* const*) noexcept;
void NativeFgOutputSubmitted(ID3D12CommandQueue*) noexcept;
std::vector<std::shared_ptr<const NativeFgPacket>> CollectNativeFgOutputs();
NativeFgOutputStatus ReadNativeFgOutputStatus();
// Resize only: synchronize already submitted work without holding the recorder lock.
void ObserveNativeFgResizeQueue(ID3D12CommandQueue*) noexcept;
bool DrainNativeFgResizeQueues(uint32_t timeoutMs) noexcept;
}
