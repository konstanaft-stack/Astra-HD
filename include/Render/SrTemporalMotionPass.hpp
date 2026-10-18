#pragma once
#include <d3d12.h>
#include <wrl/client.h>
#include <array>
#include <cstdint>
#include <memory>
#include <optional>

namespace cvr::render {
// Exact serialized b0 words. The history owner builds these; this pass does not
// invent matrix algebra, history predecessors, physical eyes or source IDs.
struct SrTemporalMotionConstants {std::array<uint32_t,120> words{};};
static_assert(sizeof(SrTemporalMotionConstants)==480);
struct SrTemporalBinding {
    Microsoft::WRL::ComPtr<ID3D12Resource> resource;
    DXGI_FORMAT viewFormat=DXGI_FORMAT_UNKNOWN;
    uint32_t width{},height{}; // whole mip0 extent, checked against actual GetDesc
    D3D12_RESOURCE_STATES beforeState=D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    // COM pins the object, not a pool cell. Pass a lease for every pooled image.
    std::shared_ptr<void> owner;
};
struct SrTemporalCompletion {ID3D12Fence* fence{};uint64_t value{};};
struct SrTemporalMotionRequest {
    SrTemporalMotionConstants constants;
    // t0/t1 current/previous MV; t2..4 current/previous/previous-previous depth;
    // t5..7 matching color. All MUST be immutable, same-domain snapshots chosen
    // by the caller. Input aliasing is allowed; input/output and output/output
    // aliases are rejected, including write/read or write/write overlap with
    // any live ticket. Declared state must already permit compute reads.
    std::array<SrTemporalBinding,8> inputs;
    std::optional<SrTemporalBinding> unusedT8;
    // u0 combinedMV, u1 specMV, u2 visual, u3 output. No clears/copies are added;
    // unwritten pixels keep caller-initialized contents. All end in NPS state.
    std::array<SrTemporalBinding,4> outputs;
    SrTemporalCompletion completion;
    // Original optional color/visual UAVs may be smaller than high-resolution
    // motion. Explicit opt-in preserves its out-of-bounds UAV-store discard.
    bool allowClippedColorStores=false;
};
enum class SrTemporalPrepareStatus {Ready,InvalidDevice,Busy,DeviceRemoved,CompileFailed,CreateFailed};
enum class SrTemporalRecordStatus {Recorded,NotPrepared,InvalidRequest,InvalidConstants,InvalidDescriptor,
    InvalidState,WrongDevice,Aliasing,InvalidCompletion,NoCapacity,DeviceRemoved};
struct SrTemporalTicket {
    uint32_t slot=UINT32_MAX;uint64_t serial{};
    SrTemporalRecordStatus status=SrTemporalRecordStatus::InvalidRequest;
    explicit operator bool() const {return serial!=0 && status==SrTemporalRecordStatus::Recorded;}
};
struct SrTemporalPipelineStats {uint32_t shaderCompileCalls{},pipelineCreateCalls{};};
class SrTemporalMotionPass {
public:
    static constexpr uint32_t SlotCount=4;
    SrTemporalMotionPass();~SrTemporalMotionPass();
    SrTemporalMotionPass(const SrTemporalMotionPass&)=delete;
    SrTemporalMotionPass& operator=(const SrTemporalMotionPass&)=delete;
    // Owner serializes ALL methods, including destruction. No internal waits.
    // Prepare is outside the render hook: cs_5_0 main, STRICT|LEVEL3, one PSO.
    // Repeating on the same ready device reuses it. Device changes refuse busy
    // slots until exact completion or explicit cancellation of never-executed work.
    SrTemporalPrepareStatus Prepare(ID3D12Device*);
    SrTemporalPipelineStats PipelineStats() const;
    // DIRECT/COMPUTE lists only. No compile, CreatePSO, resource allocation or
    // waits. GetDesc/device/typed format/extent and motionMapSize are checked
    // before any command-list mutation. 8x8 dispatch covers motionMapSize.
    // Inputs accept NPS, NPS|PS or GENERIC_READ and keep that exact state.
    // Outputs accept COMMON/COPY_DEST/COPY_SOURCE/UAV/NPS/PS/read combinations;
    // this pass transitions them to UAV, dispatches, then returns all to NPS.
    //
    // BINDING SIDE EFFECTS: sets descriptor heap, compute root signature/root
    // arguments and PSO. This is for the native NGX Evaluate boundary where
    // host/SL rebinds state. It does not preserve arbitrary host CL bindings.
    //
    // Reserve a fresh, not-already-queued fence value, increasing on its fence.
    // Two live tickets may NOT share a fence, including submitted/unknown ones.
    // Native lists can execute out of record order: give each live slot its own
    // fence. Values on different fences are unrelated. Caller owns each signal
    // timeline; it cannot be advanced by unrelated work. Signal that exact fence
    // and value on the executing queue AFTER this list and all dependent work.
    SrTemporalTicket Record(ID3D12GraphicsCommandList*,const SrTemporalMotionRequest&);
    bool SubmissionReported(SrTemporalTicket,bool executed,bool signalSucceeded);
    // Only if the recorded list was NEVER passed to ExecuteCommandLists.
    bool CancelNeverExecuted(SrTemporalTicket);
    uint32_t Collect();uint32_t BusySlots() const;bool DeviceRemoved() const;
    bool TryShutdown(); // nonblocking; unknown/recorded/inflight refuses shutdown
private:
    struct Impl;std::unique_ptr<Impl> m_impl;uint64_t m_serial{};
};
}
