#include <RED4ext/RED4ext.hpp>
#include "MenuPreview/UiConsumer.hpp"
#include "MenuPreview/PixelReadback.hpp"
#include <ostream>
#include <iomanip>
#include <charconv>
#include <string_view>
#include "MenuPreview/EditorUiSurface.hpp"
#include "Core/LiveControls.hpp"
#include "Render/ResourceAllocation.hpp"
#include "MenuPreview/PipelineEvidence.hpp"
#include "MenuPreview/ProducerRoute.hpp"
#include "MenuPreview/UiConsumerPolicy.hpp"
#include "MenuPreview/PanelDrawWitnessPolicy.hpp"
#include "MenuPreview/UiAliasingPolicy.hpp"
#include "MenuPreview/DescriptorMap.hpp"
#include "MenuPreview/UiDescriptorCopyGate.hpp"
#include "MenuPreview/WidgetAliasObservation.hpp"
#include "MenuPreview/PortraitRotationPolicy.hpp"
#include "MenuPreview/UiQualifiedSourcePolicy.hpp"
#include "Render/StereoSceneState.hpp"
#include "Utils/MemorySafe.hpp"
#include "Utils/StereoLog.hpp"
#include <RED4ext/Scripting/Natives/Generated/DynamicTexture.hpp>
#include <wrl/client.h>
#include <array>
#include <atomic>
#include <mutex>
#include <limits>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <intrin.h>
namespace cvr::detail {const uint8_t* upload_cpu_for_va(uint64_t va,uint64_t need);extern thread_local uintptr_t t_current_node_work;}
namespace cvr::menu_preview {
namespace {
bool PanelWitnessEnabled()noexcept{static const bool enabled=[] {char v[4]{};return GetEnvironmentVariableA("CPVR_EDITOR_PANEL_WITNESS",v,sizeof(v))==1&&v[0]=='1';}();return enabled;}
std::atomic<uint64_t> uiCanvasExtent{};
using Microsoft::WRL::ComPtr;
UiAllocation ReadUiAllocation(ID3D12Resource* resource)noexcept{
 const auto kind=cvr::gpu::ReadResourceAllocationKind(resource);
 const auto region=kind==2?cvr::gpu::ReadResourceHeapRegion(resource):cvr::gpu::ResourceHeapRegion{};
 return {kind,region.heap,region.offset,region.bytes};
}
struct Alias {Metadata scope;RED4ext::Handle<RED4ext::DynamicTexture> resource;uintptr_t identity{},readCPU{};uint32_t readID{};};
struct UiCbvDiagnostic {uint64_t gpu{};uint32_t bytes{};bool known{};bool operator==(const UiCbvDiagnostic&)const=default;};
struct UiDescriptorEntry {UiCbvDiagnostic cbv{};bool cbvOnly{};uintptr_t cpu{};uint64_t version{},originVersion{};uintptr_t originCPU{};bool pending{},overlap{},unorderedAccess{};uint32_t inFlight{},recordingPins{};uintptr_t resource{};uint64_t resourceGeneration{};};
struct ResourceKey {uintptr_t resource{};uint64_t generation{};bool operator==(const ResourceKey&)const=default;};
struct ResourceKeyHash {size_t operator()(const ResourceKey& key)const noexcept{return std::hash<uintptr_t>{}(key.resource)^(std::hash<uint64_t>{}(key.generation)<<1);}};
struct Heap {uintptr_t owner{};uint64_t generation{};uintptr_t cpu{},gpu{};uint32_t count{},increment{};};
struct NativeBind {uintptr_t cpu{},layout{};uint32_t id{},slot{},stage{},mip{},tableIndex{},rootIndex{},relative{};bool valid{};bool operator==(const NativeBind&)const=default;};
void InvalidateNativeIntent(std::array<NativeBind,512>& native)noexcept{if(EditorUiSeparationEnabled()){for(auto& b:native)b.valid=false;}else native={};}
struct RootTable {uintptr_t gpu{},cpu{},layout{};uint32_t count{},tableIndex{};bool proven{};};
enum class WriterReason:uint8_t {None,ObservedDraw,UnknownDraw,AliasingHazard,BarrierCountUnknown};
struct ComputeInputProof {uintptr_t resource{},cpu{};uint64_t generation{},version{};bool descriptorPinned{};};
struct ComputeEvidence {std::array<ComputeInputProof,16> inputs{};uint32_t inputCount{};};
struct Event {uintptr_t nativeNode{},writtenResource{},sourceId{},destinationId{};uint64_t resourceGeneration{},sourceGeneration{},destinationGeneration{};uintptr_t descriptorCPU{};uint64_t descriptorVersion{};WriterReason writerReason{};bool writer{},qualifiedTarget{},fullCopy{},shaderTransfer{},computeTransfer{},descriptorPinned{};ComPtr<ID3D12Resource> resource,target;std::shared_ptr<const CameraApplication> app;std::shared_ptr<ComputeEvidence> compute;};
enum class OverflowReason:uint8_t {None,AddClosed,EventCapacity,RootOutOfRange,CloseFailed,ResourceConflict,DescriptorPinFailed};
struct UiTraceRow {uintptr_t resource{},source{},backBuffer{},pipeline{},root{},descriptorCPU{};uint64_t resourceGeneration{},sourceGeneration{},descriptorVersion{},order{};uint32_t rootIndex{},relative{};bool egress{};};
struct UiRasterState {D3D12_VIEWPORT viewport{};D3D12_RECT scissor{};UINT viewportCount{},scissorCount{};bool viewportKnown{},scissorKnown{};};
struct UiTargetExtent {uint64_t width{};uint32_t height{},format{};bool known{};};
struct UiTransformBindings {std::array<D3D12_GPU_VIRTUAL_ADDRESS,64> cbvs{};uint64_t cbvMask{};D3D12_VERTEX_BUFFER_VIEW vertex0{};bool vertex0Known{};};
struct List {UiTransformBindings transform{};UiRasterState raster{};std::array<UiTargetExtent,4> targetExtents{};uintptr_t identity{},root{},pipeline{};std::array<uintptr_t,4> heaps{};std::array<RootTable,64> tables{};std::array<uintptr_t,4> targets{};std::array<uint64_t,4> targetGenerations{};std::array<NativeBind,512> native{};std::array<Event,128> events{};std::array<UiTraceRow,4> trace{};size_t count{};uint32_t traceCount{};uint64_t traceOrder{},generation{},objectGeneration{};bool closed{},submitted{},overflow{};OverflowReason overflowReason{};uintptr_t overflowDetail0{},overflowDetail1{};};
void RetirePanelWitnessLocked(const List&)noexcept;
 } // anonymous namespace
struct UiDescriptorMutationData:UiOwnedMutationStorage<UiDescriptorEntry>{using UiOwnedMutationStorage<UiDescriptorEntry>::UiOwnedMutationStorage;bool cbvDiagnostic{};};
namespace {

struct Writer {ComPtr<ID3D12Resource> resource;std::shared_ptr<const CameraApplication> app;uintptr_t queue{};uint64_t generation{},submission{};};
struct BackBuffer {ComPtr<ID3D12Resource> resource;std::shared_ptr<const UiFrameReceipt> receipt;uint64_t generation{};};
struct OffscreenCopy {uintptr_t cpu{};uint64_t version{},generation{};};
struct OffscreenSignature {uintptr_t list{},target{},root{},pipeline{};bool operator==(const OffscreenSignature&)const=default;};
struct PipelineProbeRequest {uintptr_t list{},pipeline{},root{},target{},source{},descriptorCPU{},aliasCPU{};uint64_t generation{},targetGeneration{},sourceGeneration{},descriptorVersion{},originVersion{},offset{};uint32_t epoch{},setup{},rootIndex{};bool ready{},bridge{};};
struct TransferDestination {uintptr_t list{},pipeline{},root{},resource{};uint64_t listGeneration{},resourceGeneration{};std::array<uintptr_t,4> targets{};std::array<uint64_t,4> generations{};uint32_t epoch{},setup{},backBufferMask{};bool backBuffer{},ready{};};
struct UiTransformWitness {PipelineProbeRequest probe{};Metadata scope{};UiTransformBindings bindings{};std::array<RootTable,64> tables{};std::array<uintptr_t,4> heaps{};bool captured{};};
struct UiConstantProof {uint64_t gpu{},tableGPU{},version{},heapGeneration{};uintptr_t cpu{},heap{};uint32_t bytes{};bool bound{},payloadKnown{},descriptorPresent{},pending{};UiCbvDiagnostic descriptor{};bool operator==(const UiConstantProof&)const=default;};
struct TransferLocations {std::array<ShaderSrvLocation,16> rows{};PipelineShaderEvidence shader{};uint32_t count{};bool shaderKnown{},known{};};
struct ComputeLocations {std::array<ComputeLocation,24> rows{};ComputeShaderEvidence shader{};uint32_t count{};bool shaderKnown{},known{};};
struct ComputeListProbe {uintptr_t list{},pipeline{},root{};uint64_t generation{};uint32_t epoch{},setup{};bool ready{};};
struct FinalDecisionKey {uintptr_t pipeline{},root{};};
struct FinalDecisionLocation {uint32_t rootIndex{},offset{},tableCount{};uintptr_t tableGPU{},tableCPU{},descriptorCPU{},resource{};uint64_t generation{},version{};bool tableKnown{},tableProven{},descriptorPending{},tracked{};};
struct TrackedHandle {uintptr_t cpu{};uint64_t version{};};
struct TrackedTarget {Metadata editorScope{},sourceScope{};uintptr_t editorList{},editorNode{};uint64_t editorLife{},editorRecording{},editorSubmission{};bool editorUiRequired{};std::shared_ptr<const EditorUiPublication> editorUi;uintptr_t resource{},source{},queue{},qualifiedQueue{};uint64_t generation{},sourceGeneration{},submission{},sourceSequence{};uint32_t eye{},nativeFrame{},handleCursor{};std::array<TrackedHandle,32> handles{};std::shared_ptr<const CameraApplication> qualifiedApp;uint64_t qualifiedSubmission{};};
struct OverflowSample {uintptr_t list{};uint64_t generation{};};
struct OffscreenPreflight {uint64_t directSeen{},missingList{},overflowSkipped{},entered{},noAlias{},staleAlias{},notReady{},noTargets{},onlyBackbufferOrPortrait{},noRoot{},noPipeline{},noBoundHeap{},reachedScan{},skippedCadence{},armedAtMs{};std::array<OverflowSample,2> overflowSamples{};uint32_t overflowRows{},summaries{};bool enteredLogged{},armedLogged{};};
struct NativeDiagnostic {uint32_t rasterRows{},transformRows{},transformKnownStates{};uint64_t startCalls{},drawProbeCalls{},offscreenCalls{},targetSrvWrites{},targetCopies{},egressDraws{},egressCurrent{},egressHeapBound{},egressMatches{},targetGenerationMiss{},targetSubmissions{},egressSubmissions{},traceDrops{},qualifiedDraws{},qualifiedSubmissions{},copyAdmissions{},shaderTransfers{},sourceRebinds{};uintptr_t bestTableGPU{};uint32_t epoch{},setup{},examined{},idSeen{},cpuEqual{},valid{},recordListKnown{},aliasChanges{},rows{},summaryRows{},drawRows{},sourceRows{},srvRows{},copyRows{},drawResourceRows{},drawMeaningfulRows{},drawProbeAttempts{},lineageRevision{},lastScannedRevision{},bestDrawRelation{},bestRootIndex{},heapBindRows{},rootCallRows{},offscreenAttempts{},offscreenRows{},offscreenCursor{},offscreenRevision{},offscreenLastRevision{},targetCount{},targetRows{},egressRows{},egressSubmitRows{},pipelineRows{},qualifiedRows{},bridgeCopyRows{},shaderTransferRows{},finalDecisionCount{},computeRows{},writerDecisionRows{};std::array<OffscreenCopy,32> offscreenCopies{};std::array<OffscreenSignature,12> offscreenSignatures{};std::array<TrackedTarget,16> targets{};std::array<FinalDecisionKey,4> finalDecisions{};OffscreenPreflight offscreenPreflight;uint8_t sourceWriterMask{};bool scopeKnown{},writerDescriptorKnown{},bestHeapBound{},offscreenReady{};};
inline constexpr size_t PortraitSlotCount=8,LegacyPortraitSlots=2;
struct State {std::mutex mutex;std::array<UiDescriptorEntry,2048> descriptors{};DescriptorMap<size_t,16> descriptorIndex;DescriptorMap<UiDescriptorEntry,20> persistentSources;std::unordered_map<ResourceKey,std::unordered_set<uintptr_t>,ResourceKeyHash> sourcesByResource;bool descriptorIndexReady{};size_t cursor{};std::array<Heap,32> heaps{};UiLiveListStore<List> lists;std::array<Writer,PortraitSlotCount> writers{};std::array<ComPtr<ID3D12Resource>,PortraitSlotCount> portraitPins{};std::array<uint64_t,PortraitSlotCount> portraitGenerations{},portraitLastUse{};std::array<UiAllocation,PortraitSlotCount> portraitAllocations{};uint64_t portraitUseSequence{};uint32_t portraitRotationRows{};NativePreviewMode portraitMode=NativePreviewMode::LegacyOneView;bool portraitModeKnown{};std::array<BackBuffer,8> buffers{};std::shared_ptr<const Alias> alias;NativeDiagnostic nativeDiagnostic;uint64_t serial{},submission{};std::atomic<bool> failed{},aliasKnown{};std::atomic<uint32_t> aliasReadID{};uint32_t nativeRows{},consumerRows{};std::array<uint32_t,4> matchedNativeRows{};std::atomic<uint64_t> nativeCalls{};};
State& S(){static auto* p=new State;return *p;}
struct RetiredPortraits {std::array<Writer,PortraitSlotCount> writers;std::array<ComPtr<ID3D12Resource>,PortraitSlotCount> pins;};
void RetirePortraitsLocked(RetiredPortraits& retired){
 retired.writers=std::move(S().writers);retired.pins=std::move(S().portraitPins);
 S().writers={};S().portraitPins={};S().portraitGenerations={};S().portraitAllocations={};S().portraitLastUse={};
 S().portraitUseSequence=0;S().portraitModeKnown=false;
}
void RetirePortraitSlotLocked(size_t slot,RetiredPortraits& retired){
 retired.writers[slot]=std::move(S().writers[slot]);retired.pins[slot]=std::move(S().portraitPins[slot]);
 S().writers[slot]={};S().portraitPins[slot].Reset();S().portraitGenerations[slot]=0;S().portraitAllocations[slot]={};S().portraitLastUse[slot]=0;
}
bool OffscreenTraceEnabled()noexcept{static const bool enabled=[] {char v[4]{};return GetEnvironmentVariableA("CPVR_MENU_UI_OFFSCREEN_TRACE",v,sizeof(v))==1&&v[0]=='1';}();return enabled;}
void Fail(const char* reason)noexcept{bool expected=false;if(S().failed.compare_exchange_strong(expected,true)){try{Log("[MenuPreviewUI] observer-failed reason=%s; failclosed for process\n",reason);}catch(...){}}}
void AliasDiagnostic(const char* reason,const Metadata& scope,bool hasRef=false,uint32_t readID=0,uintptr_t cpu=0)noexcept{static std::atomic<uint32_t> rows{};if(rows.fetch_add(1)<12)try{Log("[MenuPreviewUI] owned-alias reason=%s epoch=%u setup=%u sdkRef=%u readID=%u srvKnown=%u nativeCalls=%llu observerFailed=%u\n",reason,scope.screenEpoch,scope.setupID,unsigned(hasRef),readID,unsigned(cpu!=0),S().nativeCalls.load(),unsigned(S().failed.load()));}catch(...){}}
enum class ObjectKind:uint8_t {Resource,Heap,List};
struct Retire {uintptr_t object{};uint64_t generation{};ObjectKind kind{};};
void UnpinListLocked(List&)noexcept;
// The COM-private-data callback outlives CRT global destruction. Its fallback
// queue must share S()'s process lifetime as well as its immediate path.
std::mutex& retireMutex=*new std::mutex;std::vector<Retire>& retires=*new std::vector<Retire>;std::atomic<uint64_t> objectSerial{};
void DropPersistentSourceLocked(uintptr_t cpu){auto& s=S();const auto it=s.persistentSources.find(cpu);if(it==s.persistentSources.end())return;const ResourceKey key{it->second.resource,it->second.resourceGeneration};if(auto group=s.sourcesByResource.find(key);group!=s.sourcesByResource.end()){group->second.erase(cpu);if(group->second.empty())s.sourcesByResource.erase(group);}s.persistentSources.erase(it);}
void PutPersistentSourceLocked(const UiDescriptorEntry& entry){if(!entry.cpu||!entry.resource||!entry.resourceGeneration||entry.pending)return;auto& s=S();DropPersistentSourceLocked(entry.cpu);auto source=entry;source.recordingPins=0;s.persistentSources.emplace(entry.cpu,source);s.sourcesByResource[{entry.resource,entry.resourceGeneration}].insert(entry.cpu);}
void ForgetUiCbvHeapLocked(const Heap& heap){
 if(!UiRasterDiagnosticsEnabled()||!heap.increment)return;
 for(auto& d:S().descriptors)if(d.cbv.known&&d.cpu>=heap.cpu&&(d.cpu-heap.cpu)%heap.increment==0&&(d.cpu-heap.cpu)/heap.increment<heap.count){d.cbv={};d.version=++S().serial;}
}
void Forget(Retire r){if(r.kind==ObjectKind::List){if(auto* list=S().lists.Find(r.object);list&&list->objectGeneration==r.generation){RetirePanelWitnessLocked(*list);UnpinListLocked(*list);S().lists.Retire(r.object,r.generation);cvr::stereo::scene_state::Retire(reinterpret_cast<ID3D12GraphicsCommandList*>(r.object));}return;}if(r.kind==ObjectKind::Heap){for(auto& h:S().heaps)if(h.owner==r.object&&h.generation==r.generation){ForgetUiCbvHeapLocked(h);h={};}}else{for(auto& d:S().descriptors)if(d.resource==r.object&&d.resourceGeneration==r.generation){d.resource=0;d.resourceGeneration=0;d.unorderedAccess=false;++d.version;}const ResourceKey key{r.object,r.generation};if(auto group=S().sourcesByResource.find(key);group!=S().sourcesByResource.end()){for(auto cpu:group->second)S().persistentSources.erase(cpu);S().sourcesByResource.erase(group);}for(auto& target:S().nativeDiagnostic.targets)if(target.resource==r.object&&target.generation==r.generation)target={};}}
void Drain(){std::vector<Retire> pending;{std::lock_guard lock(retireMutex);pending.swap(retires);}for(auto r:pending)Forget(r);}
void RetireObject(Retire r)noexcept{try{std::unique_lock lock(S().mutex,std::try_to_lock);if(lock.owns_lock())Forget(r);else{std::lock_guard pending(retireMutex);retires.push_back(r);}}catch(...){Fail("retire-exception");}}
const GUID lifetimeKey={0x81df78b3,0x937a,0x4b49,{0x84,0x55,0x35,0xd6,0x3c,0xd1,0x41,0x28}};
class Lifetime final:public IUnknown{std::atomic<ULONG> refs{1};public:const Retire identity;explicit Lifetime(Retire r):identity(r){}
 HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid,void** out)override{if(!out)return E_POINTER;*out=nullptr;if(iid!=__uuidof(IUnknown))return E_NOINTERFACE;*out=this;AddRef();return S_OK;}
 ULONG STDMETHODCALLTYPE AddRef()override{return ++refs;}ULONG STDMETHODCALLTYPE Release()override{auto n=--refs;if(!n){RetireObject(identity);delete this;}return n;}};
uint64_t TrackObject(ID3D12Object* object,ObjectKind kind){if(!object)return 0;static std::mutex lifecycleMutex;std::lock_guard lifecycle(lifecycleMutex);IUnknown* prior{};UINT size=sizeof(prior);if(SUCCEEDED(object->GetPrivateData(lifetimeKey,&size,&prior))&&prior){auto identity=static_cast<Lifetime*>(prior)->identity;prior->Release();return identity.kind==kind?identity.generation:0;}const auto generation=objectSerial.fetch_add(1)+1;auto* p=new Lifetime({reinterpret_cast<uintptr_t>(object),generation,kind});auto hr=object->SetPrivateDataInterface(lifetimeKey,p);p->Release();return SUCCEEDED(hr)?generation:0;}
bool Active(){Lease lease;return S().aliasKnown.load(std::memory_order_acquire)&&TryAcquireCurrent(lease);}
template<class F> void Safe(F&& fn)noexcept{if(!UiConsumerEnabled()||S().failed.load())return;try{fn();}catch(...){Fail("observation-exception");}}
uint64_t TrackList(ID3D12GraphicsCommandList* list){return list&&Active()?TrackObject(list,ObjectKind::List):0;}
List* GetList(uintptr_t id,uint64_t life=0){if(life){if(auto* prior=S().lists.Find(id);prior&&prior->objectGeneration!=life)UnpinListLocked(*prior);return S().lists.Ensure(id,life);}return S().lists.Find(id);}
List* FindList(uintptr_t id){return S().lists.Find(id);}

UiDescriptorEntry* GetDescriptor(uintptr_t cpu,bool create=false){
    auto& state=S();
    // Preserve the old cpu==0 lookup, including its first empty-slot result.
    if(!cpu){for(auto& d:state.descriptors)if(!d.cpu)return &d;return nullptr;}
    if(!state.descriptorIndexReady){state.descriptorIndex.reserve(state.descriptors.size());state.descriptorIndexReady=true;}
    if(const auto it=state.descriptorIndex.find(cpu);it!=state.descriptorIndex.end()){
        if(it->second>=state.descriptors.size()){Fail("descriptor-index-out-of-range");return nullptr;}
        auto& d=state.descriptors[it->second];if(d.cpu!=cpu){Fail("descriptor-index-stale");return nullptr;}return &d;
    }
    if(!create){const auto source=state.persistentSources.find(cpu);return source==state.persistentSources.end()?nullptr:&source->second;}
    for(size_t n=0;n<state.descriptors.size();++n){const auto slot=state.cursor++%state.descriptors.size();auto& d=state.descriptors[slot];
        if(d.pending||d.recordingPins||(state.alias&&d.cpu==state.alias->readCPU))continue;
        if(d.cpu){auto node=state.descriptorIndex.extract(d.cpu);if(node.empty()){Fail("descriptor-index-missing-victim");return nullptr;}node.key()=cpu;node.mapped()=slot;if(!state.descriptorIndex.insert(std::move(node)).inserted){Fail("descriptor-index-duplicate");return nullptr;}}
        else{const auto [it,inserted]=state.descriptorIndex.emplace(cpu,slot);if(!inserted){Fail("descriptor-index-duplicate");return nullptr;}}
        d={};d.cpu=cpu;return &d;
    }
    return nullptr;
}
UiDescriptorEntry* GetUiCbvDescriptorLocked(uintptr_t cpu){
 if(!cpu)return nullptr;auto& state=S();
 if(const auto it=state.descriptorIndex.find(cpu);it!=state.descriptorIndex.end()){
  if(it->second>=state.descriptors.size()||state.descriptors[it->second].cpu!=cpu)return nullptr;
  return &state.descriptors[it->second];
 }
 // Best effort: never evict SRV/unknown ordinary entries, and never pin all free
 // cells with diagnostics. Ordinary tracking may still recycle these CBV cells.
 size_t diagnosticCount{};UiDescriptorEntry* empty=nullptr;UiDescriptorEntry* reusable=nullptr;size_t slot{};
 for(size_t i=0;i<state.descriptors.size();++i){auto& d=state.descriptors[i];diagnosticCount+=unsigned(d.cbvOnly);
  if(d.pending||d.recordingPins||(state.alias&&d.cpu==state.alias->readCPU))continue;
  if(d.cbvOnly&&(!reusable||d.version<reusable->version))reusable=&d;
  if(!d.cpu&&!empty)empty=&d;
 }
 auto* chosen=diagnosticCount<64&&empty?empty:reusable;if(!chosen)return nullptr;
 slot=size_t(chosen-state.descriptors.data());
 const auto inserted=state.descriptorIndex.emplace(cpu,slot);if(!inserted.second)return nullptr;
 if(chosen->cpu)state.descriptorIndex.erase(chosen->cpu);
 *chosen={};chosen->cpu=cpu;chosen->cbvOnly=true;return chosen;
}
bool PinDescriptorReferenceLocked(uintptr_t cpu,uint64_t version,bool& pinned)noexcept{if(!cpu)return false;const auto it=S().descriptorIndex.find(cpu);
 if(it==S().descriptorIndex.end()){const auto source=S().persistentSources.find(cpu);return source!=S().persistentSources.end()&&!source->second.pending&&source->second.version==version;}
 if(it->second>=S().descriptors.size())return false;auto& descriptor=S().descriptors[it->second];if(descriptor.cpu!=cpu||descriptor.pending||descriptor.version!=version)return false;++descriptor.recordingPins;pinned=true;return true;
}
void UnpinDescriptorReferenceLocked(uintptr_t cpu,bool& pinned)noexcept{if(!pinned)return;pinned=false;const auto it=S().descriptorIndex.find(cpu);if(it==S().descriptorIndex.end()||it->second>=S().descriptors.size()){Fail("pinned-descriptor-missing");return;}auto& descriptor=S().descriptors[it->second];if(descriptor.cpu!=cpu||!descriptor.recordingPins){Fail("pinned-descriptor-inconsistent");return;}--descriptor.recordingPins;}
bool PinRecordedEventLocked(List& list)noexcept{if(!list.count)return false;auto& event=list.events[list.count-1];if(event.descriptorCPU&&!PinDescriptorReferenceLocked(event.descriptorCPU,event.descriptorVersion,event.descriptorPinned))return false;
 if(event.compute)for(uint32_t i=0;i<event.compute->inputCount;++i){auto& input=event.compute->inputs[i];if(PinDescriptorReferenceLocked(input.cpu,input.version,input.descriptorPinned))continue;
   UnpinDescriptorReferenceLocked(event.descriptorCPU,event.descriptorPinned);for(uint32_t n=0;n<i;++n)UnpinDescriptorReferenceLocked(event.compute->inputs[n].cpu,event.compute->inputs[n].descriptorPinned);return false;}
 return true;
}
void UnpinListLocked(List& list)noexcept{for(size_t i=0;i<list.count;++i){auto& event=list.events[i];UnpinDescriptorReferenceLocked(event.descriptorCPU,event.descriptorPinned);if(event.compute)for(uint32_t n=0;n<event.compute->inputCount;++n)UnpinDescriptorReferenceLocked(event.compute->inputs[n].cpu,event.compute->inputs[n].descriptorPinned);}}
BackBuffer* Buffer(uintptr_t p){for(auto& b:S().buffers)if(reinterpret_cast<uintptr_t>(b.resource.Get())==p)return &b;return nullptr;}
bool SameUiTransformProbe(const PipelineProbeRequest& a,const PipelineProbeRequest& b){
 return a.bridge&&b.bridge&&a.list==b.list&&a.generation==b.generation&&a.target==b.target&&a.targetGeneration==b.targetGeneration&&
  a.source==b.source&&a.sourceGeneration==b.sourceGeneration&&a.descriptorCPU==b.descriptorCPU&&a.descriptorVersion==b.descriptorVersion&&
  a.aliasCPU==b.aliasCPU&&a.originVersion==b.originVersion&&a.pipeline==b.pipeline&&a.root==b.root&&a.epoch==b.epoch&&a.setup==b.setup;
}
bool SameUiTransformBindings(const UiTransformBindings& a,const UiTransformBindings& b){
 return a.cbvs==b.cbvs&&a.cbvMask==b.cbvMask&&a.vertex0Known==b.vertex0Known&&a.vertex0.BufferLocation==b.vertex0.BufferLocation&&
  a.vertex0.SizeInBytes==b.vertex0.SizeInBytes&&a.vertex0.StrideInBytes==b.vertex0.StrideInBytes;
}
bool SameUiTransformTables(const List& list,const UiTransformWitness& witness){
 if(list.heaps!=witness.heaps)return false;
 for(size_t i=0;i<list.tables.size();++i){const auto& a=list.tables[i];const auto& b=witness.tables[i];
  if(a.gpu!=b.gpu||a.cpu!=b.cpu||a.layout!=b.layout||a.count!=b.count||a.tableIndex!=b.tableIndex||a.proven!=b.proven)return false;}
 return true;
}
UiConstantProof CaptureUiConstantLocked(const List& list,const VertexConstantLocation& location,bool resolved,uint32_t requiredBytes){
 UiConstantProof proof;if(!resolved||location.rootIndex>=list.tables.size())return proof;
 if(!location.table){proof.bound=(list.transform.cbvMask&(uint64_t{1}<<location.rootIndex))!=0;proof.gpu=proof.bound?list.transform.cbvs[location.rootIndex]:0;proof.payloadKnown=proof.gpu!=0;return proof;}
 proof.tableGPU=list.tables[location.rootIndex].gpu;if(!proof.tableGPU)return proof;
 for(const auto& heap:S().heaps){
  if(!heap.owner||!heap.generation||!heap.gpu||!heap.cpu||!heap.increment||!heap.count||proof.tableGPU<heap.gpu)continue;
  bool bound=false;for(auto id:list.heaps)bound|=id==heap.owner;if(!bound)continue;
  const uint64_t distance=proof.tableGPU-heap.gpu;if(distance%heap.increment)continue;
  const uint64_t first=distance/heap.increment;if(first>=heap.count||location.descriptorOffset>=heap.count-first)continue;
  const uint64_t delta=distance+location.descriptorOffset*heap.increment;if(heap.cpu>UINTPTR_MAX-delta)return {};
  if(proof.bound)return {}; // Overlapping registered heaps are ambiguous.
  proof.bound=true;proof.heap=heap.owner;proof.heapGeneration=heap.generation;proof.cpu=heap.cpu+uintptr_t(delta);
 }
 if(!proof.bound)return proof;
 if(const auto* d=GetDescriptor(proof.cpu)){proof.descriptorPresent=true;proof.version=d->version;proof.pending=d->pending;proof.descriptor=d->cbv;
  proof.payloadKnown=!d->pending&&!d->resource&&!d->unorderedAccess&&d->cbv.known&&d->cbv.bytes>=requiredBytes;
  if(proof.payloadKnown){proof.gpu=d->cbv.gpu;proof.bytes=d->cbv.bytes;}}
 return proof;
}
// This opt-in observer owns no GPU resources, descriptors, pixel events or
// admission flags. All stored bindings are values captured in the Draw hook;
// UiSubmitted must never look them up again in the live descriptor ledger.
struct PanelSrvSnapshot {
 uint32_t root{},offset{},shaderRegister{},space{},nativeID{},nativeSlot{},nativeStage{};
 uintptr_t tableGPU{},cpu{},resource{};uint64_t version{},generation{};
 bool tableProven{},descriptorPresent{},pending{},unordered{},nativeKnown{},nativeAmbiguous{},registerKnown{},trackedIntermediate{};
 bool operator==(const PanelSrvSnapshot&)const=default;
};
struct PanelBindingSnapshot {
 uintptr_t pipeline{},root{};uint64_t ps{},vs{};uint32_t psBytes{},vsBytes{};
 std::array<uintptr_t,4> targets{};std::array<uint64_t,4> generations{};
 std::array<UiTargetExtent,4> extents{};
 std::array<PanelSrvSnapshot,8> srvs{};uint32_t srvCount{},srvTotal{},srvDrops{},bankDrops{};
 std::array<UiConstantProof,2> constants{};
 std::array<uint32_t,2> constantRoots{UINT32_MAX,UINT32_MAX};
 std::array<uint64_t,2> constantOffsets{};std::array<bool,2> constantResolved{},constantTable{};
 uint32_t backbufferMask{},portraitMask{},trackedMask{};bool readsPortrait{},portraitReadKnown{},shaderKnown{},srvsKnown{};
 bool operator==(const PanelBindingSnapshot& b)const noexcept {
  return pipeline==b.pipeline&&root==b.root&&ps==b.ps&&vs==b.vs&&psBytes==b.psBytes&&vsBytes==b.vsBytes&&
   targets==b.targets&&generations==b.generations&&srvs==b.srvs&&srvCount==b.srvCount&&srvTotal==b.srvTotal&&srvDrops==b.srvDrops&&bankDrops==b.bankDrops&&
   constants==b.constants&&constantRoots==b.constantRoots&&constantOffsets==b.constantOffsets&&constantResolved==b.constantResolved&&constantTable==b.constantTable&&
   backbufferMask==b.backbufferMask&&portraitMask==b.portraitMask&&trackedMask==b.trackedMask&&readsPortrait==b.readsPortrait&&portraitReadKnown==b.portraitReadKnown&&shaderKnown==b.shaderKnown&&srvsKnown==b.srvsKnown;
 }
};
struct PanelDescriptorAtDraw {PanelSrvSnapshot srv;UiConstantProof constant;};
struct PanelDescriptorBank {
 std::array<PanelDescriptorAtDraw,32> entries{};uint32_t count{},dropped{};
 const PanelDescriptorAtDraw* Find(uint32_t root,uint32_t offset)const noexcept{for(uint32_t i=0;i<count;++i)if(entries[i].srv.root==root&&entries[i].srv.offset==offset)return &entries[i];return nullptr;}
};
// Initial callback snapshot, before any out-of-lock cache query. Missing bank
// entries stay unknown afterwards; never resolve them using newer descriptors.
const UiDescriptorEntry* FindDescriptorDiagnostic(uintptr_t cpu)noexcept{
 if(!cpu)return nullptr;auto& state=S();const auto i=state.descriptorIndex.find(cpu);if(i!=state.descriptorIndex.end())return i->second<state.descriptors.size()&&state.descriptors[i->second].cpu==cpu?&state.descriptors[i->second]:nullptr;
 const auto p=state.persistentSources.find(cpu);return p==state.persistentSources.end()?nullptr:&p->second;
}

enum class ProducerDescriptorReason:uint8_t {Unknown,TableUnproven,OutOfRange,BankFull,NoUniqueHeapMatch,CapturedAbsent,CapturedPresent};
const char* ProducerDescriptorReasonName(ProducerDescriptorReason r)noexcept{switch(r){case ProducerDescriptorReason::TableUnproven:return "table_unproven";case ProducerDescriptorReason::OutOfRange:return "out_of_range";case ProducerDescriptorReason::BankFull:return "bank_full";case ProducerDescriptorReason::NoUniqueHeapMatch:return "no_unique_heap_match";case ProducerDescriptorReason::CapturedAbsent:return "captured_descriptor_absent";case ProducerDescriptorReason::CapturedPresent:return "captured_descriptor_present";default:return "UNKNOWN";}}
struct ProducerSrvRequest {uint32_t root{},offset{};ProducerDescriptorReason reason{};};
struct ProducerSrvRequests {std::array<ProducerSrvRequest,8> rows{};uint32_t count{};uint64_t ps{},vs{};bool present{},valid{},expectedKnown{};};
ProducerSrvRequests ParseProducerSrvRequests(std::string_view slots,std::string_view ps,std::string_view vs)noexcept{
 ProducerSrvRequests r;r.present=!slots.empty();if(!r.present)return r;
 auto hash=[](std::string_view s,uint64_t& out){if(s.starts_with("0x")||s.starts_with("0X"))s.remove_prefix(2);if(s.size()!=16)return false;auto result=std::from_chars(s.data(),s.data()+s.size(),out,16);return result.ec==std::errc{}&&result.ptr==s.data()+s.size()&&out;};
 r.expectedKnown=hash(ps,r.ps)&&hash(vs,r.vs);
 while(!slots.empty()){
  if(r.count==r.rows.size())return r;const auto comma=slots.find(',');const auto row=slots.substr(0,comma);const auto colon=row.find(':');if(colon==std::string_view::npos)return r;
  ProducerSrvRequest next;const auto root=row.substr(0,colon),off=row.substr(colon+1);if(root.empty()||off.empty())return r;
  auto a=std::from_chars(root.data(),root.data()+root.size(),next.root);auto b=std::from_chars(off.data(),off.data()+off.size(),next.offset);
  if(a.ec!=std::errc{}||a.ptr!=root.data()+root.size()||b.ec!=std::errc{}||b.ptr!=off.data()+off.size()||next.root>=64)return r;
  for(uint32_t i=0;i<r.count;++i)if(r.rows[i].root==next.root&&r.rows[i].offset==next.offset)return r;r.rows[r.count++]=next;
  if(comma==std::string_view::npos){r.valid=true;return r;}slots.remove_prefix(comma+1);if(slots.empty())return r;
 }
 return r;
}
const ProducerSrvRequests& RequestedProducerSrvs()noexcept{static const auto request=[]()noexcept{char slots[192]{},ps[32]{},vs[32]{};const auto n=GetEnvironmentVariableA("CPVR_MENU_PRODUCER_SRV_REQUESTS",slots,sizeof(slots));if(!n)return ProducerSrvRequests{};if(n>=sizeof(slots)){ProducerSrvRequests r;r.present=true;return r;}const auto p=GetEnvironmentVariableA("CPVR_MENU_UI_PS_HASH",ps,sizeof(ps)),v=GetEnvironmentVariableA("CPVR_MENU_UI_VS_HASH",vs,sizeof(vs));return ParseProducerSrvRequests({slots,n},p&&p<sizeof(ps)?std::string_view(ps,p):std::string_view{},v&&v<sizeof(vs)?std::string_view(vs,v):std::string_view{});}();return request;}
void CapturePanelDescriptorLocked(const List& list,uint32_t root,uint32_t offset,PanelDescriptorBank& bank,const NativeBind* native=nullptr,bool diagnosticOnly=false,ProducerDescriptorReason* reason=nullptr){
 if(reason)*reason=ProducerDescriptorReason::Unknown;
 if(root>=list.tables.size()){if(reason)*reason=ProducerDescriptorReason::OutOfRange;return;}const auto& table=list.tables[root];if(!table.proven||!table.cpu){if(reason)*reason=ProducerDescriptorReason::TableUnproven;return;}if(offset>=table.count){if(reason)*reason=ProducerDescriptorReason::OutOfRange;return;}
 PanelDescriptorAtDraw* entry=nullptr;for(uint32_t i=0;i<bank.count;++i)if(bank.entries[i].srv.root==root&&bank.entries[i].srv.offset==offset){entry=&bank.entries[i];break;}
 if(!entry){
  if(bank.count==bank.entries.size()){++bank.dropped;if(reason)*reason=ProducerDescriptorReason::BankFull;return;}
  const Heap* matched=nullptr;for(const auto& heap:S().heaps){if(!heap.owner||!heap.generation||!heap.gpu||!heap.cpu||!heap.increment||table.gpu<heap.gpu)continue;bool bound=false;for(auto id:list.heaps)bound|=id==heap.owner;if(!bound)continue;
   const auto delta=table.gpu-heap.gpu;if(delta%heap.increment||delta/heap.increment>=heap.count||table.count>heap.count-delta/heap.increment||table.cpu!=heap.cpu+delta)continue;
   if(matched){if(reason)*reason=ProducerDescriptorReason::NoUniqueHeapMatch;return;}matched=&heap;}
  if(!matched){if(reason)*reason=ProducerDescriptorReason::NoUniqueHeapMatch;return;}
  entry=&bank.entries[bank.count++];auto& s=entry->srv;auto& c=entry->constant;s.root=root;s.offset=offset;s.tableGPU=table.gpu;s.tableProven=true;s.cpu=table.cpu+uintptr_t(offset)*matched->increment;
  c.tableGPU=table.gpu;c.bound=true;c.heap=matched->owner;c.heapGeneration=matched->generation;c.cpu=s.cpu;
  if(const auto* d=diagnosticOnly?FindDescriptorDiagnostic(s.cpu):GetDescriptor(s.cpu)){s.descriptorPresent=true;s.version=d->version;s.resource=d->resource;s.generation=d->resourceGeneration;s.pending=d->pending;s.unordered=d->unorderedAccess;
   c.descriptorPresent=true;c.version=d->version;c.pending=d->pending;c.descriptor=d->cbv;c.payloadKnown=!d->pending&&!d->resource&&!d->unorderedAccess&&d->cbv.known;
   if(c.payloadKnown){c.gpu=d->cbv.gpu;c.bytes=d->cbv.bytes;}}
  for(uint32_t i=0;i<S().nativeDiagnostic.targetCount;++i){const auto& t=S().nativeDiagnostic.targets[i];if(t.resource==s.resource&&t.generation==s.generation)s.trackedIntermediate=true;}
 }
 if(native&&native->cpu==entry->srv.cpu){auto& s=entry->srv;
  if(s.nativeKnown&&(s.nativeID!=native->id||s.nativeSlot!=native->slot||s.nativeStage!=native->stage))s.nativeAmbiguous=true;
  if(!s.nativeAmbiguous){s.nativeKnown=true;s.nativeID=native->id;s.nativeSlot=native->slot;s.nativeStage=native->stage;}else s.nativeKnown=false;
 }
 if(reason)*reason=entry->srv.descriptorPresent?ProducerDescriptorReason::CapturedPresent:ProducerDescriptorReason::CapturedAbsent;
}
struct PanelDrawRecord {
 uint64_t ordinal{};uint32_t bindingIndex{},elements{},instances{},firstVertex{},firstIndex{},firstInstance{};
 int32_t baseVertex{};bool indexed{},nativeListKnown{};uintptr_t nativeNode{};
 UiRasterState raster{};D3D12_VERTEX_BUFFER_VIEW vertex{};bool vertexKnown{};
};
using PanelPool=PanelDrawWitnessPool<PanelDrawRecord,PanelBindingSnapshot>;
static_assert(sizeof(PanelPool::Batch)<=64*1024,"Panel witness batch exceeds its fixed resource budget");
static_assert(sizeof(PanelPool)<=128*1024,"Panel witness pool must remain bounded independently of List count");
struct PanelWitnessState {PanelPool pool;PanelWitnessScope armed{};std::atomic<uint64_t> errors{};};
PanelWitnessState& Panels(){static PanelWitnessState value;return value;}
PanelWitnessScope PanelScope(const Metadata& m){return {m.screenEpoch,m.setupID,m.controllerIdentity,m.componentIdentity};}
PanelWitnessListKey PanelKey(const List& list){return {list.identity,list.objectGeneration,list.generation};}
uintptr_t NativeList();
bool Current(const Metadata&);
void RetirePanelWitnessLocked(const List& list)noexcept{if(PanelWitnessEnabled())Panels().pool.Retire(PanelKey(list));}
void CapturePanelDrawWitness(ID3D12GraphicsCommandList* list,UINT elements,UINT instances,UINT firstVertex,bool indexed,UINT firstIndex,INT baseVertex,UINT firstInstance)noexcept {
 if(!PanelWitnessEnabled())return;
 try {
  if(!UiConsumerEnabled()||!elements||!instances)return;
  Lease live;if(!TryAcquireCurrent(live)||live.metadata.screen!=2)return;
  const auto native=NativeList();if(native&&native!=reinterpret_cast<uintptr_t>(list))return;
  UiTransformWitness witness;PanelBindingSnapshot binding;PanelDrawRecord record;PanelDescriptorBank bank;UiDescriptorEntry aliasAtDraw{};bool aliasPresent{};
  PanelWitnessListKey key;uintptr_t aliasCPU{};
  {std::lock_guard lock(S().mutex);auto* current=FindList(reinterpret_cast<uintptr_t>(list));
   if(!Panels().pool.Accepting(PanelScope(live.metadata)))return;
   if(!current||current->closed||current->overflow||!S().alias||!SamePreview(S().alias->scope,live.metadata)){++Panels().errors;return;}
   key=PanelKey(*current);aliasCPU=S().alias->readCPU;witness.scope=live.metadata;
   witness.bindings=current->transform;witness.tables=current->tables;witness.heaps=current->heaps;
   binding.pipeline=current->pipeline;binding.root=current->root;binding.targets=current->targets;binding.generations=current->targetGenerations;binding.extents=current->targetExtents;
   record.raster=current->raster;record.vertex=current->transform.vertex0;record.vertexKnown=current->transform.vertex0Known;
   if(const auto* alias=GetDescriptor(aliasCPU)){aliasAtDraw=*alias;aliasPresent=true;}
   for(unsigned i=0;i<4;++i)if(binding.targets[i]){
    if(Buffer(binding.targets[i]))binding.backbufferMask|=1u<<i;
    for(size_t p=0;p<S().portraitPins.size();++p)if(reinterpret_cast<uintptr_t>(S().portraitPins[p].Get())==binding.targets[i]&&S().portraitGenerations[p]==binding.generations[i])binding.portraitMask|=1u<<i;
    for(uint32_t p=0;p<S().nativeDiagnostic.targetCount;++p){const auto& t=S().nativeDiagnostic.targets[p];if(t.resource==binding.targets[i]&&t.generation==binding.generations[i])binding.trackedMask|=1u<<i;}
   }
   // Bounded CBV candidates first (the actual menu b1/b5 use offsets 1/0).
   // The bound of four slots is only a sampling limit, never a guessed layout.
   uint32_t cbCount{};
   for(uint32_t root=0;root<current->tables.size();++root){const auto& table=current->tables[root];if(!table.proven||!table.cpu)continue;
    const Heap* matched=nullptr;for(const auto& heap:S().heaps){if(!heap.owner||!heap.gpu||!heap.cpu||!heap.increment||table.gpu<heap.gpu)continue;bool bound=false;for(auto id:current->heaps)bound|=id==heap.owner;if(!bound)continue;const auto delta=table.gpu-heap.gpu;
     if(delta%heap.increment||delta/heap.increment>=heap.count||table.count>heap.count-delta/heap.increment||table.cpu!=heap.cpu+delta)continue;if(matched){matched=nullptr;break;}matched=&heap;}
    if(!matched)continue;
    for(uint32_t off=0;off<std::min<uint32_t>(table.count,4);++off)if(const auto* d=GetDescriptor(table.cpu+uintptr_t(off)*matched->increment);d&&(d->cbvOnly||d->cbv.known)){
     if(cbCount++<16)CapturePanelDescriptorLocked(*current,root,off,bank);else ++bank.dropped;}
   }
   for(const auto& nativeBinding:current->native)if(nativeBinding.valid)CapturePanelDescriptorLocked(*current,nativeBinding.rootIndex,nativeBinding.relative,bank,&nativeBinding);
   binding.bankDrops=bank.dropped;
  }
  // Static PSO/root cache parsing may lock or deserialize; never under S mutex.
  PipelineShaderEvidence shaders;binding.shaderKnown=QueryPipelineShaders(binding.pipeline,shaders);
  if(binding.shaderKnown){binding.ps=shaders.psHash;binding.vs=shaders.vsHash;binding.psBytes=shaders.psBytes;binding.vsBytes=shaders.vsBytes;}

  std::array<ShaderSrvLocation,16> locations{};uint32_t locationCount{};
  binding.srvsKnown=binding.shaderKnown&&shaders.srvBindingsKnown&&ResolvePixelSrvLocations(binding.root,shaders,locations,locationCount);
  std::array<RootDescriptorEvidence,8> registers{};std::array<bool,8> registerKnown{};
  if(binding.srvsKnown){binding.srvTotal=locationCount;binding.srvCount=std::min<uint32_t>(locationCount,uint32_t(binding.srvs.size()));binding.srvDrops=locationCount-binding.srvCount;
   for(uint32_t i=0;i<binding.srvCount;++i)registerKnown[i]=ResolveRootDescriptor(binding.root,locations[i].rootIndex,locations[i].descriptorOffset,registers[i]);}
  std::array<VertexConstantLocation,2> constants{};
  for(unsigned i=0;i<2;++i){binding.constantResolved[i]=ResolveVertexConstantLocation(binding.root,i?5:1,0,constants[i]);binding.constantRoots[i]=constants[i].rootIndex;binding.constantOffsets[i]=constants[i].descriptorOffset;binding.constantTable[i]=constants[i].table;}
  record.elements=elements;record.instances=instances;record.firstVertex=firstVertex;record.indexed=indexed;
  record.firstIndex=firstIndex;record.baseVertex=baseVertex;record.firstInstance=firstInstance;
  record.nativeListKnown=native!=0;record.nativeNode=cvr::detail::t_current_node_work;
  {std::lock_guard lock(S().mutex);auto* current=FindList(key.list);
   if(!current||PanelKey(*current)!=key||current->closed||current->overflow||!S().alias||S().alias->readCPU!=aliasCPU||
      !Current(witness.scope)||!SamePreview(S().alias->scope,witness.scope)||current->pipeline!=binding.pipeline||current->root!=binding.root||
      current->targets!=binding.targets||current->targetGenerations!=binding.generations||!SameUiTransformBindings(current->transform,witness.bindings)||!SameUiTransformTables(*current,witness)){++Panels().errors;return;}
   binding.portraitReadKnown=binding.srvsKnown&&!binding.srvDrops&&!bank.dropped&&aliasPresent&&!aliasAtDraw.pending&&aliasAtDraw.resource&&aliasAtDraw.resourceGeneration;
   for(uint32_t i=0;i<binding.srvCount;++i){const auto loc=locations[i];auto& out=binding.srvs[i];
    if(const auto* captured=bank.Find(loc.rootIndex,loc.descriptorOffset))out=captured->srv;
    else {out.root=loc.rootIndex;out.offset=loc.descriptorOffset;if(loc.rootIndex<witness.tables.size()){out.tableGPU=witness.tables[loc.rootIndex].gpu;out.tableProven=witness.tables[loc.rootIndex].proven;}}
    out.registerKnown=registerKnown[i];if(registerKnown[i]){out.shaderRegister=registers[i].shaderRegister;out.space=registers[i].registerSpace;}
    if(!out.descriptorPresent||out.pending||out.unordered||!out.resource||!out.generation)binding.portraitReadKnown=false;
    binding.readsPortrait|=out.descriptorPresent&&!out.pending&&aliasPresent&&!aliasAtDraw.pending&&aliasAtDraw.resource&&out.resource==aliasAtDraw.resource&&out.generation==aliasAtDraw.resourceGeneration;
   }
   for(unsigned i=0;i<2;++i)if(binding.constantResolved[i]){const auto loc=constants[i];auto& proof=binding.constants[i];
    if(loc.rootIndex>=witness.tables.size())continue;
    if(!loc.table){proof.bound=(witness.bindings.cbvMask&(uint64_t{1}<<loc.rootIndex))!=0;proof.gpu=proof.bound?witness.bindings.cbvs[loc.rootIndex]:0;proof.payloadKnown=proof.gpu!=0;}
    else if(loc.descriptorOffset<=UINT32_MAX){if(const auto* captured=bank.Find(loc.rootIndex,uint32_t(loc.descriptorOffset))){proof=captured->constant;if(proof.bytes<(i?48u:64u)){proof.payloadKnown=false;proof.gpu=0;}}else proof.tableGPU=witness.tables[loc.rootIndex].gpu;}
   }
   const auto scope=PanelScope(witness.scope);
   if(Panels().armed!=scope){Panels().armed=scope;Log("[EDITOR-PANEL-WITNESS] armed epoch=%u setup=%u owner=%llu component=%llu limits=2lists/64draws/16bindings srvLimit=8 bankLimit=32 cbCandidateLimit=16 cbCandidateOffsets=0..3; post-Draw metadata only, no pixel admission\n",scope.epoch,scope.setup,scope.owner,scope.component);}
   Panels().pool.Observe(scope,key,record,binding);
  }
 }catch(...){++Panels().errors;} // Never call Fail or alter pixel events on diagnostic loss.
}
void QualifyPanelWitnessLocked(const List& list,const Metadata& scope,uintptr_t target,uint64_t generation)noexcept{
 if(PanelWitnessEnabled())Panels().pool.Qualify(PanelScope(scope),PanelKey(list),target,generation);
}
std::shared_ptr<const PanelPool::Batch> FreezePanelWitnessLocked(const List& list,const Metadata& scope)noexcept{
 if(!PanelWitnessEnabled()||!Panels().pool.CanTakeReady(PanelScope(scope),PanelKey(list)))return {};
 try{auto batch=std::make_shared<PanelPool::Batch>();if(Panels().pool.TakeReady(PanelScope(scope),PanelKey(list),*batch))return batch;}catch(...){++Panels().errors;}
 return {};
}
const char* PanelSrvSubmissionStability(const PanelSrvSnapshot& saved){
 if(!saved.descriptorPresent||saved.pending||!saved.cpu)return "unknown-at-draw";
 const auto* now=GetDescriptor(saved.cpu);if(!now)return "unknown-not-in-ledger";
 return !now->pending&&now->version==saved.version&&now->resource==saved.resource&&now->resourceGeneration==saved.generation&&now->unorderedAccess==saved.unordered?"stable-ledger-version":"changed-or-pending";
}
const char* PanelCbvSubmissionStability(const UiConstantProof& saved){
 if(!saved.descriptorPresent||saved.pending||!saved.cpu)return "unknown-no-descriptor-proof";
 const auto* now=GetDescriptor(saved.cpu);if(!now)return "unknown-not-in-ledger";
 return !now->pending&&now->version==saved.version&&now->cbv==saved.descriptor&&!now->resource&&!now->unorderedAccess?"stable-ledger-version":"changed-or-pending";
}
void EmitPanelWitnessLocked(const std::shared_ptr<const PanelPool::Batch>& batch,uintptr_t queue,uint64_t submission)noexcept{
 if(!batch)return;
 try{
  if(!Panels().pool.Commit(batch->scope,batch->key))return;
  Log("[EDITOR-PANEL-LIST] epoch=%u setup=%u list=%llx life=%llu generation=%llu queue=%llx submission=%llu draws=%zu seen=%llu bindings=%zu recordDrops=%llu bindingDrops=%llu evictedRecords=%llu poolDrops=%llu prefixIncomplete=%u errors=%llu bridgeTarget=%llx bridgeGeneration=%llu; successful submission, stored snapshots not current descriptor lookups\n",batch->scope.epoch,batch->scope.setup,batch->key.list,batch->key.life,batch->key.generation,queue,submission,batch->count,batch->seen,batch->bindingCount,batch->recordDrops,batch->bindingDrops,batch->evictedRecords,batch->poolDrops,unsigned(batch->prefixIncomplete||Panels().errors.load()!=0),Panels().errors.load(),batch->bridgeTarget,batch->bridgeTargetGeneration);
  for(size_t i=0;i<batch->bindingCount;++i){const auto& b=batch->bindings[i];
   Log("[EDITOR-PANEL-BINDING] list=%llx generation=%llu binding=%zu pso=%llx root=%llx shaderKnown=%u PS=%016llX/%u VS=%016llX/%u srvKnown=%u srvCount=%u srvTotal=%u srvDrops=%u bankDrops=%u readsPortrait=%u readsPortraitKnown=%u writesPortraitMask=%X backbufferMask=%X trackedMask=%X\n",batch->key.list,batch->key.generation,i,b.pipeline,b.root,unsigned(b.shaderKnown),b.ps,b.psBytes,b.vs,b.vsBytes,unsigned(b.srvsKnown),b.srvCount,b.srvTotal,b.srvDrops,b.bankDrops,unsigned(b.readsPortrait),unsigned(b.portraitReadKnown),b.portraitMask,b.backbufferMask,b.trackedMask);
   for(unsigned t=0;t<4;++t)if(b.targets[t])Log("[EDITOR-PANEL-TARGET] binding=%zu slot=%u resource=%llx generation=%llu extentKnown=%u size=%llux%u format=%u\n",i,t,b.targets[t],b.generations[t],unsigned(b.extents[t].known),b.extents[t].width,b.extents[t].height,b.extents[t].format);
   for(uint32_t n=0;n<b.srvCount;++n){const auto& v=b.srvs[n];Log("[EDITOR-PANEL-SRV] binding=%zu index=%u root=%u offset=%u registerKnown=%u t=%u space=%u table=%llx tableProven=%u cpu=%llx present=%u version=%llu resource=%llx generation=%llu pending=%u unordered=%u nativeKnown=%u nativeID=%u slot=%u stage=%u trackedIntermediate=%u submitStability=%s; original post-Draw snapshot, not GPU-used contents\n",i,n,v.root,v.offset,unsigned(v.registerKnown),v.shaderRegister,v.space,v.tableGPU,unsigned(v.tableProven),v.cpu,unsigned(v.descriptorPresent),v.version,v.resource,v.generation,unsigned(v.pending),unsigned(v.unordered),unsigned(v.nativeKnown),v.nativeID,v.nativeSlot,v.nativeStage,unsigned(v.trackedIntermediate),PanelSrvSubmissionStability(v));}
   for(unsigned n=0;n<2;++n){const auto& c=b.constants[n];Log("[EDITOR-PANEL-CBV] binding=%zu b=%u resolved=%u root=%u table=%u offset=%llu tableGPU=%llx cpu=%llx descriptorPresent=%u version=%llu pending=%u gpu=%llx bytes=%u payloadKnown=%u submitStability=%s; no refreshed binding or payload read\n",i,n?5:1,unsigned(b.constantResolved[n]),b.constantRoots[n],unsigned(b.constantTable[n]),b.constantOffsets[n],c.tableGPU,c.cpu,unsigned(c.descriptorPresent),c.version,unsigned(c.pending),c.gpu,c.bytes,unsigned(c.payloadKnown),PanelCbvSubmissionStability(c));}
  }
  for(size_t i=0;i<batch->count;++i){const auto& r=batch->records[i];const auto& v=r.raster;
   Log("[EDITOR-PANEL-DRAW] list=%llx generation=%llu ordinal=%llu binding=%u indexed=%u elements=%u instances=%u firstVertex=%u firstIndex=%u baseVertex=%d firstInstance=%u nativeListKnown=%u nativeNode=%llx vbKnown=%u vbGPU=%llx vbSize=%u vbStride=%u indexedVBRead=unknown viewportCount=%u viewportKnown=%u vp=(%g,%g,%g,%g,%g,%g) scissorCount=%u scissorKnown=%u scissor=(%ld,%ld,%ld,%ld); no GPU completion or panel identity claim\n",batch->key.list,batch->key.generation,r.ordinal,r.bindingIndex,unsigned(r.indexed),r.elements,r.instances,r.firstVertex,r.firstIndex,r.baseVertex,r.firstInstance,unsigned(r.nativeListKnown),r.nativeNode,unsigned(r.vertexKnown),r.vertex.BufferLocation,r.vertex.SizeInBytes,r.vertex.StrideInBytes,v.viewportCount,unsigned(v.viewportKnown),double(v.viewport.TopLeftX),double(v.viewport.TopLeftY),double(v.viewport.Width),double(v.viewport.Height),double(v.viewport.MinDepth),double(v.viewport.MaxDepth),v.scissorCount,unsigned(v.scissorKnown),v.scissor.left,v.scissor.top,v.scissor.right,v.scissor.bottom);
  }
 }catch(...){++Panels().errors;}
}
template<size_t N> bool ReadUiUploadStable(uint64_t gpu,std::array<uint8_t,N>& output){
 static_assert(N%8==0);if(!gpu)return false;
 uintptr_t firstMapping{};
 const auto copy=[&](std::array<uint8_t,N>& bytes){
  const auto mapped=reinterpret_cast<uintptr_t>(cvr::detail::upload_cpu_for_va(gpu,N));
  if(!mapped||mapped>UINTPTR_MAX-N||(firstMapping&&mapped!=firstMapping))return false;firstMapping=mapped;
  for(size_t offset=0;offset<N;offset+=8){uint64_t word{};if(!ReadU64Safe(mapped+offset,&word))return false;std::memcpy(bytes.data()+offset,&word,8);}return true;
 };
 std::array<uint8_t,N> first{},second{};
 if(!copy(first)||!copy(second)||first!=second)return false;
 output=first;return true;
}
template<size_t N> std::array<char,N*2+1> UiHexBytes(const std::array<uint8_t,N>& bytes){
 constexpr char digits[]="0123456789ABCDEF";std::array<char,N*2+1> result{};
 for(size_t i=0;i<N;++i){result[i*2]=digits[bytes[i]>>4];result[i*2+1]=digits[bytes[i]&15];}return result;
}
void LogUiRasterLocked(const List& list,uintptr_t source,uint64_t sourceGeneration,uintptr_t target,uint64_t targetGeneration,UINT elements,UINT instances,const char* route){
 if(!UiRasterDiagnosticsEnabled()||!S().alias||S().alias->scope.screen!=2)return;
 auto& diag=S().nativeDiagnostic;if(diag.rasterRows>=6)return;
 UiTargetExtent extent{};for(size_t i=0;i<list.targets.size();++i)if(list.targets[i]==target&&list.targetGenerations[i]==targetGeneration){extent=list.targetExtents[i];break;}
 const auto& raster=list.raster;const auto& vp=raster.viewport;const auto& sc=raster.scissor;++diag.rasterRows;
 Log("[MenuPreviewUI] ui-editor-raster row=%u route=%s epoch=%u setup=%u list=%llx generation=%llu pso=%llx root=%llx source=%llx sourceGeneration=%llu target=%llx targetGeneration=%llu targetExtentKnown=%u targetWidth=%llu targetHeight=%u targetFormat=%u viewportCount=%u viewportKnown=%u vp=(%.6g,%.6g,%.6g,%.6g,%.6g,%.6g) scissorCount=%u scissorKnown=%u scissor=(%ld,%ld,%ld,%ld) elements=%u instances=%u; exact qualified Draw observation only, no completion or eye claim\n",
  diag.rasterRows,route,S().alias->scope.screenEpoch,S().alias->scope.setupID,list.identity,list.generation,list.pipeline,list.root,
  source,sourceGeneration,target,targetGeneration,unsigned(extent.known),extent.width,extent.height,extent.format,
  raster.viewportCount,unsigned(raster.viewportKnown),double(vp.TopLeftX),double(vp.TopLeftY),double(vp.Width),double(vp.Height),double(vp.MinDepth),double(vp.MaxDepth),
  raster.scissorCount,unsigned(raster.scissorKnown),sc.left,sc.top,sc.right,sc.bottom,elements,instances);
}
const char* OverflowName(OverflowReason reason)noexcept{switch(reason){case OverflowReason::AddClosed:return "add-after-close";case OverflowReason::EventCapacity:return "event-capacity";case OverflowReason::RootOutOfRange:return "root-index-out-of-range";case OverflowReason::CloseFailed:return "close-failed";case OverflowReason::ResourceConflict:return "resource-conflict";case OverflowReason::DescriptorPinFailed:return "descriptor-pin-failed";default:return "unknown";}}
void MarkOverflow(List& l,OverflowReason reason,uintptr_t detail0=0,uintptr_t detail1=0)noexcept{if(!l.overflow){l.overflowReason=reason;l.overflowDetail0=detail0;l.overflowDetail1=detail1;}l.overflow=true;}
void Add(List& l,Event e){if(e.writer&&!e.app&&e.writtenResource){for(size_t i=l.count;i>0;--i){const auto& old=l.events[i-1];if(old.writer&&(old.writtenResource?old.writtenResource:reinterpret_cast<uintptr_t>(old.resource.Get()))==e.writtenResource){if(!old.app)return;break;}}}if(!e.writer&&!e.resource&&e.target){for(size_t i=0;i<l.count;++i)if(l.events[i].target.Get()==e.target.Get())return;}if(l.closed){MarkOverflow(l,OverflowReason::AddClosed,l.count);return;}if(l.count==l.events.size()){MarkOverflow(l,OverflowReason::EventCapacity,l.count);return;}l.events[l.count++]=std::move(e);if((l.events[l.count-1].descriptorCPU||l.events[l.count-1].compute)&&!PinRecordedEventLocked(l))MarkOverflow(l,OverflowReason::DescriptorPinFailed,l.events[l.count-1].descriptorCPU);}
void RememberTargetDescriptorLocked(uintptr_t resource,uint64_t generation,uintptr_t cpu,uint64_t version,bool copy){
    if(!OffscreenTraceEnabled()||!resource||!generation||!cpu)return;auto& d=S().nativeDiagnostic;
    for(uint32_t i=0;i<d.targetCount;++i){auto& t=d.targets[i];if(t.resource!=resource||t.generation!=generation)continue;
        t.handles[t.handleCursor++%t.handles.size()]={cpu,version};if(copy)++d.targetCopies;else ++d.targetSrvWrites;
    }
}
TrackedTarget* FindTrackedTargetLocked(uintptr_t resource,uint64_t generation)noexcept{if(!resource||!generation)return nullptr;auto& d=S().nativeDiagnostic;for(uint32_t i=0;i<d.targetCount;++i)if(d.targets[i].resource==resource&&d.targets[i].generation==generation)return &d.targets[i];return nullptr;}
void AlignEditorUiLocked(const std::shared_ptr<const CameraApplication>& app,uintptr_t queue,bool& required,std::shared_ptr<const EditorUiPublication>& publication){
 required=false;publication.reset();if(!EditorUiSeparationEnabled()||!app)return;
 for(const auto& t:S().nativeDiagnostic.targets)if(t.qualifiedApp&&t.qualifiedQueue==queue&&t.qualifiedSubmission&&t.qualifiedApp->frame.sourceSequence==app->frame.sourceSequence&&t.qualifiedApp->localPoseRevision==app->localPoseRevision&&t.qualifiedApp->frame.eye==app->frame.eye&&SamePreview(t.qualifiedApp->metadata,app->metadata)&&t.qualifiedApp->metadata.controllerIdentity==app->metadata.controllerIdentity){
  required|=t.editorUiRequired;if(t.editorUi){if(publication&&publication!=t.editorUi){publication.reset();required=true;return;}publication=t.editorUi;}
 }
}
TrackedTarget* ReserveTrackedTargetLocked()noexcept{auto& d=S().nativeDiagnostic;TrackedTarget* result=nullptr;for(uint32_t i=0;i<d.targetCount;++i)if(!d.targets[i].resource){result=&d.targets[i];break;}if(!result&&d.targetCount<d.targets.size())result=&d.targets[d.targetCount++];if(result)result->sourceScope=S().alias?S().alias->scope:Metadata{};return result;}
void SeedTrackedHandlesLocked(TrackedTarget& tracked){for(const auto& entry:S().descriptors)if(entry.cpu&&!entry.pending&&entry.resource==tracked.resource&&entry.resourceGeneration==tracked.generation)tracked.handles[tracked.handleCursor++%tracked.handles.size()]={entry.cpu,entry.version};
 if(const auto group=S().sourcesByResource.find({tracked.resource,tracked.generation});group!=S().sourcesByResource.end())for(auto cpu:group->second){const auto* entry=GetDescriptor(cpu);if(entry&&!entry->pending&&entry->resource==tracked.resource&&entry->resourceGeneration==tracked.generation)tracked.handles[tracked.handleCursor++%tracked.handles.size()]={cpu,entry->version};}}
void NoteTargetProducedLocked(List& list,uintptr_t target,const UiDescriptorEntry& source,uint32_t rootIndex,uint32_t offset){
    auto& d=S().nativeDiagnostic;uint64_t generation{};for(size_t i=0;i<list.targets.size();++i)if(list.targets[i]==target){generation=list.targetGenerations[i];break;}
    if(!generation){++d.targetGenerationMiss;if(d.targetGenerationMiss<=2)Log("[MenuPreviewUI] ui-intermediate-target-rejected target=%llx list=%llx listGeneration=%llu reason=resource-generation-unavailable; no target lineage\n",target,list.identity,list.generation);return;}TrackedTarget* tracked=nullptr;
    for(uint32_t i=0;i<d.targetCount;++i)if(d.targets[i].resource==target&&d.targets[i].generation==generation){tracked=&d.targets[i];break;}
    if(!tracked){tracked=ReserveTrackedTargetLocked();if(!tracked)return;tracked->resource=target;tracked->generation=generation;tracked->source=source.resource;tracked->sourceGeneration=source.resourceGeneration;
        // One bounded ledger seed catches SRVs created before this first proven Draw.
        SeedTrackedHandlesLocked(*tracked);
        if(d.targetRows++<2)Log("[MenuPreviewUI] ui-intermediate-target epoch=%u setup=%u target=%llx generation=%llu source=%llx sourceGeneration=%llu list=%llx listGeneration=%llu pso=%llx rootIndex=%u offset=%u seededHandles=%u; exact proven range, shader sample unproven\n",d.epoch,d.setup,target,generation,source.resource,source.resourceGeneration,list.identity,list.generation,list.pipeline,rootIndex,offset,unsigned(tracked->handleCursor<tracked->handles.size()?tracked->handleCursor:tracked->handles.size()));
    }
    for(uint32_t i=0;i<list.traceCount;++i)if(!list.trace[i].egress&&list.trace[i].resource==target&&list.trace[i].resourceGeneration==generation)return;
    if(list.traceCount<list.trace.size()){auto& row=list.trace[list.traceCount++];row.resource=target;row.resourceGeneration=generation;row.source=source.resource;row.sourceGeneration=source.resourceGeneration;row.pipeline=list.pipeline;row.root=list.root;row.descriptorCPU=source.cpu;row.descriptorVersion=source.version;row.rootIndex=rootIndex;row.relative=offset;row.order=++list.traceOrder;}else ++d.traceDrops;
}
void NoteTargetEgressLocked(List& list,uintptr_t backBuffer){
    if(!OffscreenTraceEnabled())return;auto& diag=S().nativeDiagnostic;if(!diag.targetCount)return;++diag.egressDraws;bool current=false,heapBound=false;
    for(uint32_t i=0;i<diag.targetCount;++i){const auto& target=diag.targets[i];if(!target.resource||!target.generation)continue;
        for(const auto& handle:target.handles){if(!handle.cpu)continue;const auto* descriptor=GetDescriptor(handle.cpu);
            if(!descriptor||descriptor->pending||descriptor->version!=handle.version||descriptor->resource!=target.resource||descriptor->resourceGeneration!=target.generation)continue;current=true;
            for(const auto& heap:S().heaps){if(!heap.owner||!heap.gpu||!heap.increment||descriptor->cpu<heap.cpu)continue;
                bool bound=false;for(auto h:list.heaps)bound|=h==heap.owner;if(!bound)continue;
                const auto distance=descriptor->cpu-heap.cpu;if(distance%heap.increment||distance/heap.increment>=heap.count)continue;heapBound=true;const auto gpu=heap.gpu+distance;
                for(uint32_t root=0;root<list.tables.size();++root){const auto& table=list.tables[root];if(!table.proven||!table.count||gpu<table.gpu||(gpu-table.gpu)%heap.increment)continue;
                    const auto offset=(gpu-table.gpu)/heap.increment;if(offset>=table.count||table.cpu+offset*heap.increment!=descriptor->cpu)continue;
                    ++diag.egressMatches;++diag.egressCurrent;++diag.egressHeapBound;bool recorded=false,stored=false;for(uint32_t n=0;n<list.traceCount;++n)if(list.trace[n].egress&&list.trace[n].resource==target.resource&&list.trace[n].resourceGeneration==target.generation&&list.trace[n].backBuffer==backBuffer)recorded=true;
                    if(!recorded&&list.traceCount<list.trace.size()){auto& row=list.trace[list.traceCount++];row.resource=target.resource;row.resourceGeneration=target.generation;row.source=target.source;row.sourceGeneration=target.sourceGeneration;row.backBuffer=backBuffer;row.pipeline=list.pipeline;row.root=list.root;row.descriptorCPU=descriptor->cpu;row.descriptorVersion=descriptor->version;row.rootIndex=root;row.relative=uint32_t(offset);row.order=++list.traceOrder;row.egress=true;stored=true;}
                    else if(!recorded)++diag.traceDrops;
                    if(diag.egressRows<8){++diag.egressRows;Log("[MenuPreviewUI] ui-intermediate-egress-draw epoch=%u setup=%u list=%llx listGeneration=%llu backBuffer=%llx target=%llx targetGeneration=%llu pso=%llx root=%llx rootIndex=%u tableGPU=%llx descriptorCPU=%llx descriptorVersion=%llu offset=%llu source=%llx sourceGeneration=%llu traceRecorded=%u; bound proven range candidate only, shader sample unproven\n",diag.epoch,diag.setup,list.identity,list.generation,backBuffer,target.resource,target.generation,list.pipeline,list.root,root,table.gpu,descriptor->cpu,descriptor->version,offset,target.source,target.sourceGeneration,unsigned(recorded||stored));}
                    return;
                }
            }
        }
    }
    if(current)++diag.egressCurrent;if(heapBound)++diag.egressHeapBound;
}
void SelectBridgeProbeLocked(const List& list,UINT elements,UINT instances,PipelineProbeRequest& out){
    const auto& alias=S().alias;if((EditorUiSeparationEnabled()?(!elements||!instances):(elements!=6||instances!=1))||!alias||!list.root||!list.pipeline)return;
    constexpr uint32_t root=26,offset=23;const auto& table=list.tables[root];if(!table.proven||table.count!=39||!table.cpu||!table.gpu)return;
    uintptr_t target{};uint64_t targetGeneration{};uint32_t targetCount{};
    for(size_t i=0;i<list.targets.size();++i){const auto p=list.targets[i];if(!p||Buffer(p))continue;bool portrait=false;for(const auto& pin:S().portraitPins)portrait|=reinterpret_cast<uintptr_t>(pin.Get())==p;if(portrait)continue;++targetCount;target=p;targetGeneration=list.targetGenerations[i];}
    if(targetCount!=1||!targetGeneration)return;
    const auto* source=GetDescriptor(alias->readCPU);if(!source||source->pending||source->unorderedAccess||!source->resource||!source->resourceGeneration||source->originCPU!=alias->readCPU||!source->originVersion)return;
    bool pinned=false;for(size_t i=0;i<S().portraitPins.size();++i)if(source->resource==reinterpret_cast<uintptr_t>(S().portraitPins[i].Get())&&source->resourceGeneration==S().portraitGenerations[i])pinned=true;if(!pinned)return;
    for(const auto& heap:S().heaps){if(!heap.owner||!heap.gpu||!heap.increment||table.gpu<heap.gpu)continue;bool bound=false;for(auto h:list.heaps)bound|=h==heap.owner;if(!bound)continue;
        const auto distance=table.gpu-heap.gpu;if(distance%heap.increment||distance/heap.increment>=heap.count||table.count>heap.count-distance/heap.increment||table.cpu!=heap.cpu+distance||offset>=table.count)continue;
        const auto cpu=table.cpu+uintptr_t(offset)*heap.increment;const auto* descriptor=GetDescriptor(cpu);
        if(!descriptor||descriptor->pending||descriptor->unorderedAccess||descriptor->resource!=source->resource||descriptor->resourceGeneration!=source->resourceGeneration||descriptor->originCPU!=source->originCPU||descriptor->originVersion!=source->originVersion)return;
        out.list=list.identity;out.pipeline=list.pipeline;out.root=list.root;out.target=target;out.source=source->resource;out.descriptorCPU=cpu;out.aliasCPU=alias->readCPU;out.generation=list.generation;out.targetGeneration=targetGeneration;out.sourceGeneration=source->resourceGeneration;out.descriptorVersion=descriptor->version;out.originVersion=source->originVersion;out.offset=offset;out.epoch=alias->scope.screenEpoch;out.setup=alias->scope.setupID;out.rootIndex=root;out.ready=true;out.bridge=true;return;
    }
}
void SelectTransferDestinationLocked(const List& list,UINT elements,UINT instances,TransferDestination& out){
    if(!elements||!instances||!list.root||!list.pipeline||!S().nativeDiagnostic.targetCount)return;uint32_t targets{};
    for(size_t i=0;i<list.targets.size();++i){const auto resource=list.targets[i];if(!resource||!list.targetGenerations[i])continue;bool portrait=false;for(const auto& pin:S().portraitPins)portrait|=reinterpret_cast<uintptr_t>(pin.Get())==resource;if(portrait)continue;++targets;out.targets[i]=resource;out.generations[i]=list.targetGenerations[i];if(auto* backBuffer=Buffer(resource);backBuffer&&backBuffer->generation==list.targetGenerations[i]){out.backBufferMask|=1u<<i;if(!out.backBuffer){out.resource=resource;out.resourceGeneration=list.targetGenerations[i];out.backBuffer=true;}}else if(!out.resource){out.resource=resource;out.resourceGeneration=list.targetGenerations[i];}}
    if(!targets)return;out.list=list.identity;out.listGeneration=list.generation;out.pipeline=list.pipeline;out.root=list.root;out.epoch=S().nativeDiagnostic.epoch;out.setup=S().nativeDiagnostic.setup;out.ready=true;
}
bool VerifiedBridgePipeline(const PipelineProbeRequest& probe)noexcept{
    struct Proof {uintptr_t pipeline{},root{};bool exact{};ComPtr<ID3D12PipelineState> pipelinePin;ComPtr<ID3D12RootSignature> rootPin;};
    static std::mutex mutex;static std::array<Proof,8> cache{};static uint32_t cursor{},rows{};
    try{
        {std::lock_guard lock(mutex);for(const auto& entry:cache)if(entry.pipeline==probe.pipeline&&entry.root==probe.root&&entry.pipelinePin&&entry.rootPin)return entry.exact;}
        ComPtr<ID3D12PipelineState> pipelinePin(reinterpret_cast<ID3D12PipelineState*>(probe.pipeline));
        ComPtr<ID3D12RootSignature> rootPin(reinterpret_cast<ID3D12RootSignature*>(probe.root));
        PipelineShaderEvidence shader;RootDescriptorEvidence binding;
        const bool shaderKnown=QueryPipelineShaders(probe.pipeline,shader);
        const bool bindingKnown=ResolveRootDescriptor(probe.root,probe.rootIndex,probe.offset,binding);
        bool exact=shaderKnown&&shader.psHash==0x798FA69016E1DB5Dull&&shader.psBytes==2844&&bindingKnown&&
            binding.visibility==D3D12_SHADER_VISIBILITY_PIXEL&&binding.type==D3D12_DESCRIPTOR_RANGE_TYPE_SRV&&
            binding.rangeIndex==0&&binding.baseShaderRegister==54&&binding.shaderRegister==77&&binding.registerSpace==0&&
            binding.descriptorCount==39&&binding.effectiveOffset==0;
        if(EditorUiSeparationEnabled()&&shaderKnown&&shader.srvBindingsKnown){std::array<ShaderSrvLocation,16> locations{};uint32_t n{};exact=false;if(ResolvePixelSrvLocations(probe.root,shader,locations,n))for(uint32_t i=0;i<n;++i)exact|=locations[i].rootIndex==probe.rootIndex&&locations[i].descriptorOffset==probe.offset;}
        std::lock_guard lock(mutex);if(rows<4){++rows;Log("[MenuPreviewUI] pipeline-evidence epoch=%u setup=%u list=%llx listGeneration=%llu pso=%llx root=%llx rootIndex=%u offset=%llu shaderKnown=%u psHash=%016llX psBytes=%u vsHash=%016llX vsBytes=%u bindingKnown=%u visibility=%u rangeType=%u rangeIndex=%u baseRegister=%u shaderRegister=%u space=%u count=%u effectiveOffset=%llu exactBridge=%u; bounded PS/root identity proof\n",probe.epoch,probe.setup,probe.list,probe.generation,probe.pipeline,probe.root,probe.rootIndex,probe.offset,unsigned(shaderKnown),shader.psHash,shader.psBytes,shader.vsHash,shader.vsBytes,unsigned(bindingKnown),unsigned(binding.visibility),unsigned(binding.type),binding.rangeIndex,binding.baseShaderRegister,binding.shaderRegister,binding.registerSpace,binding.descriptorCount,binding.effectiveOffset,unsigned(exact));}
        auto& slot=cache[cursor++%cache.size()];slot={};slot.pipeline=probe.pipeline;slot.root=probe.root;slot.exact=exact;slot.pipelinePin=std::move(pipelinePin);slot.rootPin=std::move(rootPin);return exact;
    }catch(...){return false;}
}
TransferLocations UsedTransferLocations(uintptr_t pipeline,uintptr_t root)noexcept{
    struct Proof {uintptr_t pipeline{},root{};TransferLocations locations;ComPtr<ID3D12PipelineState> pipelinePin;ComPtr<ID3D12RootSignature> rootPin;};
    static std::mutex mutex;static std::array<Proof,32> cache{};static uint32_t cursor{};
    try{
        {std::lock_guard lock(mutex);for(const auto& entry:cache)if(entry.pipeline==pipeline&&entry.root==root&&entry.pipelinePin&&entry.rootPin)return entry.locations;}
        ComPtr<ID3D12PipelineState> pipelinePin(reinterpret_cast<ID3D12PipelineState*>(pipeline));ComPtr<ID3D12RootSignature> rootPin(reinterpret_cast<ID3D12RootSignature*>(root));
        PipelineShaderEvidence shader;TransferLocations result;result.shaderKnown=QueryPipelineShaders(pipeline,shader);if(result.shaderKnown)result.shader=shader;
        if(result.shaderKnown&&shader.srvBindingsKnown&&shader.srvBindingCount<=shader.srvBindings.size())result.known=ResolvePixelSrvLocations(root,shader,result.rows,result.count)&&result.count<=result.rows.size();
        if(!result.known)result.count=0;
        std::lock_guard lock(mutex);auto& slot=cache[cursor++%cache.size()];slot={};slot.pipeline=pipeline;slot.root=root;slot.locations=result;slot.pipelinePin=std::move(pipelinePin);slot.rootPin=std::move(rootPin);return result;
    }catch(...){return {};}
}
ComputeLocations UsedComputeLocations(uintptr_t pipeline,uintptr_t root)noexcept{
    struct Proof {uintptr_t pipeline{},root{};ComputeLocations locations;ComPtr<ID3D12PipelineState> pipelinePin;ComPtr<ID3D12RootSignature> rootPin;};
    static std::mutex mutex;static std::array<Proof,32> cache{};static uint32_t cursor{};
    try{
        {std::lock_guard lock(mutex);for(const auto& entry:cache)if(entry.pipeline==pipeline&&entry.root==root&&entry.pipelinePin&&entry.rootPin)return entry.locations;}
        ComPtr<ID3D12PipelineState> pipelinePin(reinterpret_cast<ID3D12PipelineState*>(pipeline));ComPtr<ID3D12RootSignature> rootPin(reinterpret_cast<ID3D12RootSignature*>(root));
        ComputeLocations result;result.shaderKnown=QueryComputeShader(pipeline,result.shader);
        if(result.shaderKnown&&result.shader.bindingsKnown&&result.shader.srvBindingCount<=result.shader.srvBindings.size()&&result.shader.uavBindingCount<=result.shader.uavBindings.size())result.known=ResolveComputeLocations(root,result.shader,result.rows,result.count)&&result.count<=result.rows.size();
        if(!result.known)result.count=0;
        std::lock_guard lock(mutex);auto& slot=cache[cursor++%cache.size()];slot={};slot.pipeline=pipeline;slot.root=root;slot.locations=result;slot.pipelinePin=std::move(pipelinePin);slot.rootPin=std::move(rootPin);return result;
    }catch(...){return {};}
}
bool Current(const Metadata& scope){Lease lease;return TryAcquireCurrent(lease)&&SamePreview(lease.metadata,scope)&&lease.metadata.controllerIdentity==scope.controllerIdentity;}
List* FindUiTransformWitnessLocked(uintptr_t list,uint64_t lifetime,const UiTransformWitness& witness,UINT elements,UINT instances){
 const auto& probe=witness.probe;auto* recorded=FindList(list);
 if(!recorded||recorded->objectGeneration!=lifetime||recorded->overflow||recorded->generation!=probe.generation||recorded->root!=probe.root||recorded->pipeline!=probe.pipeline||
    !SameUiTransformBindings(recorded->transform,witness.bindings)||!SameUiTransformTables(*recorded,witness)||!S().alias||S().alias->scope.screen!=2||
    !SamePreview(S().alias->scope,witness.scope)||S().alias->scope.controllerIdentity!=witness.scope.controllerIdentity||!Current(witness.scope))return nullptr;
 PipelineProbeRequest current;SelectBridgeProbeLocked(*recorded,elements,instances,current);
 return SameUiTransformProbe(current,probe)?recorded:nullptr;
}
uintptr_t NativeList(){uintptr_t array=__readgsqword(0x58),tls{},context{},list{};uint8_t ready{};if(!ReadPtrSafe(array,&tls)||!tls||!ReadU8Safe(tls+0x14,&ready)||!ready||!ReadPtrSafe(tls+0x188,&context)||!context||!ReadPtrSafe(context+0x30,&list))return 0;return list;}
// Retained native intent is usable only after this fresh TLS -> layout ->
// selected source CPU -> native ID/mip join. A cleared valid bit is not proof.
NativeBind RefreshEditorNative(uintptr_t list,const NativeBind& intent)noexcept{
 NativeBind out{};if(!intent.id||intent.id>65536||intent.slot>=128||intent.stage!=2||NativeList()!=list)return out;
 uintptr_t tls{},context{},state{},layout{},sourceArray{},selected{},manager{},holder{},entries{},cpu{};
 uint8_t tableIndex{},rootIndex{};uint32_t sourceStart{},packed{},mips{};
 if(!ReadPtrSafe(__readgsqword(0x58),&tls)||!tls||!ReadPtrSafe(tls+0x188,&context)||!context||!ReadPtrSafe(context+0x60,&state)||!state||
    !ReadPtrSafe(state+0x68,&layout)||!layout||!ReadU8Safe(layout+0x4C3+uintptr_t(intent.stage*128+intent.slot)*2,&tableIndex)||tableIndex>=64)return out;
 const auto table=layout+0x38+uintptr_t(tableIndex)*0x10;
 if(!ReadU32Safe(table+4,&sourceStart)||!ReadU32Safe(table+8,&packed)||!ReadU8Safe(table+0xE,&rootIndex))return out;
 const auto base=packed&0xffff,count=packed>>16;
 if(intent.slot<base||intent.slot-base>=count||count>128||sourceStart+intent.slot-base>=4096||
    !ReadPtrSafe(state+0x28,&sourceArray)||!sourceArray||!ReadPtrSafe(sourceArray+uintptr_t(sourceStart+intent.slot-base)*8,&selected)||
    !ReadPtrSafe(reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr))+0x3438A28,&manager)||!manager)return out;
 const auto record=manager+uintptr_t(intent.id-1)*0xB0;
 if(!ReadPtrSafe(record+0x2F218,&holder))return out;
 if(holder){if(!ReadU32Safe(holder+0x38,&mips))return out;}
 if(holder&&mips){if(mips>32||intent.mip>=mips||!ReadPtrSafe(holder+0x10,&entries)||!entries||!ReadPtrSafe(entries+uintptr_t(intent.mip)*8,&cpu))return out;}
 else if(!ReadPtrSafe(record+0x2F208,&cpu))return out;
 if(!cpu||cpu!=selected)return out;
 out=intent;out.cpu=cpu;out.layout=layout;out.tableIndex=tableIndex;out.rootIndex=rootIndex;out.relative=intent.slot-base;out.valid=true;return out;
}
UiDescriptorMutation NewMutation(size_t count){return {OwnUiMutation<UiDescriptorMutationData>(++S().serial,count)};}
void SourceDescriptorProbeLocked(const char* trigger){
    const auto& alias=S().alias;if(!alias)return;
    const auto* source=GetDescriptor(alias->readCPU);
    uintptr_t writer{};uint64_t writerGeneration{};bool sameWriter=false;
    for(size_t i=0;i<S().portraitPins.size();++i)if(S().portraitPins[i]){if(!writer){writer=reinterpret_cast<uintptr_t>(S().portraitPins[i].Get());writerGeneration=S().portraitGenerations[i];}if(source&&source->resource==reinterpret_cast<uintptr_t>(S().portraitPins[i].Get())&&source->resourceGeneration==S().portraitGenerations[i]){writer=source->resource;writerGeneration=source->resourceGeneration;sameWriter=true;break;}}
    if(sameWriter&&!S().nativeDiagnostic.writerDescriptorKnown){S().nativeDiagnostic.writerDescriptorKnown=true;++S().nativeDiagnostic.lineageRevision;}
    if(S().nativeDiagnostic.sourceRows>=4)return;++S().nativeDiagnostic.sourceRows;
    Log("[MenuPreviewUI] ui-source-srv trigger=%s epoch=%u setup=%u readID=%u aliasCPU=%llx sourceKnown=%u pending=%u sourceResource=%llx sourceGeneration=%llu originCPU=%llx originVersion=%llu writerMatch=%u writerResource=%llx writerGeneration=%llu; existing SRV ledger only\n",trigger,alias->scope.screenEpoch,alias->scope.setupID,alias->readID,alias->readCPU,unsigned(source!=nullptr),unsigned(source&&source->pending),source?source->resource:0,source?source->resourceGeneration:0,source?source->originCPU:0,source?source->originVersion:0,unsigned(sameWriter),writer,writerGeneration);
}
void DrawDescriptorProbeLocked(const List& list,uintptr_t target){
    const auto& alias=S().alias;auto& diagnostic=S().nativeDiagnostic;
    if(!alias||diagnostic.drawProbeAttempts>=64||diagnostic.drawMeaningfulRows>=8||(diagnostic.drawResourceRows>=4&&!diagnostic.writerDescriptorKnown))return;
    uint32_t matches{},relation{},rootIndex{},relative{};uintptr_t writer{},descriptorCPU{},descriptorGPU{},tableGPU{},originCPU{};uint64_t writerGeneration{};bool tableProven{},heapBound{};
    for(size_t p=0;p<S().portraitPins.size();++p)if(S().portraitPins[p]){writer=reinterpret_cast<uintptr_t>(S().portraitPins[p].Get());writerGeneration=S().portraitGenerations[p];break;}
    if(!writer)return;
    ++diagnostic.drawProbeCalls;
    if(diagnostic.drawProbeAttempts>=8&&diagnostic.drawProbeCalls%32!=0&&diagnostic.lineageRevision==diagnostic.lastScannedRevision)return;
    ++diagnostic.drawProbeAttempts;diagnostic.lastScannedRevision=diagnostic.lineageRevision;
    for(const auto& d:S().descriptors){if(!d.cpu||d.pending||!d.resource)continue;bool writerMatch=false;
        for(size_t p=0;p<S().portraitPins.size();++p)if(S().portraitPins[p]&&d.resource==reinterpret_cast<uintptr_t>(S().portraitPins[p].Get())&&d.resourceGeneration==S().portraitGenerations[p]){writerMatch=true;break;}
        if(!writerMatch)continue;++matches;if(!descriptorCPU){writer=d.resource;writerGeneration=d.resourceGeneration;descriptorCPU=d.cpu;originCPU=d.originCPU;}
        for(const auto& heap:S().heaps){if(!heap.gpu||!heap.increment||d.cpu<heap.cpu)continue;bool bound=false;for(auto h:list.heaps)if(h==heap.owner)bound=true;if(!bound)continue;
            const auto distance=d.cpu-heap.cpu;if(distance%heap.increment||distance/heap.increment>=heap.count)continue;const auto gpu=heap.gpu+distance;
            if(!heapBound){heapBound=true;descriptorCPU=d.cpu;descriptorGPU=gpu;originCPU=d.originCPU;}
            for(uint32_t root=0;root<list.tables.size();++root){const auto& table=list.tables[root];if(!table.gpu||gpu<table.gpu||(gpu-table.gpu)%heap.increment)continue;const auto offset=(gpu-table.gpu)/heap.increment;
                const bool exact=offset==0,proven=table.proven&&table.count&&offset<table.count&&table.cpu+offset*heap.increment==d.cpu;
                const uint32_t candidate=proven?3u:exact?2u:offset<128?1u:0u;if(candidate<=relation)continue;
                relation=candidate;rootIndex=root;relative=uint32_t(offset);writer=d.resource;writerGeneration=d.resourceGeneration;descriptorCPU=d.cpu;descriptorGPU=gpu;tableGPU=table.gpu;originCPU=d.originCPU;tableProven=table.proven;
            }
        }
    }
    if(!matches)diagnostic.writerDescriptorKnown=false;
    const bool improved=(heapBound&&!diagnostic.bestHeapBound)||relation>diagnostic.bestDrawRelation||
        (relation>=2&&tableGPU&&(rootIndex!=diagnostic.bestRootIndex||tableGPU!=diagnostic.bestTableGPU));
    const bool emitMeaningful=improved&&diagnostic.drawMeaningfulRows<8;
    const bool emitNegative=!emitMeaningful&&diagnostic.drawResourceRows<4;
    if(emitMeaningful){++diagnostic.drawMeaningfulRows;diagnostic.bestHeapBound|=heapBound;if(relation>diagnostic.bestDrawRelation)diagnostic.bestDrawRelation=relation;diagnostic.bestRootIndex=rootIndex;diagnostic.bestTableGPU=tableGPU;}
    else if(emitNegative)++diagnostic.drawResourceRows;
    if(emitMeaningful||emitNegative){
        uint32_t registeredCount{},rootCount{},rows{};
        for(const auto& heap:S().heaps)registeredCount+=unsigned(heap.owner!=0);
        for(const auto& table:list.tables)rootCount+=unsigned(table.gpu!=0);
        Log("[MenuPreviewUI] ui-draw-bind-state epoch=%u list=%llx boundHeap0=%llx boundHeap1=%llx boundHeap2=%llx boundHeap3=%llx registeredHeapCount=%u rootTableCount=%u; actual stored UI-list bindings\n",alias->scope.screenEpoch,list.identity,list.heaps[0],list.heaps[1],list.heaps[2],list.heaps[3],registeredCount,rootCount);
        for(uint32_t root=0;root<list.tables.size()&&rows<4;++root){const auto& table=list.tables[root];if(!table.gpu)continue;++rows;
            Log("[MenuPreviewUI] ui-draw-root-state list=%llx rootIndex=%u gpuStart=%llx cpuStart=%llx count=%u proven=%u layout=%llx\n",list.identity,root,table.gpu,table.cpu,table.count,unsigned(table.proven),table.layout);}
    }
    if(emitMeaningful||emitNegative)Log("[MenuPreviewUI] ui-draw-descriptor epoch=%u setup=%u list=%llx target=%llx root=%llx writerResource=%llx writerGeneration=%llu matchingDescriptors=%u descriptorCPU=%llx originCPU=%llx descriptorGPU=%llx rootIndex=%u tableGPU=%llx relative=%u tableProven=%u heapBound=%u relation=%u negativeRows=%u meaningfulRows=%u attempts=%u; 1=unproven-range 2=table-start 3=proven-range, candidate only, no sampling claim\n",alias->scope.screenEpoch,alias->scope.setupID,list.identity,target,list.root,writer,writerGeneration,matches,descriptorCPU,originCPU,descriptorGPU,rootIndex,tableGPU,relative,unsigned(tableProven),unsigned(heapBound),relation,diagnostic.drawResourceRows,diagnostic.drawMeaningfulRows,diagnostic.drawProbeAttempts);
    if(diagnostic.drawProbeAttempts==64)Log("[MenuPreviewUI] ui-draw-descriptor-budget epoch=%u setup=%u attempts=64 attemptsExhausted=1 negativeRows=%u meaningfulRows=%u; further full ledger scans disabled for scope\n",alias->scope.screenEpoch,alias->scope.setupID,diagnostic.drawResourceRows,diagnostic.drawMeaningfulRows);
}
void MaybeOffscreenPreflightSummaryLocked(){
    const auto& alias=S().alias;auto& d=S().nativeDiagnostic;auto& p=d.offscreenPreflight;
    if(!alias||!p.armedAtMs||p.summaries>=4||(p.directSeen&63u))return;
    constexpr uint64_t thresholds[]{1000,5000,15000,45000};const auto now=PublicationNowMs();
    if(now<p.armedAtMs||now-p.armedAtMs<thresholds[p.summaries])return;
    ++p.summaries;
    Log("[MenuPreviewUI] offscreen-preflight-summary epoch=%u setup=%u afterArmMs=%llu directSeen=%llu missingList=%llu overflowSkipped=%llu overflowRows=%u entered=%llu noAlias=%llu staleAlias=%llu notReady=%llu noTargets=%llu onlyBackbufferOrPortrait=%llu noRoot=%llu noPipeline=%llu noBoundHeap=%llu reachedScan=%llu skippedCadence=%llu attempts=%u rows=%u trackedTargets=%u targetGenerationMiss=%llu targetSrvWrites=%llu targetCopies=%llu egressDraws=%llu egressCurrent=%llu egressHeapBound=%llu egressMatches=%llu targetSubmissions=%llu egressSubmissions=%llu traceDrops=%llu; cumulative scoped gates, no sampling claim\n",alias->scope.screenEpoch,alias->scope.setupID,now-p.armedAtMs,p.directSeen,p.missingList,p.overflowSkipped,p.overflowRows,p.entered,p.noAlias,p.staleAlias,p.notReady,p.noTargets,p.onlyBackbufferOrPortrait,p.noRoot,p.noPipeline,p.noBoundHeap,p.reachedScan,p.skippedCadence,d.offscreenAttempts,d.offscreenRows,d.targetCount,d.targetGenerationMiss,d.targetSrvWrites,d.targetCopies,d.egressDraws,d.egressCurrent,d.egressHeapBound,d.egressMatches,d.targetSubmissions,d.egressSubmissions,d.traceDrops);
}
void NoteUiDirectDrawLocked(const List* list){
    if(!OffscreenTraceEnabled())return;auto& pre=S().nativeDiagnostic.offscreenPreflight;++pre.directSeen;
    if(!list){++pre.missingList;MaybeOffscreenPreflightSummaryLocked();return;}
    if(!list->overflow)return;++pre.overflowSkipped;
    if(!pre.armedAtMs)return;
    bool seen=false;for(uint32_t i=0;i<pre.overflowRows;++i)if(pre.overflowSamples[i].list==list->identity&&pre.overflowSamples[i].generation==list->generation)seen=true;
    if(!seen&&pre.overflowRows<pre.overflowSamples.size()){
        pre.overflowSamples[pre.overflowRows++]={list->identity,list->generation};uintptr_t target{};for(auto candidate:list->targets)if(candidate){target=candidate;break;}
        const auto* alias=S().alias.get();
        Log("[MenuPreviewUI] offscreen-overflow-list epoch=%u setup=%u list=%llx generation=%llu objectGeneration=%llu count=%zu closed=%u root=%llx pipeline=%llx target=%llx reason=%s detail0=%llx detail1=%llx skipped=%llu; first two distinct skipped recordings only\n",alias?alias->scope.screenEpoch:0,alias?alias->scope.setupID:0,list->identity,list->generation,list->objectGeneration,list->count,unsigned(list->closed),list->root,list->pipeline,target,OverflowName(list->overflowReason),list->overflowDetail0,list->overflowDetail1,pre.overflowSkipped);
    }
    MaybeOffscreenPreflightSummaryLocked();
}
void ObserveOffscreenDrawLocked(List& list,UINT elements,UINT instances){
    auto& diag=S().nativeDiagnostic;const auto& alias=S().alias;
    // Discovery establishes the intermediate used by the real UI consumer.
    // The caller already holds a fresh preview lease; tracing is optional.
    auto& pre=diag.offscreenPreflight;++pre.entered;
    if(!pre.enteredLogged){pre.enteredLogged=true;Log("[MenuPreviewUI] offscreen-preflight-entered epoch=%u setup=%u routing=1 aliasKnown=%u copyReady=%u list=%llx; first valid observed Draw only\n",alias?alias->scope.screenEpoch:diag.epoch,alias?alias->scope.setupID:diag.setup,unsigned(alias!=nullptr),unsigned(diag.offscreenReady),list.identity);}
    if(!alias){++pre.noAlias;return;}
    if(!Current(alias->scope)){++pre.staleAlias;MaybeOffscreenPreflightSummaryLocked();return;}
    if(!diag.offscreenReady){++pre.notReady;return;}
    if(diag.offscreenAttempts>=128||diag.offscreenRows>=12){MaybeOffscreenPreflightSummaryLocked();return;}
    bool anyTarget=false;uintptr_t target{};for(auto p:list.targets)if(p){anyTarget=true;if(!Buffer(p)){bool portrait=false;for(const auto& pin:S().portraitPins)portrait|=reinterpret_cast<uintptr_t>(pin.Get())==p;if(!portrait){target=p;break;}}}
    if(!anyTarget){++pre.noTargets;MaybeOffscreenPreflightSummaryLocked();return;}
    if(!target){++pre.onlyBackbufferOrPortrait;MaybeOffscreenPreflightSummaryLocked();return;}
    if(!list.root){++pre.noRoot;MaybeOffscreenPreflightSummaryLocked();return;}
    if(!list.pipeline){++pre.noPipeline;MaybeOffscreenPreflightSummaryLocked();return;}
    const Heap* gameHeap=nullptr;for(const auto& heap:S().heaps)if(heap.owner&&heap.gpu&&heap.increment&&heap.count){for(auto bound:list.heaps)if(bound==heap.owner){gameHeap=&heap;break;}if(gameHeap)break;}
    if(!gameHeap){++pre.noBoundHeap;MaybeOffscreenPreflightSummaryLocked();return;}
    ++diag.offscreenCalls;
    // New copies must not exhaust all scans in the first frame. Later draws
    // remain eligible at a fixed bounded sampling cadence.
    if(diag.offscreenAttempts>=8&&diag.offscreenCalls%32!=0){++pre.skippedCadence;MaybeOffscreenPreflightSummaryLocked();return;}
    ++pre.reachedScan;++diag.offscreenAttempts;diag.offscreenLastRevision=diag.offscreenRevision;
    const UiDescriptorEntry* selected=nullptr;const RootTable* selectedTable=nullptr;uint32_t selectedRoot{};uint64_t selectedGpu{},selectedOffset{};uint32_t bestScore{};
    for(const auto& copy:diag.offscreenCopies){if(!copy.cpu)continue;const auto* d=GetDescriptor(copy.cpu);
        if(!d||d->pending||d->version!=copy.version||d->resourceGeneration!=copy.generation||d->originCPU!=alias->readCPU||!d->resource||d->cpu<gameHeap->cpu)continue;
        bool pinned=false;for(size_t eye=0;eye<S().portraitPins.size();++eye)if(S().portraitPins[eye]&&d->resource==reinterpret_cast<uintptr_t>(S().portraitPins[eye].Get())&&d->resourceGeneration==S().portraitGenerations[eye])pinned=true;
        if(!pinned)continue;const auto distance=d->cpu-gameHeap->cpu;if(distance%gameHeap->increment||distance/gameHeap->increment>=gameHeap->count)continue;
        const auto descriptorGPU=gameHeap->gpu+distance;
        for(uint32_t root=0;root<list.tables.size();++root){const auto& table=list.tables[root];if(!table.gpu||descriptorGPU<table.gpu||(descriptorGPU-table.gpu)%gameHeap->increment)continue;
            const auto offset=(descriptorGPU-table.gpu)/gameHeap->increment;
            const bool rangeProven=table.proven&&table.count&&offset<table.count&&table.cpu+offset*gameHeap->increment==d->cpu;
            const uint32_t score=rangeProven?2u:1u;if(score>bestScore){bestScore=score;selected=d;selectedTable=&table;selectedRoot=root;selectedGpu=descriptorGPU;selectedOffset=offset;}
        }
    }
    if(selected){if(bestScore==2)NoteTargetProducedLocked(list,target,*selected,selectedRoot,uint32_t(selectedOffset));const OffscreenSignature signature{list.identity,target,list.root,list.pipeline};bool duplicate=false;for(uint32_t i=0;i<diag.offscreenRows;++i)if(diag.offscreenSignatures[i]==signature)duplicate=true;
        if(!duplicate){diag.offscreenSignatures[diag.offscreenRows++]=signature;
            Log("[MenuPreviewUI] offscreen-portrait-candidate epoch=%u setup=%u list=%llx listGeneration=%llu objectGeneration=%llu targetRTVResource=%llx pso=%llx root=%llx elements=%u instances=%u tableRoot=%u tableGPU=%llx tableProven=%u tableCount=%u tableLayout=%llx descriptorCPU=%llx descriptorGPU=%llx offset=%llu heap=%llx heapCPU=%llx heapGPU=%llx heapCount=%u portraitResource=%llx resourceGeneration=%llu originCPU=%llx originVersion=%llu descriptorVersion=%llu rangeProven=%u attempt=%u; candidate only, shader sample and completion unproven\n",alias->scope.screenEpoch,alias->scope.setupID,list.identity,list.generation,list.objectGeneration,target,list.pipeline,list.root,elements,instances,selectedRoot,selectedTable->gpu,unsigned(selectedTable->proven),selectedTable->count,selectedTable->layout,selected->cpu,selectedGpu,selectedOffset,gameHeap->owner,gameHeap->cpu,gameHeap->gpu,gameHeap->count,selected->resource,selected->resourceGeneration,selected->originCPU,selected->originVersion,selected->version,unsigned(bestScore==2),diag.offscreenAttempts);}}
    if(diag.offscreenAttempts==128)Log("[MenuPreviewUI] offscreen-portrait-budget epoch=%u setup=%u attempts=128 attemptsExhausted=1 rows=%u; no more scoped scans\n",alias->scope.screenEpoch,alias->scope.setupID,diag.offscreenRows);
    MaybeOffscreenPreflightSummaryLocked();
}
void NativeSignificantLocked(uint32_t id,uint32_t slot,uint32_t mip,uint8_t stage,uintptr_t caller,uintptr_t list,uintptr_t layout,uint32_t tableIndex,uint32_t rootIndex,uintptr_t selected,uintptr_t computedCPU,uintptr_t defaultCPU,bool valid,const char* firstFailure){
    const auto& alias=S().alias;if(!alias)return;
    const bool idMatch=id==alias->readID,cpuMatch=computedCPU&&computedCPU==alias->readCPU;
    if(!idMatch&&!cpuMatch)return;
    auto& d=S().nativeDiagnostic;d.idSeen+=unsigned(idMatch);d.cpuEqual+=unsigned(cpuMatch);d.valid+=unsigned(valid);d.recordListKnown+=unsigned(list!=0);
    if(d.rows<8){++d.rows;Log("[MenuPreviewUI] native-alias-probe epoch=%u setup=%u readID=%u id=%u slot=%u mip=%u stage=%u defaultCPU=%llx selected=%llx computedCPU=%llx aliasCPU=%llx list=%llx layout=%llx table=%u root=%u valid=%u firstFail=%s caller=%llx; diagnostic only\n",alias->scope.screenEpoch,alias->scope.setupID,alias->readID,id,slot,mip,unsigned(stage),defaultCPU,selected,computedCPU,alias->readCPU,list,layout,tableIndex,rootIndex,unsigned(valid),firstFailure,caller);}
}
void MaybeNativeSummaryLocked(){
    const auto& alias=S().alias;if(!alias)return;auto& d=S().nativeDiagnostic;const auto calls=S().nativeCalls.load();
    constexpr uint64_t thresholds[]{256,2048,16384,131072};
    if(d.summaryRows>=4||calls-d.startCalls<thresholds[d.summaryRows])return;
    ++d.summaryRows;
    Log("[MenuPreviewUI] native-alias-summary epoch=%u setup=%u readID=%u aliasCPU=%llx aliasChanges=%u nativeCallDelta=%llu examined=%u idSeen=%u srvCPUequal=%u valid=%u recordListKnown=%u; scoped counters, no UI submission implied\n",alias->scope.screenEpoch,alias->scope.setupID,alias->readID,alias->readCPU,d.aliasChanges,calls-d.startCalls,d.examined,d.idSeen,d.cpuEqual,d.valid,d.recordListKnown);
}

}
bool AttachEditorUiPublication(ID3D12CommandQueue* queue,uintptr_t list,uint64_t recordingGeneration,
 uintptr_t targetId,uint64_t targetGeneration,std::shared_ptr<EditorUiPublication> publication,bool required)noexcept{
 if(!EditorUiSeparationEnabled()||!queue||!list||!targetId||S().failed.load())return false;
 try{
  std::lock_guard lock(S().mutex);const auto q=reinterpret_cast<uintptr_t>(queue);const auto* l=FindList(list);auto* target=FindTrackedTargetLocked(targetId,targetGeneration);
  if(!l||!l->submitted||l->generation!=recordingGeneration||!target||target->editorList!=list||target->editorLife!=l->objectGeneration||target->editorRecording!=recordingGeneration||!target->editorSubmission||target->editorSubmission!=target->qualifiedSubmission||target->editorSubmission!=S().submission||target->qualifiedQueue!=q||!target->qualifiedApp||!Current(target->editorScope))return false;
  const auto app=target->qualifiedApp;
  if(!SamePreview(app->metadata,target->editorScope)||app->metadata.controllerIdentity!=target->editorScope.controllerIdentity)return false;
  const bool complete=publication&&publication->image&&publication->image->canvas&&publication->image->producerFence&&publication->image->producerValue&&publication->renderAspect>0;
  std::shared_ptr<const EditorUiPublication> accepted;
  if(complete){publication->scope=app->metadata;publication->sourceSequence=app->frame.sourceSequence;publication->cameraRevision=app->localPoseRevision;publication->eye=app->frame.eye;publication->nativeFrame=app->frame.nativeFrame;accepted=std::move(publication);}
  for(auto& t:S().nativeDiagnostic.targets)if(t.qualifiedApp&&t.qualifiedQueue==q&&t.qualifiedSubmission==target->editorSubmission&&t.qualifiedApp->frame.sourceSequence==app->frame.sourceSequence&&t.qualifiedApp->localPoseRevision==app->localPoseRevision&&t.qualifiedApp->frame.eye==app->frame.eye&&t.qualifiedApp->frame.nativeFrame==app->frame.nativeFrame&&SamePreview(t.qualifiedApp->metadata,app->metadata)&&t.qualifiedApp->metadata.controllerIdentity==app->metadata.controllerIdentity){t.editorUiRequired=required;t.editorUi=accepted;}
  // UiSubmitted may already have propagated this exact application to a BB.
  // Replace its immutable receipt; never patch a different source/queue/scope.
  for(auto& b:S().buffers)if(b.receipt&&b.receipt->queue==q&&b.receipt->uiSubmission==target->editorSubmission&&b.receipt->sourceSequence==app->frame.sourceSequence&&b.receipt->cameraRevision==app->localPoseRevision&&b.receipt->eye==app->frame.eye&&SamePreview(b.receipt->scope,app->metadata)&&b.receipt->scope.controllerIdentity==app->metadata.controllerIdentity){auto copy=std::make_shared<UiFrameReceipt>(*b.receipt);copy->editorUiRequired=required;copy->editorUi=accepted;b.receipt=std::move(copy);}
  return complete||!required;
 }catch(...){return false;}
}
namespace {
struct EditorClassifierWitness {
 const UiEditorDrawProof& out;
 const char* stage="target",*reason="not-qualified";
 uintptr_t pipeline{},root{},cpu{};uint64_t ps{},vs{};
 uint32_t used{},nativeKnown{},ledgerKnown{},unknownCount{},unknownMask{},first=UINT32_MAX;
 uint32_t srvRoot=UINT32_MAX,offset{},intentID{},freshID{};
 bool tableProven{},freshValid{},present{},pending{},unordered{},cbv{},overlap{},conflict{};
 ~EditorClassifierWitness()noexcept{
  if(!out.targetQualified||out.kind!=UiEditorDrawKind::Unknown)return;
  try{
   struct Seen {uint64_t ps{};uintptr_t pipeline{};const char* reason{};};
   static std::mutex mutex;static uint32_t epoch{},count{};static std::array<Seen,8> seen{};
   {std::lock_guard lock(mutex);if(out.scope.screenEpoch<epoch)return;
    if(out.scope.screenEpoch!=epoch){epoch=out.scope.screenEpoch;count=0;seen={};}
    for(uint32_t i=0;i<count;++i)if(seen[i].ps==ps&&(ps||seen[i].pipeline==pipeline)&&seen[i].reason==reason)return;
    if(count==seen.size())return;seen[count++]={ps,pipeline,reason};}
   Log("[EditorUI-ClassifierUnknown] epoch=%u setup=%u list=%llx life=%llu recording=%llu target=%llx targetGeneration=%llu stage=%s reason=%s pso=%llx root=%llx PS=%016llX VS=%016llX usedSRVs=%u nativeKnown=%u ledgerKnown=%u unknownCount=%u unknownMask=%X conflict=%u firstIssue=%u srvRoot=%u offset=%u cpu=%llx tableProven=%u intentID=%u freshID=%u freshValid=%u present=%u pending=%u unordered=%u cbv=%u overlap=%u; first distinct PS/reason, max8 per epoch, CPU classifier only\n",
    out.scope.screenEpoch,out.scope.setupID,out.list,out.objectGeneration,out.recordingGeneration,out.targetResource,out.targetGeneration,
    stage,reason,pipeline,root,ps,vs,used,nativeKnown,ledgerKnown,unknownCount,unknownMask,unsigned(conflict),first,srvRoot,offset,cpu,
    unsigned(tableProven),intentID,freshID,unsigned(freshValid),unsigned(present),unsigned(pending),unsigned(unordered),unsigned(cbv),unsigned(overlap));
  }catch(...){} // Diagnostic loss cannot fail the pixel consumer.
 }
};
}
UiEditorDrawProof InspectEditorUiDraw(ID3D12GraphicsCommandList* list,UINT elements,UINT instances)noexcept{
 UiEditorDrawProof out;
 if(!EditorUiSeparationEnabled()||!UiConsumerEnabled()||S().failed.load()||!list||!elements||!instances)return out;
 EditorClassifierWitness diag{out};
 try{
  Lease lease;if(!TryAcquireCurrent(lease)||lease.metadata.screen!=2)return out;
  out.kind=UiEditorDrawKind::Unknown;out.scope=lease.metadata;out.scopeLifetime=lease.lifetime;out.list=reinterpret_cast<uintptr_t>(list);out.nativeNode=cvr::detail::t_current_node_work;
  if(NativeList()!=out.list||out.nativeNode!=reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr))+0x20A264)return out;
  uintptr_t root{},pipeline{};ComPtr<ID3D12RootSignature> rootPin;ComPtr<ID3D12PipelineState> pipelinePin;
  {std::lock_guard lock(S().mutex);auto* l=FindList(out.list);if(!l||l->closed||l->submitted||l->overflow||!S().alias||!SamePreview(S().alias->scope,out.scope)||S().alias->scope.controllerIdentity!=out.scope.controllerIdentity)return out;
   out.objectGeneration=l->objectGeneration;out.recordingGeneration=l->generation;out.aliasID=S().alias->readID;out.aliasLifetime=S().alias;
   uint32_t count{};for(size_t i=0;i<l->targets.size();++i)if(l->targets[i]){++count;out.targetResource=l->targets[i];out.targetGeneration=l->targetGenerations[i];}if(count!=1||!out.targetGeneration||Buffer(out.targetResource))return out;
   out.targetOwner=reinterpret_cast<ID3D12Resource*>(out.targetResource);
   auto* target=FindTrackedTargetLocked(out.targetResource,out.targetGeneration);
   if(!target||!target->editorSubmission||!EditorTargetMatches({target->editorScope.screenEpoch,target->editorScope.setupID,target->editorScope.controllerIdentity,target->editorList,target->editorLife,target->resource,target->generation,target->editorNode},{out.scope.screenEpoch,out.scope.setupID,out.scope.controllerIdentity,out.list,out.objectGeneration,out.targetResource,out.targetGeneration,out.nativeNode})||!SamePreview(target->editorScope,out.scope))return out;
   out.targetQualified=true;out.queue=target->qualifiedQueue;out.qualifiedSubmission=target->editorSubmission;
   diag.stage="pipeline";diag.reason="missing-root-or-pso";
   root=l->root;pipeline=l->pipeline;diag.root=root;diag.pipeline=pipeline;if(!root||!pipeline)return out;rootPin=reinterpret_cast<ID3D12RootSignature*>(root);pipelinePin=reinterpret_cast<ID3D12PipelineState*>(pipeline);
  }
  // Cache locks and root deserialization are always outside S().mutex.
  diag.stage="psv-query";diag.reason="query-incomplete";
  const auto locations=UsedTransferLocations(pipeline,root);
  diag.ps=locations.shader.psHash;diag.vs=locations.shader.vsHash;diag.used=locations.count;
  diag.stage="psv-layout";diag.reason=!locations.shaderKnown?"shader-unknown":!locations.known?"srv-layout-unknown":locations.count!=locations.shader.srvBindingCount?"srv-layout-count":!locations.shader.colorOutputsKnown?"color-outputs-unknown":"color-output-mask";
  if(!locations.shaderKnown||!locations.known||locations.count!=locations.shader.srvBindingCount||!locations.shader.colorOutputsKnown||locations.shader.colorOutputMask!=1)return out;
  diag.reason="non-singleton-or-non-texture2d-srv";
  for(uint32_t i=0;i<locations.shader.srvBindingCount;++i)if(locations.shader.srvBindings[i].resourceKind!=2||locations.shader.srvBindings[i].firstRegister!=locations.shader.srvBindings[i].lastRegister)return out;
  // The observed editor VS has no texture reads; unknown vertex shader texture
  // usage cannot provide a negative proof from pixel PSV alone.
  diag.stage="vertex-shader";diag.reason="vs-not-exact-qualified-ui";
  if(locations.shader.vsHash!=0x8D749B2D19263CDFull||locations.shader.vsBytes!=3042)return out;
  struct Binding {RootTable table{};NativeBind intent{},fresh{};uintptr_t cpu{};UiDescriptorEntry descriptor{};bool present{};};
  std::array<Binding,16> bindings{};std::array<uintptr_t,4> heaps{};uintptr_t aliasCPU{};UiDescriptorEntry aliasDescriptor{};
  diag.stage="binding-snapshot";diag.reason="list-or-alias-changed";
  {std::lock_guard lock(S().mutex);auto* l=FindList(out.list);if(!l||l->objectGeneration!=out.objectGeneration||l->generation!=out.recordingGeneration||l->closed||l->submitted||l->root!=root||l->pipeline!=pipeline||S().alias.get()!=out.aliasLifetime.get())return out;
   heaps=l->heaps;aliasCPU=S().alias->readCPU;if(const auto* a=GetDescriptor(aliasCPU))aliasDescriptor=*a;
    for(uint32_t n=0;n<locations.count;++n){const auto loc=locations.rows[n];if(loc.rootIndex>=l->tables.size()){diag.reason="root-index-out-of-range";diag.first=n;diag.srvRoot=loc.rootIndex;diag.offset=loc.descriptorOffset;return out;}auto& b=bindings[n];b.table=l->tables[loc.rootIndex];
     for(const auto& intent:l->native)if(intent.id&&intent.stage==2&&intent.rootIndex==loc.rootIndex&&intent.relative==loc.descriptorOffset){if(b.intent.id){diag.reason="multiple-native-intents";diag.first=n;diag.srvRoot=loc.rootIndex;diag.offset=loc.descriptorOffset;return out;}b.intent=intent;}
    for(const auto& h:S().heaps){bool bound=false;for(auto id:heaps)bound|=id==h.owner;if(!bound||!h.owner||!h.gpu||!h.increment||b.table.gpu<h.gpu)continue;const auto delta=b.table.gpu-h.gpu;
     if(delta%h.increment||delta/h.increment>=h.count||b.table.count>h.count-delta/h.increment||!b.table.proven||b.table.cpu!=h.cpu+delta||loc.descriptorOffset>=b.table.count)continue;
     b.cpu=b.table.cpu+uintptr_t(loc.descriptorOffset)*h.increment;if(const auto* d=GetDescriptor(b.cpu)){b.descriptor=*d;b.present=true;}break;
    }
   }
  }
  diag.stage="native-binding-refresh";diag.reason="refresh-incomplete";
  for(uint32_t n=0;n<locations.count;++n)bindings[n].fresh=RefreshEditorNative(out.list,bindings[n].intent);
  diag.stage="binding-freshness";diag.reason="native-list-node-or-scope-changed";
  if(NativeList()!=out.list||cvr::detail::t_current_node_work!=out.nativeNode||!Current(out.scope))return out;
  EditorDrawEvidence evidence{true,true,true,true,locations.count};
  const char* unknownReason="srv-unresolved";
  const auto firstIssue=[&](uint32_t n){if(diag.first!=UINT32_MAX)return;const auto& b=bindings[n];const auto loc=locations.rows[n];
   diag.first=n;diag.srvRoot=loc.rootIndex;diag.offset=loc.descriptorOffset;diag.cpu=b.cpu;diag.tableProven=b.table.proven;
   diag.intentID=b.intent.id;diag.freshID=b.fresh.id;diag.freshValid=b.fresh.valid;diag.present=b.present;
   diag.pending=b.descriptor.pending;diag.unordered=b.descriptor.unorderedAccess;diag.cbv=b.descriptor.cbvOnly;diag.overlap=b.descriptor.overlap;};
  diag.stage="binding-revalidation";diag.reason="list-root-heaps-or-alias-changed";
  {std::lock_guard lock(S().mutex);auto* l=FindList(out.list);if(!l||l->objectGeneration!=out.objectGeneration||l->generation!=out.recordingGeneration||l->closed||l->submitted||l->overflow||l->root!=root||l->pipeline!=pipeline||l->heaps!=heaps||S().alias.get()!=out.aliasLifetime.get())return out;
    diag.reason="target-binding-changed";
    uint32_t targets{};for(size_t i=0;i<l->targets.size();++i)if(l->targets[i]){++targets;if(l->targets[i]!=out.targetResource||l->targetGenerations[i]!=out.targetGeneration)return out;}if(targets!=1)return out;
    diag.reason="alias-descriptor-changed";
   const auto* currentAlias=GetDescriptor(aliasCPU);if(bool(currentAlias)!=bool(aliasDescriptor.cpu)||(currentAlias&&(currentAlias->version!=aliasDescriptor.version||currentAlias->pending!=aliasDescriptor.pending||currentAlias->resource!=aliasDescriptor.resource||currentAlias->resourceGeneration!=aliasDescriptor.resourceGeneration)))return out;
    diag.reason="target-qualification-changed";
    const auto* target=FindTrackedTargetLocked(out.targetResource,out.targetGeneration);if(!target||target->editorSubmission!=out.qualifiedSubmission||!SamePreview(target->editorScope,out.scope))return out;
    for(uint32_t n=0;n<locations.count;++n){auto& b=bindings[n];const auto loc=locations.rows[n];const auto& now=l->tables[loc.rootIndex];if(now.gpu!=b.table.gpu||now.cpu!=b.table.cpu||now.layout!=b.table.layout||now.tableIndex!=b.table.tableIndex||now.count!=b.table.count||now.proven!=b.table.proven){diag.reason="root-table-changed";firstIssue(n);return out;}
     const auto* d=b.cpu?GetDescriptor(b.cpu):nullptr;if(bool(d)!=b.present||(d&&(d->version!=b.descriptor.version||d->resource!=b.descriptor.resource||d->resourceGeneration!=b.descriptor.resourceGeneration||d->pending!=b.descriptor.pending))){diag.reason="descriptor-version-or-resource-changed";firstIssue(n);return out;}
     if(b.intent.id&&!(l->native[b.intent.stage*128+b.intent.slot]==b.intent)){diag.reason="native-intent-changed";firstIssue(n);return out;}
    // A known overwrite/UAV/CBV conflict defeats even a native-ID negative
    // proof. Missing resource ledger is allowed; known mutation is not.
     if(d&&(d->pending||d->unorderedAccess||d->cbvOnly||d->overlap)){++diag.unknownCount;diag.unknownMask|=1u<<n;if(diag.first==UINT32_MAX)unknownReason="descriptor-mutation-or-kind-conflict";firstIssue(n);evidence.unknown=true;continue;}
    const bool native=b.cpu&&b.fresh.valid&&UiSelectedBinding(b.fresh.layout,b.fresh.tableIndex,b.fresh.rootIndex,b.fresh.relative,now.layout,now.tableIndex,loc.rootIndex,now.count)&&b.fresh.relative==loc.descriptorOffset;
    const bool ledger=d&&!d->pending&&!d->unorderedAccess&&d->resource&&d->resourceGeneration&&!aliasDescriptor.pending&&!aliasDescriptor.unorderedAccess&&aliasDescriptor.resource&&aliasDescriptor.resourceGeneration;
     diag.nativeKnown+=unsigned(native);diag.ledgerKnown+=unsigned(ledger);
    const bool nativeAlias=native&&b.fresh.id==out.aliasID&&b.fresh.cpu==aliasCPU;
    const bool nativeOther=native&&b.fresh.id!=out.aliasID&&b.fresh.cpu!=aliasCPU;
    const bool ledgerAlias=ledger&&d->resource==aliasDescriptor.resource&&d->resourceGeneration==aliasDescriptor.resourceGeneration;
    const bool ledgerOther=ledger&&!ledgerAlias;
     if(native&&(b.fresh.id==out.aliasID)!=(b.fresh.cpu==aliasCPU)){firstIssue(n);evidence.conflict=true;}
     if((nativeAlias&&ledgerOther)||(nativeOther&&ledgerAlias)){firstIssue(n);evidence.conflict=true;}
    if(nativeAlias||ledgerAlias){++evidence.alias;out.nativeBindingKnown=native;out.nativeID=b.fresh.id;out.descriptorCPU=b.cpu;out.descriptorVersion=d?d->version:0;out.sourceResource=aliasDescriptor.resource;out.sourceGeneration=aliasDescriptor.resourceGeneration;}
     else if(nativeOther||ledgerOther)++evidence.nonAlias;else {++diag.unknownCount;diag.unknownMask|=1u<<n;if(diag.first==UINT32_MAX)unknownReason=!b.cpu?"table-or-heap-unproven":!b.intent.id?"native-intent-missing-and-ledger-unknown":!b.fresh.valid?"native-refresh-invalid-and-ledger-unknown":"native-range-unproven-and-ledger-unknown";firstIssue(n);evidence.unknown=true;}
   }
   if(out.sourceResource)for(size_t i=0;i<S().portraitPins.size();++i)if(reinterpret_cast<uintptr_t>(S().portraitPins[i].Get())==out.sourceResource&&S().portraitGenerations[i]==out.sourceGeneration)out.sourceOwner=S().portraitPins[i];
    const Writer* exactWriter{};std::shared_ptr<const CameraApplication> exactLatest;
    if(out.sourceResource)for(const auto& w:S().writers){
     const auto currentApplication=w.app?ReadCurrentApplicationForSource(*w.app):nullptr;
     if(w.app&&reinterpret_cast<uintptr_t>(w.resource.Get())==out.sourceResource&&w.generation==out.sourceGeneration&&SamePreview(w.app->metadata,out.scope)){exactWriter=&w;exactLatest=currentApplication;}
    if(EditorFreshnessEnabled()&&w.app&&reinterpret_cast<uintptr_t>(w.resource.Get())==out.sourceResource&&w.generation==out.sourceGeneration&&SamePreview(w.app->metadata,out.scope)){
     FreshnessSample p;p.epoch=out.scope.screenEpoch;p.setup=out.scope.setupID;p.nativeFrame=w.app->frame.nativeFrame;p.eye=w.app->frame.eye;p.source=w.app->frame.sourceSequence;p.revision=w.app->localPoseRevision;p.previousPointer=reinterpret_cast<uintptr_t>(w.app.get());p.nextPointer=reinterpret_cast<uintptr_t>(currentApplication.get());p.otherSource=currentApplication?currentApplication->frame.sourceSequence:0;p.resource=out.sourceResource;p.generation=out.sourceGeneration;
     NoteEditorFreshness(w.app==currentApplication?FreshnessEvent::WriterMatch:FreshnessEvent::WriterMismatch,FreshnessStage::Writer,p);
    }
     if(w.app&&CameraApplicationUiSource(*w.app)&&w.app==currentApplication&&reinterpret_cast<uintptr_t>(w.resource.Get())==out.sourceResource&&w.generation==out.sourceGeneration&&SamePreview(w.app->metadata,out.scope))out.application=w.app;
   }
    if(out.sourceResource&&lease.source&&lease.source->mode==NativePreviewMode::NativeEditorDual){
     static uint64_t checks{};const auto check=++checks;
     if(check<=4||check==128||check==1024){const auto* writer=exactWriter?exactWriter->app.get():nullptr;const auto* latest=exactLatest.get();
      const char* reason=!writer?"no-exact-submitted-writer":!CameraApplicationUiSource(*writer)?"not-primary-ui-source":!latest?"no-current-source-application":writer!=latest?"writer-application-not-current":"accepted";
      Log("[EditorNativeUIWriterGate] check=%llu reason=%s epoch=%u setup=%u resource=%llx generation=%llu writer=%llx writerFrame=%u writerSource=%llu writerEye=%u latest=%llx latestFrame=%u latestSource=%llu latestEye=%u aliasResource=%llx aliasGeneration=%llu sourcePinned=%u; existing exact current-application gate unchanged\n",check,reason,out.scope.screenEpoch,out.scope.setupID,out.sourceResource,out.sourceGeneration,reinterpret_cast<uintptr_t>(writer),writer?writer->frame.nativeFrame:0,writer?writer->frame.sourceSequence:0,writer?writer->frame.eye:0,reinterpret_cast<uintptr_t>(latest),latest?latest->frame.nativeFrame:0,latest?latest->frame.sourceSequence:0,latest?latest->frame.eye:0,aliasDescriptor.resource,aliasDescriptor.resourceGeneration,unsigned(bool(out.sourceOwner)));
     }
    }
  }
   diag.stage="classify";diag.conflict=evidence.conflict;diag.reason=evidence.conflict?"native-ledger-alias-conflict":evidence.unknown?unknownReason:"non-alias-count-incomplete";
   out.kind=ClassifyEditorDraw(evidence);if(out.application){out.associationKnown=true;out.sourceSequence=out.application->frame.sourceSequence;out.eye=out.application->frame.eye;out.nativeFrame=out.application->frame.nativeFrame;}
  }catch(...){diag.stage="exception";diag.reason="classifier-exception";out.kind=UiEditorDrawKind::Unknown;}
 return out;
}

