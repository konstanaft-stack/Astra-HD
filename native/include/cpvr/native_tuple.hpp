#pragma once
#include "gpu.hpp"
#include <filesystem>

namespace cpvr {
// Frozen provenance for the pre-temporal/sign-normalization native guides.
// Camera arrays are copied from original SL constants, without pack_constants fixes.
struct NativeTupleTag {
    Extent resource_extent{},crop_extent{};
    unsigned left{},top{},format{},precision_formula{};
    float precision_scale=1,precision_bias=0;
};
struct NativeTupleMetadata {
    std::uint64_t source_id{},epoch{},command_generation{},previous_source_id{},previous_token{};
    std::uint32_t token{},viewport{},native_viewport{};
    CameraReceipt receipt;
    Fov render_fov;
    Extent sr_input{},sr_output{};
    std::array<float,104> original_camera{};
    std::array<std::uint32_t,7> original_flags{};
    std::array<NativeTupleTag,4> original_tags;
    bool fixture{};
};
struct NativeTupleStatus {
    bool running{},failed{};
    unsigned recorded{},written{},rejected{};
    std::uint64_t reserved_bytes{},peak_bytes{};
};
enum class NativeTupleCompletion { Deferred, Queued, Rejected, AlreadySettled };
class NativeTupleExport {
public:
    struct Ticket;
    using Pending=std::shared_ptr<Ticket>;
    NativeTupleExport(); ~NativeTupleExport();
    NativeTupleExport(const NativeTupleExport&)=delete;
    NativeTupleExport& operator=(const NativeTupleExport&)=delete;
    // Output path must be a new per-run folder. The IO worker alone creates/writes it.
    bool start(const std::filesystem::path&,Log={},unsigned maximum_frames=4,
               std::uint64_t maximum_readback_bytes=256ull*1024*1024);
    // Record once into the existing final-color copy list. No Execute/Wait/Map/IO.
    // Inputs must already rest in COPY_SOURCE. All allocations precede commands.
    Pending record(ID3D12Device*,ID3D12GraphicsCommandList*,const NativeTupleMetadata&,
                   const GpuImage& final_color,const GpuImage& raw_motion,const GpuImage& raw_depth);
    // Caller retains Ticket until the one-use copy has completed. An original
    // native recording must also be retired, with no replay, to certify provenance.
    NativeTupleCompletion complete(const Pending&,ID3D12Fence*,std::uint64_t,
                                    bool native_recording_retired,bool native_replayed);
    NativeTupleStatus status() const;
    // Stop admission and drain already-certified IO. No GPU wait; caller still
    // owns any pending GPU Ticket through the existing source retirement path.
    void stop();
private:
    struct Impl;std::shared_ptr<Impl> impl_;
};
}
