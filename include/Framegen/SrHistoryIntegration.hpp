#pragma once
#include "Framegen/SrHistoryOwner.hpp"
#include <atomic>
namespace sl {struct Constants;struct FrameToken;struct ViewportHandle;struct BaseStructure;}
namespace cvr::camera {struct PoseIdentity;}
namespace cvr::framegen {
// Receipts hold the same owner across the original host call. Source metadata
// is sampled before that call; its result is appended afterwards.
struct SrConstantsCall {std::shared_ptr<SrHistoryOwner> owner;SrConstantsObservation observation;};
struct SrTagsCall {std::shared_ptr<SrHistoryOwner> owner;SrTagCaptureToken token;};
struct SrEvaluationCall {std::shared_ptr<SrHistoryOwner> owner;SrSlEvaluationToken token;};
struct SrSubmissionCall {std::shared_ptr<SrHistoryOwner> owner;SrSubmissionToken token;};
bool InstallSrHistoryIntegration();
void SetSrHistoryRequested(bool) noexcept;
SrHistoryStatus ReadSrHistoryStatus() noexcept;
std::optional<SrFrameGenerationGuide> AcquireSrFrameGenerationGuide(const camera::PoseIdentity&) noexcept;
SrConstantsCall BeginSrConstants(const sl::Constants&,const sl::FrameToken&,const sl::ViewportHandle&) noexcept;
void EndSrConstants(SrConstantsCall&,uint32_t result,bool resultKnown=true) noexcept;
SrTagsCall BeginSrTags(const void* viewport,const void* tags,uint32_t count,void* list) noexcept;
void EndSrTags(SrTagsCall&,uint32_t result,bool resultKnown=true) noexcept;
void SrObserveTaggedCopy(ID3D12GraphicsCommandList*,ID3D12Resource*,ID3D12Resource*) noexcept;
SrEvaluationCall BeginSrEvaluation(uint32_t feature,const sl::FrameToken&,const sl::BaseStructure* const*,uint32_t count,void* list) noexcept;
void EndSrEvaluation(SrEvaluationCall&,uint32_t result,bool resultKnown=true) noexcept;
template<class Call,void(*Finish)(Call&,uint32_t,bool) noexcept> class SrCallCleanup {
public:
    explicit SrCallCleanup(Call& call) noexcept:call_(call){}
    // Normal End consumes owner/token; otherwise any unwinding/early return
    // closes this scope with an explicitly unknown host result.
    ~SrCallCleanup() noexcept {Finish(call_,UINT32_MAX,false);}
    SrCallCleanup(const SrCallCleanup&)=delete;
    SrCallCleanup& operator=(const SrCallCleanup&)=delete;
private:Call& call_;
};
using SrConstantsCleanup=SrCallCleanup<SrConstantsCall,EndSrConstants>;
using SrTagsCleanup=SrCallCleanup<SrTagsCall,EndSrTags>;
using SrEvaluationCleanup=SrCallCleanup<SrEvaluationCall,EndSrEvaluation>;
SrSubmissionCall SrBeforeSubmit(ID3D12CommandQueue*,UINT,ID3D12CommandList* const*) noexcept;
void SrSubmitted(SrSubmissionCall&,ID3D12CommandQueue*,UINT,ID3D12CommandList* const*) noexcept;
void SrResetList(ID3D12GraphicsCommandList*) noexcept;
struct SrHistoryWire {
    uint32_t version=1,bytes=208;
    int64_t sourceId=-1;
    uint64_t historyEpoch{},resourceEpoch{},pairedCreates{},cloneFailures{},capturedSources{};
    uint64_t correctedEvaluations{},passthroughEvaluations{},resetEvents{};
    uint32_t stage{},snapshotCells{},pendingCaptures{},livePasses{},livePairs{},resetEyeMask{};
    uint32_t adapterStatus{},nativeResult{},requested{},quiescing{};
    uint32_t companionResult{},pendingProducers{};
    uint64_t missingNativeParameters{},crossQueueWaits{},submissionFailures{};
    std::array<uint64_t,2> successfulNativeByEye{};
    uint64_t lastSelectedHandle{};
    uint32_t lastPhysicalEye=UINT32_MAX;
    int32_t lastReset=-1;
    float lastDeltaMsec{};
    uint32_t lastResetKnown{},lastDeltaKnown{};
    int32_t lastSubmissionResult{};
    uint64_t dropped{};
};
static_assert(sizeof(SrHistoryWire)==208);
static_assert(offsetof(SrHistoryWire,missingNativeParameters)==128);
static_assert(offsetof(SrHistoryWire,successfulNativeByEye)==152);
static_assert(offsetof(SrHistoryWire,lastSelectedHandle)==168);
static_assert(offsetof(SrHistoryWire,dropped)==200);
}
extern "C" {
__declspec(dllexport) extern cvr::framegen::SrHistoryWire CyberpunkVR_SrHistoryStatus;
__declspec(dllexport) extern std::atomic<uint64_t> CyberpunkVR_SrHistoryStatusSeq;
}