// Opaque to callers; every member is a copied value, never a GPU owner.
struct UiProducerInputs {
 UiRawDrawReceipt draw{};CameraApplication application{};uintptr_t list{},resource{};uint64_t resourceGeneration{};
 PanelWitnessListKey uiKey{};PanelBindingSnapshot binding{};UiTransformWitness witness{};ProducerSrvRequests requests{};bool requestShaderKnown{},requestShaderMatch{};
};
bool MatchUiProducerInputs(const std::shared_ptr<const UiProducerInputs>& data,const UiRawDrawReceipt& draw,const CameraApplication& app,uintptr_t list,uintptr_t resource)noexcept{
 return data&&data->draw==draw&&draw.pixelGeneration&&draw.drawOrder&&data->list==list&&data->resource==resource&&SameCameraApplication(data->application,app);
}
static std::shared_ptr<const UiProducerInputs> CaptureProducerInputs(ID3D12GraphicsCommandList* list,ID3D12Resource* resource,const CameraApplication& app,const UiRawDrawReceipt& draw)noexcept{
 try{
  if(!PreviewPixelsRequested()||!list||!resource||app.metadata.screen!=2||app.frame.eye>1||!draw.pixelGeneration||!draw.drawOrder)return {};
  auto data=std::make_shared<UiProducerInputs>();data->draw=draw;data->application=app;data->list=reinterpret_cast<uintptr_t>(list);data->resource=reinterpret_cast<uintptr_t>(resource);
  PanelDescriptorBank bank;auto& binding=data->binding;auto& witness=data->witness;data->requests=RequestedProducerSrvs();
  {std::lock_guard lock(S().mutex);auto* current=FindList(data->list);if(!current||current->closed||current->overflow||!Current(app.metadata))return {};
   data->uiKey=PanelKey(*current);binding.pipeline=current->pipeline;binding.root=current->root;binding.targets=current->targets;binding.generations=current->targetGenerations;binding.extents=current->targetExtents;
   witness.scope=app.metadata;witness.bindings=current->transform;witness.tables=current->tables;witness.heaps=current->heaps;
   for(unsigned i=0;i<4;++i)if(current->targets[i]==data->resource)data->resourceGeneration=current->targetGenerations[i];
   if(!data->resourceGeneration)return {};
   // Freeze a bounded bank BEFORE out-of-lock reflection, never newer descriptors afterwards.
   if(data->requests.valid&&data->requests.expectedKnown)for(uint32_t i=0;i<data->requests.count;++i){auto& r=data->requests.rows[i];CapturePanelDescriptorLocked(*current,r.root,r.offset,bank,nullptr,true,&r.reason);}
   for(const auto& native:current->native)if(native.valid)CapturePanelDescriptorLocked(*current,native.rootIndex,native.relative,bank,&native,true);
   for(uint32_t root=0;root<current->tables.size();++root){const auto& table=current->tables[root];if(!table.proven)continue;
    for(uint32_t off=0;off<std::min<uint32_t>(table.count,4);++off)CapturePanelDescriptorLocked(*current,root,off,bank,nullptr,true);
   }
   binding.bankDrops=bank.dropped;
  }
  PipelineShaderEvidence shaders;binding.shaderKnown=QueryPipelineShaders(binding.pipeline,shaders);
  if(binding.shaderKnown){binding.ps=shaders.psHash;binding.vs=shaders.vsHash;binding.psBytes=shaders.psBytes;binding.vsBytes=shaders.vsBytes;}
  data->requestShaderKnown=data->requests.valid&&data->requests.expectedKnown&&binding.shaderKnown;data->requestShaderMatch=data->requestShaderKnown&&binding.ps==data->requests.ps&&binding.vs==data->requests.vs;
  std::array<ShaderSrvLocation,16> locations{};uint32_t count{};
  binding.srvsKnown=binding.shaderKnown&&shaders.srvBindingsKnown&&ResolvePixelSrvLocations(binding.root,shaders,locations,count);
  if(binding.srvsKnown){binding.srvTotal=count;binding.srvCount=std::min<uint32_t>(count,uint32_t(binding.srvs.size()));binding.srvDrops=count-binding.srvCount;
   for(uint32_t i=0;i<binding.srvCount;++i){const auto loc=locations[i];auto& out=binding.srvs[i];if(const auto* captured=bank.Find(loc.rootIndex,loc.descriptorOffset))out=captured->srv;
    else{out.root=loc.rootIndex;out.offset=loc.descriptorOffset;if(loc.rootIndex<witness.tables.size()){out.tableGPU=witness.tables[loc.rootIndex].gpu;out.tableProven=witness.tables[loc.rootIndex].proven;}}
    RootDescriptorEvidence reg;out.registerKnown=ResolveRootDescriptor(binding.root,loc.rootIndex,loc.descriptorOffset,reg);if(out.registerKnown){out.shaderRegister=reg.shaderRegister;out.space=reg.registerSpace;}
   }
  }
  for(unsigned i=0;i<2;++i){VertexConstantLocation loc;binding.constantResolved[i]=ResolveVertexConstantLocation(binding.root,i?5:1,0,loc);binding.constantRoots[i]=loc.rootIndex;binding.constantOffsets[i]=loc.descriptorOffset;binding.constantTable[i]=loc.table;
   if(!binding.constantResolved[i]||loc.rootIndex>=witness.tables.size())continue;auto& proof=binding.constants[i];
   if(!loc.table){proof.bound=(witness.bindings.cbvMask&(uint64_t{1}<<loc.rootIndex))!=0;proof.gpu=proof.bound?witness.bindings.cbvs[loc.rootIndex]:0;}
   else if(loc.descriptorOffset<=UINT32_MAX)if(const auto* captured=bank.Find(loc.rootIndex,uint32_t(loc.descriptorOffset)))proof=captured->constant;
  }
  {std::lock_guard lock(S().mutex);auto* current=FindList(data->list);
   if(!current||PanelKey(*current)!=data->uiKey||current->closed||current->overflow||!Current(app.metadata)||current->pipeline!=binding.pipeline||current->root!=binding.root||current->targets!=binding.targets||current->targetGenerations!=binding.generations||!SameUiTransformBindings(current->transform,witness.bindings)||!SameUiTransformTables(*current,witness))return {};
   for(uint32_t i=0;i<bank.count;++i){const auto& captured=bank.entries[i].srv;const auto* now=FindDescriptorDiagnostic(captured.cpu);
    if(captured.descriptorPresent&&(!now||now->version!=captured.version||now->resource!=captured.resource||now->resourceGeneration!=captured.generation||now->pending!=captured.pending||now->unorderedAccess!=captured.unordered))return {};
   }
  }
  return data;
 }catch(...){return {};}
}
void WriteUiProducerInputsJson(std::ostream& out,const std::shared_ptr<const UiProducerInputs>& p){
 struct FormatRestore {std::ostream& out;std::ios::fmtflags flags;char fill;std::streamsize precision;~FormatRestore(){out.flags(flags);out.fill(fill);out.precision(precision);}} restore{out,out.flags(),out.fill(),out.precision()};
 out.flags(std::ios::dec);
 if(!p){out<<"{\"status\":\"UNKNOWN\",\"reason\":\"missing_stale_or_untracked\"}";return;}
 const auto& b=p->binding;const auto& d=p->draw;const auto& app=p->application;
 const auto requestStatus=!p->requests.present?"NOT_REQUESTED":!p->requests.valid?"INVALID":!p->requests.expectedKnown?"NO_EXPECTED_SHADER":"VALID";
 out<<"{\"status\":\"OBSERVED_TRACKED_VALUES\",\"pixel_recording_generation\":"<<d.pixelGeneration<<",\"draw_order\":"<<d.drawOrder<<",\"list\":"<<p->list<<",\"resource\":"<<p->resource
    <<",\"source_sequence\":"<<app.frame.sourceSequence<<",\"native_frame\":"<<app.frame.nativeFrame<<",\"eye\":"<<app.frame.eye<<",\"epoch\":"<<app.metadata.screenEpoch<<",\"setup\":"<<app.metadata.setupID
    <<",\"ui_recording_generation\":"<<p->uiKey.generation<<",\"ui_object_generation\":"<<p->uiKey.life<<",\"ui_resource_generation\":"<<p->resourceGeneration
    <<",\"indexed\":"<<(d.indexed?"true":"false")<<",\"elements\":"<<d.elements<<",\"instances\":"<<d.instances<<",\"start_element\":"<<d.startElement<<",\"start_instance\":"<<d.startInstance<<",\"base_vertex\":"<<d.baseVertex
    <<",\"pso\":"<<b.pipeline<<",\"root_signature\":"<<b.root<<",\"shader_known\":"<<(b.shaderKnown?"true":"false")<<",\"ps_hash\":\""<<std::hex<<std::setw(16)<<std::setfill('0')<<b.ps<<"\",\"vs_hash\":\""<<std::setw(16)<<b.vs<<std::dec<<"\",\"ps_bytes\":"<<b.psBytes<<",\"vs_bytes\":"<<b.vsBytes
    <<",\"srvs_known\":"<<(b.srvsKnown?"true":"false")<<",\"srv_total\":"<<b.srvTotal<<",\"srv_drops\":"<<b.srvDrops<<",\"bank_drops\":"<<b.bankDrops<<",\"request_status\":\""<<requestStatus<<"\",\"request_shader_match\":\""<<(p->requestShaderKnown?(p->requestShaderMatch?"MATCH":"MISMATCH"):"UNKNOWN")<<"\",\"requested_srvs\":[";
 for(uint32_t i=0;i<p->requests.count;++i){if(i)out<<',';const auto& r=p->requests.rows[i];out<<"{\"root\":"<<r.root<<",\"offset\":"<<r.offset<<",\"reason\":\""<<ProducerDescriptorReasonName(r.reason)<<"\"}";}out<<"],\"targets\":[";
 for(unsigned i=0;i<4;++i){if(i)out<<',';out<<"{\"resource\":"<<b.targets[i]<<",\"generation\":"<<b.generations[i]<<'}';}
 out<<"],\"heaps\":[";for(unsigned i=0;i<4;++i){if(i)out<<',';out<<p->witness.heaps[i];}
 out<<"],\"root_tables\":[";bool comma=false;for(unsigned i=0;i<p->witness.tables.size();++i){const auto& t=p->witness.tables[i];if(!t.proven)continue;if(comma)out<<',';comma=true;out<<"{\"root\":"<<i<<",\"gpu\":"<<t.gpu<<",\"cpu\":"<<t.cpu<<",\"count\":"<<t.count<<",\"proven\":true}";}
 out<<"],\"srvs\":[";for(uint32_t i=0;i<b.srvCount;++i){if(i)out<<',';const auto& v=b.srvs[i];out<<"{\"root\":"<<v.root<<",\"offset\":"<<v.offset<<",\"register_known\":"<<(v.registerKnown?"true":"false")<<",\"t\":"<<v.shaderRegister<<",\"space\":"<<v.space<<",\"table_gpu\":"<<v.tableGPU<<",\"cpu\":"<<v.cpu<<",\"descriptor_present\":"<<(v.descriptorPresent?"true":"false")<<",\"version\":"<<v.version<<",\"resource\":"<<v.resource<<",\"generation\":"<<v.generation<<",\"pending\":"<<(v.pending?"true":"false")<<",\"unordered\":"<<(v.unordered?"true":"false")<<",\"native_known\":"<<(v.nativeKnown?"true":"false")<<",\"native_id\":"<<v.nativeID<<",\"native_slot\":"<<v.nativeSlot<<",\"native_stage\":"<<v.nativeStage<<'}';}
 out<<"],\"constants\":[";for(unsigned i=0;i<2;++i){if(i)out<<',';const auto& v=b.constants[i];out<<"{\"b\":"<<(i?5:1)<<",\"resolved\":"<<(b.constantResolved[i]?"true":"false")<<",\"root\":"<<b.constantRoots[i]<<",\"table\":"<<(b.constantTable[i]?"true":"false")<<",\"offset\":"<<b.constantOffsets[i]<<",\"bound\":"<<(v.bound?"true":"false")<<",\"gpu\":"<<v.gpu<<",\"bytes\":"<<v.bytes<<",\"descriptor_present\":"<<(v.descriptorPresent?"true":"false")<<",\"pending\":"<<(v.pending?"true":"false")<<",\"payload_captured\":false}";}
 const auto& v=p->witness.bindings.vertex0;out<<"],\"vertex0\":{\"known\":"<<(p->witness.bindings.vertex0Known?"true":"false")<<",\"gpu\":"<<v.BufferLocation<<",\"bytes\":"<<v.SizeInBytes<<",\"stride\":"<<v.StrideInBytes<<",\"payload_captured\":false},\"clear_observed\":false,\"scope\":\"single matched Draw post-return CPU state; not GPU-used input contents or whole producer passes\"}";
}

