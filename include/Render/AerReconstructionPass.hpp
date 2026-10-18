#pragma once
#include <d3d12.h>
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
namespace cvr::render {
    // The caller supplies compatible source/reference poses from the same epoch and
    // coordinate space. This is source-orientation alignment, never a latest-HMD warp.
    struct AerWarpParams {
        std::array<float,4> reference{
            0,0,0,1
        }, captured{
            0,0,0,1
        };
        std::array<float,4> tangents{
            1,1,1,1
        };
        // up,down,left,right
        std::array<float,4> crop{
        };
        // left,top,right,bottom in source texels
        uint64_t referenceEpoch=0, sourceEpoch=0, referenceSpace=0, sourceSpace=0;
    };
    struct alignas(16) AerVertexConstants {
        std::array<float,4> sourceViewport{
        },color0{
        };
        std::array<float,16> matrix0{
        };
        std::array<float,4> color1{
        };
        std::array<float,16> matrix1{
        };
    };
    static_assert(sizeof(AerVertexConstants)==176);
    static_assert(offsetof(AerVertexConstants,matrix0)==32);
    static_assert(offsetof(AerVertexConstants,matrix1)==112);
    struct AerFilterParams {
        float emphasis=0, deblur=.5f, contrast=0, vibrance=0;
    };
    // Linear means normalized SDR-linear samples. This pass does not tone-map HDR.
    enum class AerEncoding {
        Linear, EncodedSrgb
    };
    struct AerColorRoute {
        AerEncoding input=AerEncoding::EncodedSrgb, output=AerEncoding::EncodedSrgb;
    };
    struct AerCompletionPoint {
        ID3D12Fence* fence=nullptr;
        uint64_t value=0;
    };
    enum class AerRecordStatus { InvalidRequest, NotPrepared, Recorded };
    struct AerPipelineStats {
        uint32_t shaderCompileCalls=0, pipelineCreateCalls=0, cachedPipelines=0;
    };
    struct AerRecordTicket {
        uint32_t slot=UINT32_MAX;
        uint64_t serial=0;
        AerRecordStatus status=AerRecordStatus::InvalidRequest;
        explicit operator bool()const{
            return serial!=0;
        }
    };
    struct AerRecordRequest {
        ID3D12Resource* source=nullptr;
        ID3D12Resource* destination=nullptr;
        AerWarpParams warp{
        };
        AerFilterParams filter{
        };
        AerColorRoute color{
        };
        AerCompletionPoint completion{
        };
        // An AddRef pins a resource object, not a capture-pool cell. Pooled sources
        // MUST also pass their Frame/ReadLease/NativeSelectionLease owner here.
        std::shared_ptr<void> sourceOwner;
        D3D12_RESOURCE_STATES sourceState=D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        uint32_t eye=0;
        // Optional one-shot simulator evidence, inert without its process-local environment setting.
        int64_t evidenceSourceId=-1,evidenceSourceFloor=-1;
        uint64_t evidencePairSerial{};
        // Diagnostic-only original source identity: renderer/game/HMD epochs,
        // request/latch/clock generations, native frame, pose ID and write ID.
        std::array<uint64_t,9> evidenceIdentity{};
    };
    class AerReconstructionPass {
        public:
        static constexpr uint32_t SlotCount=6;
        AerReconstructionPass();
        ~AerReconstructionPass();
        AerReconstructionPass(const AerReconstructionPass&)=delete;
        AerReconstructionPass& operator=(const AerReconstructionPass&)=delete;
        // Initialization/shutdown/destruction are serialized by the owning presenter.
        bool Initialize(ID3D12Device* device);
        // Call before activation on device/format/encoding transitions. Prepares
        // BOTH CAS and zero-filter bypass, so slider changes need no compilation.
        // Repeat calls with an already prepared route do not compile/create PSOs.
        bool Prepare(DXGI_FORMAT sourceFormat, DXGI_FORMAT destinationFormat,
                     const AerColorRoute&);
        AerPipelineStats PipelineStats()const;
        static bool BuildVertexConstants(const AerWarpParams&,uint32_t width,uint32_t height,AerVertexConstants&);
        // Records only. Source must already permit PS reads; destination must be in
        // RENDER_TARGET. DIRECT lists only. It preserves both states and records no
        // queue waits. Optional diagnostic copies preserve these same states.
        // The caller reserves a fresh, not-previously-queued fence
        // value and signals it on this list's queue AFTER ExecuteCommandLists.
        // A missing pipeline returns NotPrepared with no command-list or slot
        // mutation. Record never compiles shaders or creates pipeline states.
        AerRecordTicket Record(ID3D12GraphicsCommandList*,const AerRecordRequest&);
        // Report Execute and the actual completion Signal separately from recording.
        // executed=true/signalSucceeded=false is UNKNOWN and never auto-reclaimed.
        // A successful retry of that exact signal may later confirm UNKNOWN.
        bool SubmissionReported(AerRecordTicket,bool executed,bool signalSucceeded);
        // Only legal when this command list was NEVER passed to ExecuteCommandLists.
        bool CancelNeverExecuted(AerRecordTicket);
        uint32_t Collect();
        uint32_t BusySlots()const;
        bool DeviceRemoved()const;
        bool TryShutdown();
        // Nonblocking; refuses any outstanding or unknown work.
        private:
        struct Impl;
        std::unique_ptr<Impl> m_impl;
        uint64_t m_serial=0;
    };
}
// namespace cvr::render
