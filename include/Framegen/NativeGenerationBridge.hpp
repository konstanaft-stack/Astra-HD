#pragma once
#include "Framegen/Presenter.hpp"
#include <cpvr/generation.hpp>
#include <array>
#include <string>

namespace cvr::framegen {
enum class NativeBridgeCode {
    Ready,MissingIdentity,MissingCameraTime,MissingPoseTime,IncompleteCamera,
    WrongQueue,MissingProducerFence,UnsupportedResource,BackendPending,BackendFailed,
    NoSelection,ConsumerCapacity,ConsumerCompletionUnknown,HistoryResetPending,DeviceChanged,InputRetry
};
struct NativeInputResult {
    std::array<cpvr::GenerationInput,2> inputs;
    NativeBridgeCode code=NativeBridgeCode::MissingIdentity;
    explicit operator bool() const {return code==NativeBridgeCode::Ready;}
};
// Consumes the actual stamped author frame. It neither creates textures nor
// fabricates missing timestamps, matrices, native tokens or eye identities.
NativeInputResult AdaptNativeInputs(ID3D12Device*,ID3D12CommandQueue*,const Frame&,
    const std::array<cpvr::GenerationInput,2>& previous={},uint64_t generationEpoch=0);

enum class NativeAlternatingHistory {Warmup,AdjacentNative,SameEyeDelta2};
struct NativeAlternatingInputResult {
    cpvr::GenerationInput input;
    // Immutable original AER receipt and separate content/prediction times.
    // Worker source_id=AER sourceId+1 because worker zero means unknown.
    NativeEyeStamp sourceStamp;
    uint64_t captureResourceEpoch{};
    D3D12_RESOURCE_DESC colorDescriptor{}; // Actual descriptor sampled at admission.
    Microsoft::WRL::ComPtr<ID3D12CommandQueue> producerQueue;
    bool guideHistoryOnly{};
    NativeAlternatingHistory history=NativeAlternatingHistory::Warmup;
    NativeBridgeCode code=NativeBridgeCode::MissingIdentity;
    explicit operator bool() const {return code==NativeBridgeCode::Ready;}
};
// One real MAIN source in Frame[physicalEye], engineView=1. Does not need or
// fabricate a paired eye. Previous results are bounded owner-held receipts.
NativeAlternatingInputResult AdaptAlternatingMainInput(ID3D12Device*,ID3D12CommandQueue*,const Frame&,
    cpvr::Eye physicalEye,const NativeAlternatingInputResult& previousSameEye={},
    const NativeAlternatingInputResult& immediatePrevious={},uint64_t generationEpoch=0);
// Keeps measured guide/resource provenance but no color ReadLease or recursive
// middle chain. Only successful worker admission should replace owner history.
NativeAlternatingInputResult AlternatingMainGuideHistory(const NativeAlternatingInputResult&);

struct NativeSelectionLease {
    unsigned observedPolicyRevision{};
    bool observedPairedMode{};
    cpvr::GenerationSelection selected;
    Microsoft::WRL::ComPtr<ID3D12Device> device;
    Microsoft::WRL::ComPtr<ID3D12Fence> consumerFence;
    uint64_t consumerValue{};
    // copySubmitted is the legacy name for submitted consumer GPU work;
    // it must also be true for a reconstruction draw that reads the image.
    bool commandsRecorded{},submissionReported{},copySubmitted{},signalSucceeded{};
};
struct NativeBridgeStatus {NativeBridgeCode code=NativeBridgeCode::NoSelection;unsigned retainedConsumers{};};
enum class NativeAlternatingIngestStatus {Submitted,Duplicate,Retry,Rejected};
struct NativeAlternatingIngestResult {
    NativeAlternatingIngestStatus status=NativeAlternatingIngestStatus::Rejected;
    NativeBridgeCode code=NativeBridgeCode::MissingIdentity;
    NativeAlternatingHistory history=NativeAlternatingHistory::Warmup;
    explicit operator bool()const{return status==NativeAlternatingIngestStatus::Submitted;}
};
// Walk the caller's borrowed, already sorted/validated sources. No input,
// callback or pending-color ownership survives this synchronous call.
template<class Range,class Ingest>
void IngestAlternatingBatch(const Range& sources,Ingest&& ingest) {
    for(const auto& source:sources) {
        // A later success would advance the bridge watermark and permanently
        // reject this temporary failure. Let a future batch retry it first.
        if(ingest(source).status==NativeAlternatingIngestStatus::Retry)break;
    }
}
// Values from the CURRENT validated AER baseline, not from a newly attempted
// input. Selection does not require its full guides or retain these values.
// maximumSourceId remains in the original signed AER namespace (worker +1).
struct NativeAlternatingOutputContext {
    MainEyeSourceReceipt scope;
    uint64_t resourceEpoch{},captureClockGeneration{},historyEpoch{};
    struct Extent {uint32_t width{},height{};};
    std::array<Extent,2> expectedExtent{};
    std::array<int64_t,2> maximumSourceId{-1,-1};
};
class NativeGenerationBridge {
public:
    // The mode owner supplies/configures/stops this EXISTING worker. The
    // default constructor expects it to be started separately; the configured
    // overload can defer one start. Neither creates another worker/XR loop.
    explicit NativeGenerationBridge(cpvr::GenerationWorker&);
    // Optional deferred start with the author's actual device, attempted once
    // after coherent inputs arrive. Owner retains/stops/joins the worker and
    // detaches this bridge before destroying that worker or XR resources.
    NativeGenerationBridge(cpvr::GenerationWorker&,cpvr::GenerationConfig,cpvr::Log={});
    ~NativeGenerationBridge();
    NativeGenerationBridge(const NativeGenerationBridge&)=delete;
    NativeGenerationBridge& operator=(const NativeGenerationBridge&)=delete;
    bool Select(ID3D12Device*,ID3D12CommandQueue*,const Frame&,PresentationTime,Selection&);
    // No presentation time/output-slot dependency. Caller submits the current
    // validated ready batch in ascending source order. Only successful submit
    // advances ordering/history; Retry owns no new input or pending-color pin.
    NativeAlternatingIngestResult IngestAlternating(ID3D12Device*,ID3D12CommandQueue*,const Frame&,cpvr::Eye);
    // One product selection after ingestion. Scope/extent/upper-ID checks are
    // independent of the last attempted input; original consumer leases and
    // Submitted/fence/unknown-completion behavior remain unchanged.
    bool SelectAlternatingReady(ID3D12Device*,const NativeAlternatingOutputContext&,PresentationTime,Selection&);
    // Compatibility convenience. New all-ready callers use the split methods.
    bool SelectAlternating(ID3D12Device*,ID3D12CommandQueue*,const Frame&,cpvr::Eye physicalEye,PresentationTime,Selection&);
    void Submitted(const Selection&,ID3D12Fence*,uint64_t,bool copySubmitted,bool signalSucceeded,
        bool accepted,int64_t displayTime);
    void Reset(); // Metadata only; in-flight consumers remain retained.
    void Collect(); // Nonblocking fence checks, never a CPU/GPU wait.
    void Maintain(PresentationTime); // Service expiry without a new source or XR acknowledgement.
    NativeBridgeStatus Status() const;
    void RecordOwnWitness(OwnSelectionWitness&)noexcept;
    void ObserveOwnFallback(const Selection&,bool accepted,bool copySubmitted,bool signalSucceeded,int64_t displayTime)noexcept;
    uint64_t HistoryEpoch() const; // Bridge-owned history generation, not a native frame ID.
private:
    struct Impl;std::unique_ptr<Impl> impl;
};
// Brackets the author's existing color read. Generated worker textures rest
// in COMMON; author captures keep CaptureColorReadState across all consumers.
class NativeReadScope {
public:
    // pixelRead adds the shared SRV read state for generated images and
    // restores their original COMMON/COPY_SOURCE state on scope exit.
    NativeReadScope(ID3D12GraphicsCommandList*,const Selection&,unsigned eye,bool pixelRead=false);
    ~NativeReadScope();
    explicit operator bool() const {return valid;}
    D3D12_RESOURCE_STATES ReadState() const {return reading;}
private:
    ID3D12GraphicsCommandList* list{};ID3D12Resource* resource{};
    D3D12_RESOURCE_STATES resting=D3D12_RESOURCE_STATE_COPY_SOURCE;
    D3D12_RESOURCE_STATES reading=D3D12_RESOURCE_STATE_COPY_SOURCE;
    bool changed{},valid=true;
};
}