bool UiConsumerEnabled()noexcept{static const bool enabled=[] {char v[4]{};const bool envRequest=GetEnvironmentVariableA("CPVR_MENU_STEREO_UI_TEST",v,4)==1&&v[0]=='1';const bool iniRequest=g_liveControls.xrMenuStereo==1;if(!envRequest&&!iniRequest)return false;uintptr_t exe=reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));uint32_t pe{},timestamp{},size{};uint64_t binder{},flush{};const bool okay=exe&&ReadU32Safe(exe+0x3C,&pe)&&pe<0x1000&&ReadU32Safe(exe+pe+8,&timestamp)&&timestamp==0x68AF45EA&&ReadU32Safe(exe+pe+0x50,&size)&&size==0x4EFC000&&ReadU64Safe(exe+0x1FAC17,&binder)&&binder==0x0002F22037848B44ull&&ReadU64Safe(exe+0x1F22E4,&flush)&&flush==0x5653551824448844ull;try{Log("[MenuPreviewUI] enabled-initial enabled=%u timestamp=%X imageSize=%X binderPin=%u flushPin=%u iniRequest=%u envRequest=%u; startup stereo UI mode\n",unsigned(okay),timestamp,size,unsigned(binder==0x0002F22037848B44ull),unsigned(flush==0x5653551824448844ull),unsigned(iniRequest),unsigned(envRequest));}catch(...){}return okay;}();return enabled;}
uint32_t UiConsumerTimingScope()noexcept{try{Lease lease;if(!TryAcquireCurrent(lease))return 0;std::lock_guard lock(S().mutex);const auto& alias=S().alias;return alias&&SamePreview(alias->scope,lease.metadata)&&alias->scope.controllerIdentity==lease.metadata.controllerIdentity?2u:1u;}catch(...){return 0;}}
void ObserveUiOwnedAlias(const Metadata& scope,uintptr_t backend,uintptr_t resource)noexcept{
 RetiredPortraits retiredPortraits; // COM owners release after Safe/the UI mutex.
 if(UiConsumerEnabled()&&S().failed.load()){AliasDiagnostic("observer-already-failed",scope);return;}
 Safe([&]{if(!Current(scope)||!backend||!resource){AliasDiagnostic("scope-or-owner-missing",scope);return;}
    const auto& owned=*reinterpret_cast<const RED4ext::Handle<RED4ext::DynamicTexture>*>(backend+0x68);
    if(reinterpret_cast<uintptr_t>(owned.GetPtr())!=resource||!owned.refCount){AliasDiagnostic("sdk-owner-mismatch",scope,owned.refCount!=nullptr);return;}
    auto next=std::make_shared<Alias>();next->scope=scope;next->resource=owned;next->identity=resource;
    if(reinterpret_cast<uintptr_t>(next->resource.GetPtr())!=resource||!Current(scope)){AliasDiagnostic("copied-owner-or-scope-changed",scope,true);return;}
    uintptr_t backing{},again{},manager{};uint32_t id{};const auto exe=reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    if(!ReadPtrSafe(resource+0x70,&backing)||!backing){AliasDiagnostic("read-backing-missing",scope,true);return;}
    if(!ReadU32Safe(backing+0x18,&id)||!id||id>65536){AliasDiagnostic("read-id-unreadable-or-bound",scope,true,id);return;}next->readID=id;
    if(!ReadPtrSafe(resource+0x70,&again)||backing!=again){AliasDiagnostic("read-backing-changed",scope,true,id);return;}
    if(!ReadPtrSafe(exe+0x3438A28,&manager)||!manager){AliasDiagnostic("manager-missing",scope,true,id);return;}
    if(!ReadPtrSafe(manager+0x2F208+uintptr_t(id-1)*0xB0,&next->readCPU)||!next->readCPU){AliasDiagnostic("default-srv-missing",scope,true,id);return;}
    const auto cpu=next->readCPU;std::shared_ptr<const Alias> old;{std::lock_guard lock(S().mutex);old=std::move(S().alias);if(old&&!SamePreview(old->scope,scope)){RetirePortraitsLocked(retiredPortraits);S().lists.ForEach([](List& list){UnpinListLocked(list);});S().lists.Clear();for(auto& b:S().buffers)b.receipt.reset();}
        auto& d=S().nativeDiagnostic;const auto calls=S().nativeCalls.load();
        const bool newScope=!d.scopeKnown||d.epoch!=scope.screenEpoch||d.setup!=scope.setupID;
        const bool aliasChanged=old&&(old->readID!=id||old->readCPU!=cpu);
        if(newScope){d={};d.startCalls=calls;d.epoch=scope.screenEpoch;d.setup=scope.setupID;d.scopeKnown=true;}
        else if(aliasChanged)++d.aliasChanges;
        S().alias=std::move(next);S().aliasReadID.store(id,std::memory_order_release);S().aliasKnown=true;
        if(newScope||aliasChanged)SourceDescriptorProbeLocked("alias-accepted");MaybeNativeSummaryLocked();}
    AliasDiagnostic("accepted",scope,true,id,cpu);
 });}

