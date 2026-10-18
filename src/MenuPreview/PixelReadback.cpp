#include "MenuPreview/PixelReadback.hpp"
#include "MenuPreview/UiConsumer.hpp"
#include "MenuPreview/DescriptorProvenance.hpp"
#include "MenuPreview/PixelReadbackPolicy.hpp"
#include "MenuPreview/PreviewRasterProof.hpp"
#include "MenuPreview/EditorScenePolicy.hpp"
#include "Camera/ImagePoseIdentity.hpp"
#include "Render/CommandResources.hpp"
#include "Render/RenderProbeScope.hpp"
#include "Utils/StereoLog.hpp"
#include "../Render/AerPixelEvidence.hpp"
#include <unordered_map>
#include <unordered_set>

namespace cvr::menu_preview {
namespace pixels=cvr::render::pixel_evidence;
using Microsoft::WRL::ComPtr;
struct PreviewPixelFlight {
    std::shared_ptr<const CameraApplication> app;
    ComPtr<ID3D12Resource> source;ComPtr<ID3D12GraphicsCommandList> list;
    pixels::Surface surface;ComPtr<ID3D12Fence> fence;ComPtr<ID3D12CommandQueue> queue;
    PreviewRasterProof rasterAtDraw{};std::shared_ptr<const UiProducerInputs> producerInputs;
    PreviewDescriptorProof descriptor;uintptr_t expectedHandle{};uint64_t listGeneration{},value{};D3D12_RESOURCE_STATES before{};uint32_t stateProvenance{};uint64_t drawOrder{},sampleAgeMs{},sampleFirstEligibleMs{};uint32_t sampleDelayMs{};bool submitted{},unknown{},saved{};
};
namespace {
using ResourceState=PreviewPixelResourceState;
static_assert(D3D12_RESOURCE_STATE_RENDER_TARGET==4);
struct ListState {uint64_t generation{};bool open{};std::unordered_map<uintptr_t,ResourceState> resources;std::vector<std::shared_ptr<PreviewPixelFlight>> copies;std::vector<PreviewPixelObservation> observations;std::vector<std::shared_ptr<EditorSceneRecording>> editorScenes;bool successfulClose{};std::unique_ptr<PreviewRasterState> raster;};
struct Candidate {ComPtr<ID3D12Resource> resource;ComPtr<ID3D12GraphicsCommandList> list;uint64_t generation{};PreviewDescriptorProof descriptor;std::shared_ptr<const PreviewRasterProof> rasterAtDraw;UiRawDrawReceipt producerDraw{};std::shared_ptr<const UiProducerInputs> producerInputs;};
struct Node : PreviewPixelNodeWriter {std::shared_ptr<const CameraApplication> app;uint32_t work{};bool role{},ambiguous{};std::vector<Candidate> targets;bool expectedKnown{};uintptr_t expectedHandle{},actualHandle{},diagnosticResource{};PreviewPixelNodeDiagnostic diagnostic;};
struct State {
    std::recursive_mutex mutex;std::atomic<bool> lostLifecycle{};std::filesystem::path directory;PreviewPixelAttemptBudget attempts;PreviewPixelSampleClock sampleClock;uint32_t sampleDelayMs{};bool sampleDelayValid{true};uint32_t rawEndRows{},notRunRows{},eligibilityRows{},closeRows{},descriptorRows{};uint64_t generation{},recordOrder{},submitBatch{};uint32_t barrierRows[7]{},submitRows[7]{},drawRows{};
    std::unordered_map<uintptr_t,ListState> lists;std::unordered_set<uintptr_t> targets;
    std::vector<std::shared_ptr<PreviewPixelFlight>> flights;
    std::mutex pendingMutex;std::vector<std::pair<uintptr_t,bool>> pendingForget;
    State(){wchar_t p[32768]{};auto n=GetEnvironmentVariableW(L"CPVR_MENU_PIXEL_CAPTURE_DIR",p,32768);if(n&&n<32768&&std::filesystem::path(p).is_absolute())directory=p;
        wchar_t delay[16]{};SetLastError(ERROR_SUCCESS);const auto length=GetEnvironmentVariableW(L"CPVR_MENU_PIXEL_SAMPLE_DELAY_MS",delay,16);const auto error=GetLastError();
        sampleDelayValid=length<16&&ParsePreviewSampleDelay(length?delay:(error==ERROR_ENVVAR_NOT_FOUND?nullptr:L""),sampleDelayMs);
    }
};
// Diagnostic owner has process lifetime; at most two flights. Unproven fences
// retain COM resources rather than releasing in-flight GPU memory on shutdown.
State& S(){static auto* s=new State;return *s;}
thread_local std::vector<Node> nodes;
std::atomic<bool> rasterObservationLost{}; // Diagnostic only; never production lostLifecycle.
uintptr_t Address(const void* p){return reinterpret_cast<uintptr_t>(p);}
void NoteNativeDrawAdmission(const Node& n,unsigned reason,ID3D12GraphicsCommandList* list,const CameraApplication* current=nullptr,
    const PreviewDrawEligibility* eligibility=nullptr,bool descriptorCurrent=false)noexcept{
    static constexpr const char* reasons[]{"copy-role-unproven","node-application-missing","wrong-native-work","node-target-not-unique","node-target-ambiguous","target-not-currently-bound","recording-not-open","recording-generation-mismatch","current-application-mismatch","expected-rtv-unknown","bound-rtv-mismatch","candidate-rtv-mismatch","descriptor-proof-invalid","resource-layout-unsupported"};
    const auto& d=n.diagnostic;if(!d.nativeDual||d.eye>1||n.work!=0x377B58||reason>=std::size(reasons))return;
    static std::array<std::atomic<uint64_t>,2*std::size(reasons)> counts{};
    const auto count=counts[d.eye*std::size(reasons)+reason].fetch_add(1,std::memory_order_relaxed)+1;
    if(count>4&&count!=128&&count!=1024)return;
    const auto* app=n.app.get();const auto* c=n.targets.size()==1?&n.targets.front():nullptr;
    try{Log("[EditorNativeWriterAdmission] stage=draw reason=%s count=%llu epoch=%u setup=%u eye=%u work=%X context=%llx key=%llx frame=%u view=%llx list=%p resource=%llx targets=%llu ambiguous=%u role=%u app=%llx appContext=%llx appFrame=%u appSource=%llu currentRead=%u current=%llx currentContext=%llx currentKey=%llx currentFrame=%u currentSource=%llu currentFresh=%u boundList=%llx boundResource=%llx listGeneration=%llu open=%u sameGeneration=%u sameApplication=%u expectedKnown=%u expectedRTV=%llx actualRTV=%llx candidateRTV=%llx descriptorCurrent=%u; first failed predicate only, admission unchanged\n",
        reasons[reason],count,d.epoch,d.setup,d.eye,n.work,d.context,d.key,d.frame,d.view,list,c?Address(c->resource.Get()):n.diagnosticResource,uint64_t(n.targets.size()),unsigned(n.ambiguous),unsigned(n.role),reinterpret_cast<uintptr_t>(app),app?app->nativeContext:0,app?app->frame.nativeFrame:0,app?app->frame.sourceSequence:0,unsigned(eligibility!=nullptr),reinterpret_cast<uintptr_t>(current),current?current->nativeContext:0,current?CameraApplicationSourceKey(*current):0,current?current->frame.nativeFrame:0,current?current->frame.sourceSequence:0,unsigned(current&&FreshPublication(current->metadata.publishedAtMs,PublicationNowMs())),n.boundList,n.boundResource,c?c->generation:0,unsigned(eligibility&&eligibility->open),unsigned(eligibility&&eligibility->sameGeneration),unsigned(eligibility&&eligibility->sameApplication),unsigned(n.expectedKnown),n.expectedHandle,n.actualHandle,c?c->descriptor.handle:0,unsigned(descriptorCurrent));
    }catch(...){} // Diagnostic loss cannot invalidate a recording.
}
void EraseObject(uintptr_t object,bool list){if(list)S().lists.erase(object);else{S().targets.erase(object);for(auto& [_,l]:S().lists)l.resources.erase(object);}}
void DrainForgotten(){std::lock_guard lock(S().pendingMutex);for(auto [a,l]:S().pendingForget)EraseObject(a,l);S().pendingForget.clear();}
void NotRun(const char* reason,const Node& n){if(S().notRunRows++<8)Log("[MenuPreviewPixels] NOT_RUN reason=%s work=%X eye=%u frame=%u; no GPU commands appended\n",reason,n.work,n.app?n.app->frame.eye:0,n.app?n.app->frame.nativeFrame:0);}
}
bool PreviewPixelsRequested()noexcept{return !S().directory.empty();}
bool PreviewMetadataRequested()noexcept{return PreviewPixelsRequested()||UiStereoRequested();}
bool PreviewObservationRequested()noexcept{return PreviewPixelsRequested()||(UiStereoRequested()&&CurrentPreviewObservationEpoch()!=0);}
void PreviewPixelViewports(ID3D12GraphicsCommandList* list,UINT count,const D3D12_VIEWPORT* values)noexcept{try{
    if(!PreviewPixelsRequested()||!list||cvr::stereo::probe::internalCommands)return;
    std::lock_guard lock(S().mutex);const auto i=S().lists.find(Address(list));if(i==S().lists.end()||!i->second.open)return;
    if(!i->second.raster)i->second.raster=std::make_unique<PreviewRasterState>();i->second.raster->Viewports(count,values);
}catch(...){rasterObservationLost.store(true,std::memory_order_relaxed);}}
void PreviewPixelScissors(ID3D12GraphicsCommandList* list,UINT count,const D3D12_RECT* values)noexcept{try{
    if(!PreviewPixelsRequested()||!list||cvr::stereo::probe::internalCommands)return;
    std::lock_guard lock(S().mutex);const auto i=S().lists.find(Address(list));if(i==S().lists.end()||!i->second.open)return;
    if(!i->second.raster)i->second.raster=std::make_unique<PreviewRasterState>();i->second.raster->Scissors(count,values);
}catch(...){rasterObservationLost.store(true,std::memory_order_relaxed);}}
bool LockPreviewPixelLifecycle()noexcept{if(!PreviewObservationRequested())return false;try{S().mutex.lock();return true;}catch(...){S().lostLifecycle=true;return false;}}
void UnlockPreviewPixelLifecycle(bool locked)noexcept{if(locked)S().mutex.unlock();}
size_t BeginPreviewPixelNode(std::shared_ptr<const CameraApplication> app,uint32_t work,bool role,bool expectedKnown,uintptr_t expectedHandle,PreviewPixelNodeDiagnostic diagnostic){try{
    if(!PreviewObservationRequested())return 0;
    PollPreviewPixels();Node node;node.app=std::move(app);node.work=work;node.role=role;node.expectedKnown=expectedKnown;node.expectedHandle=expectedHandle;node.diagnostic=diagnostic;nodes.push_back(std::move(node));return nodes.size();
}catch(...){S().lostLifecycle=true;Log("[MenuPreviewPixels] FAIL observer_exception; capture disabled, ownership retained\n");return 0;}}
void RestorePreviewPixelNode(size_t depth){if(depth&&nodes.size()==depth)nodes.pop_back();}
void ResetPreviewPixelList(ID3D12GraphicsCommandList* list){try{
    if(!PreviewObservationRequested()||!list)return;std::lock_guard lock(S().mutex);
    S().lists[Address(list)]={++S().generation,true,{},{}};
}catch(...){S().lostLifecycle=true;Log("[MenuPreviewPixels] FAIL observer_exception; capture disabled, ownership retained\n");}}
void ClosePreviewPixelList(ID3D12GraphicsCommandList* list,bool success){try{
    if(!PreviewObservationRequested()||!list)return;std::lock_guard lock(S().mutex);
    auto i=S().lists.find(Address(list));
    if(!nodes.empty()&&nodes.back().work==0x377B58&&S().closeRows++<8){const auto& n=nodes.back();bool candidate=false;for(const auto& c:n.targets)candidate|=c.list.Get()==list;
        Log("[MenuPreviewPixels] copy-node-close list=%p success=%u role=%u candidate=%u openBefore=%u generation=%llu eye=%u frame=%u; original Close returned inside Copy node, no appended copy\n",list,unsigned(success),unsigned(n.role),unsigned(candidate),unsigned(i!=S().lists.end()&&i->second.open),i==S().lists.end()?0:i->second.generation,n.app?n.app->frame.eye:0,n.app?n.app->frame.nativeFrame:0);
    }
    if(i!=S().lists.end()){i->second.open=false;i->second.successfulClose=success;}
    if(!success&&S().notRunRows++<8)Log("[MenuPreviewPixels] native Close failed list=%p; recording rejected until successful Reset\n",list);
}catch(...){S().lostLifecycle=true;Log("[MenuPreviewPixels] FAIL observer_exception; capture disabled, ownership retained\n");}}
void ForgetPreviewPixelObject(uintptr_t object,bool list){try{
    if(!PreviewMetadataRequested())return;std::unique_lock lock(S().mutex,std::try_to_lock);
    if(!lock.owns_lock()){std::lock_guard pending(S().pendingMutex);if(S().pendingForget.size()<64)S().pendingForget.push_back({object,list});else S().lostLifecycle=true;return;}
    EraseObject(object,list);
}catch(...){S().lostLifecycle=true;Log("[MenuPreviewPixels] FAIL observer_exception; capture disabled, ownership retained\n");}}
void PreviewPixelTargetFromDescriptor(ID3D12GraphicsCommandList* list,const PreviewDescriptorProof& proof){try{
    auto* resource=reinterpret_cast<ID3D12Resource*>(proof.resource);
    if(!PreviewObservationRequested()||nodes.empty()||!list||!resource)return;
    auto& n=nodes.back();if(n.diagnostic.nativeDual)n.diagnosticResource=proof.resource;if(!n.role||!n.app||n.work!=0x377B58)return;
    if(!MatchPreviewExpectedDescriptor(n.expectedKnown,n.expectedHandle,proof.handle)||n.actualHandle!=proof.handle||!ValidatePreviewDescriptor(proof)){
        std::lock_guard lock(S().mutex);if(S().descriptorRows++<8)Log("[MenuPreviewPixels] descriptor-reject expectedKnown=%u expected=%llx actual=%llx proofHandle=%llx resource=%p; exact native destination/direct slot proof required\n",unsigned(n.expectedKnown),n.expectedHandle,n.actualHandle,proof.handle,resource);return;
    }
    std::lock_guard lock(S().mutex);S().targets.insert(Address(resource));
    n.ResolveTarget(Address(list),Address(resource));
    for(const auto& c:n.targets)if(c.resource.Get()==resource&&c.list.Get()==list)return;
    if(!n.targets.empty())n.ambiguous=true;
    if(n.targets.size()>=4){n.ambiguous=true;return;}
    const auto i=S().lists.find(Address(list));
    n.targets.push_back({resource,list,i==S().lists.end()?0:i->second.generation,proof});
    if(S().descriptorRows++<16)Log("[MenuPreviewPixels] descriptor-accepted expected=%llx actual=%llx resource=%p heap=%p heapGeneration=%llu resourceGeneration=%llu write=%llu; direct CreateRTV proof, no legacy cache\n",n.expectedHandle,proof.handle,resource,reinterpret_cast<void*>(proof.heap),proof.heapGeneration,proof.resourceGeneration,proof.writeSequence);
    if(i!=S().lists.end()&&i->second.observations.size()<32)i->second.observations.push_back({Address(list),Address(resource),i->second.generation,++S().recordOrder,0,0,0,0,0,0,n.app->frame.nativeFrame,n.app->frame.eye,n.app->frame.sourceSequence});
}catch(...){S().lostLifecycle=true;Log("[MenuPreviewPixels] FAIL observer_exception; capture disabled, ownership retained\n");}}
void PreviewPixelTargetsBegin(ID3D12GraphicsCommandList* list,UINT count,uintptr_t actualHandle){try{
    if(!PreviewObservationRequested()||nodes.empty())return;auto& n=nodes.back();n.OmChange(Address(list),count);n.actualHandle=actualHandle;
    if(n.role&&n.app&&n.work==0x377B58&&count==1){const bool directKnown=bool(ReadPreviewDescriptor(actualHandle));std::lock_guard lock(S().mutex);
        if(S().descriptorRows++<8)Log("[MenuPreviewPixels] descriptor-bind expectedKnown=%u expected=%llx actual=%llx handleMatch=%u directSlotKnown=%u frame=%u eye=%u; unknown/copy-mutated descriptors reject\n",unsigned(n.expectedKnown),n.expectedHandle,actualHandle,unsigned(MatchPreviewExpectedDescriptor(n.expectedKnown,n.expectedHandle,actualHandle)),unsigned(directKnown),n.app->frame.nativeFrame,n.app->frame.eye);
    }
}catch(...){S().lostLifecycle=true;}}
void PreviewPixelDrawReturned(ID3D12GraphicsCommandList* list,bool indexed,UINT elements,UINT instances,UINT startElement,INT baseVertex,UINT startInstance){try{
    if(!PreviewObservationRequested()||nodes.empty()||!list||!elements||!instances||cvr::stereo::probe::internalCommands)return;
    auto& n=nodes.back();if(!n.role||!n.app||n.work!=0x377B58||n.targets.size()!=1||n.ambiguous){
        NoteNativeDrawAdmission(n,!n.role?0:!n.app?1:n.work!=0x377B58?2:n.targets.size()!=1?3:4,list);return;}
    std::lock_guard lock(S().mutex);DrainForgotten();auto& c=n.targets.front();auto l=S().lists.find(Address(list));const auto current=ReadCurrentApplicationForSource(*n.app);
    const PreviewDrawEligibility e{n.role,bool(n.app),n.targets.size()==1&&!n.ambiguous,
        n.CurrentlyBound(Address(list),Address(c.resource.Get()))&&c.list.Get()==list,
        l!=S().lists.end()&&l->second.open,l!=S().lists.end()&&l->second.generation==c.generation,
        current&&current->frame.key==n.app->frame.key&&current->frame.sourceSequence==n.app->frame.sourceSequence&&current->nativeContext==n.app->nativeContext,elements,instances};
    const bool admitted=AdmitPreviewDrawReceipt(e);
    if(!admitted||!n.expectedKnown||n.actualHandle!=n.expectedHandle||c.descriptor.handle!=n.expectedHandle){
        const unsigned reason=!admitted?(!e.currentBoundTarget?5:!e.open?6:!e.sameGeneration?7:8):!n.expectedKnown?9:n.actualHandle!=n.expectedHandle?10:11;
        NoteNativeDrawAdmission(n,reason,list,current.get(),&e);return;}
    const bool descriptorCurrent=ValidatePreviewDescriptor(c.descriptor);if(!descriptorCurrent){NoteNativeDrawAdmission(n,12,list,current.get(),&e,false);return;}
    const auto d=c.resource->GetDesc();if(d.Dimension!=D3D12_RESOURCE_DIMENSION_TEXTURE2D||d.MipLevels!=1||d.DepthOrArraySize!=1||d.SampleDesc.Count!=1||!(d.Flags&D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET)){NoteNativeDrawAdmission(n,13,list,current.get(),&e,descriptorCurrent);return;}
    const auto order=++S().recordOrder;n.drawOrder=order;
    if(PreviewPixelsRequested()&&n.app->frame.eye<2&&!(S().attempts.consumed&(1u<<n.app->frame.eye))){
        c.rasterAtDraw.reset();try{const PreviewRasterState unknown;const auto& raster=l->second.raster?*l->second.raster:unknown;
            c.rasterAtDraw=std::make_shared<PreviewRasterProof>(raster.Freeze(c.generation,order,Address(c.resource.Get()),rasterObservationLost.load(std::memory_order_relaxed)));
        }catch(...){/* Missing diagnostic proof never changes the original draw admission. */}
    }
    l->second.resources[Address(c.resource.Get())].DrawRequired(order);
    c.producerInputs.reset();
    const bool captureInputs=PreviewPixelsRequested()&&n.app->frame.eye<2&&!(S().attempts.consumed&(1u<<n.app->frame.eye));
    c.producerDraw={c.generation,order,elements,instances,startElement,startInstance,baseVertex,indexed};
    ObserveUiPortraitWriter(list,c.resource.Get(),n.app,captureInputs?&c.producerDraw:nullptr,captureInputs?&c.producerInputs:nullptr);
    PreviewPixelObservation observation{Address(list),Address(c.resource.Get()),c.generation,order,indexed?5u:4u,0,0,0,0,0,n.app->frame.nativeFrame,n.app->frame.eye,n.app->frame.sourceSequence,elements,instances,startElement,startInstance,baseVertex};
    if(l->second.observations.size()<32)l->second.observations.push_back(observation);
    if(S().drawRows++<16)Log("[MenuPreviewPixels] copy-draw-returned api=%s record=%llu list=%p resource=%p generation=%llu elements=%u instances=%u startElement=%u baseVertex=%d startInstance=%u frame=%u eye=%u source=%llu; actual original Draw returned targeting unique bound Copy role, recorded command NOT GPU completion; stateProvenance=draw_required_state_contract assumption valid original game Draw, NOT observed barrier\n",indexed?"DrawIndexedInstanced":"DrawInstanced",order,list,c.resource.Get(),c.generation,elements,instances,startElement,baseVertex,startInstance,n.app->frame.nativeFrame,n.app->frame.eye,n.app->frame.sourceSequence);
}catch(...){S().lostLifecycle=true;Log("[MenuPreviewPixels] FAIL draw_receipt_exception; capture disabled\n");}}
void PreviewPixelBarriers(ID3D12GraphicsCommandList* list,UINT count,const D3D12_RESOURCE_BARRIER* b){try{
    if(!PreviewObservationRequested()||!list||!b)return;std::lock_guard lock(S().mutex);
    auto l=S().lists.find(Address(list));
    for(UINT k=0;k<count;++k){const auto& v=b[k];
        ID3D12Resource* exact=nullptr;uint32_t kind=0,before=0,after=0,subresource=0;
        if(v.Type==D3D12_RESOURCE_BARRIER_TYPE_TRANSITION&&S().targets.contains(Address(v.Transition.pResource))){exact=v.Transition.pResource;kind=1;before=unsigned(v.Transition.StateBefore);after=unsigned(v.Transition.StateAfter);subresource=v.Transition.Subresource;}
        else if(v.Type==D3D12_RESOURCE_BARRIER_TYPE_UAV&&v.UAV.pResource&&S().targets.contains(Address(v.UAV.pResource))){exact=v.UAV.pResource;kind=2;}
        else if(v.Type==D3D12_RESOURCE_BARRIER_TYPE_ALIASING){if(S().targets.contains(Address(v.Aliasing.pResourceBefore)))exact=v.Aliasing.pResourceBefore;else if(S().targets.contains(Address(v.Aliasing.pResourceAfter)))exact=v.Aliasing.pResourceAfter;kind=exact?3:(!v.Aliasing.pResourceBefore||!v.Aliasing.pResourceAfter)?6:0;}
        if(v.Type==D3D12_RESOURCE_BARRIER_TYPE_UAV&&!v.UAV.pResource)kind=6;
        if(exact||(kind==6&&!S().targets.empty())){const auto order=++S().recordOrder;const auto generation=l==S().lists.end()?0:l->second.generation;
            if(S().barrierRows[kind]++<(kind==6?8u:32u))Log("[MenuPreviewPixels] %s record=%llu list=%p resource=%p generation=%llu listKnown=%u open=%u kind=%u before=%X after=%X flags=%X subresource=%u aliasBefore=%p aliasAfter=%p; observed native barrier, wildcard resource0 is hazard only, no cross-list state admission\n",kind==6?"wildcard-hazard":"target-barrier",order,list,exact,generation,unsigned(l!=S().lists.end()),unsigned(l!=S().lists.end()&&l->second.open),kind,before,after,unsigned(v.Flags),subresource,v.Type==D3D12_RESOURCE_BARRIER_TYPE_ALIASING?v.Aliasing.pResourceBefore:nullptr,v.Type==D3D12_RESOURCE_BARRIER_TYPE_ALIASING?v.Aliasing.pResourceAfter:nullptr);
            if(l!=S().lists.end()&&l->second.observations.size()<32){bool duplicateWildcard=false;if(kind==6)for(const auto& o:l->second.observations)duplicateWildcard|=o.kind==6;if(!duplicateWildcard)l->second.observations.push_back({Address(list),Address(exact),generation,order,kind,before,after,unsigned(v.Flags),subresource});}
        }
        if(l==S().lists.end()||!l->second.open)continue;
        if(v.Type==D3D12_RESOURCE_BARRIER_TYPE_ALIASING){
            if(!v.Aliasing.pResourceBefore||!v.Aliasing.pResourceAfter){for(auto& [_,r]:l->second.resources)r.Invalidate();}
            else{for(auto* resource:{v.Aliasing.pResourceBefore,v.Aliasing.pResourceAfter})if(S().targets.contains(Address(resource)))l->second.resources[Address(resource)].Invalidate();}
        }else if(v.Type==D3D12_RESOURCE_BARRIER_TYPE_TRANSITION&&S().targets.contains(Address(v.Transition.pResource))){
            const auto d=v.Transition.pResource->GetDesc();auto& r=l->second.resources[Address(v.Transition.pResource)];
            const bool complete=v.Transition.Subresource==D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES||
                (v.Transition.Subresource==0&&d.MipLevels==1&&d.DepthOrArraySize==1);
            r.Transition(unsigned(v.Transition.StateAfter),complete,v.Flags==D3D12_RESOURCE_BARRIER_FLAG_NONE);
        }else if(v.Type==D3D12_RESOURCE_BARRIER_TYPE_UAV){
            if(v.UAV.pResource){if(S().targets.contains(Address(v.UAV.pResource)))l->second.resources[Address(v.UAV.pResource)].Invalidate();}
            else for(auto& [_,r]:l->second.resources)r.Invalidate();
        }
    }
}catch(...){S().lostLifecycle=true;Log("[MenuPreviewPixels] FAIL observer_exception; capture disabled, ownership retained\n");}}
void EndPreviewPixelNode(){try{
    // Independent production staging: no CPU readback attempt/time budget.
    if(EditorSceneCaptureEnabled()&&!nodes.empty()){
        const auto& n=nodes.back();
        if(n.app&&n.app->metadata.screen==2&&n.work==0x377B58){
            std::lock_guard lock(S().mutex);DrainForgotten();
            const bool unique=n.targets.size()==1&&!n.ambiguous;const auto* c=unique?&n.targets.front():nullptr;
            const auto l=c?S().lists.find(Address(c->list.Get())):S().lists.end();const bool listKnown=l!=S().lists.end();
            const auto r=listKnown?l->second.resources[Address(c->resource.Get())]:ResourceState{};const auto current=ReadCurrentApplicationForSource(*n.app);const auto now=PublicationNowMs();
            const bool descriptorCurrent=c&&n.expectedKnown&&c->descriptor.handle==n.expectedHandle&&ValidatePreviewDescriptor(c->descriptor);
            const bool appKey=current&&current->frame.key==n.app->frame.key&&current->frame.sourceSequence==n.app->frame.sourceSequence&&current->nativeContext==n.app->nativeContext;
            const bool appRevision=appKey&&current->localPoseRevision==n.app->localPoseRevision;
            const bool appFrame=appKey&&current->frame.eye==n.app->frame.eye&&current->frame.nativeFrame==n.app->frame.nativeFrame;
            const bool appOrigin=appKey&&current->projection.origin==n.app->projection.origin;
            const bool fresh=current&&FreshPublication(current->metadata.publishedAtMs,now);
            PreviewRawEndProof proof{{n.role,bool(n.app),listKnown&&l->second.open,r.known,!unique,listKnown&&l->second.generation==c->generation,n.work,n.app->frame.eye,
                r.MatchesDraw(n.drawOrder)&&descriptorCurrent},!S().lostLifecycle.load(),listKnown,listKnown&&l->second.editorScenes.size()<2,c&&c->list->GetType()==D3D12_COMMAND_LIST_TYPE_DIRECT,
                appKey&&appRevision&&appFrame&&appOrigin&&fresh,n.actualHandle==n.expectedHandle};
            const bool admitted=AdmitPreviewRawEnd(proof);bool recorded{};
            if(admitted)if(auto stage=RecordEditorSceneImage(c->list.Get(),c->resource.Get(),D3D12_RESOURCE_STATES(r.value),n.app,c->generation)){l->second.editorScenes.push_back(std::move(stage));recorded=true;}
            if(n.role&&S().rawEndRows++<8){
                const char* reason=recorded?"recorded":admitted?"producer-refused":!n.role?"role":!unique?"target-unique":!proof.lifecycleGood?"lifecycle":!listKnown?"list-unknown":!proof.contents.open?"list-closed":!proof.contents.sameListGeneration?"generation":!r.known?"state-unknown":!r.MatchesDraw(n.drawOrder)?"historic-draw":!descriptorCurrent?"descriptor":!proof.capacity?"capacity":!proof.direct?"list-type":!appKey?"app-key":!appRevision?"app-revision":!appFrame?"app-frame":!appOrigin?"app-origin":!fresh?"app-stale":"other";
                Log("[EditorSceneEndGate] reason=%s admitted=%u recorded=%u eye=%u frame=%u source=%llu list=%p generation=%llu/%llu role=%u unique=%u open=%u stateKnown=%u state=%X drawOrder=%llu/%llu descriptorCurrent=%u expected=%llx drawDescriptor=%llx postHandle=%llx postMatches=%u lifecycle=%u capacity=%u direct=%u appKey=%u appRevision=%u appFrame=%u appOrigin=%u fresh=%u; historical contents, post-Draw OM binding diagnostic only\n",reason,unsigned(admitted),unsigned(recorded),n.app->frame.eye,n.app->frame.nativeFrame,n.app->frame.sourceSequence,c?c->list.Get():nullptr,listKnown?l->second.generation:0,c?c->generation:0,unsigned(n.role),unsigned(unique),unsigned(proof.contents.open),unsigned(r.known),r.value,n.drawOrder,r.drawOrder,unsigned(descriptorCurrent),n.expectedHandle,c?c->descriptor.handle:0,n.actualHandle,unsigned(proof.postDrawBindingMatches),unsigned(proof.lifecycleGood),unsigned(proof.capacity),unsigned(proof.direct),unsigned(appKey),unsigned(appRevision),unsigned(appFrame),unsigned(appOrigin),unsigned(fresh));
            }
        }
    }
    if(!PreviewPixelsRequested()||nodes.empty())return;const auto& n=nodes.back();
    if(!n.role||!n.app||n.work!=0x377B58||n.app->frame.eye>1)return;
    std::lock_guard lock(S().mutex);DrainForgotten();const auto now=PublicationNowMs();
    const auto previouslyConsumed=S().attempts.consumed;
    if(!S().attempts.Available(n.app->frame.eye,now)){if(previouslyConsumed!=S().attempts.consumed)Log("[MenuPreviewPixels] NOT_RUN bounded_5s_eligibility_window_expired eye=%u; no copy appended\n",n.app->frame.eye);return;}
    if(S().lostLifecycle.load()){NotRun("lifecycle_invalidation_could_not_be_serialized",n);return;}
    if(n.targets.size()!=1||n.ambiguous){NotRun("target_ambiguous_or_absent",n);return;}
    const auto& c=n.targets.front();auto l=S().lists.find(Address(c.list.Get()));
    const auto r=l==S().lists.end()?ResourceState{}:l->second.resources[Address(c.resource.Get())];
    const PreviewCopyEligibility e{n.role,bool(n.app),l!=S().lists.end()&&l->second.open,r.known,n.ambiguous,
        l!=S().lists.end()&&l->second.generation==c.generation,n.work,n.app->frame.eye,
        r.MatchesDraw(n.drawOrder)&&n.expectedKnown&&c.descriptor.handle==n.expectedHandle&&ValidatePreviewDescriptor(c.descriptor)};
    const auto listType=c.list->GetType();
    if(!AdmitPreviewPixelCopy(e)||listType!=D3D12_COMMAND_LIST_TYPE_DIRECT){
        if(S().eligibilityRows++<8)Log("[MenuPreviewPixels] eligibility-reject list=%p resource=%p listKnown=%u open=%u sameGeneration=%u currentGeneration=%llu recordedGeneration=%llu type=%u stateKnown=%u state=%X stateProvenance=%u currentDraw=%u drawOrder=%llu role=%u app=%u ambiguous=%u eye=%u frame=%u; no GPU command\n",c.list.Get(),c.resource.Get(),unsigned(l!=S().lists.end()),unsigned(e.open),unsigned(e.sameListGeneration),l==S().lists.end()?0:l->second.generation,c.generation,unsigned(listType),unsigned(r.known),unsigned(r.value),r.provenance,unsigned(e.currentDraw),r.drawOrder,unsigned(e.role),unsigned(e.app),unsigned(e.ambiguous),n.app->frame.eye,n.app->frame.nativeFrame);
        NotRun("list_open_generation_type_or_actual_state_unknown",n);return;
    }
    const auto current=ReadCurrentApplicationForSource(*n.app);if(!current||current->frame.key!=n.app->frame.key||current->frame.sourceSequence!=n.app->frame.sourceSequence||current->nativeContext!=n.app->nativeContext||!FreshPublication(current->metadata.publishedAtMs,now)){NotRun("scope_invalidated",n);return;}
    if(!S().sampleDelayValid){NotRun("invalid_CPVR_MENU_PIXEL_SAMPLE_DELAY_MS_expected_integer_0_3000",n);return;}
    uint64_t sampleAge{};if(!S().sampleClock.Ready(n.app->frame.eye,now,S().sampleDelayMs,true,sampleAge))return;
    S().attempts.Consume(n.app->frame.eye); // at most one actual GPU attempt per eye, even on failure
    auto flight=std::make_shared<PreviewPixelFlight>();flight->app=n.app;flight->source=c.resource;flight->list=c.list;
    if(c.rasterAtDraw&&c.rasterAtDraw->generation==c.generation&&c.rasterAtDraw->drawOrder==r.drawOrder&&c.rasterAtDraw->resource==Address(c.resource.Get()))flight->rasterAtDraw=*c.rasterAtDraw;
    if(c.producerDraw.pixelGeneration==c.generation&&c.producerDraw.drawOrder==r.drawOrder&&MatchUiProducerInputs(c.producerInputs,c.producerDraw,*n.app,Address(c.list.Get()),Address(c.resource.Get())))flight->producerInputs=c.producerInputs;
    flight->descriptor=c.descriptor;flight->expectedHandle=n.expectedHandle;flight->listGeneration=c.generation;flight->before=D3D12_RESOURCE_STATES(r.value);flight->stateProvenance=r.provenance;flight->drawOrder=r.drawOrder;flight->sampleAgeMs=sampleAge;flight->sampleDelayMs=S().sampleDelayMs;flight->sampleFirstEligibleMs=S().sampleClock.firstEligible[n.app->frame.eye];
    ComPtr<ID3D12Device> device,sourceDevice;ComPtr<IUnknown> a,z;
    const auto d=c.resource->GetDesc();
    if(d.Dimension!=D3D12_RESOURCE_DIMENSION_TEXTURE2D||d.MipLevels!=1||d.DepthOrArraySize!=1||d.SampleDesc.Count!=1||
       FAILED(c.list->GetDevice(IID_PPV_ARGS(&device)))||FAILED(c.resource->GetDevice(IID_PPV_ARGS(&sourceDevice)))||
       FAILED(device.As(&a))||FAILED(sourceDevice.As(&z))||a!=z||!pixels::Allocate(device.Get(),c.resource.Get(),flight->surface)||
       FAILED(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&flight->fence)))){NotRun("allocation_or_single_surface_contract_failed",n);return;}
    if(!cvr::gpu::KeepCommandResources(c.list.Get(),{c.resource.Get(),flight->surface.readback.Get(),flight->fence.Get()})){NotRun("command_resource_lifetime_unavailable",n);return;}
    l->second.copies.push_back(flight);S().flights.push_back(flight); // allocate ownership before any appended GPU command
    {cvr::stereo::probe::Scope internal;pixels::Copy(c.list.Get(),c.resource.Get(),D3D12_RESOURCE_STATES(r.value),flight->surface);}
    Log("[MenuPreviewPixels] recorded list=%p resource=%p listGeneration=%llu state=%X stateProvenance=%u drawOrder=%llu eye=%u frame=%u source=%llu sampleAgeMs=%llu sampleDelayMs=%u; SAME list after exact role writer, NOT GPU-complete\n",
        c.list.Get(),c.resource.Get(),c.generation,unsigned(r.value),r.provenance,r.drawOrder,n.app->frame.eye,n.app->frame.nativeFrame,n.app->frame.sourceSequence,sampleAge,S().sampleDelayMs);
}catch(...){S().lostLifecycle=true;Log("[MenuPreviewPixels] FAIL observer_exception; capture disabled, ownership retained\n");}}
PreviewPixelSubmission PreparePreviewPixels(UINT count,ID3D12CommandList* const* lists,ID3D12CommandQueue* queue)noexcept{
    PreviewPixelSubmission p;if((!PreviewPixelsRequested()&&!EditorSceneCaptureEnabled())||(count&&!lists)||count>64)return p;
    std::array<std::shared_ptr<EditorSceneRecording>,128> ordered{};
    try{std::lock_guard lock(S().mutex);DrainForgotten();if(PreviewPixelsRequested())p.batch=++S().submitBatch;
        // Fixed refs must be captured before optional diagnostic allocations:
        // allocation failure may skip publication, never the replay queue wait.
        for(UINT i=0;i<count;++i){auto l=S().lists.find(Address(lists[i]));if(l==S().lists.end())continue;
            for(const auto& r:l->second.editorScenes)if(p.editorSceneCount<p.editorScenes.size()){
                const auto n=p.editorSceneCount++;p.editorScenes[n]={Address(lists[i]),l->second.generation,r,EditorSceneRecordingSubmissionValid(l->second.open,l->second.successfulClose,l->second.generation,l->second.generation)};ordered[n]=r;
            }
        }
        if(PreviewPixelsRequested())for(UINT i=0;i<count;++i){auto l=S().lists.find(Address(lists[i]));if(l==S().lists.end())continue;
            for(auto observation:l->second.observations){observation.listIndex=i;p.observations.push_back(observation);}
            for(auto& f:l->second.copies)if(!f->unknown&&PreviewPixelSubmissionGeneration(l->second.open,l->second.generation,f->listGeneration))p.flights.push_back(f);
        }
    }catch(...){p.flights.clear();p.observations.clear();}
    BeforeEditorSceneExecute(queue,std::span(ordered).first(p.editorSceneCount));return p;
}
void SubmittedPreviewPixels(ID3D12CommandQueue* queue,PreviewPixelSubmission&& p,EditorSceneExecute execute,uint64_t uiSubmission)noexcept{
    std::array<bool,128> valid{};
    if(p.editorSceneCount)try{std::lock_guard lock(S().mutex);DrainForgotten();for(uint32_t n=0;n<p.editorSceneCount;++n){const auto& r=p.editorScenes[n];const auto l=S().lists.find(r.list);valid[n]=r.preparedValid&&l!=S().lists.end()&&EditorSceneRecordingSubmissionValid(l->second.open,l->second.successfulClose,l->second.generation,r.generation);}}catch(...){}
    // Stale references still receive an ordering Signal for the original ECL;
    // they cannot publish an association for a recycled native recording.
    for(uint32_t n=0;n<p.editorSceneCount;++n)SubmittedEditorSceneImages(queue,std::span(&p.editorScenes[n].recording,1),valid[n]?uiSubmission:0,execute,valid[n]);
    if(!PreviewPixelsRequested()||!queue||queue->GetDesc().Type!=D3D12_COMMAND_LIST_TYPE_DIRECT)return;
    try{std::lock_guard lock(S().mutex);
        for(const auto& o:p.observations)if(S().submitRows[o.kind]++<(o.kind==6?8u:32u))Log("[MenuPreviewPixels] executed-target-event batch=%llu queue=%p listIndex=%u list=%p generation=%llu resource=%p record=%llu kind=%u before=%X after=%X flags=%X subresource=%u frame=%u eye=%u source=%llu elements=%u instances=%u startElement=%u baseVertex=%d startInstance=%u; original ECL returned, kind0 candidate NOT draw; kind4/5 actual Draw return; no cross-list state admission\n",p.batch,queue,o.listIndex,reinterpret_cast<void*>(o.list),o.generation,reinterpret_cast<void*>(o.resource),o.recordOrder,o.kind,o.before,o.after,o.flags,o.subresource,o.frame,o.eye,o.source,o.elements,o.instances,o.startElement,o.baseVertex,o.startInstance);
        for(auto& f:p.flights){
        if(f->queue&&f->queue.Get()!=queue){f->unknown=true;Log("[MenuPreviewPixels] FAIL replay_foreign_queue list=%p; readback retained, no map\n",f->list.Get());continue;}f->queue=queue;
        const auto value=f->value+1;if(FAILED(queue->Signal(f->fence.Get(),value))){f->unknown=true;Log("[MenuPreviewPixels] FAIL queue_signal; readback retained, no map\n");continue;}
        f->value=value;f->submitted=true;
        Log("[MenuPreviewPixels] submitted queue=%p list=%p eye=%u frame=%u fence=%p value=%llu; actual Execute completed before Signal\n",queue,f->list.Get(),f->app->frame.eye,f->app->frame.nativeFrame,f->fence.Get(),value);
    }}catch(...){}
}
void PollPreviewPixels()noexcept{
    if(!PreviewPixelsRequested())return;
    try{std::lock_guard submit(cvr::camera::ImageSubmissionMutex());std::lock_guard lock(S().mutex);DrainForgotten();
        for(auto& f:S().flights){if(!f->submitted||f->unknown||f->saved)continue;
            const auto done=f->fence->GetCompletedValue();if(done==UINT64_MAX){f->unknown=true;Log("[MenuPreviewPixels] FAIL device_removed_fence; retained without map\n");continue;}if(done<f->value)continue;
            std::filesystem::create_directories(S().directory);const auto stem="eye"+std::to_string(f->app->frame.eye)+"-source"+std::to_string(f->app->frame.sourceSequence);
            const bool saved=pixels::SaveSurface(f->surface,S().directory/(stem+".raw"));f->saved=true;
            std::ofstream out(S().directory/(stem+".json"));const auto& m=f->app->metadata;
            out<<"{\"status\":\""<<(saved?"PASS":"FAIL")<<"\",\"stage\":\"CopyToTexture raw pixels, UI consumption unproven\",\"eye\":"<<f->app->frame.eye
               <<",\"epoch\":"<<m.screenEpoch<<",\"setup\":"<<m.setupID<<",\"component\":"<<m.componentIdentity<<",\"camera_key\":"<<m.virtualCameraNameHash
               <<",\"native_frame\":"<<f->app->frame.nativeFrame<<",\"source_sequence\":"<<f->app->frame.sourceSequence
               <<",\"resource\":"<<Address(f->source.Get())<<",\"actual_rtv_handle\":"<<f->descriptor.handle<<",\"expected_rtv_handle\":"<<f->expectedHandle<<",\"descriptor_heap_generation\":"<<f->descriptor.heapGeneration<<",\"descriptor_resource_generation\":"<<f->descriptor.resourceGeneration<<",\"descriptor_write\":"<<f->descriptor.writeSequence<<",\"descriptor_provenance\":\"observed_direct_CreateRTV_live_heap_resource_no_descriptor_copies\""<<",\"list_generation\":"<<f->listGeneration<<",\"state_before\":"<<unsigned(f->before)
               <<",\"state_provenance\":\""<<(f->stateProvenance==2?"draw_required_state_contract":"observed_complete_transition")<<"\",\"draw_order\":"<<f->drawOrder
               <<",\"sample_age_ms\":"<<f->sampleAgeMs<<",\"sample_delay_ms\":"<<f->sampleDelayMs<<",\"sample_first_eligible_ms\":"<<f->sampleFirstEligibleMs
               <<",\"sample_clock\":\"PublicationNowMs\",\"sample_provenance\":\"diagnostic_delay_after_first_exact_eligible_Copy_per_eye_not_stability_or_pairing\""
               <<",\"completed_fence\":"<<f->value<<",\"width\":"<<f->surface.description.Width<<",\"height\":"<<f->surface.description.Height
               <<",\"format\":"<<unsigned(f->surface.description.Format)<<",\"row_bytes\":"<<f->surface.rowBytes<<",\"rows\":"<<f->surface.rows<<",\"raster_at_draw\":";
            WritePreviewRasterProofJson(out,f->rasterAtDraw);out<<",\"producer_inputs\":";WriteUiProducerInputsJson(out,f->producerInputs);out<<"}";
            Log("[MenuPreviewPixels] saved=%u eye=%u frame=%u source=%llu completedFence=%llu bytes=%llu; raw format preserved\n",unsigned(saved),f->app->frame.eye,f->app->frame.nativeFrame,f->app->frame.sourceSequence,f->value,f->surface.bytes);
        }
    }catch(...){}
}
}
