#include "MenuPreview/PixelReadback.hpp"
#include "MenuPreview/TargetDescriptor.hpp"
#include "MenuPreview/ImageLineage.hpp"
#include "Utils/StereoLog.hpp"
#include "Utils/MemorySafe.hpp"
#include <atomic>
#include <new>
namespace cvr::menu_preview {
namespace {
thread_local NodeBindingStack nodeBindings;
auto images=std::make_shared<ImageLineageStore>();
std::mutex lifetimeMutex;
// Diagnostic cache only. Ownership/acceptance still belongs exclusively to the
// immutable ledger. Bounded entries avoid a new unbounded resource registry.
struct DiagnosticBinding {uintptr_t list{},resource{};NodeBindingSlot app;uint64_t reportedGeneration{};uint32_t workRva{};};
std::mutex diagnosticMutex;
std::array<DiagnosticBinding,16> diagnosticBindings{};
size_t diagnosticCursor{};
std::atomic<bool> diagnosticUsed{};
std::atomic<uint32_t> nodeAcceptedRows{},nodeRejectedRows{},rtvRows{},prepareRows{},commitRows{};
std::atomic<uint32_t> copyNodeRows{},copyRtvRows{},copyEndRows{},descriptorRows{};
void NoteNativeNodeAdmission(const PreviewPixelNodeDiagnostic& d,unsigned reason,const CameraApplication* candidate,
    bool clockKnown,uint64_t now,uint64_t backing=0,uint64_t viewBacking=0,bool componentRead=false,bool viewRead=false,bool expectedKnown=false)noexcept{
    static constexpr const char* reasons[]{"clock-unknown","no-published-source-app","context-mismatch","native-frame-mismatch","application-stale","source-key-mismatch","lookup-returned-empty","node-match-rejected","lease-source-mismatch","component-backing-unreadable","view-backing-unreadable","component-backing-zero","current-component-view-backing-mismatch","expected-rtv-unresolved"};
    if(!d.nativeDual||d.eye>1||reason>=std::size(reasons))return;
    static std::array<std::atomic<uint64_t>,2*std::size(reasons)> counts{};
    const auto count=counts[d.eye*std::size(reasons)+reason].fetch_add(1,std::memory_order_relaxed)+1;
    if(count>4&&count!=128&&count!=1024)return;
    try{Log("[EditorNativeWriterAdmission] stage=node reason=%s count=%llu epoch=%u setup=%u eye=%u work=377B58 context=%llx key=%llx frame=%u clockKnown=%u view=%llx candidate=%llx candidateContext=%llx candidateKey=%llx candidateFrame=%u candidateSource=%llu candidateFresh=%u componentRead=%u componentBacking=%llx viewRead=%u viewBacking=%llx expectedKnown=%u; diagnostic only, native admission unchanged\n",
        reasons[reason],count,d.epoch,d.setup,d.eye,d.context,d.key,d.frame,unsigned(clockKnown),d.view,reinterpret_cast<uintptr_t>(candidate),candidate?candidate->nativeContext:0,candidate?CameraApplicationSourceKey(*candidate):0,candidate?candidate->frame.nativeFrame:0,candidate?candidate->frame.sourceSequence:0,unsigned(candidate&&FreshPublication(candidate->metadata.publishedAtMs,now)),unsigned(componentRead),backing,unsigned(viewRead),viewBacking,unsigned(expectedKnown));
    }catch(...){} // Diagnostic loss cannot alter native writer admission.
}
void ForgetDiagnostic(uintptr_t address,bool list){
    if(!diagnosticUsed.load(std::memory_order_relaxed))return;
    std::lock_guard lock(diagnosticMutex);
    for(auto& row:diagnosticBindings)if((list?row.list:row.resource)==address)row={};
}
void RememberDiagnostic(uintptr_t list,uintptr_t resource,const NodeBindingSlot& app){
    diagnosticUsed.store(true,std::memory_order_relaxed);
    std::lock_guard lock(diagnosticMutex);
    DiagnosticBinding* slot=nullptr;
    for(auto& row:diagnosticBindings)if(row.list==list&&row.resource==resource){slot=&row;break;}
    if(!slot)slot=&diagnosticBindings[diagnosticCursor++%diagnosticBindings.size()];
    const auto previous=(slot->resource==resource)?slot->reportedGeneration:0;
    *slot={list,resource,app,previous,nodeBindings.WorkRva()};
}
void NotePrepared(ID3D12CommandQueue* queue,UINT n,ID3D12CommandList* const* lists,bool prepared){
    if(prepareRows.load(std::memory_order_relaxed)>=8)return;
    const auto current=ReadPreviewScopeApplication();if(!current)return;
    std::lock_guard lock(diagnosticMutex);
    for(UINT i=0;i<n;++i)for(const auto& row:diagnosticBindings)
        if(row.list==reinterpret_cast<uintptr_t>(lists[i])&&row.app&&row.app->frame.key==current->frame.key){
            if(prepareRows.fetch_add(1,std::memory_order_relaxed)<8)
                Log("[MenuPreviewLineage] prepare-known-rtv queue=%p list=%p resource=%p lists=%u preparedValid=%u key=%llx frame=%u source=%llu eye=%u recordingWork=%X; observed recording candidate, write count/acceptance unknown\n",
                    queue,reinterpret_cast<void*>(row.list),reinterpret_cast<void*>(row.resource),n,unsigned(prepared),
                    row.app->frame.key.virtualCamera,row.app->frame.nativeFrame,row.app->frame.sourceSequence,row.app->frame.eye,row.workRva);
            return;
        }
}
void NoteCommitted(ID3D12CommandQueue* queue){
    if(commitRows.load(std::memory_order_relaxed)>=8||!HasCurrentPreviewApplications())return;
    std::lock_guard lock(diagnosticMutex);
    for(auto& row:diagnosticBindings){
        if(!row.resource)continue;
        const auto image=images->Read(row.resource);
        if(!image||image.queue!=reinterpret_cast<uintptr_t>(queue)||image.generation==row.reportedGeneration)continue;
        row.reportedGeneration=image.generation;
        if(commitRows.fetch_add(1,std::memory_order_relaxed)<8)
            Log("[MenuPreviewLineage] commit-visible-rtv queue=%p resource=%p generation=%llu key=%llx frame=%u source=%llu eye=%u work=%X descKnown=%u format=%u size=%llux%u mip=%u array=%u samples=%u; immutable submitted RTV-bind metadata, no draw/GPU completion/UI-read proof\n",
                queue,reinterpret_cast<void*>(image.resource),image.generation,image.pose->frame.key.virtualCamera,
                image.pose->frame.nativeFrame,image.pose->frame.sourceSequence,image.pose->frame.eye,image.pose.workRva,
                unsigned(image.pose.descriptorKnown),image.pose.format,image.pose.width,image.pose.height,image.pose.mips,image.pose.array,image.pose.samples);
        if(commitRows.load(std::memory_order_relaxed)>=8)return;
    }
}
const GUID lifetimeKey={0x3bd76a45,0xbe21,0x45d1,{0x9d,0x90,0xe8,0x5c,0x93,0x44,0xb1,0x6a}};
class Lifetime final:public IUnknown {
    std::atomic<ULONG> refs{1};std::weak_ptr<ImageLineageStore> owner{images};uintptr_t address;bool list;
public:
    Lifetime(uintptr_t a,bool l):address(a),list(l){}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id,void** out)override{
        if(!out)return E_POINTER;*out=nullptr;if(id!=__uuidof(IUnknown))return E_NOINTERFACE;*out=this;AddRef();return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef()override{return ++refs;}
    ULONG STDMETHODCALLTYPE Release()override{
        auto left=--refs;if(!left){if(auto p=owner.lock()){if(list)p->ForgetList(address);else p->ForgetResource(address);}ForgetDiagnostic(address,list);ForgetPreviewPixelObject(address,list);delete this;}return left;
    }
};
bool Track(ID3D12Object* object,bool list,bool create){
    if(!object)return false;IUnknown* existing=nullptr;Lifetime* created=nullptr;HRESULT hr=E_FAIL;
    {std::lock_guard lock(lifetimeMutex);UINT size=sizeof(existing);hr=object->GetPrivateData(lifetimeKey,&size,&existing);
        if((FAILED(hr)||!existing)&&create){created=new(std::nothrow) Lifetime(reinterpret_cast<uintptr_t>(object),list);
            hr=created?object->SetPrivateDataInterface(lifetimeKey,created):E_OUTOFMEMORY;}}
    const bool owned=SUCCEEDED(hr)&&(existing||created);
    if(existing)existing->Release();if(created)created->Release();return owned;
}
void RefreshScope(){images->ObserveScope(ReadPreviewScopeApplication());}
}
PreviewNodeScopeToken EnterPreviewNodeBinding(uintptr_t context,uint64_t key,bool clockKnown,uint32_t frame,uint32_t work,uintptr_t viewObject)noexcept{
    auto app=ReadCameraApplicationForNode(context,key,clockKnown,frame);
    PreviewPixelNodeDiagnostic diagnostic;
    try{if(work==0x377B58&&key&&PreviewObservationRequested()){
        Lease known;if(TryAcquireForVirtualCamera(key,known)&&known.metadata.screen==2&&known.source&&known.source->mode==NativePreviewMode::NativeEditorDual){
            diagnostic={context,viewObject,key,frame,known.metadata.screenEpoch,known.metadata.setupID,known.source->eye,true};
            if(!app){CameraApplication probe;probe.metadata=known.metadata;probe.source=*known.source;
                const auto candidate=ReadCurrentApplicationForSource(probe);const auto now=PublicationNowMs();
                const unsigned reason=!clockKnown?0:!candidate?1:candidate->nativeContext!=context?2:candidate->frame.nativeFrame!=frame?3:
                    !FreshPublication(candidate->metadata.publishedAtMs,now)?4:CameraApplicationSourceKey(*candidate)!=key?5:6;
                NoteNativeNodeAdmission(diagnostic,reason,candidate.get(),clockKnown,now);
            }
        }
    }}catch(...){} // Diagnostic lookup failure never changes the original node path.
    if(!app){auto token=nodeBindings.Enter({});token.pixelDepth=BeginPreviewPixelNode({},work,false,false,0,diagnostic);return token;}
    const auto now=PublicationNowMs();
    const bool admitted=MatchPreviewNode(app,context,key,clockKnown,frame,now);
    if(key==CameraApplicationSourceKey(*app)){
        auto& rows=admitted?nodeAcceptedRows:nodeRejectedRows;
        if(rows.fetch_add(1,std::memory_order_relaxed)<8)
            Log("[MenuPreviewLineage] node-%s context=%llx appliedContext=%llx key=%llx nativeFrame=%u appliedFrame=%u clockKnown=%u fresh=%u source=%llu eye=%u work=%X; camera receipt admission only\n",
                admitted?"admitted":"rejected",context,app->nativeContext,key,frame,app->frame.nativeFrame,
                unsigned(clockKnown),unsigned(FreshPublication(app->metadata.publishedAtMs,now)),app->frame.sourceSequence,app->frame.eye,work);
    }
    bool roleMatch=false;TargetDescriptor expected;
    if(work==0x377B58&&key==CameraApplicationSourceKey(*app)&&(PreviewObservationRequested()||copyNodeRows.load(std::memory_order_relaxed)<8)){
        Lease lease;uint64_t resource{},backing{},viewBacking{};
        const bool leased=TryAcquireForVirtualCamera(key,lease)&&lease.metadata.screenEpoch==app->metadata.screenEpoch&&
            lease.metadata.setupID==app->metadata.setupID&&lease.metadata.componentIdentity==app->metadata.componentIdentity&&
            (app->source.mode!=NativePreviewMode::NativeEditorDual||(lease.source&&*lease.source==app->source));
        const auto sourceComponent=CameraApplicationSourceComponent(*app);
        const bool resourceKnown=leased&&ReadU64Safe(sourceComponent+0x1E8,&resource)&&resource&&ReadU64Safe(resource+0x78,&backing);
        const bool viewKnown=viewObject&&ReadU64Safe(viewObject+0xF58,&viewBacking);
        roleMatch=admitted&&resourceKnown&&viewKnown&&backing&&backing==viewBacking;
        if(roleMatch&&PreviewObservationRequested())expected=ResolvePreviewDefaultTarget(backing);
        if(diagnostic.nativeDual&&(!roleMatch||!expected.known)){
            const unsigned reason=!admitted?7:!leased?8:!resourceKnown?9:!viewKnown?10:!backing?11:backing!=viewBacking?12:13;
            NoteNativeNodeAdmission(diagnostic,reason,app.get(),clockKnown,now,backing,viewBacking,resourceKnown,viewKnown,expected.known);
        }
        if(roleMatch&&copyNodeRows.load(std::memory_order_relaxed)<8)Log("[MenuPreviewLineage] copy-target-descriptor known=%u textureID=%u manager=%llx expectedCPUHandle=%llx; pinned default-mip RTV mapping, NOT COM or completion\n",unsigned(expected.known),expected.textureID,expected.manager,expected.expectedHandle);
        if(copyNodeRows.fetch_add(1,std::memory_order_relaxed)<8)Log("[MenuPreviewLineage] copy-node-role work=%X admitted=%u context=%llx view=%llx component=%llx resource=%llx colorBacking=%llx viewBacking=%llx leased=%u readable=%u roleMatch=%u frame=%u source=%llu eye=%u; intrusive CPU backing identity only, NOT COM or GPU completion\n",
            work,unsigned(admitted),context,viewObject,app->metadata.componentIdentity,resource,backing,viewBacking,unsigned(leased),unsigned(resourceKnown&&viewKnown),
            unsigned(admitted&&resourceKnown&&viewKnown&&backing&&backing==viewBacking),frame,app->frame.sourceSequence,app->frame.eye);
    }
    auto token=nodeBindings.Enter(admitted?app:NodeBindingSlot{},work);
    token.pixelDepth=BeginPreviewPixelNode(admitted?app:NodeBindingSlot{},work,roleMatch,expected.known,expected.expectedHandle,diagnostic);return token;
}
void NotePreviewNodeEpilogue(uint32_t work)noexcept{
    EndPreviewPixelNode();
    const auto& app=nodeBindings.Current();
    if(work==0x377B58&&app&&copyEndRows.fetch_add(1,std::memory_order_relaxed)<8)
        Log("[MenuPreviewLineage] copy-node-return work=%X key=%llx frame=%u source=%llu eye=%u; actual native work returned, no list/resource/state/completion inferred\n",
            work,app->frame.key.virtualCamera,app->frame.nativeFrame,app->frame.sourceSequence,app->frame.eye);
}
void RestorePreviewNodeBinding(PreviewNodeScopeToken token)noexcept{RestorePreviewPixelNode(token.pixelDepth);nodeBindings.Restore(token);}
bool PreviewRtvTrackingActive()noexcept{return HasCurrentPreviewApplications();}
void ObservePreviewRtvBind(ID3D12GraphicsCommandList* list,ID3D12Resource* resource){
    if(!list||!resource)return;RefreshScope();
    // Avoid tracking every game target. Once this resource has preview lineage,
    // ALL later observed RTV binds, including unknown/non-preview, revoke it.
    const auto& binding=nodeBindings.Current();
    if(!binding&&!Track(resource,false,false))return;
    if(!Track(resource,false,true)||!Track(list,true,true))return;
    RtvBindingPayload payload{binding};payload.workRva=nodeBindings.WorkRva();
    const bool copy=payload.workRva==0x377B58;
    if(binding&&((commitRows.load(std::memory_order_relaxed)<8&&descriptorRows.fetch_add(1,std::memory_order_relaxed)<64)||
                 (copy&&copyRtvRows.load(std::memory_order_relaxed)<8))){
        const auto d=resource->GetDesc();payload.width=d.Width;payload.height=d.Height;payload.format=unsigned(d.Format);
        payload.mips=d.MipLevels;payload.array=d.DepthOrArraySize;payload.samples=d.SampleDesc.Count;payload.descriptorKnown=true;
    }
    images->RecordBinding(reinterpret_cast<uintptr_t>(list),reinterpret_cast<uintptr_t>(resource),payload);
    if(binding&&PreviewRtvTrackingActive()){
        RememberDiagnostic(reinterpret_cast<uintptr_t>(list),reinterpret_cast<uintptr_t>(resource),binding);
        if(rtvRows.fetch_add(1,std::memory_order_relaxed)<16||(copy&&copyRtvRows.fetch_add(1,std::memory_order_relaxed)<8)){
            Log("[MenuPreviewLineage] rtv-bind-candidate list=%p resource=%p key=%llx frame=%u source=%llu eye=%u work=%X descKnown=%u format=%u size=%llux%u; RTV-bind candidate NOT draw\n",
                list,resource,binding->frame.key.virtualCamera,binding->frame.nativeFrame,binding->frame.sourceSequence,
                binding->frame.eye,payload.workRva,unsigned(payload.descriptorKnown),payload.format,payload.width,payload.height);
        }
    }else ForgetDiagnostic(reinterpret_cast<uintptr_t>(resource),false);
}
void ResetPreviewImageList(ID3D12GraphicsCommandList* list){
    if(Track(list,true,PreviewObservationRequested())){images->Reset(reinterpret_cast<uintptr_t>(list));ResetPreviewPixelList(list);}
    ForgetDiagnostic(reinterpret_cast<uintptr_t>(list),true);
}
void ObservePreviewImageBarriers(ID3D12GraphicsCommandList* list,UINT count,const D3D12_RESOURCE_BARRIER* barriers){
    PreviewPixelBarriers(list,count,barriers);
    if(!list||!barriers||!PreviewRtvTrackingActive())return;
    const auto revoke=[&](ID3D12Resource* resource){
        if(resource&&Track(resource,false,false)&&Track(list,true,true))
            {images->Record(reinterpret_cast<uintptr_t>(list),reinterpret_cast<uintptr_t>(resource),{});ForgetDiagnostic(reinterpret_cast<uintptr_t>(resource),false);}
    };
    for(UINT i=0;i<count;++i){const auto& b=barriers[i];
        if(b.Type==D3D12_RESOURCE_BARRIER_TYPE_ALIASING){
            if(!b.Aliasing.pResourceBefore||!b.Aliasing.pResourceAfter)images->Invalidate();
            revoke(b.Aliasing.pResourceBefore);revoke(b.Aliasing.pResourceAfter);
        }else if(b.Type==D3D12_RESOURCE_BARRIER_TYPE_UAV){
            if(!b.UAV.pResource)images->Invalidate();else revoke(b.UAV.pResource);
        }else if(b.Type==D3D12_RESOURCE_BARRIER_TYPE_TRANSITION){
            const auto unknownWrite=D3D12_RESOURCE_STATE_COPY_DEST|D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
            if(b.Transition.StateAfter&unknownWrite)revoke(b.Transition.pResource);
        }
    }
}
PreviewImageSubmission PreparePreviewImageLists(ID3D12CommandQueue* queue,UINT n,ID3D12CommandList* const* lists)noexcept{
    try{RefreshScope();if(!queue||(n&&!lists)){images->Invalidate();return {};}
        auto prepared=images->Prepare(reinterpret_cast<uintptr_t>(queue),n,[&](size_t i){return reinterpret_cast<uintptr_t>(lists[i]);});
        if(HasCurrentPreviewApplications())NotePrepared(queue,n,lists,bool(prepared));
        return prepared;
    }catch(...){images->Invalidate();return {};}
}
void CommitPreviewImageLists(ID3D12CommandQueue* queue,PreviewImageSubmission&& submission)noexcept{
    try{RefreshScope();images->Commit(reinterpret_cast<uintptr_t>(queue),std::move(submission),PublicationNowMs());NoteCommitted(queue);}
    catch(...){images->Invalidate();}
}
SubmittedPreviewRtvBinding ReadSubmittedPreviewRtvBinding(ID3D12Resource* resource){
    RefreshScope();return images->Read(reinterpret_cast<uintptr_t>(resource));
}
bool ValidateSubmittedPreviewRtvBinding(const SubmittedPreviewRtvBinding& image,ID3D12CommandQueue* queue){
    RefreshScope();return images->Validate(image,reinterpret_cast<uintptr_t>(queue));
}
}