void RegisterUiBackBuffer(ID3D12Resource* resource)noexcept{Safe([&]{if(!resource)return;const auto desc=resource->GetDesc();if(desc.Width>=128&&desc.Width<=8192&&desc.Height>=128&&desc.Height<=8192)uiCanvasExtent.store((uint64_t(desc.Width)<<32)|desc.Height,std::memory_order_release);const auto generation=TrackObject(resource,ObjectKind::Resource);if(!generation)return;ComPtr<ID3D12Resource> held=resource;std::lock_guard lock(S().mutex);if(Buffer(reinterpret_cast<uintptr_t>(resource)))return;for(auto& b:S().buffers)if(!b.resource){b.resource=std::move(held);b.generation=generation;return;}Fail("backbuffer-capacity");});}
void ClearUiBackBuffers()noexcept{uiCanvasExtent.store(0,std::memory_order_release);if(!UiConsumerEnabled())return;try{std::array<BackBuffer,8> retired;std::lock_guard lock(S().mutex);retired.swap(S().buffers);S().lists.ForEach([](List& l){l.targets={};for(auto& e:l.events)e.target.Reset();});}catch(...){Fail("backbuffer-cleanup-exception");}}
bool ReadUiCanvasExtent(uint32_t& width,uint32_t& height)noexcept{const auto size=uiCanvasExtent.load(std::memory_order_acquire);width=uint32_t(size>>32);height=uint32_t(size);return width>=128&&height>=128;}
std::shared_ptr<const UiFrameReceipt> AcquireUiFrameForCapture(ID3D12Resource* r,ID3D12CommandQueue* q)noexcept{if(!UiConsumerEnabled()||S().failed.load()||!r||!q)return {};try{std::shared_ptr<const UiFrameReceipt> result;{std::lock_guard lock(S().mutex);if(auto* b=Buffer(reinterpret_cast<uintptr_t>(r)))result=b->receipt;}if(result&&result->queue==reinterpret_cast<uintptr_t>(q)&&Current(result->scope))return result;}catch(...){Fail("capture-query-exception");}return {};}
bool ValidateUiFrameForCapture(const UiFrameReceipt& r,ID3D12Resource* b,ID3D12CommandQueue* q)noexcept{auto current=AcquireUiFrameForCapture(b,q);return current&&current->uiSubmission==r.uiSubmission&&current->sourceSequence==r.sourceSequence&&current->cameraRevision==r.cameraRevision;}
bool RegisterBoundUiHeap(ID3D12DescriptorHeap* heap){
 if(!heap)return true;
 const auto desc=heap->GetDesc();
 if(desc.Type!=D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV || !(desc.Flags&D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE))return true;
 const auto owner=reinterpret_cast<uintptr_t>(heap);
 {std::lock_guard lock(S().mutex);Drain();for(const auto& existing:S().heaps)if(existing.owner==owner)return true;}
 ComPtr<ID3D12Device> device;
 if(FAILED(heap->GetDevice(IID_PPV_ARGS(&device)))){Fail("bound-ui-heap-device");return false;}
 Heap next;next.owner=owner;next.generation=TrackObject(heap,ObjectKind::Heap);
 next.cpu=heap->GetCPUDescriptorHandleForHeapStart().ptr;
 next.gpu=heap->GetGPUDescriptorHandleForHeapStart().ptr;
 next.count=desc.NumDescriptors;next.increment=device->GetDescriptorHandleIncrementSize(desc.Type);
 if(!next.generation||!next.cpu||!next.gpu||!next.increment){Fail("bound-ui-heap-incomplete");return false;}
 {std::lock_guard lock(S().mutex);Drain();
  for(const auto& existing:S().heaps)if(existing.owner==owner)return true;
  for(auto& existing:S().heaps)if(!existing.owner){existing=next;return true;}}
 Fail("bound-ui-heap-capacity");return false;
}
void UiDescriptorHeapCreated(ID3D12Device* device,ID3D12DescriptorHeap* heap)noexcept{Safe([&]{if(!heap||!device)return;auto d=heap->GetDesc();if(d.Type!=D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV || !(d.Flags&D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE))return;
 // The game creates more than 32 unrelated heaps before the editor appears.
 // Keep the fixed pool for active menu heaps; pre-existing heaps are admitted
 // when the actual UI command list binds them below.
 if(!Active())return;
 Heap next;next.owner=reinterpret_cast<uintptr_t>(heap);next.generation=TrackObject(heap,ObjectKind::Heap);next.cpu=heap->GetCPUDescriptorHandleForHeapStart().ptr;next.gpu=heap->GetGPUDescriptorHandleForHeapStart().ptr;next.count=d.NumDescriptors;next.increment=device->GetDescriptorHandleIncrementSize(d.Type);
 ComPtr<IUnknown> identity;const auto qi=heap->QueryInterface(__uuidof(IUnknown),reinterpret_cast<void**>(identity.GetAddressOf()));
 static std::atomic<uint32_t> rows{};const bool emit=rows.fetch_add(1,std::memory_order_relaxed)<16;bool registered=false;uint32_t registeredCount{};
 std::lock_guard lock(S().mutex);Drain();if(next.generation)for(auto& h:S().heaps)if(!h.owner){h=next;registered=true;break;}
 for(const auto& h:S().heaps)registeredCount+=unsigned(h.owner!=0);
 if(next.generation&&!registered)Fail("shader-visible-heap-capacity");
 if(emit)Log("[MenuPreviewUI] ui-heap-created owner=%llx unknown=%llx qi=%X generation=%llu cpu=%llx gpu=%llx count=%u increment=%u flags=%X registered=%u registeredHeapCount=%u; live COM args, identity released\n",next.owner,reinterpret_cast<uintptr_t>(identity.Get()),unsigned(qi),next.generation,next.cpu,next.gpu,next.count,next.increment,unsigned(d.Flags),unsigned(registered),registeredCount);});}
