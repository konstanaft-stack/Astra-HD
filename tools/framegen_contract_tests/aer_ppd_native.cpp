#include <Windows.h>
#include "Overlay/PpdNativeApply.hpp"
#include <array>
#include <atomic>
#include <iostream>
#include <stdexcept>
#include <thread>
#include "ppd-call-sites.hpp"
using namespace cvr::ppd;
extern "C" {
extern std::atomic<uint64_t> CyberpunkVR_PpdNativeStatusSeq;
extern NativeApplyTelemetryPacket CyberpunkVR_PpdNativeStatus;
}
static void Check(bool ok,const char* text){if(!ok)throw std::runtime_error(text);}
struct EngineSettings {
    std::array<uint32_t,64> words{};
    uintptr_t Pointer(){return reinterpret_cast<uintptr_t>(words.data());}
    NativeSize Active()const{return {words[0x18/4],words[0x1c/4]};}
    NativeSize Pending()const{return {words[0x84/4],words[0x88/4]};}
    void Current(NativeSize v){words[0x18/4]=v.width;words[0x1c/4]=v.height;}
    void Target(NativeSize v){words[0x84/4]=v.width;words[0x88/4]=v.height;}
    EngineSettings(){words.fill(0xD15C0DE);Current({1920,1080});Target({1920,1080});}
    // Independent engine model: exactly the recovered changed-predicate input.
    bool ChangeRequired()const{return Active()!=Pending();}
    void EngineConsumesPending(){Current(Pending());}
};
static void Reset(){auto s=ReadNativeApplyStatus();if(s.OwnsDimensions())Check(CancelNativeSize(s.revision),"failed cleanup");productionCallSites::shared[81]=0;}
static void StartupAuthorFallback(){
    EngineSettings e;Check(!ReadNativeRequestedSize(),"default state owns source dimensions");
    Check(!ProcessNativeSizeAtSettingsHook(e.Pointer(),true),"default native owner bypassed author mode");
    Check(WriteAuthorNativeSize(e.Pointer(),2048,2048),"author startup override stopped working");
    Check(e.Active()==NativeSize{2048,2048} && e.Pending()==e.Active(),"author four-store baseline changed");
}
static void RequestDoesNotTouchEngine(){
    EngineSettings e;const auto before=e.words;const auto r=RequestNativeSize(2560,2848);
    Check(r && ReadNativeApplyStatus().stage==NativeApplyStage::Requested,"valid request was not queued");
    Check(e.words==before,"request touched engine before callback");
    auto pair=ReadNativeRequestedSize();Check(pair && pair->revision==r && pair->size==NativeSize{2560,2848},"coherent request pair unavailable");
}
static void NativePendingAndLaterObservation(){
    EngineSettings e;const auto old=e.Active();const auto r=RequestNativeSize(2560,2848);
    Check(r && ProcessNativeSizeAtSettingsHook(e.Pointer(),true),"actual settings hook did not take request");
    Check(e.Active()==old && e.Pending()==NativeSize{2560,2848} && e.ChangeRequired(),"dispatch erased native changed predicate");
    auto s=ReadNativeApplyStatus();Check(s.stage==NativeApplyStage::Dispatched && !s.observationVisit,"pending store was marked observed");
    e.EngineConsumesPending();Check(ProcessNativeSizeAtSettingsHook(e.Pointer(),true),"later hook lost ownership");s=ReadNativeApplyStatus();
    Check(s.stage==NativeApplyStage::ObservedNative && s.observedActive==e.Active() && s.observationVisit>s.dispatchVisit,"later native fields not observed separately");
    // Native active is set before DXGI success in the engine. There is no Applied
    // or GPU acceptance bit in this API and no fake call to a render callback.
}
static void BackgroundCannotOverwritePending(){
    EngineSettings e;RequestNativeSize(2560,2848);ProcessNativeSizeAtSettingsHook(e.Pointer(),true);const auto before=e.words;
    Check(!WriteAuthorNativeSize(e.Pointer(),2048,2048) && e.words==before,"cached background author writer erased live pending dimensions");
}
static void MapDefersWithoutReleasingOwnership(){
    EngineSettings e;const auto before=e.words;const auto r=RequestNativeSize(2560,2848);
    Check(ProcessNativeSizeAtSettingsHook(e.Pointer(),false) && e.words==before,"map suspension wrote pending settings");
    Check(ReadNativeApplyStatus().stage==NativeApplyStage::Requested,"map deferred request disappeared");
    Check(!WriteAuthorNativeSize(e.Pointer(),2048,2048) && ReadNativeRequestedSize()->revision==r,"map defer released owner");
    Check(ProcessNativeSizeAtSettingsHook(e.Pointer(),true) && e.Pending()==NativeSize{2560,2848},"request did not resume on actual allowed callback");
}
static void RevisionCancellationAndRestore(){
    EngineSettings e;const auto a=RequestNativeSize(2560,2848);const auto b=RequestNativeSize(3008,3344);
    Check(a && b>a && !CancelNativeSize(a),"old UI request cancelled replacement");
    Check(ReadNativeRequestedSize()->revision==b && CancelNativeSize(b),"current cancellation failed");
    Check(!ReadNativeRequestedSize() && !ProcessNativeSizeAtSettingsHook(e.Pointer(),true),"cancel left hidden source override");
    Check(WriteAuthorNativeSize(e.Pointer(),2048,2048) && e.Active()==e.Pending(),"OFF author mode did not recover");
}
static void InvalidRequestPreservesCurrent(){
    const auto r=RequestNativeSize(2560,2848);Check(r!=0,"initial request failed");
    Check(!RequestNativeSize(0,128) && !RequestNativeSize(128,0) && !RequestNativeSize(16385,128) && !RequestNativeSize(UINT32_MAX,UINT32_MAX),"invalid texture dimensions accepted");
    Check(ReadNativeRequestedSize()->revision==r,"invalid request replaced valid ownership");
}
static void CallbackPointerNotCached(){
    EngineSettings a,b;RequestNativeSize(2560,2848);ProcessNativeSizeAtSettingsHook(a.Pointer(),true);const auto old=a.words;
    b.Current({2560,2848});b.Target({2560,2848});ProcessNativeSizeAtSettingsHook(b.Pointer(),true);
    Check(a.words==old && ReadNativeApplyStatus().observedActive==b.Active(),"old callback pointer used instead of current argument");
}
static void ReplacementRejectsOldNativeObservation(){
    EngineSettings e;RequestNativeSize(2560,2848);ProcessNativeSizeAtSettingsHook(e.Pointer(),true);e.EngineConsumesPending();
    const auto r=RequestNativeSize(3008,3344);ProcessNativeSizeAtSettingsHook(e.Pointer(),true);auto s=ReadNativeApplyStatus();
    Check(s.revision==r && s.stage==NativeApplyStage::Dispatched && s.observationVisit==0 && e.Pending()==NativeSize{3008,3344},"previous request's native result acknowledged a replacement");
}
static void NativeDriftWithdrawsObservation(){
    EngineSettings e;RequestNativeSize(2560,2848);ProcessNativeSizeAtSettingsHook(e.Pointer(),true);e.EngineConsumesPending();ProcessNativeSizeAtSettingsHook(e.Pointer(),true);
    e.Target({1920,1080});ProcessNativeSizeAtSettingsHook(e.Pointer(),true);
    Check(ReadNativeApplyStatus().stage==NativeApplyStage::Dispatched && e.Pending()==NativeSize{2560,2848},"native target drift retained stale observation");
    e.Current({1920,1080});ProcessNativeSizeAtSettingsHook(e.Pointer(),true);
    Check(ReadNativeApplyStatus().stage==NativeApplyStage::Dispatched && !ReadNativeApplyStatus().observationVisit,"native active drift retained stale observation");
}
static void BadPointerClosesObservation(){
    RequestNativeSize(2560,2848);Check(ProcessNativeSizeAtSettingsHook(0x10000,true),"bad pointer released owner");
    const auto s=ReadNativeApplyStatus();Check(s.stage==NativeApplyStage::Error && s.error==NativeApplyError::ReadFailed && !s.observationVisit,"failed read reported native dimensions");
}
static void PartialWriteCannotBeObserved(){
    SYSTEM_INFO info{};GetSystemInfo(&info);const auto n=info.dwPageSize;
    auto* allocation=static_cast<uint8_t*>(VirtualAlloc(nullptr,size_t(n)*2,MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE));Check(allocation!=nullptr,"fixture alloc failed");
    const auto ptr=reinterpret_cast<uintptr_t>(allocation+n-0x88);
    *reinterpret_cast<uint32_t*>(ptr+0x18)=1920;*reinterpret_cast<uint32_t*>(ptr+0x1c)=1080;
    *reinterpret_cast<uint32_t*>(ptr+0x84)=1920;*reinterpret_cast<uint32_t*>(ptr+0x88)=1080;
    DWORD old{};Check(VirtualProtect(allocation+n,n,PAGE_READONLY,&old)!=0,"fixture protection failed");
    RequestNativeSize(2560,2848);ProcessNativeSizeAtSettingsHook(ptr,true);const auto s=ReadNativeApplyStatus();
    const bool closed=s.stage==NativeApplyStage::Error && s.error==NativeApplyError::PendingWriteFailed && !s.observationVisit &&
        *reinterpret_cast<uint32_t*>(ptr+0x18)==1920 && *reinterpret_cast<uint32_t*>(ptr+0x84)==2560 && *reinterpret_cast<uint32_t*>(ptr+0x88)==1080;
    VirtualFree(allocation,0,MEM_RELEASE);Check(closed,"partial pending store was treated as native acceptance");
}
static void WriterContentionPreservesOwner(){
    EngineSettings e;RequestNativeSize(2560,2848);std::atomic<unsigned> wrote{};
    std::thread legacy([&]{for(int i=0;i<1000;++i)if(WriteAuthorNativeSize(e.Pointer(),2048,2048))++wrote;});
    for(int i=0;i<1000;++i)ProcessNativeSizeAtSettingsHook(e.Pointer(),true);legacy.join();
    Check(wrote==0 && e.Active()==NativeSize{1920,1080} && e.Pending()==NativeSize{2560,2848},"legacy writer crossed active ownership");
}
static void GuardBytesUnchanged(){
    EngineSettings e;const auto before=e.words;RequestNativeSize(2560,2848);ProcessNativeSizeAtSettingsHook(e.Pointer(),true);
    for(size_t i=0;i<e.words.size();++i)if(i!=0x84/4 && i!=0x88/4)Check(e.words[i]==before[i],"native apply wrote outside pending size fields");
}
static void ActualAuthorCallbackAndBackground(){
    EngineSettings e;productionCallSites::OnSettingsResCallback(reinterpret_cast<void*>(e.Pointer()));
    Check(e.Active()==NativeSize{2048,2048} && e.Pending()==e.Active(),"actual author callback no longer applies startup pair");
    e.Current({1920,1080});e.Target({1920,1080});productionCallSites::ApplySettingsResolutionOverride(e.Pointer());
    Check(e.Active()==NativeSize{2048,2048} && e.Pending()==e.Active(),"actual author background call changed");
}
static void ActualNativeCallSites(){
    EngineSettings e;RequestNativeSize(2560,2848);const auto original=e.words;
    productionCallSites::ApplySettingsResolutionOverride(e.Pointer());Check(e.words==original,"actual cached pointer caller wrote before dispatch");
    productionCallSites::OnSettingsResCallback(reinterpret_cast<void*>(e.Pointer()));
    Check(e.Active()==NativeSize{1920,1080} && e.Pending()==NativeSize{2560,2848},"actual hook bypassed pending-only request");
    const auto dispatched=e.words;productionCallSites::ApplySettingsResolutionOverride(e.Pointer());Check(e.words==dispatched,"actual background override erased live request");
    e.EngineConsumesPending();productionCallSites::OnSettingsResCallback(reinterpret_cast<void*>(e.Pointer()));
    Check(ReadNativeApplyStatus().stage==NativeApplyStage::ObservedNative,"actual hook did not observe later engine state");
}
static void ActualWorldMapGuard(){
    EngineSettings e;productionCallSites::shared[81]=1;const auto original=e.words;
    productionCallSites::OnSettingsResCallback(reinterpret_cast<void*>(e.Pointer()));Check(e.words==original,"author map guard lost");
    RequestNativeSize(2560,2848);productionCallSites::OnSettingsResCallback(reinterpret_cast<void*>(e.Pointer()));
    Check(e.words==original && ReadNativeApplyStatus().stage==NativeApplyStage::Requested,"native map guard lost");
    productionCallSites::shared[81]=0;productionCallSites::OnSettingsResCallback(reinterpret_cast<void*>(e.Pointer()));
    Check(e.Pending()==NativeSize{2560,2848} && e.Active()==NativeSize{1920,1080},"actual callback did not resume after map closed");
}
static void ErrorNeedsNewOwnerAction(){
    EngineSettings e;RequestNativeSize(2560,2848);ProcessNativeSizeAtSettingsHook(0x10000,true);
    e.Current({2560,2848});e.Target({2560,2848});const auto before=e.words;
    Check(ProcessNativeSizeAtSettingsHook(e.Pointer(),true) && ReadNativeApplyStatus().stage==NativeApplyStage::Error && e.words==before,"failed dispatch silently acknowledged a later matching state");
    Check(!WriteAuthorNativeSize(e.Pointer(),2048,2048),"error released ownership to legacy writer");
    RequestNativeSize(2560,2848);ProcessNativeSizeAtSettingsHook(e.Pointer(),true);
    Check(ReadNativeApplyStatus().stage==NativeApplyStage::Dispatched,"explicit retry did not reset error");
    ProcessNativeSizeAtSettingsHook(e.Pointer(),true);Check(ReadNativeApplyStatus().stage==NativeApplyStage::ObservedNative,"explicit retry could not be observed later");
}
static void WidthOnlyPreservesSavedEaxSemantics(){
    EngineSettings e;
    // Pinned EXE21C584 loads pending width before the existing hook at21C58A.
    // The trampoline restores that EAX, then compares it with current+18.
    const auto savedEax=e.Pending().width;RequestNativeSize(2560,1080);
    productionCallSites::OnSettingsResCallback(reinterpret_cast<void*>(e.Pointer()));
    const bool changedThisInvocation=savedEax!=e.Active().width || e.Pending().height!=e.Active().height;
    Check(!changedThisInvocation && e.ChangeRequired(),"fixture lost the one-invocation width-only EAX distinction");
    const auto nextEax=e.Pending().width;
    const bool changedNextInvocation=nextEax!=e.Active().width || e.Pending().height!=e.Active().height;
    Check(changedNextInvocation && ReadNativeApplyStatus().stage==NativeApplyStage::Dispatched,"width-only request fabricated observation before engine consumption");
}
static void ActualHookCounterOnly(){
    const auto before=productionCallSites::CyberpunkVR_PpdSettingsHookVisits.load();EngineSettings e;
    RequestNativeSize(2560,2848);ProcessNativeSizeAtSettingsHook(e.Pointer(),true);
    Check(productionCallSites::CyberpunkVR_PpdSettingsHookVisits.load()==before,"helper call fabricated a native hook visit");
    productionCallSites::OnSettingsResCallback(reinterpret_cast<void*>(e.Pointer()));
    Check(productionCallSites::CyberpunkVR_PpdSettingsHookVisits.load()==before+1,"actual callback did not increment unconditional hook counter");
    CancelNativeSize(ReadNativeApplyStatus().revision);
    productionCallSites::OnSettingsResCallback(nullptr);
    Check(productionCallSites::CyberpunkVR_PpdSettingsHookVisits.load()==before+2,"idle/invalid-pointer actual callback was omitted from visits");
}
static void StatusPacketOnlyOnStateChange(){
    EngineSettings e;const auto before=CyberpunkVR_PpdNativeStatusSeq.load();const auto r=RequestNativeSize(2560,2848);
    auto seq=CyberpunkVR_PpdNativeStatusSeq.load();
    Check(seq!=before && !(seq&1) && CyberpunkVR_PpdNativeStatus.version==1 && CyberpunkVR_PpdNativeStatus.bytes==72 &&
        CyberpunkVR_PpdNativeStatus.revision==r && CyberpunkVR_PpdNativeStatus.stage==uint32_t(NativeApplyStage::Requested),"new request not published in seqlock packet");
    ProcessNativeSizeAtSettingsHook(e.Pointer(),false);Check(CyberpunkVR_PpdNativeStatusSeq.load()==seq,"deferred helper visit republished unchanged packet");
    ProcessNativeSizeAtSettingsHook(e.Pointer(),true);ProcessNativeSizeAtSettingsHook(e.Pointer(),true);seq=CyberpunkVR_PpdNativeStatusSeq.load();
    ProcessNativeSizeAtSettingsHook(e.Pointer(),true);Check(CyberpunkVR_PpdNativeStatusSeq.load()==seq,"unchanged size/stage republished per callback");
    e.EngineConsumesPending();ProcessNativeSizeAtSettingsHook(e.Pointer(),true);
    Check(CyberpunkVR_PpdNativeStatusSeq.load()!=seq && !(CyberpunkVR_PpdNativeStatusSeq.load()&1) &&
        CyberpunkVR_PpdNativeStatus.activeWidth==2560 && CyberpunkVR_PpdNativeStatus.activeHeight==2848 &&
        CyberpunkVR_PpdNativeStatus.stage==uint32_t(NativeApplyStage::ObservedNative),"native field observation not published");
}
int main(){unsigned passed=0,failed=0;const std::pair<const char*,void(*)()> tests[]={
    {"author_startup",StartupAuthorFallback},{"request_only",RequestDoesNotTouchEngine},{"pending_then_observed",NativePendingAndLaterObservation},
    {"worker_exclusion",BackgroundCannotOverwritePending},{"map_defer",MapDefersWithoutReleasingOwnership},{"cancel_revision",RevisionCancellationAndRestore},
    {"invalid_request",InvalidRequestPreservesCurrent},{"callback_pointer",CallbackPointerNotCached},{"replacement",ReplacementRejectsOldNativeObservation},
    {"native_drift",NativeDriftWithdrawsObservation},{"read_failure",BadPointerClosesObservation},{"partial_write",PartialWriteCannotBeObserved},
    {"contention",WriterContentionPreservesOwner},{"write_boundary",GuardBytesUnchanged},{"actual_author_calls",ActualAuthorCallbackAndBackground},
    {"actual_native_calls",ActualNativeCallSites},{"actual_map_guard",ActualWorldMapGuard},{"error_explicit_retry",ErrorNeedsNewOwnerAction},
    {"width_only_saved_eax",WidthOnlyPreservesSavedEaxSemantics},{"actual_hook_counter",ActualHookCounterOnly},{"status_packet",StatusPacketOnlyOnStateChange}};
    for(const auto& [name,test]:tests){try{Reset();test();++passed;std::cout<<"PASS "<<name<<'\n';}
        catch(const std::exception& e){++failed;std::cout<<"FAIL "<<name<<": "<<e.what()<<'\n';}}
    Reset();std::cout<<"RESULT "<<passed<<" passed, "<<failed<<" failed\n";return failed?1:0;}
