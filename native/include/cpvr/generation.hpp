#pragma once
#include "gpu.hpp"
#include "phase_metrics.hpp"
#include "resource_metrics.hpp"
#include "resource_pool.hpp"
#include <span>
#include <mutex>

namespace cpvr {
enum class MotionConvention { Unknown, NativeStreamlineForwardUvScaled, BackwardUvNormalized, NativeStreamlineBackwardUvScaled };
struct GenerationMiddle {
    std::shared_ptr<const EyeFrame> frame;
    std::uint64_t native_frame_token{}, previous_native_frame_token{}, guides_previous_source_id{};
    std::int64_t guides_previous_time_ns{};
    MotionConvention motion_convention = MotionConvention::Unknown;
};
struct MotionGuideSample {
    std::shared_ptr<const SourceGuides> guides;
    std::uint64_t source_id{}, native_frame_token{}, previous_native_frame_token{}, previous_source_id{};
    std::int64_t time_ns{}, previous_time_ns{};
    MotionConvention convention = MotionConvention::Unknown;
};
struct MotionRecordRequest {
    MotionGuideSample previous, current;
    std::optional<MotionGuideSample> middle;
    bool reset{};
};
struct MotionNormalization {
    std::array<float,2> scale{}, bias{}, current_jitter_uv{};
};
Validation validate_motion_request(const MotionRecordRequest&);
std::optional<MotionNormalization> motion_normalization(const SourceGuides& current,
    const SourceGuides& previous, Extent current_extent, Extent previous_extent, MotionConvention);
struct MotionRecorded {
    // New resources finish NON_PIXEL_SHADER_RESOURCE|COPY_SOURCE; flags/scales describe
    // backward normalized UV with jitter removed, not native SL forward vectors.
    std::shared_ptr<const SourceGuides> guides;
    // Corrected backward displacement on native current jittered sample grid.
    // SR keeps actual jitterOffset/raw depth and supplies scales-1 to Streamline.
    GpuImage sr_motion;
    // Unique descriptor heap/targets/constant buffer; retain until caller's GPU fence.
    std::shared_ptr<void> lifetime;
};
class MotionRecorder {
public:
    MotionRecorder(); ~MotionRecorder();
    MotionRecorder(const MotionRecorder&) = delete;
    MotionRecorder& operator=(const MotionRecorder&) = delete;
    bool initialize(ID3D12Device*,Log);
    // Records into an already open list. No Execute, Signal, GPU wait or color needed.
    // Caller orders raw producer work on its queue and retains all inputs + lifetime.
    std::optional<MotionRecorded> record(ID3D12GraphicsCommandList*,const MotionRecordRequest&);
private:
    struct Impl;std::unique_ptr<Impl> impl_;
};
struct GenerationInput {
    std::shared_ptr<const EyeFrame> frame;
    // Exact predecessor represented by the normalized camera/object motion.
    // Zero means unknown span: retain real color, reset/warm history, no FG claim.
    std::uint64_t guides_previous_source_id{};
    std::int64_t guides_previous_time_ns{};
    std::uint64_t native_frame_token{}, previous_native_frame_token{};
    MotionConvention motion_convention = MotionConvention::Unknown;
    // Optional immediate native predecessor for original two-step MV composition.
    // This is a flat owned link, not an unbounded recursive frame chain.
    GenerationMiddle immediate_previous;
};
enum class PresentationPattern { Chessboard, RealThenGeneratedStereo, Custom };
struct GenerationPolicy {
    unsigned generated_per_interval = 1; // 0 disables; vendor batch supports 1..5.
    Cadence cadence = Cadence::AlternateEyes;
    std::int64_t maximum_eye_skew_ns = 8'000'000;
    std::int64_t maximum_age_ns = 150'000'000;
    std::int64_t maximum_interval_ns = 250'000'000;
    PresentationPattern presentation_pattern = PresentationPattern::Chessboard;
    // Each token is LEFT/RIGHT provenance; comma separated, up to16 tokens.
    std::string custom_sequence = "RR,GG,GR,RG";
    bool operator==(const GenerationPolicy&) const = default;
};
GenerationPolicy generation_policy_for_source(const Settings&, Cadence actual_source);
struct GenerationConfig {
    std::wstring backend_library, snippet_directory;
    std::wstring driver_library = L"C:\\Windows\\System32\\DriverStore\\FileRepository\\nv_dispi.inf_amd64_b20cc8aeaed64fc2\\nvngx.dll";
    std::string driver_sha256 = "3F8517407C8869B6CF28BD85D17B8FF0707EF380E86AF39F85A73E3401CC6FC4";
    // Latest prepared official NVIDIA 310.9.1. Other snippets are rejected.
    std::string snippet_sha256 = "FF6E90EB78B827927DFF5B4ECC6B1C870C2E9BCA29ED9F48C7D348CC9E170B82";
    bool initialize_ngx{};
    bool enable_verified_compatibility_gate{};
    unsigned queue_capacity = 8, products_per_eye = 12;
    GenerationPolicy policy;
};
struct GenerationOrigin {
    bool generated{};
    std::uint64_t previous_source_id{}, current_source_id{};
    std::uint64_t previous_native_pair_id{}, current_native_pair_id{};
    std::int64_t previous_time_ns{}, current_time_ns{};
    unsigned index{}, count{}; // Vendor ordinal / count; phase = index/(count+1).
};
struct GenerationProduct {
    std::shared_ptr<const EyeFrame> frame;
    GenerationOrigin origin;
};
struct InterpolationStamp {
    std::int64_t content_time_ns{};
    Pose render_pose;
    Fov fov;
    GenerationOrigin origin;
};
struct GenerationCursor {
    std::uint64_t epoch{}, space_epoch{}, pair_sequence{};
    std::array<std::int64_t,2> eye_times{};
    bool valid{};
    unsigned pattern_step{};
    std::shared_ptr<const StereoFrame> last_selection;
    std::array<GenerationOrigin,2> last_origins;
    // Pacing clock only; never substitute it for an eye's actual content time.
    std::int64_t display_time_ns{},content_clock_ns{};
};
struct SelectionDiagnostics {
    std::array<unsigned,2> expected_products{};
    std::array<std::int64_t,2> newest_real_ns{},newest_generated_ns{};
    unsigned compatible_pairs{},future_pairs{},expired_pairs{},skew_pairs{},regressed_pairs{};
    std::int64_t minimum_candidate_skew_ns=-1,minimum_candidate_age_ns=-1;
    std::int64_t desired_content_ns{},display_period_ns{};
    unsigned early_for_slot{};
};
struct GenerationSelection {
    std::shared_ptr<const StereoFrame> frame;
    std::array<GenerationOrigin,2> origins;
    bool new_presentation{}, pattern_available{};
    // Incoming batch was attempted under the selector lock (invalid/stopped
    // input can still be rejected). False leaves admission to the caller.
    bool inputs_handled{};
    std::string requested_step;
    SelectionDiagnostics diagnostics;
};
// CPU metadata operations, also used by the worker. No vendor or GPU invocation.
Validation validate_generation_policy(const GenerationPolicy&);
std::optional<InterpolationStamp> interpolation_stamp(const EyeFrame&, const EyeFrame&,
                                                     unsigned index, unsigned count);
GenerationSelection choose_generation_pair(std::span<const GenerationProduct> left,
    std::span<const GenerationProduct> right, std::int64_t target_time_ns,
    std::int64_t predicted_period_ns, const GenerationPolicy&, GenerationCursor&);
struct GenerationStatus {
    bool worker_running{}, backend_attached{}, failed{}, evaluating{}, requested_pattern_available{};
    bool history_reset_pending{};
    std::uint64_t history_reset_requests{},history_resets{};
    std::uint64_t accepted_inputs{}, dropped_inputs{}, completed_batches{},
        generated_outputs{}, vendor_suppressed_batches{}, unknown_guide_spans{},
        selected_pairs{}, generated_selected_pairs{};
    std::array<std::uint64_t,2> generated_per_eye{};
    GenerationPolicy active_policy,requested_policy;
    bool policy_pending{};
    unsigned queued_inputs{}, ready_generated_unpaired{};
    std::string waiting_pattern;
    std::array<Extent,2> actual_generated_sizes{};
    // CPU-wall intervals (including waits), grouped by requested G count0..5.
    // These are not GPU timestamp durations or physical motion-to-photon.
    struct Timings {PhaseDurationSummary queued,private_snapshot,backend_batch;};
    std::array<Timings,6> timings;
    std::uint64_t timing_source_id{},timing_native_token{},timing_epoch{},timing_revision{};
    unsigned timing_eye{},timing_generated_count{};
    ResourceUsage private_textures;
    TexturePoolStats private_inputs_pool,generated_pool;
    std::uint64_t copy_command_creations{},copy_command_resets{};
    std::uint64_t late_reference_inputs{},expired_batches{},expired_products{};
    std::uint64_t real_published_early{},real_published_worker{};
    std::uint64_t generated_published_early{},generated_ready_ahead{},generated_published_final{};
    std::array<std::int64_t,2> latest_input_time_ns{};
    std::int64_t selection_target_ns{};
    std::int64_t selection_examined_ns{};
    SelectionDiagnostics selection;
    struct SelectionFlow {
        std::uint64_t calls{},lock_misses{},new_pairs{},proposal_retries{},cursor_repeats{},
            empty_no_history{},empty_expired_history{},empty_other{},lock_repeats{};
    } selection_flow;
    std::string phase = "Generation worker not started";
};
class GenerationWorker {
public:
    GenerationWorker();
    ~GenerationWorker();
    GenerationWorker(const GenerationWorker&) = delete;
    GenerationWorker& operator=(const GenerationWorker&) = delete;
    // Starts a dedicated owner worker. true means started, not NGX attached/evaluating.
    bool start(ID3D12Device*, const GenerationConfig&, Log);
    // Bounded try-lock operations: never wait for worker/GPU on game/XR callbacks.
    bool submit(const GenerationInput&);
    bool configure(const GenerationPolicy&);
    // Same serialized owner as select/ack. Nonblocking and coalesced; the
    // existing generation owner retires completed work and clears history.
    // Pending reset suppresses stale selection and late revision publication.
    bool request_history_reset() noexcept;
    GenerationSelection select(std::int64_t target_time_ns, std::int64_t predicted_period_ns,
                               std::span<const GenerationInput> incoming={});
    // Nonblocking expiry only; never creates a presentation proposal. May consume
    // an existing XR acknowledgement; never advances presentation speculatively.
    // Safe to service when the producer has no coherent new pair.
    bool retire_expired(std::int64_t target_time_ns);
    // Commit only after this pair was accepted by projection xrEndFrame.
    // configure/select/acknowledge are externally serialized by the XR owner.
    // A busy acknowledgement is retained for commit before its next mutation.
    bool acknowledge(std::uint64_t pair_id,std::uint64_t epoch,std::uint64_t space_epoch,
                     std::int64_t first_display_time_ns=0);
    GenerationStatus status() const;
    // Application shutdown only. Joins worker, which alone may wait for vendor GPU work.
    void stop();
    // Read only after stop() returns (or before start on a quiescent owner).
    // False means retained/unknown GPU ownership; do not start a replacement.
    bool stopped_cleanly() const noexcept;
private:
    friend struct GenerationWorkerTestAccess;
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
// Metadata-only fixture boundary; never attaches NVIDIA or creates GPU work.
struct GenerationWorkerTestAccess {
    static void seed(GenerationWorker&,const GenerationPolicy&,std::span<const GenerationProduct>,std::span<const GenerationProduct>);
    static std::unique_lock<std::mutex> lock(GenerationWorker&);
    static void invalidate(GenerationWorker&);
    static GenerationCursor committed_cursor(const GenerationWorker&);
    static bool configure(GenerationWorker&,const GenerationPolicy&);
    // Fixture device/identity only; no owner thread, NVIDIA call or GPU submit.
    // CPU readiness fixture only; backend_ready does not attach NVIDIA or run a GPU.
    static void enable_submission(GenerationWorker&,ID3D12Device*,std::uint64_t epoch,std::uint64_t space_epoch,bool backend_ready=false);
    static void process_real_only(GenerationWorker&,const GenerationInput&);
    static std::uint64_t revision(const GenerationWorker&);
    static bool publish_generated(GenerationWorker&,const GenerationProduct&,std::uint64_t revision,std::uint64_t batch_completed);
    static bool apply_history_reset(GenerationWorker&); // CPU fixture owner step; never a running worker.
    // Explicit WARP fixture: production private-copy/normalization and history,
    // no worker thread, backend library, NGX evaluation or XR submission.
    static bool snapshot_only(GenerationWorker&,ID3D12Device*,ID3D12CommandQueue*,const GenerationInput&);
    static GenerationInput snapshot_history(const GenerationWorker&,Eye);
};
}