UiDescriptorMutation BeginUiSrvWrite(D3D12_CPU_DESCRIPTOR_HANDLE handle)noexcept{UiDescriptorMutation result;Safe([&]{std::lock_guard lock(S().mutex);Drain();DropPersistentSourceLocked(handle.ptr);auto* d=GetDescriptor(handle.ptr,true);if(!d)return;result=NewMutation(1);auto m=result.state;if(!m)return;m->destinations[0]=handle.ptr;UiBeginDescriptorMutation(d->version,d->inFlight,d->overlap,m->token);m->enlisted[0]=1;d->pending=true;if(UiRasterDiagnosticsEnabled()){d->cbvOnly=false;d->cbv={};}d->resource=0;d->resourceGeneration=0;d->unorderedAccess=false;});return result;}
UiDescriptorMutation BeginUiDescriptorOverwrite(D3D12_CPU_DESCRIPTOR_HANDLE handle)noexcept{UiDescriptorMutation result;Safe([&]{std::lock_guard lock(S().mutex);Drain();DropPersistentSourceLocked(handle.ptr);if(auto* d=GetDescriptor(handle.ptr)){result=NewMutation(1);auto m=result.state;if(!m)return;m->destinations[0]=handle.ptr;UiBeginDescriptorMutation(d->version,d->inFlight,d->overlap,m->token);m->enlisted[0]=1;d->pending=true;if(UiRasterDiagnosticsEnabled())d->cbv={};d->resource=0;d->resourceGeneration=0;d->unorderedAccess=false;}});return result;}
UiDescriptorMutation BeginUiCbvWrite(D3D12_CPU_DESCRIPTOR_HANDLE handle)noexcept{
 if(!UiRasterDiagnosticsEnabled()||!UiConsumerEnabled()||S().failed.load())return {};
 Lease lease;if(!TryAcquireCurrent(lease)||lease.metadata.screen!=2)return BeginUiDescriptorOverwrite(handle);
 UiDescriptorMutation result;
 try{
  std::lock_guard lock(S().mutex);Drain();auto next=NewMutation(1);
  if(next.state)if(auto* d=GetUiCbvDescriptorLocked(handle.ptr)){
   DropPersistentSourceLocked(handle.ptr);result=std::move(next);auto& m=*result.state;m.cbvDiagnostic=true;m.destinations[0]=handle.ptr;
   UiBeginDescriptorMutation(d->version,d->inFlight,d->overlap,m.token);m.enlisted[0]=1;
   d->pending=true;d->cbvOnly=true;d->cbv={};d->resource=0;d->resourceGeneration=0;d->unorderedAccess=false;
  }
 }catch(...){} // Diagnostic capacity/allocation loss must not fail the pixel observer.
 if(!result)return BeginUiDescriptorOverwrite(handle); // Preserve ordinary overwrite revocation even when diagnostic capture drops.
 return result;
}
void FinishUiCbvWrite(UiDescriptorMutation token,const D3D12_CONSTANT_BUFFER_VIEW_DESC* desc,D3D12_CPU_DESCRIPTOR_HANDLE handle)noexcept{
 if(!UiRasterDiagnosticsEnabled())return;
 if(token.state&&!token.state->cbvDiagnostic){FinishUiSrvWrite(std::move(token),nullptr,handle);return;}
 Safe([&]{auto m=std::move(token.state);if(!m)return;
  UiCbvDiagnostic payload;
  if(desc&&desc->BufferLocation&&!(desc->BufferLocation&255)&&desc->SizeInBytes&&!(desc->SizeInBytes&255)&&desc->SizeInBytes<=65536&&desc->BufferLocation<=UINT64_MAX-desc->SizeInBytes)
   payload={desc->BufferLocation,desc->SizeInBytes,true};
  std::lock_guard lock(S().mutex);Drain();if(m->finished)return;m->finished=true;if(!m->enlisted[0])return;
  auto* d=GetDescriptor(handle.ptr);if(!d||m->destinations[0]!=handle.ptr)return;
  const bool accepted=UiFinishEnlistedMutation(true,d->version,d->inFlight,d->overlap,m->token);
  d->pending=d->inFlight!=0;d->cbv={};d->resource=0;d->resourceGeneration=0;d->unorderedAccess=false;
  if(accepted&&!d->pending){d->cbv=payload;d->originCPU=handle.ptr;d->originVersion=m->token;}
  // CBVs never enter the resource/SRV persistent source registry. Eviction is unknown.
 });
}
void FinishUiSrvWrite(UiDescriptorMutation token,ID3D12Resource* resource,D3D12_CPU_DESCRIPTOR_HANDLE handle,bool unorderedAccess)noexcept{Safe([&]{auto m=std::move(token.state);if(!m)return;const auto generation=TrackObject(resource,ObjectKind::Resource);std::lock_guard lock(S().mutex);Drain();if(m->finished)return;m->finished=true;if(!m->enlisted[0])return;auto* d=GetDescriptor(handle.ptr);if(!d||m->destinations[0]!=handle.ptr)return;
 const bool accepted=UiFinishEnlistedMutation(m->enlisted[0]!=0,d->version,d->inFlight,d->overlap,m->token);if(UiRasterDiagnosticsEnabled())d->cbv={};d->pending=d->inFlight!=0;if(accepted){d->resource=generation?reinterpret_cast<uintptr_t>(resource):0;d->resourceGeneration=generation;d->originCPU=handle.ptr;d->originVersion=m->token;d->unorderedAccess=unorderedAccess;if(!d->pending){PutPersistentSourceLocked(*d);if(!unorderedAccess)RememberTargetDescriptorLocked(d->resource,generation,d->cpu,d->version,false);}}else{if(UiRasterDiagnosticsEnabled())d->cbv={};d->resource=0;d->resourceGeneration=0;d->unorderedAccess=false;}
 if(S().alias){bool writerMatch=false;for(size_t i=0;i<S().portraitPins.size();++i)if(resource&&S().portraitPins[i].Get()==resource&&S().portraitGenerations[i]==generation)writerMatch=true;
  if(accepted&&writerMatch&&!S().nativeDiagnostic.writerDescriptorKnown){S().nativeDiagnostic.writerDescriptorKnown=true;++S().nativeDiagnostic.lineageRevision;}
  if(S().nativeDiagnostic.srvRows<8)
  if(handle.ptr==S().alias->readCPU||writerMatch){++S().nativeDiagnostic.srvRows;Log("[MenuPreviewUI] ui-srv-write epoch=%u setup=%u cpu=%llx aliasCPU=%llx resource=%llx resourceGeneration=%llu writerMatch=%u accepted=%u pending=%u originVersion=%llu; observed CreateSRV return only\n",S().alias->scope.screenEpoch,S().alias->scope.setupID,handle.ptr,S().alias->readCPU,reinterpret_cast<uintptr_t>(resource),generation,unsigned(writerMatch),unsigned(accepted),unsigned(d->pending),d->originVersion);}}
 });}
