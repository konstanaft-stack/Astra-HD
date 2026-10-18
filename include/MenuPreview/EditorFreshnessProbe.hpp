#pragma once
#include <array>
#include <cstdint>
#include <mutex>
namespace cvr::menu_preview {
struct CameraApplication;
enum class FreshnessEvent:uint32_t {Duplicate,SameSourceReplace,OtherReplace,OlderReject,WriterMatch,WriterMismatch,PairAccept,PairReadMiss,PairAdmissionReject,RawUnavailable,Fresh,Repeat,Replay,Empty,EndError,AcceptedOther,RetainedAccepted,ScopeAbsent,Count};
enum class FreshnessStage:uint32_t {Publication,Writer,Pair,Terminal};
struct FreshnessSample {
 uint64_t qpc{},source{},otherSource{},revision{},otherRevision{},control{},origin{},previousPointer{},nextPointer{},resource{},generation{},rawLeft{},rawRight{};int64_t displayTime{};
 uint32_t epoch{},setup{},nativeFrame{},eye{},flags{};
};
struct FreshnessSnapshot {std::array<uint64_t,size_t(FreshnessEvent::Count)> counts{};std::array<FreshnessSample,4> last{};uint64_t intervalMs{};uint32_t epoch{},armedEpoch{},row{};bool intervalKnown{};};
// Fixed scalar storage only. Runtime clock/logging stays outside this policy.
class EditorFreshnessRecorder {
 std::mutex mutex_;std::array<uint64_t,size_t(FreshnessEvent::Count)> counts_{};std::array<FreshnessSample,4> last_{};
 bool armed_{},clockStarted_{};uint32_t epoch_{},armedEpoch_{},rows_{};uint64_t intervalStart_{},lastEmit_{};
public:
 void Scope(bool enabled,uint32_t epoch,uint64_t nowMs){if(!enabled)return;std::lock_guard lock(mutex_);
  if(!armed_&&epoch){armed_=true;armedEpoch_=epoch;intervalStart_=nowMs;clockStarted_=true;}epoch_=epoch;
  if(armed_&&!epoch)++counts_[size_t(FreshnessEvent::ScopeAbsent)];
 }
 bool Armed(){std::lock_guard lock(mutex_);return armed_;}
 void Note(bool enabled,FreshnessEvent event,FreshnessStage stage,const FreshnessSample& value){
  if(!enabled||size_t(event)>=counts_.size()||size_t(stage)>=last_.size())return;
  std::lock_guard lock(mutex_);if(!armed_){if(!value.epoch)return;armed_=true;epoch_=armedEpoch_=value.epoch;}

  ++counts_[size_t(event)];if((event==FreshnessEvent::Fresh||event==FreshnessEvent::Repeat)&&(value.flags&1))++counts_[size_t(FreshnessEvent::RetainedAccepted)];last_[size_t(stage)]=value;
 }
 bool Take(bool enabled,uint64_t nowMs,FreshnessSnapshot& out){
  if(!enabled)return false;std::lock_guard lock(mutex_);
  if(!armed_||rows_>=32||(rows_>=4&&(nowMs<lastEmit_||nowMs-lastEmit_<1000)))return false;
  out={counts_,last_,clockStarted_&&nowMs>=intervalStart_?nowMs-intervalStart_:0,epoch_,armedEpoch_,++rows_,clockStarted_&&nowMs>=intervalStart_};counts_={};lastEmit_=nowMs;intervalStart_=nowMs;clockStarted_=true;return true;
 }
};
bool EditorFreshnessEnabled()noexcept;
void EditorFreshnessScope(uint32_t epoch)noexcept;
void NoteEditorFreshness(FreshnessEvent,FreshnessStage,const FreshnessSample&)noexcept;
void NoteEditorPublication(const CameraApplication* previous,const CameraApplication* incoming,bool duplicate,bool older)noexcept;
void EmitEditorFreshness()noexcept; // Terminal XR thread only; never called under source locks.
}
