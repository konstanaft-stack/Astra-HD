#include "MenuPreview/EditorUiSurface.hpp"
#include "MenuPreview/UiConsumer.hpp"
#include "MenuPreview/EditorUiPoolPolicy.hpp"
#include "MenuPreview/DescriptorProvenance.hpp"
#include "Core/VrCoreShared.hpp"
#include "Render/CommandResources.hpp"
#include "Render/RenderProbeScope.hpp"
#include "Runtimes/CaptureBufferLeases.hpp"
#include "Runtimes/CapturePoolPolicy.hpp"
#include <array>
#include <atomic>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <vector>

extern void Log(const char*,...);
namespace cvr::detail {
using EditorOmFunction=void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*,UINT,
 const D3D12_CPU_DESCRIPTOR_HANDLE*,BOOL,const D3D12_CPU_DESCRIPTOR_HANDLE*);
EditorOmFunction command_list_original_om(ID3D12GraphicsCommandList*);
}
namespace cvr::menu_preview {
using Microsoft::WRL::ComPtr;
namespace {
constexpr size_t Slots=3,MaxCanvases=3,MaxRetired=3;
struct CopySlot {
 ComPtr<ID3D12Resource> image,source;
 ComPtr<ID3D12DescriptorHeap> sourceRtv;
 ComPtr<ID3D12CommandAllocator> allocator;
 ComPtr<ID3D12GraphicsCommandList> commands;
 uint64_t value{};
};
struct Pool {
 std::mutex mutex;
 ComPtr<ID3D12Device> device;
 ComPtr<ID3D12Fence> fence;
 ComPtr<ID3D12CommandQueue> lastQueue;
 D3D12_RESOURCE_DESC description{};
 std::array<CopySlot,Slots> slots;
 cvr::capture::BufferLeases leases;
 uintptr_t published{};
 uint64_t publishedValue{};
 uint32_t lastSlot=Slots-1;
 uint64_t value{};
 bool unknown{};
};
struct SurfaceUseState;
struct Surface {
 ComPtr<ID3D12Resource> canvas;
 ComPtr<ID3D12DescriptorHeap> heap;
 D3D12_CPU_DESCRIPTOR_HANDLE rtv{};
 std::shared_ptr<Pool> pool;
 uintptr_t list{},target{},controller{};
 uint64_t life{},targetGeneration{};
 uint32_t epoch{};
 uint64_t lastUse{};std::weak_ptr<SurfaceUseState> nativeUse;
};
struct SurfaceUseState {std::shared_ptr<Surface> surface;};
class SurfaceUseToken final : public IUnknown {
 std::atomic<ULONG> refs{1};std::shared_ptr<SurfaceUseState> state;
public:
 explicit SurfaceUseToken(std::shared_ptr<SurfaceUseState> value):state(std::move(value)){}
 HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id,void** out)override{if(!out)return E_POINTER;*out=nullptr;if(id!=__uuidof(IUnknown))return E_NOINTERFACE;*out=this;AddRef();return S_OK;}
 ULONG STDMETHODCALLTYPE AddRef()override{return ++refs;}
 ULONG STDMETHODCALLTYPE Release()override{const auto n=--refs;if(!n)delete this;return n;}
};
struct Recording {
 UiEditorDrawProof proof;
 std::shared_ptr<Surface> surface;
 uint64_t generation{};
 uint32_t panel{},portrait{},unknown{};
 bool initialized{},invalid{},closed{},retired{},surfaceAllocationRejected{};
};
struct Binding {D3D12_CPU_DESCRIPTOR_HANDLE rtv{};uintptr_t target{};std::optional<PreviewDescriptorProof> descriptor;std::unordered_set<uintptr_t> unclassified;bool unknownAll{},unknownTarget{};};
struct State {
 std::mutex mutex;
 std::unordered_map<uintptr_t,Binding> bindings;
 std::unordered_map<uintptr_t,std::shared_ptr<Recording>> recordings;
 std::vector<std::shared_ptr<Surface>> canvases;
 std::vector<std::shared_ptr<Pool>> retired;
 std::shared_ptr<Pool> pool;
 uint32_t epoch{};
 uintptr_t controller{};
 uint64_t generation{};
 bool failed{};
};
// Unknown GPU completion is retained until process teardown, including its queue.
State& S(){static auto* value=new State;return *value;}
std::atomic<uint64_t> imageSerial{},logRows{};
std::atomic<uint32_t> availabilityRows{};
std::atomic<uint32_t> unclassifiedRows{};
std::atomic<uint32_t> auxiliaryRows{};
thread_local uint32_t privateCopyDepth{};
void Note(const char* reason,const Recording* r=nullptr,uint64_t serial=0)noexcept{
 if(logRows.fetch_add(1,std::memory_order_relaxed)>=96)return;
 try{Log("[EditorUiSurface] %s list=%llx recording=%llu target=%llx epoch=%u setup=%u panel=%u portrait=%u unknown=%u invalid=%u serial=%llu; independent native UI, not a mixed-frame crop\n",reason,
  r?r->proof.list:0,r?r->proof.recordingGeneration:0,r?r->proof.targetResource:0,
  r?r->proof.scope.screenEpoch:0,r?r->proof.scope.setupID:0,r?r->panel:0,r?r->portrait:0,
  r?r->unknown:0,unsigned(r&&r->invalid),serial);}catch(...){}
}
bool Done(Pool& p,uint64_t value){
 if(p.unknown||!p.fence)return false;
 const auto done=p.fence->GetCompletedValue();
 if(done==UINT64_MAX){p.unknown=true;return false;}
 return !value||done>=value;
}
bool Idle(const std::shared_ptr<Pool>& pool){
 if(!pool)return true;std::lock_guard lock(pool->mutex);return Done(*pool,pool->value);
}
void CollectLocked(){
 auto& s=S();
 for(auto i=s.retired.begin();i!=s.retired.end();)if(Idle(*i))i=s.retired.erase(i);else ++i;
}
bool Description(const D3D12_RESOURCE_DESC& d){
 const bool format=d.Format==DXGI_FORMAT_R16G16B16A16_FLOAT||d.Format==DXGI_FORMAT_R8G8B8A8_UNORM||d.Format==DXGI_FORMAT_R8G8B8A8_UNORM_SRGB||d.Format==DXGI_FORMAT_B8G8R8A8_UNORM||d.Format==DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
 // Bounds also limit the editor-only pool to six 32 MiB images at FP16.
 return format&&d.Dimension==D3D12_RESOURCE_DIMENSION_TEXTURE2D&&d.Width&&d.Width<=2048&&d.Height&&d.Height<=2048&&d.DepthOrArraySize==1&&d.MipLevels==1&&d.SampleDesc.Count==1&&d.SampleDesc.Quality==0;
}
bool NativeUiView(const PreviewDescriptorProof& descriptor){
 // This is the view written by the native CreateRTV mutation, not a resource-
 // only inference from its dimensions. Auxiliary native mips are not panels.
 return descriptor.rtvViewKnown&&descriptor.rtvFormat==DXGI_FORMAT_R16G16B16A16_FLOAT&&
  descriptor.rtvDimension==D3D12_RTV_DIMENSION_TEXTURE2D&&descriptor.rtvMip==0;
}
std::optional<D3D12_RESOURCE_DESC> OwnedDescription(D3D12_RESOURCE_DESC resource,const PreviewDescriptorProof& view){
 if(!NativeUiView(view)||resource.Format!=view.rtvFormat||!resource.MipLevels)return {};
 // Native UI has five mips; only its proven mip-zero RTV is rendered here.
 // Owned canvas/snapshot and the XR copy retain the one-level image contract.
 resource.MipLevels=1;
 if(!Description(resource))return {};
 return resource;
}
bool SameDescription(const D3D12_RESOURCE_DESC& a,const D3D12_RESOURCE_DESC& b){
 return a.Dimension==b.Dimension&&a.Width==b.Width&&a.Height==b.Height&&a.DepthOrArraySize==b.DepthOrArraySize&&a.MipLevels==b.MipLevels&&a.Format==b.Format&&a.SampleDesc.Count==b.SampleDesc.Count&&a.SampleDesc.Quality==b.SampleDesc.Quality;
}
bool NewTexture(ID3D12Device* device,D3D12_RESOURCE_DESC d,bool render,ComPtr<ID3D12Resource>& out){
 d.Alignment=0;d.Layout=D3D12_TEXTURE_LAYOUT_UNKNOWN;d.Flags=render?D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET:D3D12_RESOURCE_FLAG_NONE;
 D3D12_HEAP_PROPERTIES heap{};heap.Type=D3D12_HEAP_TYPE_DEFAULT;
 D3D12_CLEAR_VALUE clear{};clear.Format=d.Format;
 return SUCCEEDED(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&d,render?D3D12_RESOURCE_STATE_RENDER_TARGET:D3D12_RESOURCE_STATE_COPY_SOURCE,render?&clear:nullptr,IID_PPV_ARGS(&out)));
}
bool RotateLocked(uint32_t epoch,uintptr_t controller,const D3D12_RESOURCE_DESC& desc,ID3D12Device* device){
 auto& s=S();CollectLocked();
 if(s.failed)return false;
 if(s.pool&&s.epoch==epoch&&s.controller==controller&&s.pool->device.Get()==device&&SameDescription(s.pool->description,desc))return true;
 if(s.pool&&!Idle(s.pool)){
  if(s.retired.size()>=MaxRetired){s.failed=true;return false;}
  s.retired.push_back(s.pool);
 }
 auto pool=std::make_shared<Pool>();pool->device=device;pool->description=desc;
 if(FAILED(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&pool->fence))))return false;
 pool->fence->SetName(L"CVR_editor_UI_producer");
 s.canvases.clear();s.pool=std::move(pool);s.epoch=epoch;s.controller=controller;return true;
}
// Expiry proves native recording/prepared/submitted owners all retired through
// CommandResources. A bare command-list Reset is not GPU completion.
bool ReusableSurface(const std::shared_ptr<Surface>& surface){
 if(!surface||!surface->pool||!surface->nativeUse.expired())return false;
 std::lock_guard lock(surface->pool->mutex);return Done(*surface->pool,surface->lastUse);
}
std::shared_ptr<Surface> RebindSurfaceLocked(const std::shared_ptr<Surface>& surface,const UiEditorDrawProof& proof){
 auto& s=S();
 // Prepared old UI records may remain alive, but must never see a new binding.
 for(auto i=s.recordings.begin();i!=s.recordings.end();)if(i->second->surface==surface){i->second->retired=true;i=s.recordings.erase(i);}else ++i;
 surface->list=proof.list;surface->life=proof.objectGeneration;surface->target=proof.targetResource;surface->targetGeneration=proof.targetGeneration;
 surface->epoch=proof.scope.screenEpoch;surface->controller=proof.scope.controllerIdentity;return surface;
}
std::shared_ptr<Surface> ReuseSurfaceLocked(const UiEditorDrawProof& proof){
 auto& s=S();
 for(const auto& item:s.canvases)if(item->list==proof.list&&item->life==proof.objectGeneration&&item->target==proof.targetResource&&item->targetGeneration==proof.targetGeneration&&ReusableSurface(item))return RebindSurfaceLocked(item,proof);
 for(const auto& item:s.canvases)if(ReusableSurface(item))return RebindSurfaceLocked(item,proof);
 return {};
}
std::shared_ptr<Surface> GetSurfaceLocked(const UiEditorDrawProof& proof,const PreviewDescriptorProof& view){
 auto& s=S();if(!proof.targetOwner||proof.targetOwner.Get()!=reinterpret_cast<ID3D12Resource*>(proof.targetResource))return {};
 const auto native=proof.targetOwner->GetDesc();const auto owned=OwnedDescription(native,view);
 if(availabilityRows.fetch_add(1,std::memory_order_relaxed)<4)Log("[EditorUiAvailability] target=%llx desc=%ux%u format=%u dimension=%u mipLevels=%u array=%u samples=%u quality=%u flags=%u rtvKnown=%u rtvFormat=%u rtvDimension=%u rtvMip=%u descriptorSequence=%llu ownedMipLevels=%u supported=%u failedLatch=%u cached=%zu retired=%zu; atomic native view proof\n",proof.targetResource,unsigned(native.Width),native.Height,unsigned(native.Format),unsigned(native.Dimension),native.MipLevels,native.DepthOrArraySize,native.SampleDesc.Count,native.SampleDesc.Quality,unsigned(native.Flags),unsigned(view.rtvViewKnown),view.rtvFormat,view.rtvDimension,view.rtvMip,view.writeSequence,owned?owned->MipLevels:0,unsigned(bool(owned)),unsigned(s.failed),s.canvases.size(),s.retired.size());
 if(!owned)return {};const auto desc=*owned;
 ComPtr<ID3D12Device> device;const auto deviceResult=proof.targetOwner->GetDevice(IID_PPV_ARGS(&device));if(FAILED(deviceResult)){Log("[EditorUiAvailability] GetDevice failed hr=%08X\n",unsigned(deviceResult));return {};}
 if(!RotateLocked(proof.scope.screenEpoch,proof.scope.controllerIdentity,desc,device.Get())){Note("pool-rotation-rejected");return {};}
 if(auto reusable=ReuseSurfaceLocked(proof))return reusable;
 if(s.canvases.size()>=MaxCanvases){
  // Pressure only; token Release has no source callbacks or State lock.
  cvr::gpu::CollectCommandResources();
  if(auto reusable=ReuseSurfaceLocked(proof))return reusable;
  return {};
 }
 auto surface=std::make_shared<Surface>();surface->pool=s.pool;surface->list=proof.list;surface->life=proof.objectGeneration;surface->target=proof.targetResource;surface->targetGeneration=proof.targetGeneration;surface->epoch=proof.scope.screenEpoch;surface->controller=proof.scope.controllerIdentity;
 D3D12_DESCRIPTOR_HEAP_DESC heap{};heap.Type=D3D12_DESCRIPTOR_HEAP_TYPE_RTV;heap.NumDescriptors=1;
 if(!NewTexture(device.Get(),desc,true,surface->canvas)||FAILED(device->CreateDescriptorHeap(&heap,IID_PPV_ARGS(&surface->heap))))return {};
 surface->canvas->SetName(L"CVR_editor_private_UI_native_canvas");surface->rtv=surface->heap->GetCPUDescriptorHandleForHeapStart();
 D3D12_RENDER_TARGET_VIEW_DESC ownView{};ownView.Format=desc.Format;ownView.ViewDimension=D3D12_RTV_DIMENSION_TEXTURE2D;
 device->CreateRenderTargetView(surface->canvas.Get(),&ownView,surface->rtv);
 s.canvases.push_back(surface);return surface;
}
void Barrier(ID3D12GraphicsCommandList* list,ID3D12Resource* resource,D3D12_RESOURCE_STATES before,D3D12_RESOURCE_STATES after){
 D3D12_RESOURCE_BARRIER b{};b.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;b.Transition={resource,D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,before,after};list->ResourceBarrier(1,&b);
}
struct ReadOwner {
 std::shared_ptr<Pool> pool;uintptr_t image{};bool active{};
 ~ReadOwner(){if(active){std::lock_guard lock(pool->mutex);pool->leases.Unpin(image);}}
};
}
struct EditorUiSubmissionData {std::vector<std::shared_ptr<Recording>> recordings;};
bool EditorUiSeparationEnabled()noexcept{
 static const int diagnosticOverride=[] {
  char value[4]{};const auto length=GetEnvironmentVariableA("CPVR_EDITOR_UI_SEPARATION",value,sizeof(value));
  if(length==1&&value[0]=='0')return 0;
  if(length==1&&value[0]=='1')return 1;
  return -1;
 }();
 return diagnosticOverride>=0?diagnosticOverride!=0:UiStereoRequested();
}
bool EditorUiPrivateCommands()noexcept{return privateCopyDepth!=0;}
EditorUiPrivateCommandScope::EditorUiPrivateCommandScope()noexcept:previousInternal(cvr::stereo::probe::internalCommands){cvr::stereo::probe::internalCommands=true;++privateCopyDepth;}
EditorUiPrivateCommandScope::~EditorUiPrivateCommandScope(){--privateCopyDepth;cvr::stereo::probe::internalCommands=previousInternal;}
void ObserveEditorUiTargets(ID3D12GraphicsCommandList* list,UINT count,const D3D12_CPU_DESCRIPTOR_HANDLE* handles,BOOL,const D3D12_CPU_DESCRIPTOR_HANDLE* depth,ID3D12Resource* resource)noexcept{
 if(!EditorUiSeparationEnabled()||!list||cvr::stereo::probe::internalCommands)return;
 try{
  Binding binding;
  if(count==1&&handles&&handles[0].ptr&&!depth){
   binding.rtv=handles[0];binding.target=reinterpret_cast<uintptr_t>(resource);binding.descriptor=ReadPreviewDescriptor(binding.rtv.ptr);
   if(!binding.target&&binding.descriptor)binding.target=binding.descriptor->resource;
   if(binding.descriptor&&binding.descriptor->resource!=binding.target)binding.descriptor.reset();
  }
  binding.unknownTarget=count>0&&!binding.target;
  std::lock_guard lock(S().mutex);const auto id=reinterpret_cast<uintptr_t>(list);
  const auto old=S().bindings.find(id);
  if(old!=S().bindings.end()){binding.unclassified=std::move(old->second.unclassified);binding.unknownAll=old->second.unknownAll;}
  if(old!=S().bindings.end()||((binding.rtv.ptr||binding.unknownTarget)&&S().bindings.size()<2048))S().bindings[id]=std::move(binding);
 }catch(...){}
}
EditorUiDrawRoute BeginEditorUiDraw(ID3D12GraphicsCommandList* list,const UiEditorDrawProof& proof)noexcept{
 EditorUiDrawRoute route;
 if(!EditorUiSeparationEnabled()||!list||cvr::stereo::probe::internalCommands)return route;
 try{
  const auto original=cvr::detail::command_list_original_om(list);if(!original)return route;
  std::lock_guard lock(S().mutex);auto& s=S();const auto id=reinterpret_cast<uintptr_t>(list);
  const auto bound=s.bindings.find(id);const auto existing=s.recordings.find(id);
  const auto boundTarget=bound!=s.bindings.end()?bound->second.target:0;
  if(proof.kind==UiEditorDrawKind::Outside||!proof.targetQualified){
   if(bound!=s.bindings.end()&&bound->second.descriptor){
    const auto& view=*bound->second.descriptor;
    if(view.resource==boundTarget&&view.rtvViewKnown&&view.rtvFormat==DXGI_FORMAT_R16G16B16A16_FLOAT&&
       view.rtvDimension==D3D12_RTV_DIMENSION_TEXTURE2D&&view.rtvMip>0){
     const auto current=ReadPreviewDescriptor(bound->second.rtv.ptr);
     if(current&&*current==view){
      // This native RTV cannot address mip0 or our separately committed UI
      // canvas. Keep its original draw, without tainting mip0 completeness.
      if(auxiliaryRows.fetch_add(1,std::memory_order_relaxed)<4)Log("[EditorUiAuxiliary] list=%llx target=%llx rtvMip=%u descriptorSequence=%llu; fresh native nonzero mip draw preserved, mip0 UI untouched\n",id,boundTarget,view.rtvMip,view.writeSequence);
      return route;
     }
    }
   }
   const bool touchesRecording=existing!=s.recordings.end()&&boundTarget&&existing->second->proof.targetResource==boundTarget;
   if(bound!=s.bindings.end()&&bound->second.unknownTarget){bound->second.unknownAll=true;if(existing!=s.recordings.end())existing->second->invalid=true;}
   if(boundTarget){
    if(bound->second.unclassified.size()<32)bound->second.unclassified.insert(boundTarget);
    else{bound->second.unknownAll=true;if(existing!=s.recordings.end())existing->second->invalid=true;}
   }
   if(touchesRecording){
    existing->second->invalid=true;++existing->second->unknown;
    if(unclassifiedRows.fetch_add(1,std::memory_order_relaxed)<8){const auto& d=bound->second.descriptor;
     Log("[EditorUiUnclassified] list=%llx recording=%llu nativeNode=%llx kind=%u targetQualified=%u boundTarget=%llx rtvKnown=%u rtvDimension=%u rtvMip=%u descriptorSequence=%llu; original draw preserved, recording incomplete\n",id,existing->second->proof.recordingGeneration,proof.nativeNode,unsigned(proof.kind),unsigned(proof.targetQualified),boundTarget,unsigned(d&&d->rtvViewKnown),d?d->rtvDimension:0,d?d->rtvMip:0,d?d->writeSequence:0);
    }
   }
   return route;
  }
  if(id!=proof.list||!proof.objectGeneration||!proof.recordingGeneration||!proof.targetGeneration||proof.scope.screen!=2||!proof.scope.screenEpoch||!proof.scope.controllerIdentity)return route;
  auto& recording=s.recordings[id];
  if(!recording||recording->proof.objectGeneration!=proof.objectGeneration||recording->proof.recordingGeneration!=proof.recordingGeneration){
   if(recording)recording->retired=true;
   recording=std::make_shared<Recording>();recording->proof=proof;recording->generation=++s.generation;
   if(bound!=s.bindings.end()&&(bound->second.unknownAll||bound->second.unclassified.contains(proof.targetResource)))recording->invalid=true;
  }
  auto& r=*recording;
  if(r.surfaceAllocationRejected)return route; // Capacity failure is final for this recording; unrelated invalid tags keep native routing.
  if(r.closed||r.retired||r.proof.targetResource!=proof.targetResource||r.proof.targetGeneration!=proof.targetGeneration||r.proof.scope.screenEpoch!=proof.scope.screenEpoch||r.proof.scope.setupID!=proof.scope.setupID||r.proof.scope.controllerIdentity!=proof.scope.controllerIdentity){r.invalid=true;return route;}
  const auto binding=s.bindings.find(id);
  // DescriptorProvenance and UiConsumer have independent generation domains.
  // Revalidate each in its own ledger; only the retained resource pointer joins them.
  if(binding==s.bindings.end()||!binding->second.descriptor||binding->second.descriptor->resource!=proof.targetResource){r.invalid=true;Note("binding-rejected",&r);return route;}
  const auto descriptor=ReadPreviewDescriptor(binding->second.rtv.ptr);
  if(!descriptor||*descriptor!=*binding->second.descriptor){r.invalid=true;return route;}
  if(proof.kind==UiEditorDrawKind::Unknown){++r.unknown;r.invalid=true;return route;}
  if(!NativeUiView(*descriptor)){r.invalid=true;++r.unknown;Note("native-rtv-view-rejected",&r);return route;}
  if(!r.surface)r.surface=GetSurfaceLocked(proof,*descriptor);
  if(!r.surface){r.invalid=true;r.surfaceAllocationRejected=true;Note("surface-unavailable",&r);return route;}
  if(!r.initialized){
   auto use=std::make_shared<SurfaceUseState>();use->surface=r.surface;
   ComPtr<SurfaceUseToken> token;token.Attach(new SurfaceUseToken(use));
   if(!cvr::gpu::KeepCommandResources(list,{r.surface->canvas.Get(),r.surface->heap.Get(),token.Get()})){r.invalid=true;Note("resource-retention-failed",&r);return route;}
   r.surface->nativeUse=use;
  }
  {std::lock_guard poolLock(r.surface->pool->mutex);if(r.surface->pool->unknown){r.invalid=true;return route;}}
  if(!r.initialized){
   cvr::stereo::probe::Scope internal;const float transparent[4]{};
   list->ClearRenderTargetView(r.surface->rtv,transparent,0,nullptr);r.initialized=true;
  }
  if(proof.kind==UiEditorDrawKind::Portrait){++r.portrait;return route;}
  if(proof.kind!=UiEditorDrawKind::Panel){r.invalid=true;return route;}
  ++r.panel;route.original=binding->second.rtv;route.lifetime=r.surface;
  original(list,1,&r.surface->rtv,FALSE,nullptr);route.redirected=true;
 }catch(...){Note("draw-exception");}
 return route;
}
void EndEditorUiDraw(ID3D12GraphicsCommandList* list,const EditorUiDrawRoute& route)noexcept{
 if(!route.redirected||!list||!route.original.ptr)return;
 if(const auto original=cvr::detail::command_list_original_om(list))original(list,1,&route.original,FALSE,nullptr);
}
void ResetEditorUiList(ID3D12GraphicsCommandList* list)noexcept{
 if(!EditorUiSeparationEnabled()||!list||cvr::stereo::probe::internalCommands)return;
 try{std::lock_guard lock(S().mutex);const auto id=reinterpret_cast<uintptr_t>(list);S().bindings.erase(id);const auto found=S().recordings.find(id);if(found!=S().recordings.end()){found->second->retired=true;S().recordings.erase(found);}}catch(...){}
}
void CloseEditorUiList(ID3D12GraphicsCommandList* list,bool success)noexcept{
 if(!EditorUiSeparationEnabled()||!list||cvr::stereo::probe::internalCommands)return;
 try{std::lock_guard lock(S().mutex);const auto found=S().recordings.find(reinterpret_cast<uintptr_t>(list));if(found!=S().recordings.end()){found->second->closed=success;found->second->invalid|=!success;}}catch(...){}
}
EditorUiSubmission PrepareEditorUiSubmission(ID3D12CommandQueue* queue,UINT count,ID3D12CommandList* const* lists)noexcept{
 EditorUiSubmission result;if(!EditorUiSeparationEnabled()||!queue||!lists)return result;
 try{
  auto batch=std::make_shared<EditorUiSubmissionData>();std::lock_guard lock(S().mutex);CollectLocked();
  for(UINT i=0;i<count;++i){const auto found=S().recordings.find(reinterpret_cast<uintptr_t>(lists[i]));if(found==S().recordings.end()||found->second->retired)continue;auto r=found->second;
   if(!r->closed)r->invalid=true;
   if(r->surface){auto& pool=*r->surface->pool;std::lock_guard poolLock(pool.mutex);
    if(pool.unknown||pool.fence->GetCompletedValue()==UINT64_MAX){pool.unknown=true;r->invalid=true;}
    // Serialized submission order also covers an unexpected queue change.
    if(pool.lastQueue&&pool.lastQueue.Get()!=queue&&pool.value&&FAILED(queue->Wait(pool.fence.Get(),pool.value))){pool.unknown=true;r->invalid=true;}
   }
   batch->recordings.push_back(std::move(r));
  }
  if(!batch->recordings.empty())result.state=std::move(batch);
 }catch(...){Note("prepare-exception");}
 return result;
}
void EditorUiSubmitted(ID3D12CommandQueue* queue,EditorUiSubmission&& submission,EditorUiExecute execute)noexcept{
 auto batch=std::move(submission.state);if(!batch||!queue)return;
 for(const auto& record:batch->recordings){
  std::shared_ptr<EditorUiPublication> publication;
  bool producerSignaled=false;
  try{
   // Reset/recycle after Prepare must never revive the old generation.
   {std::lock_guard lock(S().mutex);const auto i=S().recordings.find(record->proof.list);if(record->retired||i==S().recordings.end()||i->second!=record)continue;}
   const bool complete=execute&&record->closed&&!record->invalid&&record->initialized&&record->panel&&record->portrait&&record->surface;
   if(complete){
    const auto surface=record->surface;const auto pool=surface->pool;
    auto frame=std::make_shared<cvr::hud::Frame>();auto owner=std::make_shared<ReadOwner>();owner->pool=pool;
    publication=std::make_shared<EditorUiPublication>();publication->scope=record->proof.scope;publication->image=frame;
    const auto rw=GetForcedRenderWidthValue(),rh=GetForcedRenderHeightValue();publication->renderAspect=rh?float(rw)/float(rh):0.f;
    std::lock_guard lock(pool->mutex);
    std::array<uintptr_t,Slots> addresses{};
    for(size_t i=0;i<Slots;++i){auto& slot=pool->slots[i];
     if(!slot.image&&!pool->unknown){if(!NewTexture(pool->device.Get(),pool->description,false,slot.image)){pool->unknown=true;break;}slot.image->SetName(L"CVR_editor_UI_immutable_snapshot");}
     if(slot.image&&Done(*pool,slot.value))addresses[i]=reinterpret_cast<uintptr_t>(slot.image.Get());
    }
    const auto chosen=pool->unknown?std::optional<uint32_t>{}:cvr::capture::ReserveMainWrite(addresses,pool->published,pool->lastSlot,pool->leases);
    if(chosen){
     auto& slot=pool->slots[*chosen];const auto address=addresses[*chosen];bool writing=true;
     const auto endWrite=[&] {if(writing){pool->leases.EndWrite(address);writing=false;}};
     // Do not re-enter game UI lifecycle observers while owning the pool lock.
     // CommandResources lifetime is managed explicitly for this private list.
     EditorUiPrivateCommandScope privateCommands;
     bool ready=true;
     if(!slot.allocator||!slot.commands){
      // Partial CreateCommandList failure leaves no usable context. Rebuild it
      // next time instead of dereferencing a surviving allocator/null list.
      slot.commands.Reset();slot.allocator.Reset();
      ready=SUCCEEDED(pool->device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&slot.allocator)))&&SUCCEEDED(pool->device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,slot.allocator.Get(),nullptr,IID_PPV_ARGS(&slot.commands)));
      if(!ready){slot.commands.Reset();slot.allocator.Reset();}
     }
     else ready=SUCCEEDED(slot.allocator->Reset())&&SUCCEEDED(slot.commands->Reset(slot.allocator.Get(),nullptr));
     if(ready){
      cvr::stereo::probe::Scope internal;
      cvr::gpu::ResetCommandResources(slot.commands.Get());
      ready=cvr::gpu::KeepCommandResources(slot.commands.Get(),{surface->canvas.Get(),surface->heap.Get(),slot.image.Get()});
      if(ready){
       Barrier(slot.commands.Get(),surface->canvas.Get(),D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_COPY_SOURCE);
       Barrier(slot.commands.Get(),slot.image.Get(),D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_COPY_DEST);
       slot.commands->CopyResource(slot.image.Get(),surface->canvas.Get());
       Barrier(slot.commands.Get(),slot.image.Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_COPY_SOURCE);
       Barrier(slot.commands.Get(),surface->canvas.Get(),D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_RENDER_TARGET);
       ready=SUCCEEDED(slot.commands->Close());
      }
     }
     if(ready&&pool->value<UINT64_MAX-1){
      // Hold operands before Execute; failed Signal cannot drop in-flight owners.
      slot.source=surface->canvas;slot.sourceRtv=surface->heap;
      ID3D12CommandList* lists[]{slot.commands.Get()};auto resources=cvr::gpu::PrepareCommandResources(1,lists);
      const auto value=++pool->value;execute(queue,1,lists);pool->lastQueue=queue;
      const auto signaled=queue->Signal(pool->fence.Get(),value);slot.value=value;
      producerSignaled=true;surface->lastUse=value;
      cvr::gpu::SubmitCommandResources(queue,std::move(resources));
      if(SUCCEEDED(signaled)&&pool->fence->GetCompletedValue()!=UINT64_MAX){
       endWrite();
       if(pool->leases.Pin(address)){
        owner->image=address;owner->active=true;frame->canvas=slot.image;frame->retainedOwner=owner;frame->producerFence=pool->fence;frame->producerValue=value;
        frame->restingState=D3D12_RESOURCE_STATE_COPY_SOURCE;frame->serial=++imageSerial;frame->generation=record->generation;frame->stamp=GetTickCount64();
        pool->published=address;pool->publishedValue=frame->producerValue;pool->lastSlot=*chosen;
       }else publication.reset();
      }else{pool->unknown=true;publication.reset();}
     }else publication.reset();
     endWrite();
    }else publication.reset();
   }
   // Even an incomplete UI recording has already written the private canvas.
   // Order a later queue against that write, not only successful snapshots.
   if(record->surface&&!producerSignaled){
    auto& pool=*record->surface->pool;std::lock_guard lock(pool.mutex);
    if(!pool.unknown&&pool.value<UINT64_MAX-1){
     const auto value=++pool.value;pool.lastQueue=queue;
     if(FAILED(queue->Signal(pool.fence.Get(),value))||pool.fence->GetCompletedValue()==UINT64_MAX)pool.unknown=true;
     record->surface->lastUse=value;
    }
   }
   const bool attached=AttachEditorUiPublication(queue,record->proof.list,record->proof.recordingGeneration,record->proof.targetResource,record->proof.targetGeneration,publication,true);
   bool abandoned=false;
   if(!attached&&publication&&publication->image&&record->surface){
    auto& pool=*record->surface->pool;const auto& frame=*publication->image;std::lock_guard poolLock(pool.mutex);
    if(pool.fence&&pool.fence->GetCompletedValue()==UINT64_MAX)pool.unknown=true;
    abandoned=AbandonRejectedUiPublication(pool.unknown,reinterpret_cast<uintptr_t>(pool.fence.Get()),
        reinterpret_cast<uintptr_t>(frame.producerFence.Get()),reinterpret_cast<uintptr_t>(frame.canvas.Get()),
        frame.producerValue,pool.published,pool.publishedValue);
   }
   Note(attached&&publication?"snapshot-attached":abandoned?"snapshot-attach-rejected-abandoned":complete?"snapshot-unavailable":"incomplete-recording",record.get(),publication&&publication->image?publication->image->serial:0);
  }catch(...){
   Note("submission-exception",record.get());
   AttachEditorUiPublication(queue,record->proof.list,record->proof.recordingGeneration,record->proof.targetResource,record->proof.targetGeneration,{},true);
  }
 }
}
void ClearEditorUiSurfaces()noexcept{
 if(!EditorUiSeparationEnabled())return;
 try{std::lock_guard lock(S().mutex);auto& s=S();CollectLocked();for(auto& [_,r]:s.recordings)r->retired=true;s.recordings.clear();s.bindings.clear();s.canvases.clear();
  if(s.pool&&!Idle(s.pool)){s.retired.push_back(s.pool);if(s.retired.size()>MaxRetired)s.failed=true;}
  s.pool.reset();s.epoch=0;s.controller=0;
 }catch(...){}
}
}