UiDescriptorMutation BeginUiDescriptorCopy(ID3D12Device* device,UINT dc,const D3D12_CPU_DESCRIPTOR_HANDLE* ds,const UINT* dz,UINT sc,const D3D12_CPU_DESCRIPTOR_HANDLE* ss,const UINT* sz,D3D12_DESCRIPTOR_HEAP_TYPE type)noexcept{UiDescriptorMutation result;Safe([&]{if(type!=D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV||!device)return;
 auto& state=S();
 if(UiCanSkipInactiveSingleCopy(dc,ds,dz,state.descriptorIndex,state.persistentSources,
     []()noexcept{return CurrentPreviewObservationEpoch();}))return;
 const auto inc=device->GetDescriptorHandleIncrementSize(type);std::lock_guard lock(state.mutex);Drain();
 auto invalidate=[&]{for(auto& d:S().descriptors){bool affected=!inc||dc>128||!ds;for(UINT i=0;!affected&&i<dc;++i){const uint64_t n=dz?dz[i]:1;const auto start=ds[i].ptr;if(d.cpu>=start&&(d.cpu-start)/inc<n&&(d.cpu-start)%inc==0)affected=true;}if(affected){if(UiRasterDiagnosticsEnabled())d.cbv={};d.resource=0;d.resourceGeneration=0;d.overlap=d.inFlight!=0;d.pending=d.inFlight!=0;d.version=++S().serial;}}
  if(!inc||dc>128||!ds){S().persistentSources.clear();S().sourcesByResource.clear();return;}
  for(auto it=S().persistentSources.begin();it!=S().persistentSources.end();){bool affected=false;for(UINT i=0;!affected&&i<dc;++i){const auto start=ds[i].ptr;const uint64_t n=dz?dz[i]:1;affected=it->first>=start&&(it->first-start)%inc==0&&(it->first-start)/inc<n;}const auto cpu=it->first;++it;if(affected)DropPersistentSourceLocked(cpu);}
 };
 if(!inc||!ds||!ss||dc>128||sc>128){invalidate();return;}uint64_t totalD{},totalS{};for(UINT i=0;i<dc;++i)totalD+=dz?dz[i]:1;for(UINT i=0;i<sc;++i)totalS+=sz?sz[i]:1;if(totalD>128||totalS>128||totalD!=totalS){invalidate();return;}
 // SRV creation keeps source identity available before a preview opens.
 // Outside a fresh preview, only maintain already tracked destinations:
 // propagating every world source into new GPU slots is unnecessary work.
 // Existing destinations still use the full versioned mutation protocol.
 const bool trackNewDestinations=CurrentPreviewObservationEpoch()!=0;
 bool observed=false;
 if(trackNewDestinations)for(UINT i=0;i<sc&&!observed;++i){const UINT n=sz?sz[i]:1;for(UINT j=0;j<n;++j)if(GetDescriptor(ss[i].ptr+uintptr_t(j)*inc)){observed=true;break;}}
 for(UINT i=0;i<dc&&!observed;++i){const UINT n=dz?dz[i]:1;for(UINT j=0;j<n;++j)if(GetDescriptor(ds[i].ptr+uintptr_t(j)*inc)){observed=true;break;}}
 if(!observed)return;
 result=NewMutation(size_t(totalD));auto m=result.state;if(!m)return;size_t index{};for(UINT i=0;i<sc;++i){const UINT n=sz?sz[i]:1;for(UINT j=0;j<n;++j){if(auto* d=GetDescriptor(ss[i].ptr+uintptr_t(j)*inc);d&&!d->pending)m->sources[index]=*d;++index;}}index=0;for(UINT i=0;i<dc;++i){const UINT n=dz?dz[i]:1;for(UINT j=0;j<n;++j)m->destinations[index++]=ds[i].ptr+uintptr_t(j)*inc;}for(size_t i=0;i<m->destinations.size();++i){const bool knownDestination=GetDescriptor(m->destinations[i])!=nullptr;DropPersistentSourceLocked(m->destinations[i]);const bool cbvSource=UiRasterDiagnosticsEnabled()&&!m->sources[i].resource&&m->sources[i].cbv.known;auto* d=cbvSource&&!knownDestination?(trackNewDestinations?GetUiCbvDescriptorLocked(m->destinations[i]):nullptr):GetDescriptor(m->destinations[i],knownDestination||(trackNewDestinations&&m->sources[i].resource!=0));if(!d)continue;UiBeginDescriptorMutation(d->version,d->inFlight,d->overlap,m->token);m->enlisted[i]=1;d->pending=true;if(UiRasterDiagnosticsEnabled())d->cbv={};d->resource=0;d->resourceGeneration=0;}});return result;}
void FinishUiDescriptorCopy(UiDescriptorMutation token)noexcept{Safe([&]{auto m=std::move(token.state);if(!m)return;std::lock_guard lock(S().mutex);Drain();if(m->finished)return;m->finished=true;for(size_t i=0;i<m->destinations.size();++i){if(!m->enlisted[i])continue;auto* dest=GetDescriptor(m->destinations[i]);const auto* original=GetDescriptor(m->sources[i].cpu);if(!dest)continue;const bool accepted=UiFinishEnlistedMutation(m->enlisted[i]!=0,dest->version,dest->inFlight,dest->overlap,m->token);dest->pending=dest->inFlight!=0;if(UiRasterDiagnosticsEnabled())dest->cbv={};
  const bool sourceCurrent=original&&!original->pending&&original->version==m->sources[i].version;
  if(accepted&&sourceCurrent){if(UiRasterDiagnosticsEnabled()&&!m->sources[i].resource){dest->cbv=m->sources[i].cbv;if(dest->cbv.known)dest->cbvOnly=true;}if(UiRasterDiagnosticsEnabled()&&m->sources[i].resource)dest->cbvOnly=false;dest->resource=m->sources[i].resource;dest->resourceGeneration=m->sources[i].resourceGeneration;dest->originCPU=m->sources[i].originCPU;dest->originVersion=m->sources[i].originVersion;dest->unorderedAccess=m->sources[i].unorderedAccess;if(!dest->pending&&!dest->unorderedAccess)RememberTargetDescriptorLocked(dest->resource,dest->resourceGeneration,dest->cpu,dest->version,true);}else{dest->resource=0;dest->resourceGeneration=0;dest->unorderedAccess=false;}
  if(S().alias){const auto& source=m->sources[i];bool writerMatch=false;for(size_t p=0;p<S().portraitPins.size();++p)if(S().portraitPins[p]&&source.resource==reinterpret_cast<uintptr_t>(S().portraitPins[p].Get())&&source.resourceGeneration==S().portraitGenerations[p])writerMatch=true;
   if(accepted&&sourceCurrent&&writerMatch&&!S().nativeDiagnostic.writerDescriptorKnown){S().nativeDiagnostic.writerDescriptorKnown=true;++S().nativeDiagnostic.lineageRevision;}
   if(accepted&&sourceCurrent&&writerMatch&&source.originCPU==S().alias->readCPU){auto& diag=S().nativeDiagnostic;diag.offscreenCopies[diag.offscreenCursor++%diag.offscreenCopies.size()]={m->destinations[i],dest->version,dest->resourceGeneration};diag.offscreenReady=true;++diag.offscreenRevision;
    auto& pre=diag.offscreenPreflight;if(!pre.armedLogged){pre.armedLogged=true;pre.armedAtMs=PublicationNowMs();Log("[MenuPreviewUI] offscreen-preflight-armed epoch=%u setup=%u routing=1 destCPU=%llx portraitResource=%llx resourceGeneration=%llu originCPU=%llx directSeenBeforeArm=%llu missingListBeforeArm=%llu overflowBeforeArm=%llu enteredBeforeArm=%llu notReadyBeforeArm=%llu; accepted exact writer copy, no draw claim\n",S().alias->scope.screenEpoch,S().alias->scope.setupID,m->destinations[i],source.resource,source.resourceGeneration,source.originCPU,pre.directSeen,pre.missingList,pre.overflowSkipped,pre.entered,pre.notReady);}}
   if(S().nativeDiagnostic.copyRows<8)
   if(source.cpu==S().alias->readCPU||source.originCPU==S().alias->readCPU||m->destinations[i]==S().alias->readCPU||writerMatch){++S().nativeDiagnostic.copyRows;Log("[MenuPreviewUI] ui-descriptor-copy epoch=%u setup=%u sourceCPU=%llx destCPU=%llx aliasCPU=%llx sourceResource=%llx sourceGeneration=%llu originCPU=%llx originVersion=%llu writerMatch=%u sourceVersionCurrent=%u accepted=%u destPending=%u destResource=%llx; observed CopyDescriptors return only\n",S().alias->scope.screenEpoch,S().alias->scope.setupID,source.cpu,m->destinations[i],S().alias->readCPU,source.resource,source.resourceGeneration,source.originCPU,source.originVersion,unsigned(writerMatch),unsigned(sourceCurrent),unsigned(accepted),unsigned(dest->pending),dest->resource);}}
 }});}

void UiDescriptorHeaps(ID3D12GraphicsCommandList* list,UINT count,ID3D12DescriptorHeap* const* heaps)noexcept{Safe([&]{const auto lifetime=TrackList(list);if(!lifetime)return;
 if(count<=4&&heaps)for(UINT i=0;i<count;++i)if(!RegisterBoundUiHeap(heaps[i]))return;
 std::optional<std::array<Heap,32>> registered;Metadata scope{};uint32_t rowsToLog{},registeredCount{};
 {std::lock_guard lock(S().mutex);auto* l=GetList(reinterpret_cast<uintptr_t>(list),lifetime);if(!l)return;l->heaps={};l->tables={};InvalidateNativeIntent(l->native);if(count>4||!heaps)return;for(UINT i=0;i<count;++i)l->heaps[i]=reinterpret_cast<uintptr_t>(heaps[i]);
  auto& diag=S().nativeDiagnostic;if(S().alias&&diag.heapBindRows<4&&Current(S().alias->scope)){rowsToLog=count<4-diag.heapBindRows?count:4-diag.heapBindRows;diag.heapBindRows+=rowsToLog;scope=S().alias->scope;if(rowsToLog){registered.emplace(S().heaps);for(const auto& h:*registered)registeredCount+=unsigned(h.owner!=0);}}}
 for(uint32_t i=0;i<rowsToLog;++i){auto* heap=heaps[i];ComPtr<IUnknown> identity;if(heap)heap->QueryInterface(__uuidof(IUnknown),reinterpret_cast<void**>(identity.GetAddressOf()));
  const auto desc=heap?heap->GetDesc():D3D12_DESCRIPTOR_HEAP_DESC{};const auto cpu=heap?heap->GetCPUDescriptorHandleForHeapStart().ptr:0;const auto gpu=heap&&(desc.Flags&D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE)?heap->GetGPUDescriptorHandleForHeapStart().ptr:0;
  bool ownerMatch=false,unknownMatch=false;uintptr_t registeredCPU{},registeredGPU{};uint32_t registeredIncrement{};for(const auto& h:*registered){if(h.owner==reinterpret_cast<uintptr_t>(heap)&&heap){ownerMatch=true;registeredCPU=h.cpu;registeredGPU=h.gpu;registeredIncrement=h.increment;}if(h.owner==reinterpret_cast<uintptr_t>(identity.Get())&&identity)unknownMatch=true;}
  Log("[MenuPreviewUI] ui-heaps-call epoch=%u setup=%u list=%llx boundCount=%u index=%u heap=%llx unknown=%llx type=%u flags=%X cpu=%llx gpu=%llx count=%u registeredHeapCount=%u ownerMatch=%u unknownMatch=%u registeredCPU=%llx registeredGPU=%llx registeredIncrement=%u; live COM args, identity released\n",scope.screenEpoch,scope.setupID,reinterpret_cast<uintptr_t>(list),count,i,reinterpret_cast<uintptr_t>(heap),reinterpret_cast<uintptr_t>(identity.Get()),unsigned(desc.Type),unsigned(desc.Flags),cpu,gpu,desc.NumDescriptors,registeredCount,unsigned(ownerMatch),unsigned(unknownMatch),registeredCPU,registeredGPU,registeredIncrement);
 }
 });}
void UiGraphicsRootSignature(ID3D12GraphicsCommandList* list,ID3D12RootSignature* root)noexcept{Safe([&]{const auto lifetime=TrackList(list);if(!lifetime)return;std::lock_guard lock(S().mutex);if(auto* l=GetList(reinterpret_cast<uintptr_t>(list),lifetime);l&&l->root!=reinterpret_cast<uintptr_t>(root)){l->root=reinterpret_cast<uintptr_t>(root);l->tables={};InvalidateNativeIntent(l->native);l->transform.cbvs={};l->transform.cbvMask=0;}});}
void UiPipelineState(ID3D12GraphicsCommandList* list,ID3D12PipelineState* pipeline)noexcept{Safe([&]{const auto lifetime=TrackList(list);if(!lifetime)return;std::lock_guard lock(S().mutex);if(auto* l=GetList(reinterpret_cast<uintptr_t>(list),lifetime))l->pipeline=reinterpret_cast<uintptr_t>(pipeline);});}
void UiGraphicsRootTable(ID3D12GraphicsCommandList* list,UINT root,D3D12_GPU_DESCRIPTOR_HANDLE gpu,uintptr_t caller)noexcept{Safe([&]{const auto lifetime=TrackList(list);if(!lifetime)return;
 RootTable table;table.gpu=gpu.ptr;uintptr_t array=__readgsqword(0x58),tls{},context{},actual{},state{},layout{};uint8_t ready{};
 const auto exe=reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
 if(caller==exe+0x1F2552&&ReadPtrSafe(array,&tls)&&tls&&ReadU8Safe(tls+0x14,&ready)&&ready&&ReadPtrSafe(tls+0x188,&context)&&context&&ReadPtrSafe(context+0x30,&actual)&&actual==reinterpret_cast<uintptr_t>(list)&&ReadPtrSafe(context+0x60,&state)&&state&&ReadPtrSafe(state+0x68,&layout)&&layout){
  for(uint32_t t=0;t<64;++t){const auto row=layout+0x38+uintptr_t(t)*0x10;uint8_t kind{},index{};uint32_t packedCount{};
   if(!ReadU8Safe(row+0xD,&kind)||!ReadU8Safe(row+0xE,&index)||index!=root||kind==2||!ReadU32Safe(row+8,&packedCount))continue;
   const uint32_t count=(packedCount>>16)&0xFFFF;if(!count||count>128)continue;
   uintptr_t cpuBase{},gpuBase{};uint32_t increment{};if(!ReadPtrSafe(state+0x50,&cpuBase)||!ReadPtrSafe(state+0x58,&gpuBase)||!ReadU32Safe(state+0x10,&increment)||!increment||gpu.ptr<gpuBase||(gpu.ptr-gpuBase)%increment)continue;
   table.cpu=cpuBase+(gpu.ptr-gpuBase);table.count=count;table.tableIndex=t;table.layout=layout;table.proven=true;break;
  }
 }
 std::lock_guard lock(S().mutex);if(auto* l=GetList(reinterpret_cast<uintptr_t>(list),lifetime)){if(root>=l->tables.size()){MarkOverflow(*l,OverflowReason::RootOutOfRange,root);return;}l->tables[root]=table;
  auto& diag=S().nativeDiagnostic;if(S().alias&&diag.rootCallRows<8&&Current(S().alias->scope)){++diag.rootCallRows;uint32_t registeredCount{};for(const auto& h:S().heaps)registeredCount+=unsigned(h.owner!=0);
   Log("[MenuPreviewUI] ui-root-table-call epoch=%u setup=%u list=%llx rootIndex=%u gpuStart=%llx caller=%llx proven=%u layout=%llx tableIndex=%u cpuStart=%llx count=%u boundHeap0=%llx boundHeap1=%llx boundHeap2=%llx boundHeap3=%llx registeredHeapCount=%u; actual callback, not Draw candidate\n",S().alias->scope.screenEpoch,S().alias->scope.setupID,reinterpret_cast<uintptr_t>(list),root,gpu.ptr,caller,unsigned(table.proven),table.layout,table.tableIndex,table.cpu,table.count,l->heaps[0],l->heaps[1],l->heaps[2],l->heaps[3],registeredCount);}}
 });}
bool UiRasterDiagnosticsEnabled()noexcept{static const bool enabled=[] {char v[4]{};return GetEnvironmentVariableA("CPVR_MENU_UI_RASTER_TRACE",v,sizeof(v))==1&&v[0]=='1';}();return enabled;}
void UiGraphicsConstantBuffer(ID3D12GraphicsCommandList* list,UINT root,D3D12_GPU_VIRTUAL_ADDRESS address)noexcept{
 if(!UiRasterDiagnosticsEnabled()||root>=64)return;Safe([&]{const auto lifetime=TrackList(list);if(!lifetime)return;
  std::lock_guard lock(S().mutex);if(auto* l=GetList(reinterpret_cast<uintptr_t>(list),lifetime)){l->transform.cbvs[root]=address;l->transform.cbvMask|=uint64_t{1}<<root;}});
}
void UiVertexBuffers(ID3D12GraphicsCommandList* list,UINT start,UINT count,const D3D12_VERTEX_BUFFER_VIEW* views)noexcept{
 if(!UiRasterDiagnosticsEnabled()||start!=0||!count)return;Safe([&]{const auto lifetime=TrackList(list);if(!lifetime)return;
  std::lock_guard lock(S().mutex);if(auto* l=GetList(reinterpret_cast<uintptr_t>(list),lifetime)){
   l->transform.vertex0Known=views!=nullptr;l->transform.vertex0=views?views[0]:D3D12_VERTEX_BUFFER_VIEW{};}});
}
void UiRasterViewports(ID3D12GraphicsCommandList* list,UINT count,const D3D12_VIEWPORT* viewports)noexcept{
 if(!UiRasterDiagnosticsEnabled())return;Safe([&]{const auto lifetime=TrackList(list);if(!lifetime)return;
  std::lock_guard lock(S().mutex);if(auto* l=GetList(reinterpret_cast<uintptr_t>(list),lifetime)){
   l->raster.viewportCount=count;l->raster.viewportKnown=count==1&&viewports;
   l->raster.viewport=count&&viewports?viewports[0]:D3D12_VIEWPORT{};}});
}
void UiRasterScissors(ID3D12GraphicsCommandList* list,UINT count,const D3D12_RECT* scissors)noexcept{
 if(!UiRasterDiagnosticsEnabled())return;Safe([&]{const auto lifetime=TrackList(list);if(!lifetime)return;
  std::lock_guard lock(S().mutex);if(auto* l=GetList(reinterpret_cast<uintptr_t>(list),lifetime)){
   l->raster.scissorCount=count;l->raster.scissorKnown=count==1&&scissors;
   l->raster.scissor=count&&scissors?scissors[0]:D3D12_RECT{};}});
}
void UiTargets(ID3D12GraphicsCommandList* list,UINT count,ID3D12Resource* const* resources)noexcept{Safe([&]{const auto lifetime=TrackList(list);if(!lifetime)return;
 std::array<uintptr_t,4> identities{};std::array<uint64_t,4> generations{};std::array<UiTargetExtent,4> extents{};
 const bool rasterTrace=UiRasterDiagnosticsEnabled();
 if(count<=4&&resources)for(UINT i=0;i<count;++i){identities[i]=reinterpret_cast<uintptr_t>(resources[i]);if(resources[i]){
  generations[i]=TrackObject(resources[i],ObjectKind::Resource);
  if(rasterTrace&&generations[i]){const auto desc=resources[i]->GetDesc();extents[i]={desc.Width,desc.Height,uint32_t(desc.Format),true};}}}
 std::lock_guard lock(S().mutex);if(auto* l=GetList(reinterpret_cast<uintptr_t>(list),lifetime)){
  l->targets=identities;l->targetGenerations=generations;l->targetExtents=extents;}});}
void ObserveUiPortraitWriter(ID3D12GraphicsCommandList* list,ID3D12Resource* resource,std::shared_ptr<const CameraApplication> app,const UiRawDrawReceipt* diagnosticDraw,std::shared_ptr<const UiProducerInputs>* diagnosticInputs)noexcept{
 if(diagnosticInputs){diagnosticInputs->reset();if(diagnosticDraw&&app)*diagnosticInputs=CaptureProducerInputs(list,resource,*app,*diagnosticDraw);}
 RetiredPortraits retiredPortraits; // Events/GPU keeps retain independent owners.
 if(app&&!CameraApplicationUiSource(*app))return; // Native right has raw capture, no invented UI writer.
 Safe([&]{if(!resource||!app||!Current(app->metadata))return;const auto generation=TrackObject(resource,ObjectKind::Resource);if(!generation)return;const auto allocation=ReadUiAllocation(resource);
 static const bool extentTrace=[] {char value[2]{};return GetEnvironmentVariableA("CPVR_MENU_UI_OFFSCREEN_TRACE",value,sizeof(value))==1&&value[0]=='1';}();
 if(extentTrace&&app->metadata.screen==2){const auto desc=resource->GetDesc();const auto key=(uint64_t(app->metadata.screenEpoch)<<32)|(uint64_t(uint16_t(desc.Width))<<16)|uint16_t(desc.Height);static std::atomic<uint64_t> observed{};if(observed.exchange(key)!=key)Log("[SpatialEditor] GPU-writer epoch=%u setup=%u frame=%u eye=%u resource=%p size=%llux%u format=%u; owned native writer, completion belongs to UI capture\n",app->metadata.screenEpoch,app->metadata.setupID,app->frame.nativeFrame,app->frame.eye,resource,desc.Width,desc.Height,unsigned(desc.Format));}
 const auto lifetime=TrackList(list);Event e;e.writer=true;e.writerReason=WriterReason::ObservedDraw;e.resource=resource;e.resourceGeneration=generation;e.app=std::move(app);std::lock_guard lock(S().mutex);Drain();
 const bool nativeDual=e.app->source.mode==NativePreviewMode::NativeEditorDual;
 if(nativeDual&&!EditorNativeSourceRegistered(e.app->metadata,e.app->source))return;
 if(S().portraitModeKnown&&S().portraitMode!=e.app->source.mode){RetirePortraitsLocked(retiredPortraits);for(auto& b:S().buffers)b.receipt.reset();}
 S().portraitMode=e.app->source.mode;S().portraitModeKnown=true;
 size_t pin=0;uintptr_t retiredResource{};
 if(nativeDual){
  if(S().portraitUseSequence==std::numeric_limits<uint64_t>::max())return;
  std::array<PortraitRotationCell,PortraitSlotCount> cells{};
  for(size_t i=0;i<cells.size();++i)cells[i]={reinterpret_cast<uintptr_t>(S().portraitPins[i].Get()),S().portraitGenerations[i],S().portraitLastUse[i]};
  const auto* alias=S().alias?GetDescriptor(S().alias->readCPU):nullptr;
  const auto selected=SelectPortraitRotationSlot(cells,reinterpret_cast<uintptr_t>(resource),generation,
      alias&&!alias->pending?alias->resource:0,alias&&!alias->pending?alias->resourceGeneration:0);
  if(!selected)return;pin=*selected;
  if(S().portraitPins[pin]&&(S().portraitPins[pin].Get()!=resource||S().portraitGenerations[pin]!=generation)){
   retiredResource=reinterpret_cast<uintptr_t>(S().portraitPins[pin].Get());RetirePortraitSlotLocked(pin,retiredPortraits);
  }
  S().portraitLastUse[pin]=++S().portraitUseSequence;
 }else{
  for(;pin<LegacyPortraitSlots;++pin)if(!S().portraitPins[pin]||S().portraitPins[pin].Get()==resource)break;
  if(pin==LegacyPortraitSlots){for(auto& b:S().buffers)b.receipt.reset();return;}
 }
 const bool writerChanged=S().portraitPins[pin].Get()!=resource||S().portraitGenerations[pin]!=generation;
 S().portraitPins[pin]=e.resource;S().portraitGenerations[pin]=generation;S().portraitAllocations[pin]=allocation;
 if(writerChanged){if(S().portraitRotationRows++<16)Log("[MenuPreviewUI] portrait-allocation resource=%llx generation=%llu kind=%u heap=%llu offset=%llu bytes=%llu slot=%llu nativeDual=%u retired=%llx; bounded exact generation rotation, allocation evidence read outside UI mutex\n",reinterpret_cast<uintptr_t>(resource),generation,allocation.kind,allocation.heap,allocation.offset,allocation.bytes,uint64_t(pin),unsigned(nativeDual),retiredResource);S().nativeDiagnostic.writerDescriptorKnown=true;++S().nativeDiagnostic.lineageRevision;}
 if(pin<2&&!(S().nativeDiagnostic.sourceWriterMask&(1u<<pin))){S().nativeDiagnostic.sourceWriterMask|=uint8_t(1u<<pin);SourceDescriptorProbeLocked("portrait-writer");}
 if(Active())if(auto* l=GetList(reinterpret_cast<uintptr_t>(list),lifetime))Add(*l,std::move(e));});}
void ObserveUiNativeBinding(uint32_t id,uint32_t slot,uint32_t mip,uint8_t stage,uintptr_t caller)noexcept{
 const auto call=S().nativeCalls.fetch_add(1);if(call<8)try{Log("[MenuPreviewUI] native-bind-callback id=%u slot=%u stage=%u aliasKnown=%u active=%u failed=%u\n",id,slot,unsigned(stage),unsigned(S().aliasKnown.load()),unsigned(Active()),unsigned(S().failed.load()));}catch(...){}
 Safe([&]{if(!Active())return;
  const bool idMatch=id&&id==S().aliasReadID.load(std::memory_order_acquire);
  if(slot>=128||stage>=4){if(idMatch){std::lock_guard lock(S().mutex);NativeSignificantLocked(id,slot,mip,stage,caller,0,0,0,0,0,0,0,false,"slot-or-stage-bound");}return;}
  const auto list=NativeList();if(!list){static std::atomic<uint32_t> rows{};if(rows.fetch_add(1)<4)Log("[MenuPreviewUI] native-binding-rejected reason=no-initialized-recording-list\n");if(idMatch){std::lock_guard lock(S().mutex);NativeSignificantLocked(id,slot,mip,stage,caller,0,0,0,0,0,0,0,false,"no-recording-list");}return;}
  const auto lifetime=TrackList(reinterpret_cast<ID3D12GraphicsCommandList*>(list));if(!lifetime){if(idMatch){std::lock_guard lock(S().mutex);NativeSignificantLocked(id,slot,mip,stage,caller,list,0,0,0,0,0,0,false,"list-lifetime");}return;}
  NativeBind bind;bind.id=id;bind.slot=slot;bind.stage=stage;bind.mip=mip;
  uintptr_t array=__readgsqword(0x58),tls{},context{},state{},layout{},manager{},selected{},computedCPU{};
  uint8_t tableIndex{},rootIndex{};const char* firstFailure="layout-or-slot-map";
  const auto exe=reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
  if(ReadPtrSafe(array,&tls)&&tls&&ReadPtrSafe(tls+0x188,&context)&&context&&ReadPtrSafe(context+0x60,&state)&&state&&ReadPtrSafe(state+0x68,&layout)&&layout&&ReadU8Safe(layout+0x4C3+uintptr_t(stage*128+slot)*2,&tableIndex)&&tableIndex<64){
   firstFailure="table-row";const auto table=layout+0x38+uintptr_t(tableIndex)*0x10;uint32_t sourceStart{},packedBase{};
   if(ReadU32Safe(table+4,&sourceStart)&&ReadU32Safe(table+8,&packedBase)&&ReadU8Safe(table+0xE,&rootIndex)){
    firstFailure="slot-range";const uint32_t base=packedBase&0xFFFF,count=(packedBase>>16)&0xFFFF;
    if(slot>=base&&slot-base<count&&count<=128&&sourceStart+slot-base<4096){uintptr_t sourceArray{};
     firstFailure="selected-or-manager";
     if(ReadPtrSafe(state+0x28,&sourceArray)&&sourceArray&&ReadPtrSafe(sourceArray+uintptr_t(sourceStart+slot-base)*8,&selected)&&id&&id<=65536&&ReadPtrSafe(exe+0x3438A28,&manager)&&manager){uintptr_t holder{},entries{};uint32_t mips{};const auto record=manager+uintptr_t(id-1)*0xB0;
      firstFailure="mip-or-cpu";
      if(ReadPtrSafe(record+0x2F218,&holder)){if(holder&&ReadU32Safe(holder+0x38,&mips)&&mips){if(mips<=32&&mip<mips&&ReadPtrSafe(holder+0x10,&entries)&&entries)ReadPtrSafe(entries+uintptr_t(mip)*8,&computedCPU);}else ReadPtrSafe(record+0x2F208,&computedCPU);}
      if(computedCPU){firstFailure="selected-cpu-mismatch";if(selected==computedCPU){bind.cpu=computedCPU;bind.layout=layout;bind.tableIndex=tableIndex;bind.rootIndex=rootIndex;bind.relative=slot-base;bind.valid=true;firstFailure="none";}}
     }
    }
   }
  }
  std::lock_guard lock(S().mutex);auto& diagnostic=S().nativeDiagnostic;++diagnostic.examined;
  uintptr_t defaultCPU{};if(manager&&S().alias&&(id==S().alias->readID||(computedCPU&&computedCPU==S().alias->readCPU)))ReadPtrSafe(manager+uintptr_t(id-1)*0xB0+0x2F208,&defaultCPU);
  if(auto* l=GetList(list,lifetime)){l->native[uint32_t(stage)*128+slot]=bind;if(bind.valid&&S().alias&&computedCPU!=S().alias->readCPU)firstFailure="alias-cpu-mismatch";NativeSignificantLocked(id,slot,mip,stage,caller,list,layout,tableIndex,rootIndex,selected,computedCPU,defaultCPU,bind.valid,firstFailure);
   if(bind.valid&&S().alias&&bind.cpu==S().alias->readCPU&&S().matchedNativeRows[stage]++<8)Log("[MenuPreviewUI] matched-preview-slot list=%llx id=%u slot=%u stage=%u table=%u root=%u selected=%u caller=%llx\n",list,id,slot,unsigned(stage),bind.tableIndex,bind.rootIndex,bind.relative,caller);
   if(S().nativeRows++<32)Log("[MenuPreviewUI] native-slot-binding list=%llx id=%u srvCPU=%llx slot=%u stage=%u table=%u root=%u selected=%u valid=%u caller=%llx; planned only\n",list,id,bind.cpu,slot,unsigned(stage),bind.tableIndex,bind.rootIndex,bind.relative,unsigned(bind.valid),caller);
  }else NativeSignificantLocked(id,slot,mip,stage,caller,0,layout,tableIndex,rootIndex,selected,computedCPU,defaultCPU,bind.valid,"list-store-missing");
  MaybeNativeSummaryLocked();
 });}
