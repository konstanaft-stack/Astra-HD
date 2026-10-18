#pragma once
#include <array>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstddef>
#include "Framegen/PairedGeometryDiagnostics.hpp"
namespace cvr::framegen {
enum class OwnBridgeReason:uint32_t {PreSelect,InvalidTime,MissingDevice,DeviceChanged,ConsumerCapacity,BackendFailed,HistoryResetPending,NoCandidate,EpochSpaceMismatch,EyeOrColorNotReady,CurrentMetadataMissing,GeneratedOriginMismatch,PreviousMetadataMissing,ProjectionMissing,ExtentMismatch,PreviousGeometryUnresolved,CurrentGeometryUnresolved,CropMismatch,RenderedFovDelta,LiveSourceExtentMismatch,ResourceShapeFormatTransferState,BackendDetached,Ready};
enum class OwnPresenterPath:uint32_t {Unknown,CurrentRealFallback,NativeSelected,PreSelectionReal};
enum class OwnDownstreamReason:uint32_t {NotExamined,InitialRealAdmissionRejected,InitialRealGeometryRejected,ForcedRealZoom,RealFallbackGeometryRejected,SelectedIdentityMissing,SelectedNativeGeometryRejected,ColorPrepareRejected,TargetCropExtentRejected,ProducerWaitFailed,GeometryReady};
enum class OwnOutputReason:uint32_t {NotExamined,AllocatorNotReady,RecordFailed,CloseFailed,SignalFailed,ReleaseFailed,XrEndFailed,AcceptedProjection,Replay};
struct OwnWitnessGeometry {
 bool previousExamined{},currentExamined{},previousResolved{},currentResolved{},comparisonKnown{};
 uint32_t previousWidth{},previousHeight{},currentWidth{},currentHeight{},outputWidth{},outputHeight{};
 std::array<float,4> previousCrop{},currentCrop{},previousFov{},currentFov{},delta{};
 uint32_t cropMismatch{};float maximumDelta{};
 void Compare()noexcept{comparisonKnown=previousResolved&&currentResolved;if(!comparisonKnown)return;cropMismatch=0;maximumDelta=0;
  for(unsigned i=0;i<4;++i){if(previousCrop[i]!=currentCrop[i])cropMismatch|=1u<<i;delta[i]=currentFov[i]-previousFov[i];if(!std::isfinite(delta[i]))comparisonKnown=false;maximumDelta=std::max(maximumDelta,std::abs(delta[i]));}}
};
struct OwnWitnessEye {
 bool known{},generated{},originKnown{};uint64_t source{},previousSource{},currentSource{},previousNativePair{},currentNativePair{};
 int64_t contentTime{},previousTime{},currentTime{};uint32_t generationIndex{},generationCount{},nativeFrame{},engineView{};
 OwnWitnessGeometry geometry;bool displayExamined{},displayResolved{};uint32_t displayInputMask{},targetWidth{},targetHeight{};
 std::array<float,4> displayCrop{},displayFov{};
};
struct OwnWitnessCounters {
 bool coherent{};uint64_t acceptedInputs{},completedBatches{},generatedOutputs{},selectedPairs{},generatedSelectedPairs{},calls{},newPairs{},proposalRetries{},cursorRepeats{},lockMisses{},lockRepeats{},emptyNoHistory{},emptyExpired{},emptyOther{},resetRequests{},resets{};
 int64_t maximumAge{};
};
struct OwnWorkerDecision {
 bool freshNoCandidate{},requestedKnown{},inputsHandled{},patternAvailable{};std::array<char,2> requestedStep{'?','?'};
 std::array<unsigned,2> expectedProducts{};std::array<int64_t,2> newestReal{},newestGenerated{};
 unsigned compatible{},future{},expired{},skew{},regressed{},earlyForSlot{};
 int64_t minimumSkew=-1,minimumAge=-1,desiredContent{},period{};
};
template<class Selected>void CaptureOwnWorkerDecision(OwnWorkerDecision& out,const Selected& selected)noexcept{
 out={};out.inputsHandled=selected.inputs_handled;out.patternAvailable=selected.pattern_available;
 if(selected.requested_step.size()==2&&(selected.requested_step[0]=='R'||selected.requested_step[0]=='G')&&(selected.requested_step[1]=='R'||selected.requested_step[1]=='G')){out.requestedKnown=true;out.requestedStep={selected.requested_step[0],selected.requested_step[1]};}
 const auto& d=selected.diagnostics;out.freshNoCandidate=!selected.frame&&out.inputsHandled&&out.requestedKnown&&d.display_period_ns>0;
 out.expectedProducts=d.expected_products;out.newestReal=d.newest_real_ns;out.newestGenerated=d.newest_generated_ns;
 out.compatible=d.compatible_pairs;out.future=d.future_pairs;out.expired=d.expired_pairs;out.skew=d.skew_pairs;out.regressed=d.regressed_pairs;out.earlyForSlot=d.early_for_slot;
 out.minimumSkew=d.minimum_candidate_skew_ns;out.minimumAge=d.minimum_candidate_age_ns;out.desiredContent=d.desired_content_ns;out.period=d.display_period_ns;
}
// Fixed scalar/array data only: never a frame/resource/guide or ownership lease.
struct OwnSelectionWitness {
 uint64_t attempt{},pair{},history{},space{},candidateHistory{},candidateSpace{},resource{},clockGeneration{},sourceSerial{},admittedSerial{},fallbackSerial{};
 int64_t target{},period{},displayTime{};bool candidateKnown{},newPresentation{},baselineCalibration{},realOnly{},replay{};uint32_t generatedMask{};
 OwnBridgeReason bridge=OwnBridgeReason::PreSelect;uint32_t bridgeEye=UINT32_MAX;
 OwnPresenterPath presenter=OwnPresenterPath::Unknown;OwnDownstreamReason downstream=OwnDownstreamReason::NotExamined;uint32_t downstreamEye=UINT32_MAX;
 OwnOutputReason output=OwnOutputReason::NotExamined;
 OwnWorkerDecision workerDecision;PairedGeometryDiagnostic initialGeometry;
 bool inputExamined{};uint32_t inputCode{},inputSubmittedMask{};
 uint32_t initialPairedAdmission{};bool initialLease{},initialGeometryExamined{},initialGeometryResolved{};
 std::array<OwnWitnessEye,2> eyes{};OwnWitnessCounters selectedCounters{},submittedCounters{};
 bool submissionKnown{},submissionReported{},copySubmitted{},signalReported{},signalValidatedKnown{},signalSucceeded{},accepted{},ackCall{},ackReturnKnown{},ackReturn{};
 uint64_t samePairRun{};int64_t samePairFirstTarget{};
 bool AckMayBeDeferred()const noexcept{return ackCall&&ackReturnKnown&&!ackReturn;}
};
inline void OwnWitnessSubmission(OwnSelectionWitness& w,bool accepted,bool copy,bool signal,int64_t display,bool ackCall,bool ackKnown,bool ackReturn)noexcept{
 w.submissionKnown=true;w.submissionReported=true;w.accepted=accepted;w.copySubmitted=copy;w.signalReported=signal;w.displayTime=display;w.ackCall=ackCall;w.ackReturnKnown=ackKnown;w.ackReturn=ackReturn;
}
template<class Selection>void ResetSelectionPreservingOwnWitness(Selection& selection){const auto w=selection.ownWitness;selection={};selection.ownWitness=w;}
// Serialized XR owner only. No cross-thread last-record correlation.
class OwnWitnessRing {
 std::array<OwnSelectionWitness,64> rows_{};size_t count_{},cursor_{};uint64_t latest_{},evicted_{},lastPair_{},lastHistory_{},lastSpace_{},lastExpectedHistory_{},lastExpectedSpace_{},lastResource_{},lastClock_{},run_{},printed_{},nextLog_{},lastFinished_{};int64_t firstTarget_{};
 std::array<uint64_t,23> bridgeCounts_{};
 std::array<uint64_t,11> downstreamCounts_{};std::array<uint64_t,9> outputCounts_{};
 void Terminal(const OwnSelectionWitness& w)noexcept{if(size_t(w.downstream)<downstreamCounts_.size())++downstreamCounts_[size_t(w.downstream)];if(size_t(w.output)<outputCounts_.size())++outputCounts_[size_t(w.output)];}
public:
 void Store(OwnSelectionWitness& w)noexcept{
  if(!w.attempt)return;
  for(size_t i=0;i<count_;++i)if(rows_[i].attempt==w.attempt){if(w.submissionKnown&&!rows_[i].submissionKnown)Terminal(w);w.samePairRun=rows_[i].samePairRun;w.samePairFirstTarget=rows_[i].samePairFirstTarget;rows_[i]=w;return;}
  if(w.attempt<=latest_)return;
  w.samePairRun=w.candidateKnown&&w.pair==lastPair_&&w.candidateHistory==lastHistory_&&w.candidateSpace==lastSpace_&&w.history==lastExpectedHistory_&&w.space==lastExpectedSpace_&&w.resource==lastResource_&&w.clockGeneration==lastClock_?run_+1:1;
  w.samePairFirstTarget=w.samePairRun>1?firstTarget_:w.target;run_=w.samePairRun;firstTarget_=w.samePairFirstTarget;
  lastPair_=w.candidateKnown?w.pair:0;lastHistory_=w.candidateHistory;lastSpace_=w.candidateSpace;lastExpectedHistory_=w.history;lastExpectedSpace_=w.space;lastResource_=w.resource;lastClock_=w.clockGeneration;latest_=w.attempt;
  const auto reason=size_t(w.bridge);if(reason<bridgeCounts_.size())++bridgeCounts_[reason];
  if(w.submissionKnown)Terminal(w);
  if(count_==rows_.size())++evicted_;else ++count_;rows_[cursor_]=w;cursor_=(cursor_+1)%rows_.size();
 }
 const OwnSelectionWitness* Find(uint64_t id)const noexcept{for(size_t i=0;i<count_;++i)if(rows_[i].attempt==id)return &rows_[i];return nullptr;}
 bool LogDue(const OwnSelectionWitness& w,uint64_t now)noexcept{if(!w.submissionKnown||!w.attempt||w.attempt<=lastFinished_)return false;lastFinished_=w.attempt;if(printed_<8||now>=nextLog_){++printed_;nextLog_=now+1000;return true;}return false;}
 size_t Count()const noexcept{return count_;}uint64_t Evicted()const noexcept{return evicted_;}
 uint64_t ReasonCount(OwnBridgeReason reason)const noexcept{return bridgeCounts_[size_t(reason)];}
 uint64_t DownstreamCount(OwnDownstreamReason reason)const noexcept{return downstreamCounts_[size_t(reason)];}
 uint64_t OutputCount(OwnOutputReason reason)const noexcept{return outputCounts_[size_t(reason)];}
};
bool OwnSelectionWitnessEnabled()noexcept;
uint64_t NextOwnSelectionAttempt()noexcept;
}
