#pragma once
#include "gpu.hpp"
#include "generation.hpp"
#include "native_tuple.hpp"
#include <dxgi1_4.h>
#include <span>
#include <vector>
#include <optional>
#include <unordered_map>

namespace sl { struct BaseStructure; struct Constants; }

namespace cpvr {
struct GameSourceStatus {
    std::uint64_t accepted_sr{},submitted_guides{},published{},dropped{},generation_resets{};
    std::uint64_t sr_budget_warnings{};
    bool hooks_installed{},device_ready{};
    bool cleanup_complete{};
    unsigned pending_retirement{};
    unsigned capture_live{},capture_high_water{};
    std::uint64_t capture_created{},capture_backpressure{},capture_retired_unjoined{};
    unsigned tag_snapshot_live{},tag_snapshot_high_water{},tag_declarations{};
    std::uint64_t tag_snapshot_created{},tag_snapshot_retired{},tag_snapshot_backpressure{};
    std::string last_drop;
    std::uint64_t content_epoch{};
};
struct GameSourceFrame {
    GenerationInput generation;
    Extent sr_input{},sr_output{};
    DXGI_FORMAT sr_output_format=DXGI_FORMAT_UNKNOWN;
};
struct ExecuteReceipt { std::uint64_t serial{}; };
class GameSource {
public:
    static constexpr unsigned capture_capacity=6;
    // The live producer renders one eye and advances only after its Present copy.
    // DisplayPattern controls presentation; it does not change this production fact.
    static constexpr Cadence production_cadence() noexcept { return Cadence::AlternateEyes; }
    // PID17332 native raw guides vs independent clip/depth projection: backward
    // UV after the declared scale, not forward. Keep normalized SR/MFG together.
    static constexpr MotionConvention production_motion_convention() noexcept { return MotionConvention::NativeStreamlineBackwardUvScaled; }
    GameSource();
    ~GameSource();
    bool initialize(ID3D12Device*,CameraBridge&,Log);
    ComPtr<ID3D12Device> native_device() const;
    void pause_capture_for_resize(bool);
    void pause_capture_for_menu(bool);
    std::uint64_t restart_world_history();
    bool ready_for_resize();
    bool install_hooks();
    void configure(const Settings&);
    // Notifications bracket the true native original ExecuteCommandLists call.
    ExecuteReceipt before_execute(ID3D12CommandQueue*,std::span<ID3D12CommandList* const>);
    void after_execute(ExecuteReceipt);
    void command_reset(ID3D12GraphicsCommandList*,bool success);
    void command_close(ID3D12GraphicsCommandList*,bool success);
    void record_barriers(ID3D12GraphicsCommandList*,UINT,const D3D12_RESOURCE_BARRIER*);
    // Called before original Present, with factory-observed true swapchain queue.
    bool on_present(IDXGISwapChain3*,ID3D12CommandQueue*,CameraReceipt* captured=nullptr);
    std::vector<GameSourceFrame> drain();
    bool arm_native_capture(const std::filesystem::path& output_directory);
    NativeTupleStatus native_capture_status() const;
    GameSourceStatus status() const;
    void request_stop();
    // Called before the host's public slShutdown invalidates shared NGX state.
    // The callback must retire our NVIDIA client on its owner thread.
    void before_host_shutdown(std::function<bool()>);
    bool shutdown();
private:
    friend struct GameSourceTestAccess;
    struct Impl;
    std::shared_ptr<Impl> impl_;
};
// Narrow offline fixture admission: records real owned copies and enters the
// production capture lifecycle, without installing hooks or emulating SL ABI.
struct GameSourceTestAccess {
    // Actual MinHook boundary against a fixture-owned shutdown function. No
    // Cyberpunk offsets, Streamline implementation, or game launch are emulated.
    static bool install_host_shutdown_boundary(GameSource&,void* native_function,Log);
    static std::pair<ComPtr<ID3D12Fence>,bool> final_resize_witness(GameSource&);
    static bool record_capture(GameSource&,ID3D12GraphicsCommandList*,const CameraReceipt&,
                               std::uint32_t token,ID3D12Resource* motion,ID3D12Resource* depth,Extent output);
    static bool remap_audit_tag_after_capture(GameSource&);
    static bool record_volatile_tag(GameSource&,ID3D12GraphicsCommandList*,ID3D12Resource*,bool declare=false);
    static std::shared_ptr<void> recording_lease(GameSource&,bool volatile_tag);
    static std::optional<D3D12_RESOURCE_STATES> evaluation_state(GameSource&,ID3D12GraphicsCommandList*,
        ID3D12Resource*,D3D12_RESOURCE_STATES declared,std::uint64_t declaration);
};
namespace source_math {
sl::Constants adjusted_sr_constants(const sl::Constants&,const SourceGuides& normalized);
bool sr_evaluation_completed(int result) noexcept;
struct TagDescription {
    std::uint32_t type{},state=UINT32_MAX,lifecycle{},resource_type{};
    std::uintptr_t resource{};
    std::array<std::uint32_t,4> region{}; // left, top, width, height
    std::uint32_t precision_formula{};
    float precision_bias{},precision_scale=1;
    std::uint64_t revision{};
    std::uint64_t state_declaration{};
};
struct InputLayout {
    bool valid{};std::uint32_t viewport{};
    std::vector<TagDescription> tags;
    std::vector<const sl::BaseStructure*> nodes;
    std::string reason;
};
InputLayout parse_inputs(std::span<const sl::BaseStructure* const>);
InputLayout parse_tags(const void* tags,std::uint32_t count);
struct TransactionSelection {
    bool valid{};std::uint32_t viewport{};std::uint64_t receipt_serial{},generation{};
    std::array<TagDescription,4> tags;
    std::string reason;
};
// Same API-order resolver is used by capture and CPU regressions. It owns scalar
// declarations only; runtime retains actual resources and GPU snapshot leases.
class TransactionLedger {
public:
    std::vector<TagDescription> tag(std::uint32_t viewport,std::optional<std::uint32_t> frame,
                                    std::span<const TagDescription>);
    void constant(std::uint32_t frame,std::uint32_t viewport,std::uint64_t receipt_serial);
    void reset(std::uintptr_t command,bool success);
    void close(std::uintptr_t command,bool success);
    TransactionSelection resolve(std::uint32_t frame,const InputLayout&,std::uintptr_t command) const;
    bool retains_revision(std::uint64_t revision) const noexcept;
    void clear();
    void clear_declarations();
private:
    struct Declaration {std::uint32_t viewport{};std::optional<std::uint32_t> frame;TagDescription tag;};
    struct Constant {std::uint32_t frame{},viewport{};std::uint64_t serial{};};
    struct Recording {std::uint64_t generation{};bool valid{},closed{};};
    std::uint64_t next_revision_{};
    std::vector<Declaration> declarations_;
    std::vector<Constant> constants_;
    std::unordered_map<std::uintptr_t,Recording> recordings_;
};
bool capture_command_type(D3D12_COMMAND_LIST_TYPE) noexcept;
class ResourceStates {
public:
    void alias(std::uint64_t event,std::uintptr_t before,std::uintptr_t after);
    void transition(std::uint64_t event,std::uintptr_t resource,D3D12_RESOURCE_STATES before,
                    D3D12_RESOURCE_STATES after,D3D12_RESOURCE_BARRIER_FLAGS flags);
    std::optional<D3D12_RESOURCE_STATES> resolve(std::uintptr_t resource,D3D12_RESOURCE_STATES declared,
                                               std::uint64_t declaration,std::string& reason) const;
private:
    struct Event {std::uint64_t serial{};D3D12_RESOURCE_STATES state{};bool ambiguous{};};
    std::uint64_t wildcard_{};
    std::unordered_map<std::uintptr_t,Event> resources_;
};
// Production copy primitive used by GameSource and its actual D3D12 fixture.
// Source state is an already resolved authoritative state, not guessed COMMON.
HRESULT record_mip0_copy(ID3D12Device*,ID3D12GraphicsCommandList*,ID3D12Resource*,
                        D3D12_RESOURCE_STATES,const std::array<std::uint32_t,4>& region,
                        GpuImage&,std::string& reason);
// Projection is SL's row-major view-to-clip matrix. Unrecognized projection drops.
bool projection_fov(const std::array<float,16>&,Fov&) noexcept;
bool receipt_matches(const CameraReceipt&,const std::array<float,104>&) noexcept;
std::optional<CameraReceipt> select_receipt(const CameraReceiptWindow&,const std::array<float,104>&,
                                          std::int64_t current_qpc,std::int64_t qpc_frequency,std::optional<std::uint32_t> native_frame={}) noexcept;
bool contiguous_tokens(std::uint32_t previous,std::uint32_t current,bool reset) noexcept;
bool is_main_view(std::uintptr_t main_object,std::uintptr_t object,std::uintptr_t main_context,
                  std::uintptr_t context,std::uint64_t key) noexcept;
}
}