void UiDrawReturned(ID3D12GraphicsCommandList* list,UINT elements,UINT instances,UINT firstVertex,bool indexed,UINT firstIndex,INT baseVertex,UINT firstInstance)noexcept{CapturePanelDrawWitness(list,elements,instances,firstVertex,indexed,firstIndex,baseVertex,firstInstance);UiTransformWitness transformWitness;PipelineProbeRequest pipelineProbe;TransferDestination transferDestination;UiRetiredQualifiedOwners<std::shared_ptr<const CameraApplication>,std::shared_ptr<const EditorUiPublication>> retiredQualified;Safe([&]{const auto lifetime=TrackList(list);if(!lifetime)return;if(!Active())return;if(!elements||!instances)return;std::lock_guard lock(S().mutex);Drain();auto* l=GetList(reinterpret_cast<uintptr_t>(list),lifetime);NoteUiDirectDrawLocked(l);if(!l||l->overflow)return;
 ObserveOffscreenDrawLocked(*l,elements,instances);SelectBridgeProbeLocked(*l,elements,instances,pipelineProbe);SelectTransferDestinationLocked(*l,elements,instances,transferDestination);
 for(size_t targetIndex=0;targetIndex<l->targets.size();++targetIndex){const auto target=l->targets[targetIndex];if(!target)continue;if(!Buffer(target)){bool portrait=false;for(auto& pin:S().portraitPins)if(reinterpret_cast<uintptr_t>(pin.Get())==target)portrait=true;if(portrait){Event unknown;unknown.writer=true;unknown.writerReason=WriterReason::UnknownDraw;unknown.nativeNode=cvr::detail::t_current_node_work;unknown.writtenResource=target;unknown.resourceGeneration=l->targetGenerations[targetIndex];Add(*l,std::move(unknown));}continue;}Event e;e.target=Buffer(target)->resource;NoteTargetEgressLocked(*l,target);
 if(l->root&&S().alias){auto* source=GetDescriptor(S().alias->readCPU);if(source&&!source->pending&&source->resource){for(const auto& bind:l->native){if(!bind.valid||bind.cpu!=S().alias->readCPU||bind.rootIndex>=l->tables.size())continue;const auto& table=l->tables[bind.rootIndex];if(!table.proven||!UiSelectedBinding(bind.layout,bind.tableIndex,bind.rootIndex,bind.relative,table.layout,table.tableIndex,bind.rootIndex,table.count))continue;for(auto& heap:S().heaps){if(!heap.gpu)continue;bool bound=false;for(auto h:l->heaps)if(h==heap.owner)bound=true;if(!bound||table.gpu<heap.gpu||(table.gpu-heap.gpu)%heap.increment)continue;const auto distance=(table.gpu-heap.gpu)/heap.increment;if(distance>=heap.count||table.count>heap.count-distance||table.cpu!=heap.cpu+distance*heap.increment)continue;
 const auto cpu=table.cpu+uintptr_t(bind.relative)*heap.increment;auto* d=GetDescriptor(cpu);if(d&&!d->pending&&d->resource==source->resource&&d->resourceGeneration==source->resourceGeneration&&d->originCPU==source->originCPU&&d->originVersion==source->originVersion){if(e.resource&&reinterpret_cast<uintptr_t>(e.resource.Get())!=d->resource){MarkOverflow(*l,OverflowReason::ResourceConflict,reinterpret_cast<uintptr_t>(e.resource.Get()),d->resource);return;}for(size_t pin=0;pin<S().portraitPins.size();++pin)if(reinterpret_cast<uintptr_t>(S().portraitPins[pin].Get())==d->resource&&S().portraitGenerations[pin]==d->resourceGeneration)e.resource=S().portraitPins[pin];e.resourceGeneration=d->resourceGeneration;e.descriptorCPU=cpu;e.descriptorVersion=d->version;}
 }}}
 }

 if(S().alias&&S().nativeDiagnostic.drawRows<4){bool nativeMatch=false,rootTableMatch=false;
  for(const auto& bind:l->native){if(!bind.valid||bind.cpu!=S().alias->readCPU)continue;nativeMatch=true;if(bind.rootIndex>=l->tables.size())continue;const auto& table=l->tables[bind.rootIndex];if(l->root&&table.proven&&UiSelectedBinding(bind.layout,bind.tableIndex,bind.rootIndex,bind.relative,table.layout,table.tableIndex,bind.rootIndex,table.count)){rootTableMatch=true;break;}}
  const char* barrier=!nativeMatch?"noNativeMatch":!rootTableMatch?"noRootTable":!(e.descriptorCPU&&e.resource)?"noDescriptorLineage":"admittedDraw";
  ++S().nativeDiagnostic.drawRows;Log("[MenuPreviewUI] ui-draw-barrier epoch=%u setup=%u list=%llx target=%llx root=%llx nativeMatch=%u rootTableMatch=%u descriptorCPU=%llx sampledResource=%llx barrier=%s; first UI target draws only\n",S().alias->scope.screenEpoch,S().alias->scope.setupID,reinterpret_cast<uintptr_t>(list),target,l->root,unsigned(nativeMatch),unsigned(rootTableMatch),e.descriptorCPU,reinterpret_cast<uintptr_t>(e.resource.Get()),barrier);
 }
 DrawDescriptorProbeLocked(*l,target);
 Add(*l,std::move(e));}
 });
 // Cache locks and root deserialization stay outside S().mutex. No Draw is
 // admitted from a descriptor-range overlap alone.
 if(pipelineProbe.bridge)try{if(VerifiedBridgePipeline(pipelineProbe))Safe([&]{const auto lifetime=TrackList(list);if(!lifetime||!Active())return;std::lock_guard lock(S().mutex);Drain();
   auto* recorded=GetList(reinterpret_cast<uintptr_t>(list),lifetime);if(!recorded||recorded->overflow||recorded->generation!=pipelineProbe.generation||!S().alias||!Current(S().alias->scope))return;
   PipelineProbeRequest current;SelectBridgeProbeLocked(*recorded,elements,instances,current);
   if(!current.bridge||current.target!=pipelineProbe.target||current.targetGeneration!=pipelineProbe.targetGeneration||current.source!=pipelineProbe.source||current.sourceGeneration!=pipelineProbe.sourceGeneration||current.descriptorCPU!=pipelineProbe.descriptorCPU||current.descriptorVersion!=pipelineProbe.descriptorVersion||current.aliasCPU!=pipelineProbe.aliasCPU||current.originVersion!=pipelineProbe.originVersion||current.pipeline!=pipelineProbe.pipeline||current.root!=pipelineProbe.root||current.epoch!=pipelineProbe.epoch||current.setup!=pipelineProbe.setup)return;
   QualifyPanelWitnessLocked(*recorded,S().alias->scope,current.target,current.targetGeneration);
   auto& diag=S().nativeDiagnostic;TrackedTarget* tracked=nullptr;for(uint32_t i=0;i<diag.targetCount;++i)if(diag.targets[i].resource==current.target&&diag.targets[i].generation==current.targetGeneration){tracked=&diag.targets[i];break;}
   if(!tracked){tracked=ReserveTrackedTargetLocked();if(!tracked)return;tracked->resource=current.target;tracked->generation=current.targetGeneration;tracked->source=current.source;tracked->sourceGeneration=current.sourceGeneration;}
   const bool nativeDual=S().portraitModeKnown&&S().portraitMode==NativePreviewMode::NativeEditorDual&&S().alias->scope.screen==2;
   const auto action=DecideUiQualifiedSource(nativeDual,!nativeDual||SameEditorApplicationScope(tracked->sourceScope,S().alias->scope),tracked->source,tracked->sourceGeneration,current.source,current.sourceGeneration);
   if(action==UiQualifiedSourceAction::Reject)return;
   if(action==UiQualifiedSourceAction::Replace){const auto previous=tracked->source;const auto previousGeneration=tracked->sourceGeneration;
    retiredQualified=ReplaceUiQualifiedTargetSource(*tracked,current.source,current.sourceGeneration,S().alias->scope);SeedTrackedHandlesLocked(*tracked);++diag.lineageRevision;
    const auto row=++diag.sourceRebinds;if(row<=4||row==128||row==1024)try{Log("[EditorNativeQualifiedSource] count=%llu epoch=%u setup=%u target=%llx targetGeneration=%llu oldSource=%llx oldGeneration=%llu source=%llx sourceGeneration=%llu list=%llx recording=%llu descriptor=%llx descriptorVersion=%llu; verified current primary alias Draw, prior qualification retired\n",row,S().alias->scope.screenEpoch,S().alias->scope.setupID,current.target,current.targetGeneration,previous,previousGeneration,current.source,current.sourceGeneration,current.list,current.generation,current.descriptorCPU,current.descriptorVersion);}catch(...){}
   }
   Event e;if(EditorUiSeparationEnabled())e.nativeNode=cvr::detail::t_current_node_work;e.qualifiedTarget=true;e.sourceId=current.source;e.sourceGeneration=current.sourceGeneration;e.destinationId=current.target;e.destinationGeneration=current.targetGeneration;e.descriptorCPU=current.descriptorCPU;e.descriptorVersion=current.descriptorVersion;Add(*recorded,std::move(e));if(recorded->overflow)return;
   if(UiRasterDiagnosticsEnabled()&&S().alias->scope.screen==2&&diag.transformRows<6){transformWitness={current,S().alias->scope,recorded->transform,recorded->tables,recorded->heaps,true};}
   LogUiRasterLocked(*recorded,current.source,current.sourceGeneration,current.target,current.targetGeneration,elements,instances,"qualified-portrait");
   ++diag.qualifiedDraws;if(diag.qualifiedRows<4){++diag.qualifiedRows;Log("[MenuPreviewUI] ui-qualified-portrait-draw epoch=%u setup=%u list=%llx generation=%llu target=%llx targetGeneration=%llu source=%llx sourceGeneration=%llu descriptorCPU=%llx descriptorVersion=%llu psHash=%016llX rootIndex=26 shaderRegister=77; exact Draw recorded, queue/eye pending submission\n",diag.epoch,diag.setup,recorded->identity,recorded->generation,current.target,current.targetGeneration,current.source,current.sourceGeneration,current.descriptorCPU,current.descriptorVersion,0x798FA69016E1DB5Dull);}
  });
 }catch(...){}
 // Root/shader cache resolution and mapped-upload copies never run under S().mutex.
 if(transformWitness.captured)try{
  PipelineShaderEvidence shaders;
  const auto& witness=transformWitness;const auto& probe=witness.probe;const auto& bindings=witness.bindings;
  if(QueryPipelineShaders(probe.pipeline,shaders)&&shaders.psHash==0x798FA69016E1DB5Dull&&shaders.vsHash==0x8D749B2D19263CDFull&&shaders.psBytes==2844&&shaders.vsBytes==3042){
   VertexConstantLocation locationB1,locationB5;
   const bool resolvedB1=ResolveVertexConstantLocation(probe.root,1,0,locationB1);
   const bool resolvedB5=ResolveVertexConstantLocation(probe.root,5,0,locationB5);
   UiConstantProof constantB1,constantB5;bool descriptorsCaptured=false;
   Safe([&]{const auto lifetime=TrackList(list);if(!lifetime)return;std::lock_guard lock(S().mutex);Drain();
    const auto* recorded=FindUiTransformWitnessLocked(reinterpret_cast<uintptr_t>(list),lifetime,witness,elements,instances);if(!recorded)return;
    constantB1=CaptureUiConstantLocked(*recorded,locationB1,resolvedB1,64);
    constantB5=CaptureUiConstantLocked(*recorded,locationB5,resolvedB5,48);descriptorsCaptured=true;
   });
   if(descriptorsCaptured){
    const uint64_t gpuB1=constantB1.gpu,gpuB5=constantB5.gpu;
    std::array<uint8_t,64> bytesB1{};std::array<uint8_t,48> bytesB5{};std::array<uint8_t,144> bytesVB{};
    const bool knownB1=constantB1.payloadKnown&&ReadUiUploadStable(gpuB1,bytesB1),knownB5=constantB5.payloadKnown&&ReadUiUploadStable(gpuB5,bytesB5);
    const auto& vb=bindings.vertex0;uint64_t gpuVB{};
    const uint64_t offset=uint64_t(firstVertex)*vb.StrideInBytes;
    if(bindings.vertex0Known&&firstVertex!=UINT32_MAX&&elements==6&&instances==1&&vb.StrideInBytes==24&&
       offset<=vb.SizeInBytes&&bytesVB.size()<=uint64_t(vb.SizeInBytes)-offset&&vb.BufferLocation<=UINT64_MAX-offset)gpuVB=vb.BufferLocation+offset;
    const bool knownVB=ReadUiUploadStable(gpuVB,bytesVB);
    const auto hexB1=UiHexBytes(bytesB1);const auto hexB5=UiHexBytes(bytesB5);const auto hexVB=UiHexBytes(bytesVB);
    Safe([&]{const auto lifetime=TrackList(list);if(!lifetime)return;std::lock_guard lock(S().mutex);Drain();
     const auto* recorded=FindUiTransformWitnessLocked(reinterpret_cast<uintptr_t>(list),lifetime,witness,elements,instances);if(!recorded)return;
     if(CaptureUiConstantLocked(*recorded,locationB1,resolvedB1,64)!=constantB1||CaptureUiConstantLocked(*recorded,locationB5,resolvedB5,48)!=constantB5)return;
     auto& diag=S().nativeDiagnostic;const unsigned knownMask=unsigned(knownB1)|(unsigned(knownB5)<<1);
     if(diag.transformRows>=6||(knownMask!=3&&(diag.transformKnownStates&(1u<<knownMask))))return;
     diag.transformKnownStates|=1u<<knownMask;++diag.transformRows;
     try{Log("[MenuPreviewUI] ui-editor-transform row=%u epoch=%u setup=%u list=%llx generation=%llu pso=%llx root=%llx source=%llx sourceGeneration=%llu target=%llx targetGeneration=%llu b1Root=%u b1Resolved=%u b1Bound=%u b1GPU=%llx b1Known=%u b1HEX=%s b5Root=%u b5Resolved=%u b5Bound=%u b5GPU=%llx b5Known=%u b5HEX=%s vb0Bound=%u vb0GPU=%llx vb0Size=%u vb0Stride=%u firstVertex=%u vbReadGPU=%llx vbKnown=%u vbHEX=%s elements=%u instances=%u b1Table=%u b1Offset=%llu b1TableGPU=%llx b1CPU=%llx b1Version=%llu b1Heap=%llx b1HeapGeneration=%llu b1Size=%u b1PayloadKnown=%u b1DescriptorKnown=%u b1Pending=%u b5Table=%u b5Offset=%llu b5TableGPU=%llx b5CPU=%llx b5Version=%llu b5Heap=%llx b5HeapGeneration=%llu b5Size=%u b5PayloadKnown=%u b5DescriptorKnown=%u b5Pending=%u; double-identical CPU mapped bytes only, no GPU completion or ownership claim\n",
      diag.transformRows,witness.scope.screenEpoch,witness.scope.setupID,probe.list,probe.generation,probe.pipeline,probe.root,probe.source,probe.sourceGeneration,probe.target,probe.targetGeneration,
      locationB1.rootIndex,unsigned(resolvedB1),unsigned(constantB1.bound),gpuB1,unsigned(knownB1),knownB1?hexB1.data():"NA",
      locationB5.rootIndex,unsigned(resolvedB5),unsigned(constantB5.bound),gpuB5,unsigned(knownB5),knownB5?hexB5.data():"NA",
      unsigned(bindings.vertex0Known),vb.BufferLocation,vb.SizeInBytes,vb.StrideInBytes,firstVertex,gpuVB,unsigned(knownVB),knownVB?hexVB.data():"NA",elements,instances,
      unsigned(locationB1.table),locationB1.descriptorOffset,constantB1.tableGPU,constantB1.cpu,constantB1.version,constantB1.heap,constantB1.heapGeneration,constantB1.bytes,unsigned(constantB1.payloadKnown),unsigned(constantB1.descriptorPresent),unsigned(constantB1.pending),
      unsigned(locationB5.table),locationB5.descriptorOffset,constantB5.tableGPU,constantB5.cpu,constantB5.version,constantB5.heap,constantB5.heapGeneration,constantB5.bytes,unsigned(constantB5.payloadKnown),unsigned(constantB5.descriptorPresent),unsigned(constantB5.pending));}catch(...){}
    });
   }
  }
 }catch(...){} // Diagnostic failure must not change pixel admission.
 if(transferDestination.ready){const auto locations=UsedTransferLocations(transferDestination.pipeline,transferDestination.root);
  if((locations.known&&locations.count)||transferDestination.backBuffer)Safe([&]{const auto lifetime=TrackList(list);if(!lifetime||!Active())return;std::lock_guard lock(S().mutex);Drain();auto* recorded=GetList(reinterpret_cast<uintptr_t>(list),lifetime);if(!recorded||recorded->overflow||recorded->generation!=transferDestination.listGeneration||recorded->pipeline!=transferDestination.pipeline||recorded->root!=transferDestination.root||!S().alias||!Current(S().alias->scope))return;
   TransferDestination current;SelectTransferDestinationLocked(*recorded,elements,instances,current);if(!current.ready||current.targets!=transferDestination.targets||current.generations!=transferDestination.generations||current.epoch!=transferDestination.epoch||current.setup!=transferDestination.setup)return;
   const bool colorKnown=locations.shaderKnown&&locations.shader.colorOutputsKnown;const auto outputMask=colorKnown?locations.shader.colorOutputMask:0u;const bool outputMaskValid=outputMask&&(outputMask&~uint32_t(0xF))==0;
   auto& diag=S().nativeDiagnostic;bool emitDecision=false;uint32_t decisionIndex{};if(current.backBuffer&&diag.finalDecisionCount<diag.finalDecisions.size()){bool seen=false;for(uint32_t i=0;i<diag.finalDecisionCount;++i)seen|=diag.finalDecisions[i].pipeline==current.pipeline&&diag.finalDecisions[i].root==current.root;if(!seen){decisionIndex=++diag.finalDecisionCount;diag.finalDecisions[decisionIndex-1]={current.pipeline,current.root};emitDecision=true;}}
   std::array<FinalDecisionLocation,16> decisionLocations{};
   const Heap* boundHeap=nullptr;for(const auto& heap:S().heaps)if(heap.owner&&heap.gpu&&heap.increment&&heap.count){for(auto h:recorded->heaps)if(h==heap.owner){boundHeap=&heap;break;}if(boundHeap)break;}
   uintptr_t sourceId{},descriptorCPU{};uint64_t sourceGeneration{},descriptorVersion{},selectedOffset{};uint32_t selectedRoot{},matches{},descriptorMissing{},descriptorStale{},descriptorUntracked{};
   for(uint32_t i=0;i<locations.count;++i){const auto loc=locations.rows[i];auto& decision=decisionLocations[i];decision.rootIndex=loc.rootIndex;decision.offset=loc.descriptorOffset;if(loc.rootIndex>=recorded->tables.size())continue;
    const auto& table=recorded->tables[loc.rootIndex];decision.tableKnown=true;decision.tableProven=table.proven;decision.tableCount=table.count;decision.tableGPU=table.gpu;decision.tableCPU=table.cpu;
    if(!boundHeap||!table.proven||!table.count||loc.descriptorOffset>=table.count||!table.cpu||!table.gpu)continue;const auto& heap=*boundHeap;if(table.gpu<heap.gpu)continue;const auto distance=table.gpu-heap.gpu;if(distance%heap.increment||distance/heap.increment>=heap.count||table.count>heap.count-distance/heap.increment||table.cpu!=heap.cpu+distance)continue;
    const auto cpu=table.cpu+uintptr_t(loc.descriptorOffset)*heap.increment;decision.descriptorCPU=cpu;const auto* descriptor=GetDescriptor(cpu);if(!descriptor){++descriptorMissing;continue;}decision.descriptorPending=descriptor->pending;decision.resource=descriptor->resource;decision.generation=descriptor->resourceGeneration;decision.version=descriptor->version;decision.tracked=FindTrackedTargetLocked(descriptor->resource,descriptor->resourceGeneration)!=nullptr;
    if(descriptor->pending||descriptor->unorderedAccess||!descriptor->resource||!descriptor->resourceGeneration)++descriptorStale;else if(!decision.tracked)++descriptorUntracked;
    bool isOutput=false;for(uint32_t t=0;t<current.targets.size();++t)if((outputMask&(1u<<t))&&descriptor->resource==current.targets[t]&&descriptor->resourceGeneration==current.generations[t])isOutput=true;
    if(descriptor->pending||descriptor->unorderedAccess||!descriptor->resource||!descriptor->resourceGeneration||isOutput||!decision.tracked)continue;
    ++matches;sourceId=descriptor->resource;sourceGeneration=descriptor->resourceGeneration;descriptorCPU=cpu;descriptorVersion=descriptor->version;selectedRoot=loc.rootIndex;selectedOffset=loc.descriptorOffset;
   }
   if(emitDecision){Log("[MenuPreviewUI] ui-final-transfer-decision epoch=%u setup=%u decision=%u list=%llx listGeneration=%llu boundBackBuffer=%llx boundBackBufferMask=%X colorOutputsKnown=%u colorOutputMask=%X writtenBackBufferMask=%X elements=%u instances=%u pso=%llx root=%llx shaderKnown=%u psHash=%016llX srvBindingsKnown=%u srvBindingCount=%u resolvedKnown=%u resolvedCount=%u boundHeap=%llx descriptorMissing=%u descriptorStale=%u descriptorUntracked=%u matches=%u ambiguous=%u qualifiedSubmissions=%llu shaderTransfers=%llu copyAdmissions=%llu; first four distinct backbuffer PSO/root only\n",diag.epoch,diag.setup,decisionIndex,current.list,current.listGeneration,current.resource,current.backBufferMask,unsigned(colorKnown),outputMask,outputMask&current.backBufferMask,elements,instances,current.pipeline,current.root,unsigned(locations.shaderKnown),locations.shader.psHash,unsigned(locations.shader.srvBindingsKnown),locations.shader.srvBindingCount,unsigned(locations.known),locations.count,boundHeap?boundHeap->owner:0,descriptorMissing,descriptorStale,descriptorUntracked,matches,unsigned(matches>1),diag.qualifiedSubmissions,diag.shaderTransfers,diag.copyAdmissions);
    for(uint32_t i=0;i<locations.count;++i){const auto& row=decisionLocations[i];Log("[MenuPreviewUI] ui-final-transfer-location decision=%u index=%u rootIndex=%u offset=%u tableKnown=%u tableProven=%u tableCount=%u tableGPU=%llx tableCPU=%llx descriptorCPU=%llx descriptorPending=%u descriptorVersion=%llu resource=%llx resourceGeneration=%llu tracked=%u; actual bound descriptor only\n",decisionIndex,i,row.rootIndex,row.offset,unsigned(row.tableKnown),unsigned(row.tableProven),row.tableCount,row.tableGPU,row.tableCPU,row.descriptorCPU,unsigned(row.descriptorPending),row.version,row.resource,row.generation,unsigned(row.tracked));}
   }
   if(matches!=1||!outputMaskValid)return;
   for(uint32_t t=0;t<current.targets.size();++t)if((outputMask&(1u<<t))&&(!current.targets[t]||!current.generations[t]))return;
   for(uint32_t t=0;t<current.targets.size();++t)if(outputMask&(1u<<t)){const auto destination=current.targets[t],destinationGeneration=current.generations[t];if(!destination||!destinationGeneration)return;const bool backBuffer=(current.backBufferMask&(1u<<t))!=0;
    if(backBuffer){const auto* registered=Buffer(destination);if(!registered||registered->generation!=destinationGeneration)return;}
    else if(!FindTrackedTargetLocked(destination,destinationGeneration)){auto* target=ReserveTrackedTargetLocked();if(!target)return;target->resource=destination;target->generation=destinationGeneration;target->source=sourceId;target->sourceGeneration=sourceGeneration;SeedTrackedHandlesLocked(*target);}
    Event e;e.shaderTransfer=true;e.sourceId=sourceId;e.sourceGeneration=sourceGeneration;e.destinationId=destination;e.destinationGeneration=destinationGeneration;e.descriptorCPU=descriptorCPU;e.descriptorVersion=descriptorVersion;Add(*recorded,std::move(e));if(recorded->overflow)return;
    if(backBuffer)LogUiRasterLocked(*recorded,sourceId,sourceGeneration,destination,destinationGeneration,elements,instances,"qualified-backbuffer-transfer");
    ++diag.shaderTransfers;if(diag.shaderTransferRows<8){++diag.shaderTransferRows;Log("[MenuPreviewUI] ui-shader-transfer-recorded epoch=%u setup=%u list=%llx generation=%llu source=%llx sourceGeneration=%llu destination=%llx destinationGeneration=%llu rtvSlot=%u backBuffer=%u descriptorCPU=%llx descriptorVersion=%llu pso=%llx root=%llx rootIndex=%u offset=%llu; PSV singleton Texture2D SRV and actual PS color-output mask, app pending submission\n",diag.epoch,diag.setup,current.list,current.listGeneration,sourceId,sourceGeneration,destination,destinationGeneration,t,unsigned(backBuffer),descriptorCPU,descriptorVersion,current.pipeline,current.root,selectedRoot,selectedOffset);}
   }
  });
 }
}
void UiDispatchReturned(ID3D12GraphicsCommandList* list,UINT x,UINT y,UINT z)noexcept{
 if(!list||!x||!y||!z||!UiConsumerEnabled())return;ComputeListProbe probe;
 Safe([&]{const auto lifetime=TrackList(list);if(!lifetime||!Active())return;std::lock_guard lock(S().mutex);auto* recorded=GetList(reinterpret_cast<uintptr_t>(list),lifetime);if(!recorded||recorded->overflow||!recorded->pipeline||!S().nativeDiagnostic.targetCount)return;probe={recorded->identity,recorded->pipeline,0,recorded->generation,S().nativeDiagnostic.epoch,S().nativeDiagnostic.setup,true};});
 if(!probe.ready)return;cvr::stereo::scene_state::Bindings before;if(!cvr::stereo::scene_state::SnapshotCompute(list,before)||!before.computeRoot||!before.heapsSeen||!before.heapCount||before.heapCount>before.heaps.size())return;
 probe.root=reinterpret_cast<uintptr_t>(before.computeRoot);const auto locations=UsedComputeLocations(probe.pipeline,probe.root);if(!locations.known||!locations.count)return;
 cvr::stereo::scene_state::Bindings after;if(!cvr::stereo::scene_state::SnapshotCompute(list,after)||after.computeRoot!=before.computeRoot||after.computeTableMask!=before.computeTableMask||after.heapCount!=before.heapCount||!after.heapsSeen)return;
 for(UINT i=0;i<before.heapCount;++i)if(after.heaps[i]!=before.heaps[i])return;
 for(uint32_t i=0;i<locations.count;++i){const auto index=locations.rows[i].rootIndex;if(index>=before.computeTables.size()||after.computeTables[index].ptr!=before.computeTables[index].ptr)return;}
 Safe([&]{const auto lifetime=TrackList(list);if(!lifetime||!Active())return;std::lock_guard lock(S().mutex);Drain();auto* recorded=GetList(reinterpret_cast<uintptr_t>(list),lifetime);if(!recorded||recorded->overflow||recorded->generation!=probe.generation||recorded->pipeline!=probe.pipeline||!S().alias||!Current(S().alias->scope)||S().nativeDiagnostic.epoch!=probe.epoch||S().nativeDiagnostic.setup!=probe.setup)return;
  const Heap* heap=nullptr;for(const auto& candidate:S().heaps)if(candidate.owner&&candidate.gpu&&candidate.increment&&candidate.count){for(UINT i=0;i<after.heapCount;++i)if(reinterpret_cast<uintptr_t>(after.heaps[i])==candidate.owner){heap=&candidate;break;}if(heap)break;}if(!heap)return;
  std::array<ComputeInputProof,16> inputs{};uint32_t inputCount{};struct Output {uintptr_t resource{},cpu{};uint64_t generation{},version{};};std::array<Output,8> outputs{};uint32_t outputCount{};
  for(uint32_t i=0;i<locations.count;++i){const auto loc=locations.rows[i];if(loc.rootIndex>=after.computeTables.size()||!(after.computeTableMask&(1ull<<loc.rootIndex)))continue;const auto gpu=after.computeTables[loc.rootIndex].ptr;if(gpu<heap->gpu)continue;const auto start=gpu-heap->gpu;if(start%heap->increment||start/heap->increment>=heap->count||loc.descriptorOffset>=heap->count-start/heap->increment)continue;
   const auto cpu=heap->cpu+start+uintptr_t(loc.descriptorOffset)*heap->increment;const auto* descriptor=GetDescriptor(cpu);if(!descriptor||descriptor->pending||!descriptor->resource||!descriptor->resourceGeneration||descriptor->unorderedAccess!=loc.unorderedAccess)continue;
   if(loc.unorderedAccess){if(outputCount<outputs.size())outputs[outputCount++]={descriptor->resource,cpu,descriptor->resourceGeneration,descriptor->version};continue;}
   if(!FindTrackedTargetLocked(descriptor->resource,descriptor->resourceGeneration))continue;bool duplicate=false;for(uint32_t n=0;n<inputCount;++n)duplicate|=inputs[n].cpu==cpu;if(!duplicate&&inputCount<inputs.size())inputs[inputCount++]={descriptor->resource,cpu,descriptor->resourceGeneration,descriptor->version,false};
  }
  if(!outputCount)return;
  for(uint32_t o=0;o<outputCount;++o){const auto output=outputs[o];bool inPlace=false;for(uint32_t i=0;i<inputCount;++i)inPlace|=inputs[i].resource==output.resource&&inputs[i].generation==output.generation;if(inPlace)continue;
   auto* backBuffer=Buffer(output.resource);if(backBuffer&&backBuffer->generation!=output.generation)backBuffer=nullptr;
   bool portrait=false;for(size_t p=0;p<S().portraitPins.size();++p)portrait|=reinterpret_cast<uintptr_t>(S().portraitPins[p].Get())==output.resource&&S().portraitGenerations[p]==output.generation;
   if(!inputCount&&!portrait&&!backBuffer&&!FindTrackedTargetLocked(output.resource,output.generation))continue;
   if(inputCount&&!portrait&&!backBuffer&&!FindTrackedTargetLocked(output.resource,output.generation)){auto* target=ReserveTrackedTargetLocked();if(!target)continue;target->resource=output.resource;target->generation=output.generation;target->source=inputs[0].resource;target->sourceGeneration=inputs[0].generation;SeedTrackedHandlesLocked(*target);}
   Event e;e.computeTransfer=true;e.destinationId=output.resource;e.destinationGeneration=output.generation;e.descriptorCPU=output.cpu;e.descriptorVersion=output.version;if(inputCount){e.compute=std::make_shared<ComputeEvidence>();e.compute->inputCount=inputCount;for(uint32_t i=0;i<inputCount;++i)e.compute->inputs[i]=inputs[i];}Add(*recorded,std::move(e));if(recorded->overflow)return;
   auto& diag=S().nativeDiagnostic;if(diag.computeRows<8){++diag.computeRows;Log("[MenuPreviewUI] ui-compute-transfer-recorded epoch=%u setup=%u list=%llx generation=%llu csHash=%016llX groups=%u,%u,%u inputCount=%u output=%llx outputGeneration=%llu outputUavCPU=%llx outputVersion=%llu backBuffer=%u; declared Texture2D UAV write, zero tracked inputs means invalidation only\n",diag.epoch,diag.setup,recorded->identity,recorded->generation,locations.shader.csHash,x,y,z,inputCount,output.resource,output.generation,output.cpu,output.version,unsigned(backBuffer!=nullptr));}
  }
 });
}
void UiCopyResourceReturned(ID3D12GraphicsCommandList* list,ID3D12Resource* destination,ID3D12Resource* source)noexcept{Safe([&]{if(!list||!destination)return;const auto lifetime=TrackList(list);if(!lifetime||!Active())return;
 const auto destinationGeneration=TrackObject(destination,ObjectKind::Resource);const auto sourceGeneration=source?TrackObject(source,ObjectKind::Resource):0;
 std::lock_guard lock(S().mutex);Drain();auto* recorded=GetList(reinterpret_cast<uintptr_t>(list),lifetime);if(!recorded||recorded->overflow)return;
 const auto destinationId=reinterpret_cast<uintptr_t>(destination),sourceId=reinterpret_cast<uintptr_t>(source);bool relevant=false;
 if(auto* backBuffer=Buffer(destinationId);backBuffer&&backBuffer->generation==destinationGeneration)relevant=true;
 for(size_t p=0;p<S().portraitPins.size();++p)relevant|=reinterpret_cast<uintptr_t>(S().portraitPins[p].Get())==destinationId&&S().portraitGenerations[p]==destinationGeneration;
 for(uint32_t i=0;i<S().nativeDiagnostic.targetCount;++i){const auto& target=S().nativeDiagnostic.targets[i];relevant|=(target.resource==sourceId&&target.generation==sourceGeneration)||(target.resource==destinationId&&target.generation==destinationGeneration);}
 if(FindTrackedTargetLocked(sourceId,sourceGeneration)&&!Buffer(destinationId)&&!FindTrackedTargetLocked(destinationId,destinationGeneration)&&destinationGeneration){if(auto* tracked=ReserveTrackedTargetLocked()){tracked->resource=destinationId;tracked->generation=destinationGeneration;tracked->source=sourceId;tracked->sourceGeneration=sourceGeneration;SeedTrackedHandlesLocked(*tracked);}}
 if(!relevant)return;Event e;e.fullCopy=true;e.sourceId=sourceId;e.sourceGeneration=sourceGeneration;e.destinationId=destinationId;e.destinationGeneration=destinationGeneration;Add(*recorded,std::move(e));
 });}
void UiResourceBarriers(ID3D12GraphicsCommandList* list,UINT count,const D3D12_RESOURCE_BARRIER* barriers)noexcept{Safe([&]{
 const auto lifetime=TrackList(list);if(!lifetime||!Active()||!barriers)return;
 struct AliasEvidence {uintptr_t before{},after{};UiAllocation beforeAllocation{},afterAllocation{};};
 std::array<AliasEvidence,64> aliases{};uint32_t aliasCount{};bool overflow=count>4096;
 // D3D calls stay outside the UI mutex. Allocation metadata is immutable for each resource.
 if(!overflow)for(UINT i=0;i<count;++i){const auto& barrier=barriers[i];if(barrier.Type!=D3D12_RESOURCE_BARRIER_TYPE_ALIASING)continue;
  if(aliasCount==aliases.size()){overflow=true;break;}
  const auto& a=barrier.Aliasing;aliases[aliasCount++]={reinterpret_cast<uintptr_t>(a.pResourceBefore),reinterpret_cast<uintptr_t>(a.pResourceAfter),ReadUiAllocation(a.pResourceBefore),ReadUiAllocation(a.pResourceAfter)};
 }
 if(!aliasCount&&!overflow)return;
 std::lock_guard lock(S().mutex);auto* l=GetList(reinterpret_cast<uintptr_t>(list),lifetime);if(!l)return;
 for(size_t p=0;p<S().portraitPins.size();++p){auto* resource=S().portraitPins[p].Get();if(!resource)continue;bool invalidate=overflow;
  const auto& allocation=S().portraitAllocations[p];
  for(uint32_t i=0;i<aliasCount&&!invalidate;++i){const auto& a=aliases[i];
   invalidate=UiAliasInvalidates(reinterpret_cast<uintptr_t>(resource),allocation,a.before,a.beforeAllocation,a.after,a.afterAllocation);
   static uint32_t aliasRows{};if(aliasRows<12){++aliasRows;Log("[MenuPreviewUI] portrait-alias-allocation list=%llx eventOrder=%llu resource=%llx kind=%u heap=%llu offset=%llu bytes=%llu before=%llx beforeKind=%u beforeHeap=%llu beforeOffset=%llu beforeBytes=%llu after=%llx afterKind=%u afterHeap=%llu afterOffset=%llu afterBytes=%llu invalidate=%u; bounded actual allocation comparison\n",reinterpret_cast<uintptr_t>(list),uint64_t(l->count),reinterpret_cast<uintptr_t>(resource),allocation.kind,allocation.heap,allocation.offset,allocation.bytes,a.before,a.beforeAllocation.kind,a.beforeAllocation.heap,a.beforeAllocation.offset,a.beforeAllocation.bytes,a.after,a.afterAllocation.kind,a.afterAllocation.heap,a.afterAllocation.offset,a.afterAllocation.bytes,unsigned(invalidate));}
  }
  if(invalidate){Event e;e.writer=true;e.writerReason=overflow?WriterReason::BarrierCountUnknown:WriterReason::AliasingHazard;e.writtenResource=reinterpret_cast<uintptr_t>(resource);e.resourceGeneration=S().portraitGenerations[p];Add(*l,std::move(e));}
 }
});}
void UiResetList(ID3D12GraphicsCommandList* list)noexcept{Safe([&]{const auto lifetime=TrackList(list);if(!lifetime)return;const bool active=Active();std::lock_guard lock(S().mutex);const auto id=reinterpret_cast<uintptr_t>(list);auto* l=active?GetList(id,lifetime):S().lists.Find(id);if(!l||l->objectGeneration!=lifetime)return;RetirePanelWitnessLocked(*l);UnpinListLocked(*l);if(EditorUiSeparationEnabled())for(auto& t:S().nativeDiagnostic.targets)if(t.editorList==id&&t.editorLife==lifetime){t.editorUiRequired=false;t.editorUi.reset();}l->raster={};l->targetExtents={};l->transform={};if(active)S().lists.Reset(id,lifetime);else S().lists.Retire(id,lifetime);});}
void UiCloseList(ID3D12GraphicsCommandList* list,bool success)noexcept{Safe([&]{const auto lifetime=TrackList(list);if(!lifetime)return;if(!Active())return;std::lock_guard lock(S().mutex);if(auto* l=GetList(reinterpret_cast<uintptr_t>(list),lifetime)){l->closed=success;if(!success)MarkOverflow(*l,OverflowReason::CloseFailed);}});}
struct UiSubmissionData {
 struct Snapshot {uintptr_t list{};uint64_t generation{},objectGeneration{};std::vector<Event> events;std::array<UiTraceRow,4> trace{};uint32_t traceCount{};std::shared_ptr<const PanelPool::Batch> panelWitness;};
 Metadata scope;std::vector<Snapshot> lists;
};
UiSubmission PrepareUiSubmission(UINT count,ID3D12CommandList* const* lists)noexcept{
 UiSubmission token;Safe([&]{if(!Active()||!lists||count>64)return;Lease lease;if(!TryAcquireCurrent(lease))return;auto batch=std::make_shared<UiSubmissionData>();batch->scope=lease.metadata;std::lock_guard lock(S().mutex);Drain();
 for(UINT i=0;i<count;++i){auto* l=FindList(reinterpret_cast<uintptr_t>(lists[i]));if(!l)continue;if(!l->closed||l->overflow){for(auto& b:S().buffers)b.receipt.reset();return;}UiSubmissionData::Snapshot row;row.list=l->identity;row.generation=l->generation;row.objectGeneration=l->objectGeneration;row.panelWitness=FreezePanelWitnessLocked(*l,batch->scope);if(OffscreenTraceEnabled()){row.trace=l->trace;row.traceCount=l->traceCount;}
 for(size_t n=0;n<l->count;++n){auto e=l->events[n];if(e.descriptorCPU){const auto* d=GetDescriptor(e.descriptorCPU);if(!d||d->pending||d->version!=e.descriptorVersion||d->resource!=reinterpret_cast<uintptr_t>(e.resource.Get())||d->resourceGeneration!=e.resourceGeneration)e.resource.Reset();}row.events.push_back(std::move(e));}batch->lists.push_back(std::move(row));}
 token.state=std::move(batch);});return token;
}
const char* WriterReasonName(WriterReason reason)noexcept{switch(reason){case WriterReason::ObservedDraw:return "observed-portrait-draw";case WriterReason::UnknownDraw:return "other-portrait-draw";case WriterReason::AliasingHazard:return "aliasing-barrier";case WriterReason::BarrierCountUnknown:return "barrier-count-unknown";default:return "unknown";}}
void WriterDecisionLocked(const char* action,const char* reason,uintptr_t resource,uint64_t generation,uintptr_t list,uintptr_t queue,const CameraApplication* app,uintptr_t nativeNode=0){auto& diag=S().nativeDiagnostic;if(diag.writerDecisionRows>=8)return;++diag.writerDecisionRows;const auto exe=reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));Log("[MenuPreviewUI] ui-writer-decision epoch=%u setup=%u action=%s reason=%s list=%llx queue=%llx resource=%llx resourceGeneration=%llu eye=%u source=%llu revision=%llu nativeNode=%llx nativeWork=%llx; first eight scoped set/revoke decisions\n",diag.epoch,diag.setup,action,reason,list,queue,resource,generation,app?app->frame.eye:0,app?app->frame.sourceSequence:0,app?app->localPoseRevision:0,nativeNode,nativeNode>=exe?nativeNode-exe:0);}
void NoteNativeUnknownWriterLocked(const Event& event,const Writer& prior,uintptr_t resource,uintptr_t list,uintptr_t queue)noexcept{
 const auto reason=size_t(event.writerReason);if(reason>=5)return;
 static std::array<uint64_t,5> counts{};const auto count=++counts[reason];if(count>4&&count!=128&&count!=1024)return;
 const auto* app=prior.app.get();const auto exe=reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
 try{Log("[EditorNativeUnknownWriter] reason=%s count=%llu epoch=%u setup=%u list=%llx queue=%llx resource=%llx generation=%llu nativeNode=%llx nativeWork=%llx priorApp=%llx priorContext=%llx priorFrame=%u priorSource=%llu priorEye=%u priorControls=%llu priorOrigin=%llu priorBinding=%llu; exact resident primary portrait write, revocation unchanged\n",WriterReasonName(event.writerReason),count,S().nativeDiagnostic.epoch,S().nativeDiagnostic.setup,list,queue,resource,event.resourceGeneration,event.nativeNode,event.nativeNode>=exe?event.nativeNode-exe:0,reinterpret_cast<uintptr_t>(app),app?app->nativeContext:0,app?app->frame.nativeFrame:0,app?app->frame.sourceSequence:0,app?app->frame.eye:0,app?app->editorControlRevision:0,app?app->projection.origin:0,app?app->source.bindingGeneration:0);
 }catch(...){}
}
void RevokePortraitWriterLocked(uintptr_t resource,uint64_t generation,const char* reason,uintptr_t list,uintptr_t queue){bool pinned=false;for(size_t p=0;p<S().portraitPins.size();++p)pinned|=reinterpret_cast<uintptr_t>(S().portraitPins[p].Get())==resource&&S().portraitGenerations[p]==generation;if(!pinned)return;
 for(auto& writer:S().writers)if(reinterpret_cast<uintptr_t>(writer.resource.Get())==resource&&writer.generation==generation){WriterDecisionLocked("revoke",reason,resource,generation,list,queue,writer.app.get());writer={};return;}
}
uint64_t UiSubmitted(ID3D12CommandQueue* queue,UiSubmission&& token)noexcept{uint64_t actualSubmission{};Safe([&]{auto batch=std::move(token.state);if(!queue||!batch||!Current(batch->scope))return;std::lock_guard lock(S().mutex);Drain();const auto q=reinterpret_cast<uintptr_t>(queue);const auto submission=++S().submission;actualSubmission=submission;
 for(const auto& snapshot:batch->lists){auto* l=GetList(snapshot.list);if(!l||l->objectGeneration!=snapshot.objectGeneration||!UiRecordingValid(l->generation,snapshot.generation,l->closed,l->overflow)){for(auto& b:S().buffers)b.receipt.reset();return;}}
 for(const auto& snapshot:batch->lists)EmitPanelWitnessLocked(snapshot.panelWitness,q,submission);
 std::array<uintptr_t,8> touched{};size_t touchCount{};
 for(const auto& snapshot:batch->lists)for(const auto& e:snapshot.events){
  if(e.qualifiedTarget){auto* target=FindTrackedTargetLocked(e.destinationId,e.destinationGeneration);if(!target)continue;target->qualifiedApp.reset();target->qualifiedSubmission=0;target->editorUiRequired=false;target->editorUi.reset();target->editorSubmission=0;
   const auto* descriptor=GetDescriptor(e.descriptorCPU);const auto* alias=S().alias?GetDescriptor(S().alias->readCPU):nullptr;
   if(!descriptor||descriptor->pending||descriptor->unorderedAccess||descriptor->version!=e.descriptorVersion||descriptor->resource!=e.sourceId||descriptor->resourceGeneration!=e.sourceGeneration||!alias||alias->pending||alias->unorderedAccess||alias->resource!=e.sourceId||alias->resourceGeneration!=e.sourceGeneration||descriptor->originCPU!=alias->originCPU||descriptor->originVersion!=alias->originVersion)continue;
   if(S().portraitModeKnown&&S().portraitMode==NativePreviewMode::NativeEditorDual&&
      !UiQualifiedSourceEventMatches(target->source,target->sourceGeneration,e.sourceId,e.sourceGeneration))continue;
   for(const auto& writer:S().writers){if(!writer.app||reinterpret_cast<uintptr_t>(writer.resource.Get())!=e.sourceId||writer.generation!=e.sourceGeneration||writer.queue!=q||!writer.submission||writer.submission>submission||!SamePreview(writer.app->metadata,batch->scope))continue;
    UiOrderedWriter policy{e.sourceId,q,writer.app->frame.sourceSequence,writer.app->localPoseRevision,writer.app->metadata.screenEpoch,writer.app->metadata.setupID,writer.app->frame.eye,writer.app->frame.nativeFrame};if(!UiCanConsume(policy,e.sourceId,q,batch->scope.screenEpoch,batch->scope.setupID))continue;
    target->qualifiedApp=writer.app;target->qualifiedSubmission=submission;target->qualifiedQueue=q;if(EditorUiSeparationEnabled()&&batch->scope.screen==2&&e.nativeNode==reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr))+0x20A264){target->editorScope=batch->scope;target->editorList=snapshot.list;target->editorLife=snapshot.objectGeneration;target->editorRecording=snapshot.generation;target->editorNode=e.nativeNode;target->editorSubmission=submission;}++S().nativeDiagnostic.qualifiedSubmissions;break;
   }continue;
  }
  if(e.shaderTransfer){auto* destination=FindTrackedTargetLocked(e.destinationId,e.destinationGeneration);auto* backBuffer=Buffer(e.destinationId);if(backBuffer&&backBuffer->generation!=e.destinationGeneration)backBuffer=nullptr;if(!destination&&!backBuffer)continue;
   if(destination){destination->qualifiedApp.reset();destination->qualifiedSubmission=0;destination->editorUiRequired=false;destination->editorUi.reset();}if(backBuffer)backBuffer->receipt.reset();
   const auto* descriptor=GetDescriptor(e.descriptorCPU);if(!descriptor||descriptor->pending||descriptor->unorderedAccess||descriptor->version!=e.descriptorVersion||descriptor->resource!=e.sourceId||descriptor->resourceGeneration!=e.sourceGeneration)continue;
   std::shared_ptr<const CameraApplication> app;uint64_t producerSubmission{};
   if(const auto* source=FindTrackedTargetLocked(e.sourceId,e.sourceGeneration);source&&source->qualifiedApp&&source->qualifiedQueue==q&&source->qualifiedSubmission&&source->qualifiedSubmission<=submission){app=source->qualifiedApp;producerSubmission=source->qualifiedSubmission;}
   if(!app)for(const auto& writer:S().writers)if(writer.app&&reinterpret_cast<uintptr_t>(writer.resource.Get())==e.sourceId&&writer.generation==e.sourceGeneration&&writer.queue==q&&writer.submission&&writer.submission<=submission){app=writer.app;producerSubmission=writer.submission;break;}
   if(!app||!SamePreview(app->metadata,batch->scope))continue;UiOrderedWriter policy{e.sourceId,q,app->frame.sourceSequence,app->localPoseRevision,app->metadata.screenEpoch,app->metadata.setupID,app->frame.eye,app->frame.nativeFrame};if(!UiCanConsume(policy,e.sourceId,q,batch->scope.screenEpoch,batch->scope.setupID))continue;
   if(destination){destination->qualifiedApp=app;destination->qualifiedQueue=q;destination->qualifiedSubmission=submission;AlignEditorUiLocked(app,q,destination->editorUiRequired,destination->editorUi);continue;}
   auto receipt=std::make_shared<UiFrameReceipt>();receipt->application=app;receipt->scope=app->metadata;receipt->eye=app->frame.eye;receipt->nativeFrame=app->frame.nativeFrame;receipt->sourceSequence=app->frame.sourceSequence;receipt->cameraRevision=app->localPoseRevision;receipt->projection=app->projection;receipt->uiSubmission=submission;receipt->queue=q;receipt->backBuffer=e.destinationId;AlignEditorUiLocked(app,q,receipt->editorUiRequired,receipt->editorUi);backBuffer->receipt=std::move(receipt);
   if(S().consumerRows++<32)Log("[MenuPreviewUI] submitted-consumer-shader queue=%llx backBuffer=%llx sampledIntermediate=%llx generation=%llu eye=%u source=%llu revision=%llu producerSubmission=%llu drawSubmission=%llu; PSV singleton Texture2D SRV and exact same-queue lineage, GPU completion belongs to capture\n",q,e.destinationId,e.sourceId,e.sourceGeneration,backBuffer->receipt->eye,backBuffer->receipt->sourceSequence,backBuffer->receipt->cameraRevision,producerSubmission,submission);continue;
  }
  if(e.computeTransfer){RevokePortraitWriterLocked(e.destinationId,e.destinationGeneration,"compute-uav-write",snapshot.list,q);auto* destination=FindTrackedTargetLocked(e.destinationId,e.destinationGeneration);auto* backBuffer=Buffer(e.destinationId);if(backBuffer&&backBuffer->generation!=e.destinationGeneration)backBuffer=nullptr;if(!destination&&!backBuffer)continue;
   if(destination){destination->qualifiedApp.reset();destination->qualifiedSubmission=0;destination->editorUiRequired=false;destination->editorUi.reset();}if(backBuffer)backBuffer->receipt.reset();
   if(!e.compute||!e.compute->inputCount||e.compute->inputCount>e.compute->inputs.size())continue;const auto* output=GetDescriptor(e.descriptorCPU);
   if(!output||output->pending||!output->unorderedAccess||output->version!=e.descriptorVersion||output->resource!=e.destinationId||output->resourceGeneration!=e.destinationGeneration)continue;
   std::shared_ptr<const CameraApplication> app;bool valid=true;for(uint32_t i=0;i<e.compute->inputCount;++i){const auto& input=e.compute->inputs[i];const auto* descriptor=GetDescriptor(input.cpu);const auto* source=FindTrackedTargetLocked(input.resource,input.generation);
    if(!descriptor||descriptor->pending||descriptor->unorderedAccess||descriptor->version!=input.version||descriptor->resource!=input.resource||descriptor->resourceGeneration!=input.generation||!source||!source->qualifiedApp||source->qualifiedQueue!=q||!source->qualifiedSubmission||source->qualifiedSubmission>submission||!SamePreview(source->qualifiedApp->metadata,batch->scope)){valid=false;break;}
    const auto& candidate=*source->qualifiedApp;if(!app){app=source->qualifiedApp;continue;}if(app->frame.key!=candidate.frame.key||app->frame.sourceSequence!=candidate.frame.sourceSequence||app->frame.eye!=candidate.frame.eye||app->frame.nativeFrame!=candidate.frame.nativeFrame||app->nativeContext!=candidate.nativeContext||app->localPoseRevision!=candidate.localPoseRevision||!SamePreview(app->metadata,candidate.metadata)){valid=false;break;}
   }
   if(!valid||!app)continue;const auto& first=e.compute->inputs[0];UiOrderedWriter policy{first.resource,q,app->frame.sourceSequence,app->localPoseRevision,app->metadata.screenEpoch,app->metadata.setupID,app->frame.eye,app->frame.nativeFrame};if(!UiCanConsume(policy,first.resource,q,batch->scope.screenEpoch,batch->scope.setupID))continue;
   if(destination){destination->qualifiedApp=app;destination->qualifiedQueue=q;destination->qualifiedSubmission=submission;AlignEditorUiLocked(app,q,destination->editorUiRequired,destination->editorUi);continue;}
   auto receipt=std::make_shared<UiFrameReceipt>();receipt->application=app;receipt->scope=app->metadata;receipt->eye=app->frame.eye;receipt->nativeFrame=app->frame.nativeFrame;receipt->sourceSequence=app->frame.sourceSequence;receipt->cameraRevision=app->localPoseRevision;receipt->projection=app->projection;receipt->uiSubmission=submission;receipt->queue=q;receipt->backBuffer=e.destinationId;AlignEditorUiLocked(app,q,receipt->editorUiRequired,receipt->editorUi);backBuffer->receipt=std::move(receipt);
   if(S().consumerRows++<32)Log("[MenuPreviewUI] submitted-consumer-compute queue=%llx backBuffer=%llx outputGeneration=%llu inputCount=%u eye=%u source=%llu revision=%llu submission=%llu; exact PSV SRV/UAV and same-queue lineage, GPU completion belongs to capture\n",q,e.destinationId,e.destinationGeneration,e.compute->inputCount,backBuffer->receipt->eye,backBuffer->receipt->sourceSequence,backBuffer->receipt->cameraRevision,submission);continue;
  }
  if(e.fullCopy){RevokePortraitWriterLocked(e.destinationId,e.destinationGeneration,e.sourceId?"full-copy-write":"unknown-copy-write",snapshot.list,q);auto* destination=FindTrackedTargetLocked(e.destinationId,e.destinationGeneration);auto* source=FindTrackedTargetLocked(e.sourceId,e.sourceGeneration);std::shared_ptr<const CameraApplication> app;
   uint64_t producerSubmission{};if(source&&source->qualifiedApp&&source->qualifiedQueue==q&&source->qualifiedSubmission&&source->qualifiedSubmission<=submission&&SamePreview(source->qualifiedApp->metadata,batch->scope)){app=source->qualifiedApp;producerSubmission=source->qualifiedSubmission;}
   if(destination){destination->qualifiedApp.reset();destination->qualifiedSubmission=0;destination->editorUiRequired=false;destination->editorUi.reset();if(app){destination->qualifiedApp=app;destination->qualifiedQueue=q;destination->qualifiedSubmission=submission;AlignEditorUiLocked(app,q,destination->editorUiRequired,destination->editorUi);}}
   auto* backBuffer=Buffer(e.destinationId);if(!backBuffer||backBuffer->generation!=e.destinationGeneration)continue;backBuffer->receipt.reset();if(!app)continue;
   UiOrderedWriter policy{e.sourceId,q,app->frame.sourceSequence,app->localPoseRevision,app->metadata.screenEpoch,app->metadata.setupID,app->frame.eye,app->frame.nativeFrame};if(!UiCanConsume(policy,e.sourceId,q,batch->scope.screenEpoch,batch->scope.setupID))continue;
   auto receipt=std::make_shared<UiFrameReceipt>();receipt->application=app;receipt->scope=app->metadata;receipt->eye=app->frame.eye;receipt->nativeFrame=app->frame.nativeFrame;receipt->sourceSequence=app->frame.sourceSequence;receipt->cameraRevision=app->localPoseRevision;receipt->projection=app->projection;receipt->uiSubmission=submission;receipt->queue=q;receipt->backBuffer=e.destinationId;AlignEditorUiLocked(app,q,receipt->editorUiRequired,receipt->editorUi);backBuffer->receipt=std::move(receipt);++S().nativeDiagnostic.copyAdmissions;
   if(S().consumerRows++<32)Log("[MenuPreviewUI] submitted-consumer-copy queue=%llx backBuffer=%llx intermediate=%llx intermediateGeneration=%llu eye=%u source=%llu revision=%llu producerSubmission=%llu copySubmission=%llu; qualified Draw then full CopyResource on same queue, GPU completion belongs to capture\n",q,e.destinationId,e.sourceId,e.sourceGeneration,backBuffer->receipt->eye,backBuffer->receipt->sourceSequence,backBuffer->receipt->cameraRevision,producerSubmission,submission);continue;
  }
  if(e.writer){const auto resource=e.writtenResource?e.writtenResource:reinterpret_cast<uintptr_t>(e.resource.Get());Writer* row=nullptr;
   const bool nativeDual=S().portraitModeKnown&&S().portraitMode==NativePreviewMode::NativeEditorDual;
   if(nativeDual){
    // A frozen old event still owns its COM operand, but cannot resurrect a
    // proof cell after its exact resource/generation was evicted.
    if(e.app&&(e.app->source.mode!=NativePreviewMode::NativeEditorDual||!CameraApplicationUiSource(*e.app)||!EditorNativeSourceRegistered(e.app->metadata,e.app->source)))continue;
    for(size_t pin=0;pin<S().portraitPins.size();++pin)if(reinterpret_cast<uintptr_t>(S().portraitPins[pin].Get())==resource&&S().portraitGenerations[pin]==e.resourceGeneration){row=&S().writers[pin];break;}
   }else{
    if(e.app&&e.app->source.mode!=NativePreviewMode::LegacyOneView)continue;
    for(size_t pin=0;pin<LegacyPortraitSlots;++pin){auto& w=S().writers[pin];if(reinterpret_cast<uintptr_t>(w.resource.Get())==resource&&w.generation==e.resourceGeneration){row=&w;break;}}
    if(!row&&e.app)for(size_t pin=0;pin<LegacyPortraitSlots;++pin)if(!S().writers[pin].resource){row=&S().writers[pin];break;}
   }
   if(row){if(nativeDual&&!e.app)NoteNativeUnknownWriterLocked(e,*row,resource,snapshot.list,q);
    if(e.app&&SamePreview(e.app->metadata,batch->scope)){*row={e.resource,e.app,q,e.resourceGeneration,submission};WriterDecisionLocked("set",WriterReasonName(e.writerReason),resource,e.resourceGeneration,snapshot.list,q,row->app.get(),e.nativeNode);}
    else if(row->app){WriterDecisionLocked("revoke",WriterReasonName(e.writerReason),resource,e.resourceGeneration,snapshot.list,q,row->app.get(),e.nativeNode);*row={};}}
   continue;}
 auto* b=Buffer(reinterpret_cast<uintptr_t>(e.target.Get()));if(!b)continue;bool first=true;for(size_t t=0;t<touchCount;++t)if(touched[t]==reinterpret_cast<uintptr_t>(e.target.Get()))first=false;if(first){b->receipt.reset();if(touchCount<8)touched[touchCount++]=reinterpret_cast<uintptr_t>(e.target.Get());}
 if(!e.resource)continue;const auto* proof=GetDescriptor(e.descriptorCPU);if(!proof||proof->pending||proof->version!=e.descriptorVersion||proof->resource!=reinterpret_cast<uintptr_t>(e.resource.Get())||proof->resourceGeneration!=e.resourceGeneration)continue;
 for(auto& w:S().writers){if(!w.app)continue;UiOrderedWriter policy{reinterpret_cast<uintptr_t>(w.resource.Get()),w.queue,w.app->frame.sourceSequence,w.app->localPoseRevision,w.app->metadata.screenEpoch,w.app->metadata.setupID,w.app->frame.eye,w.app->frame.nativeFrame};if(!UiCanConsume(policy,reinterpret_cast<uintptr_t>(e.resource.Get()),q,batch->scope.screenEpoch,batch->scope.setupID)||!SamePreview(w.app->metadata,batch->scope))continue;
 auto r=std::make_shared<UiFrameReceipt>();r->application=w.app;r->scope=w.app->metadata;r->eye=w.app->frame.eye;r->nativeFrame=w.app->frame.nativeFrame;r->sourceSequence=w.app->frame.sourceSequence;r->cameraRevision=w.app->localPoseRevision;r->projection=w.app->projection;r->uiSubmission=submission;r->queue=q;r->backBuffer=reinterpret_cast<uintptr_t>(e.target.Get());AlignEditorUiLocked(w.app,q,r->editorUiRequired,r->editorUi);b->receipt=std::move(r);if(S().consumerRows++<32)Log("[MenuPreviewUI] submitted-consumer queue=%llx backBuffer=%llx eye=%u source=%llu revision=%llu submission=%llu; ordered writer+bound target Draw, GPU completion belongs to capture\n",q,b->receipt->backBuffer,b->receipt->eye,b->receipt->sourceSequence,b->receipt->cameraRevision,submission);
 }}
 if(OffscreenTraceEnabled())for(const auto& snapshot:batch->lists)for(uint32_t i=0;i<snapshot.traceCount;++i){const auto& row=snapshot.trace[i];TrackedTarget* tracked=nullptr;auto& diag=S().nativeDiagnostic;
  for(uint32_t t=0;t<diag.targetCount;++t)if(diag.targets[t].resource==row.resource&&diag.targets[t].generation==row.resourceGeneration){tracked=&diag.targets[t];break;}
  if(!tracked||tracked->source!=row.source||tracked->sourceGeneration!=row.sourceGeneration)continue;
  if(!row.egress){tracked->queue=q;tracked->submission=submission;tracked->sourceSequence=0;tracked->eye=0;tracked->nativeFrame=0;uint64_t writerSubmission{};
   for(const auto& writer:S().writers)if(writer.app&&reinterpret_cast<uintptr_t>(writer.resource.Get())==row.source&&writer.generation==row.sourceGeneration&&writer.queue==q&&writer.submission&&writer.submission<submission&&SamePreview(writer.app->metadata,batch->scope)){tracked->sourceSequence=writer.app->frame.sourceSequence;tracked->eye=writer.app->frame.eye;tracked->nativeFrame=writer.app->frame.nativeFrame;writerSubmission=writer.submission;break;}
   ++diag.targetSubmissions;if(diag.targetSubmissions<=4)Log("[MenuPreviewUI] ui-intermediate-submitted epoch=%u setup=%u queue=%llx submission=%llu list=%llx listGeneration=%llu order=%llu target=%llx targetGeneration=%llu source=%llx sourceGeneration=%llu writerKnown=%u writerSubmission=%llu sourceSequenceCandidate=%llu eyeCandidate=%u frameCandidate=%u; recorded Draw submitted, candidate only\n",diag.epoch,diag.setup,q,submission,snapshot.list,snapshot.generation,row.order,row.resource,row.resourceGeneration,row.source,row.sourceGeneration,unsigned(writerSubmission!=0),writerSubmission,tracked->sourceSequence,tracked->eye,tracked->nativeFrame);continue;}
  const auto* descriptor=GetDescriptor(row.descriptorCPU);const bool descriptorCurrent=descriptor&&!descriptor->pending&&descriptor->version==row.descriptorVersion&&descriptor->resource==row.resource&&descriptor->resourceGeneration==row.resourceGeneration;
  const bool sameQueueOrdered=descriptorCurrent&&tracked->queue==q&&tracked->submission&&tracked->submission<=submission;
  if(sameQueueOrdered)++diag.egressSubmissions;
  if(diag.egressSubmitRows<8){++diag.egressSubmitRows;Log("[MenuPreviewUI] ui-intermediate-egress-submitted epoch=%u setup=%u queue=%llx submission=%llu list=%llx listGeneration=%llu order=%llu backBuffer=%llx target=%llx targetGeneration=%llu targetSubmission=%llu targetQueue=%llx descriptorCPU=%llx descriptorVersion=%llu descriptorCurrent=%u sameQueueOrdered=%u writerKnown=%u sourceSequenceCandidate=%llu eyeCandidate=%u frameCandidate=%u pso=%llx root=%llx rootIndex=%u offset=%u; candidate only, shader sample unproven\n",diag.epoch,diag.setup,q,submission,snapshot.list,snapshot.generation,row.order,row.backBuffer,row.resource,row.resourceGeneration,tracked->submission,tracked->queue,row.descriptorCPU,row.descriptorVersion,unsigned(descriptorCurrent),unsigned(sameQueueOrdered),unsigned(tracked->sourceSequence!=0),tracked->sourceSequence,tracked->eye,tracked->nativeFrame,row.pipeline,row.root,row.rootIndex,row.relative);}
 }
 for(const auto& snapshot:batch->lists)if(auto* l=GetList(snapshot.list);l&&l->generation==snapshot.generation&&l->objectGeneration==snapshot.objectGeneration)l->submitted=true;
 });return actualSubmission;}
}

