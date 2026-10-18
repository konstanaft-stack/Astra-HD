#include "MenuPreview/EditorUiSurface.hpp"
#include "MenuPreview/UiConsumer.hpp"
#include "MenuPreview/DescriptorProvenance.hpp"
#include "MenuPreview/EditorUiPoolPolicy.hpp"
#include "Render/CommandResources.hpp"
#include <dxgi1_6.h>
#include <array>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>
#include <vector>

using Microsoft::WRL::ComPtr;
using namespace cvr::menu_preview;
namespace {
constexpr UINT Extent=64;
bool configuredUiStereo{};
unsigned uiStereoGateCalls{};
struct NativeView {DXGI_FORMAT format;D3D12_RTV_DIMENSION dimension;UINT mip;bool known;};
const NativeView KnownNativeView{DXGI_FORMAT_R16G16B16A16_FLOAT,D3D12_RTV_DIMENSION_TEXTURE2D,0,true};
std::optional<NativeView> fixtureNativeView=KnownNativeView;
uint64_t descriptorWriteSequence{1};
std::unordered_map<uintptr_t,PreviewDescriptorProof> auxiliaryProofs;
std::unordered_set<uintptr_t> pendingAuxiliary;
enum class AuxProofState {Fresh,Stale,Pending,Unproven};
D3D12_CPU_DESCRIPTOR_HANDLE originalRtv{},boundRtv{};
ID3D12Resource* originalResource{};
ID3D12Device* originalDevice{};
ID3D12DescriptorHeap* originalHeap{};
uint64_t nativeExecutes{};
uint64_t privateExecutes{};
ID3D12CommandList* submittingOriginal{};
bool privateExecuteViolation{},nativeExecutePrivateLeak{};
struct Attachment {std::shared_ptr<EditorUiPublication> publication;bool required{};};
std::vector<Attachment> attachments;
bool attachedBeforeExecute{};
bool rejectPublication{};uint64_t rejectedPublications{};std::weak_ptr<cvr::hud::Frame> rejectedFrame;
void Check(bool value,const char* message){if(!value)throw std::runtime_error(message);}
void Hr(HRESULT value){if(FAILED(value)){char message[80];sprintf_s(message,"D3D12 HRESULT 0x%08lx",value);throw std::runtime_error(message);}}
void STDMETHODCALLTYPE OriginalOm(ID3D12GraphicsCommandList* list,UINT count,
 const D3D12_CPU_DESCRIPTOR_HANDLE* handles,BOOL contiguous,const D3D12_CPU_DESCRIPTOR_HANDLE* depth){
 boundRtv=count&&handles?handles[0]:D3D12_CPU_DESCRIPTOR_HANDLE{};
 list->OMSetRenderTargets(count,handles,contiguous,depth);
}
void STDMETHODCALLTYPE NativeExecute(ID3D12CommandQueue* queue,UINT count,ID3D12CommandList* const* lists){
 nativeExecutePrivateLeak|=EditorUiPrivateCommands();
 ++nativeExecutes;queue->ExecuteCommandLists(count,lists);
}
void STDMETHODCALLTYPE PrivateExecute(ID3D12CommandQueue* queue,UINT count,ID3D12CommandList* const* lists){
 // Record failures across the production noexcept boundary; assert after return.
 privateExecuteViolation|=!EditorUiPrivateCommands()||count!=1||!lists||lists[0]==submittingOriginal;
 ++privateExecutes;queue->ExecuteCommandLists(count,lists);
}
}
unsigned int GetForcedRenderWidthValue(){return Extent;}
unsigned int GetForcedRenderHeightValue(){return Extent;}
void Log(const char* format,...){va_list args;va_start(args,format);vprintf(format,args);va_end(args);}
namespace cvr::detail {
using Om=void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*,UINT,const D3D12_CPU_DESCRIPTOR_HANDLE*,BOOL,const D3D12_CPU_DESCRIPTOR_HANDLE*);
Om command_list_original_om(ID3D12GraphicsCommandList*){return OriginalOm;}
}
namespace cvr::menu_preview {
bool UiConsumerEnabled()noexcept{++uiStereoGateCalls;return configuredUiStereo;}
std::optional<PreviewDescriptorProof> ReadPreviewDescriptor(uintptr_t handle)noexcept{
 if(pendingAuxiliary.contains(handle))return {};
 if(handle!=originalRtv.ptr){const auto found=auxiliaryProofs.find(handle);return found==auxiliaryProofs.end()?std::nullopt:std::optional{found->second};}
 PreviewDescriptorProof proof{handle,reinterpret_cast<uintptr_t>(originalResource),reinterpret_cast<uintptr_t>(originalDevice),reinterpret_cast<uintptr_t>(originalHeap),1,1,descriptorWriteSequence};
 if(fixtureNativeView){proof.rtvFormat=static_cast<uint32_t>(fixtureNativeView->format);proof.rtvDimension=static_cast<uint32_t>(fixtureNativeView->dimension);proof.rtvMip=fixtureNativeView->mip;proof.rtvViewKnown=fixtureNativeView->known;}
 return proof;
}
bool ValidatePreviewDescriptor(const PreviewDescriptorProof& proof)noexcept{
 const auto current=ReadPreviewDescriptor(proof.handle);return current&&*current==proof;
}
bool AttachEditorUiPublication(ID3D12CommandQueue*,uintptr_t,uint64_t,uintptr_t,uint64_t,
 std::shared_ptr<EditorUiPublication> publication,bool required)noexcept{
 attachedBeforeExecute|=nativeExecutes==0;
 if(publication&&rejectPublication){++rejectedPublications;rejectedFrame=publication->image;return false;}
 if(publication){publication->sourceSequence=17;publication->cameraRevision=7;publication->nativeFrame=9;publication->eye=0;}
 attachments.push_back({std::move(publication),required});return true;
}
}
namespace {
struct Gpu {
 ComPtr<ID3D12Device> device;ComPtr<ID3D12CommandQueue> queue;
 ComPtr<ID3D12GraphicsCommandList> list;ComPtr<ID3D12Fence> fence;
 std::vector<ComPtr<ID3D12CommandAllocator>> allocators;
 ComPtr<ID3D12Resource> target;ComPtr<ID3D12DescriptorHeap> heap;
 std::vector<D3D12_CPU_DESCRIPTOR_HANDLE> auxiliaryRtvs;
 uint64_t fenceValue{},recording{};
 explicit Gpu(UINT mipLevels=1){
  ComPtr<IDXGIFactory4> factory;Hr(CreateDXGIFactory1(IID_PPV_ARGS(&factory)));
  ComPtr<IDXGIAdapter> adapter;Hr(factory->EnumWarpAdapter(IID_PPV_ARGS(&adapter)));
  Hr(D3D12CreateDevice(adapter.Get(),D3D_FEATURE_LEVEL_11_0,IID_PPV_ARGS(&device)));
  D3D12_COMMAND_QUEUE_DESC desc{};desc.Type=D3D12_COMMAND_LIST_TYPE_DIRECT;Hr(device->CreateCommandQueue(&desc,IID_PPV_ARGS(&queue)));
  Hr(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&fence)));
  D3D12_HEAP_PROPERTIES properties{};properties.Type=D3D12_HEAP_TYPE_DEFAULT;
  D3D12_RESOURCE_DESC texture{};texture.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;texture.Width=Extent;texture.Height=Extent;
  texture.DepthOrArraySize=1;texture.MipLevels=static_cast<UINT16>(mipLevels);texture.Format=DXGI_FORMAT_R16G16B16A16_FLOAT;texture.SampleDesc.Count=1;texture.Flags=D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
  Hr(device->CreateCommittedResource(&properties,D3D12_HEAP_FLAG_NONE,&texture,D3D12_RESOURCE_STATE_RENDER_TARGET,nullptr,IID_PPV_ARGS(&target)));
  D3D12_DESCRIPTOR_HEAP_DESC hd{};hd.Type=D3D12_DESCRIPTOR_HEAP_TYPE_RTV;hd.NumDescriptors=mipLevels;Hr(device->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&heap)));
  originalRtv=heap->GetCPUDescriptorHandleForHeapStart();D3D12_RENDER_TARGET_VIEW_DESC view{};view.Format=texture.Format;view.ViewDimension=D3D12_RTV_DIMENSION_TEXTURE2D;view.Texture2D.MipSlice=0;
  device->CreateRenderTargetView(target.Get(),&view,originalRtv);
  originalResource=target.Get();originalDevice=device.Get();originalHeap=heap.Get();
  auxiliaryProofs.clear();pendingAuxiliary.clear();
  const auto increment=device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
  for(UINT mip=1;mip<mipLevels;++mip){
   const D3D12_CPU_DESCRIPTOR_HANDLE handle{originalRtv.ptr+SIZE_T(mip)*increment};view.Texture2D.MipSlice=mip;
   device->CreateRenderTargetView(target.Get(),&view,handle);auxiliaryRtvs.push_back(handle);
   PreviewDescriptorProof proof{handle.ptr,reinterpret_cast<uintptr_t>(target.Get()),reinterpret_cast<uintptr_t>(device.Get()),reinterpret_cast<uintptr_t>(heap.Get()),1,1,++descriptorWriteSequence};
   proof.rtvFormat=DXGI_FORMAT_R16G16B16A16_FLOAT;proof.rtvDimension=D3D12_RTV_DIMENSION_TEXTURE2D;proof.rtvMip=mip;proof.rtvViewKnown=true;auxiliaryProofs.emplace(handle.ptr,proof);
  }
 }
 ID3D12CommandAllocator* Allocator(){ComPtr<ID3D12CommandAllocator> value;Hr(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&value)));allocators.push_back(value);return value.Get();}
 void Wait(ID3D12Fence* f,uint64_t value){
  Check(f&&value&&f->GetCompletedValue()!=UINT64_MAX,"invalid/removed producer fence");
  HANDLE event=CreateEventW(nullptr,FALSE,FALSE,nullptr);Check(event!=nullptr,"CreateEvent");
  const auto hr=f->SetEventOnCompletion(value,event);if(FAILED(hr)){CloseHandle(event);Hr(hr);}
  const auto result=WaitForSingleObject(event,15000);CloseHandle(event);Check(result==WAIT_OBJECT_0,"WARP fence timeout");
  Check(f->GetCompletedValue()!=UINT64_MAX,"removed device is not completion");
 }
 void Finish(){Hr(queue->Signal(fence.Get(),++fenceValue));Wait(fence.Get(),fenceValue);cvr::gpu::CollectCommandResources();}
 void Begin(const NativeView* nativeView=&KnownNativeView){
  if(!list)Hr(device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,Allocator(),nullptr,IID_PPV_ARGS(&list)));
  else Hr(list->Reset(Allocator(),nullptr));
  ++recording;cvr::gpu::ResetCommandResources(list.Get());ResetEditorUiList(list.Get());
  fixtureNativeView=nativeView?std::optional<NativeView>{*nativeView}:std::nullopt;
  ++descriptorWriteSequence;
  OriginalOm(list.Get(),1,&originalRtv,FALSE,nullptr);
  ObserveEditorUiTargets(list.Get(),1,&originalRtv,FALSE,nullptr,target.Get());
 }
 UiEditorDrawProof Proof(UiEditorDrawKind kind){
  UiEditorDrawProof p{};p.kind=kind;p.scope.screen=2;p.scope.screenEpoch=3;p.scope.controllerIdentity=11;p.scope.componentIdentity=12;
  p.scope.setupID=0;p.scope.publishedAtMs=PublicationNowMs();p.list=reinterpret_cast<uintptr_t>(list.Get());p.nativeNode=91;
  // This ledger is independent from DescriptorProvenance's resourceGeneration=1.
  p.objectGeneration=1;p.recordingGeneration=recording;p.targetResource=reinterpret_cast<uintptr_t>(target.Get());p.targetGeneration=41;p.targetOwner=target;
  p.queue=reinterpret_cast<uintptr_t>(queue.Get());p.descriptorCPU=originalRtv.ptr;p.targetQualified=true;p.nativeBindingKnown=true;p.associationKnown=true;
  p.scopeLifetime=std::make_shared<int>(1);p.aliasLifetime=p.scopeLifetime;p.sourceOwner=target;p.sourceResource=p.targetResource;p.sourceGeneration=1;
  p.sourceSequence=17;p.nativeFrame=9;p.eye=0;
  auto app=std::make_shared<CameraApplication>();app->metadata=p.scope;app->frame.sourceSequence=17;app->frame.nativeFrame=9;app->localPoseRevision=7;app->projection.valid=true;p.application=std::move(app);
  return p;
 }
 void Draw(UiEditorDrawKind kind,const float* color){
  const auto route=BeginEditorUiDraw(list.Get(),Proof(kind));
  if(kind==UiEditorDrawKind::Panel){Check(route.redirected,"qualified Panel did not route to private target");Check(boundRtv.ptr&&boundRtv.ptr!=originalRtv.ptr,"Panel still bound to original RT");}
  if(kind==UiEditorDrawKind::Portrait)Check(!route.redirected&&boundRtv.ptr==originalRtv.ptr,"Portrait must retain original RT");
  if(color)list->ClearRenderTargetView(boundRtv,color,0,nullptr);
  EndEditorUiDraw(list.Get(),route);Check(boundRtv.ptr==originalRtv.ptr,"End failed to restore native original RT");
 }
 void UnqualifiedDraw(UiEditorDrawKind kind){
  auto proof=kind==UiEditorDrawKind::Outside?UiEditorDrawProof{}:Proof(kind);proof.kind=kind;proof.targetQualified=false;
  const auto route=BeginEditorUiDraw(list.Get(),proof);Check(!route.redirected,"unqualified draw was redirected");
  EndEditorUiDraw(list.Get(),route);Check(boundRtv.ptr==originalRtv.ptr,"unqualified draw changed original RT");
 }
 void AuxiliaryDraw(UINT mip,UiEditorDrawKind kind,AuxProofState state=AuxProofState::Fresh){
  Check(mip&&mip<=auxiliaryRtvs.size(),"auxiliary mip fixture missing");const auto handle=auxiliaryRtvs[mip-1];
  auto& descriptor=auxiliaryProofs.at(handle.ptr);descriptor.rtvViewKnown=state!=AuxProofState::Unproven;descriptor.writeSequence=++descriptorWriteSequence;
  pendingAuxiliary.erase(handle.ptr);if(state==AuxProofState::Pending)pendingAuxiliary.insert(handle.ptr);
  OriginalOm(list.Get(),1,&handle,FALSE,nullptr);ObserveEditorUiTargets(list.Get(),1,&handle,FALSE,nullptr,target.Get());
  if(state==AuxProofState::Stale)descriptor.writeSequence=++descriptorWriteSequence;
  auto proof=kind==UiEditorDrawKind::Outside?UiEditorDrawProof{}:Proof(kind);proof.kind=kind;proof.targetQualified=false;proof.nativeNode=102+mip;proof.descriptorCPU=handle.ptr;
  const auto route=BeginEditorUiDraw(list.Get(),proof);Check(!route.redirected&&boundRtv.ptr==handle.ptr,"auxiliary mip draw was redirected");
  const float auxiliaryColor[]{0,1,0,1};list->ClearRenderTargetView(handle,auxiliaryColor,0,nullptr);
  EndEditorUiDraw(list.Get(),route);Check(boundRtv.ptr==handle.ptr,"auxiliary draw changed original mip binding");pendingAuxiliary.erase(handle.ptr);
  OriginalOm(list.Get(),1,&originalRtv,FALSE,nullptr);ObserveEditorUiTargets(list.Get(),1,&originalRtv,FALSE,nullptr,target.Get());
 }
 void RejectedView(const NativeView* nativeView){
  attachments.clear();Begin(nativeView);
  for(const auto kind:{UiEditorDrawKind::Panel,UiEditorDrawKind::Portrait}){
   const auto route=BeginEditorUiDraw(list.Get(),Proof(kind));
   Check(!route.redirected,"unsafe native view redirected to private target");
   EndEditorUiDraw(list.Get(),route);Check(boundRtv.ptr==originalRtv.ptr,"unsafe view changed native binding");
  }
  Submit(ClosePrepare());Check(!attachments.empty(),"unsafe view omitted required marker");
  for(const auto& a:attachments)Check(a.required&&!a.publication,"unsafe view published UI or lost required marker");
 }
 EditorUiSubmission ClosePrepare(){Hr(list->Close());CloseEditorUiList(list.Get(),true);ID3D12CommandList* commands[]{list.Get()};return PrepareEditorUiSubmission(queue.Get(),1,commands);}
 void Submit(EditorUiSubmission token,bool finish=true){
  ID3D12CommandList* commands[]{list.Get()};auto resources=cvr::gpu::PrepareCommandResources(1,commands);
  Check(!EditorUiPrivateCommands(),"private command identity leaked before native submission");submittingOriginal=list.Get();
  NativeExecute(queue.Get(),1,commands);cvr::gpu::SubmitCommandResources(queue.Get(),std::move(resources));
  Check(!EditorUiPrivateCommands(),"native Execute acquired private command identity");
  EditorUiSubmitted(queue.Get(),std::move(token),PrivateExecute);
  Check(!EditorUiPrivateCommands(),"private command identity leaked after EditorUiSubmitted");
  Check(!privateExecuteViolation&&!nativeExecutePrivateLeak,"private/native Execute identity or list mismatch");submittingOriginal=nullptr;if(finish)Finish();
 }
 void CheckColor(ID3D12Resource* image,D3D12_RESOURCE_STATES resting,const std::array<uint16_t,4>& expected){
  Check(image!=nullptr,"missing image");const auto desc=image->GetDesc();Check(desc.Format==DXGI_FORMAT_R16G16B16A16_FLOAT,"snapshot format lost FP16");
  D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};UINT rows{};UINT64 rowSize{},size{};device->GetCopyableFootprints(&desc,0,1,0,&footprint,&rows,&rowSize,&size);
  D3D12_HEAP_PROPERTIES properties{};properties.Type=D3D12_HEAP_TYPE_READBACK;D3D12_RESOURCE_DESC buffer{};buffer.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;buffer.Width=size;buffer.Height=1;buffer.DepthOrArraySize=1;buffer.MipLevels=1;buffer.SampleDesc.Count=1;buffer.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  ComPtr<ID3D12Resource> readback;Hr(device->CreateCommittedResource(&properties,D3D12_HEAP_FLAG_NONE,&buffer,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&readback)));
  ComPtr<ID3D12GraphicsCommandList> copy;Hr(device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,Allocator(),nullptr,IID_PPV_ARGS(&copy)));
  D3D12_RESOURCE_BARRIER barrier{};barrier.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;barrier.Transition={image,D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,resting,D3D12_RESOURCE_STATE_COPY_SOURCE};
  if(resting!=D3D12_RESOURCE_STATE_COPY_SOURCE)copy->ResourceBarrier(1,&barrier);
  D3D12_TEXTURE_COPY_LOCATION dst{};dst.pResource=readback.Get();dst.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;dst.PlacedFootprint=footprint;
  D3D12_TEXTURE_COPY_LOCATION src{};src.pResource=image;src.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;copy->CopyTextureRegion(&dst,0,0,0,&src,nullptr);
  if(resting!=D3D12_RESOURCE_STATE_COPY_SOURCE){std::swap(barrier.Transition.StateBefore,barrier.Transition.StateAfter);copy->ResourceBarrier(1,&barrier);}
  Hr(copy->Close());ID3D12CommandList* commands[]{copy.Get()};queue->ExecuteCommandLists(1,commands);Finish();
  void* mapped{};D3D12_RANGE range{0,static_cast<SIZE_T>(size)};Hr(readback->Map(0,&range,&mapped));bool same=true;
  for(UINT y=0;y<desc.Height;++y){const auto* pixels=reinterpret_cast<const uint16_t*>(static_cast<const uint8_t*>(mapped)+footprint.Offset+SIZE_T(y)*footprint.Footprint.RowPitch);for(UINT x=0;x<desc.Width;++x)for(UINT c=0;c<4;++c)same&=pixels[x*4+c]==expected[c];}
  D3D12_RANGE written{};readback->Unmap(0,&written);Check(same,"GPU pixel mismatch in original/private snapshot");
 }
 void CheckPublication(const std::shared_ptr<EditorUiPublication>& p,const std::array<uint16_t,4>& expected){
  Check(p&&p->image&&p->image->canvas,"missing owned UI snapshot");Check(p->scope.screenEpoch==3,"wrong publication epoch");
  Check(p->image->retainedOwner!=nullptr,"snapshot lacks retained producer lease");Wait(p->image->producerFence.Get(),p->image->producerValue);
  CheckColor(p->image->canvas.Get(),p->image->restingState,expected);
 }
};
const float Red[]{1,0,0,1},Green[]{0,1,0,1},Blue[]{0,0,1,1};
const std::array<uint16_t,4> RedHalf{0x3c00,0,0,0x3c00},GreenHalf{0,0x3c00,0,0x3c00},BlueHalf{0,0,0x3c00,0x3c00};
}
int main(int argc,char** argv)try{
 if(argc>1){
  Check(argc==5&&std::strcmp(argv[1],"--activation")==0,"unknown test arguments");
  configuredUiStereo=std::strcmp(argv[2],"1")==0;
  const bool unset=std::strcmp(argv[3],"unset")==0;
  Check(SetEnvironmentVariableA("CPVR_EDITOR_UI_SEPARATION",unset?nullptr:argv[3])!=FALSE,"activation environment setup");
  const bool expected=std::strcmp(argv[4],"1")==0;
  const bool actual=EditorUiSeparationEnabled();Check(actual==expected,"production activation gate result mismatch");
  const bool forced=std::strcmp(argv[3],"0")==0||std::strcmp(argv[3],"1")==0;
  Check(forced?uiStereoGateCalls==0:uiStereoGateCalls!=0,"activation did not use the expected override/config path");
  std::cout<<"PASS production activation config="<<configuredUiStereo<<" override="<<argv[3]<<" result="<<actual<<"\n";return 0;
 }
 SetEnvironmentVariableW(L"CPVR_EDITOR_UI_SEPARATION",L"1");Check(EditorUiSeparationEnabled(),"separation env gate disabled");
 Gpu gpu;
 {uintptr_t published=100;uint64_t version=20;
  Check(!AbandonRejectedUiPublication(false,1,1,100,19,published,version)&&published==100&&version==20,"stale value abandoned newer publication");
  Check(!AbandonRejectedUiPublication(false,1,1,101,20,published,version)&&published==100&&version==20,"wrong resource abandoned publication");
  Check(!AbandonRejectedUiPublication(false,1,2,100,20,published,version)&&published==100&&version==20,"wrong fence abandoned publication");
  Check(!AbandonRejectedUiPublication(true,1,1,100,20,published,version)&&published==100&&version==20,"unknown GPU completion abandoned publication");
  Check(AbandonRejectedUiPublication(false,1,1,100,20,published,version)&&!published&&!version,"exact rejected publication did not clear exclusion");}
 gpu.Begin();gpu.Draw(UiEditorDrawKind::Panel,Red);gpu.Draw(UiEditorDrawKind::Portrait,Blue);auto token=gpu.ClosePrepare();
 Check(attachments.empty(),"published before native Execute");gpu.Submit(std::move(token));Check(!attachedBeforeExecute,"attached before native Execute");
 Check(attachments.size()==1&&attachments.back().required,"complete target did not require independent UI");auto held=attachments.back().publication;gpu.CheckPublication(held,RedHalf);gpu.CheckColor(gpu.target.Get(),D3D12_RESOURCE_STATE_RENDER_TARGET,BlueHalf);
 attachments.clear();gpu.Begin();gpu.Draw(UiEditorDrawKind::Panel,Green);gpu.Draw(UiEditorDrawKind::Portrait,Blue);gpu.Submit(gpu.ClosePrepare());
 Check(attachments.size()==1,"new recording did not publish");auto newer=attachments.back().publication;gpu.CheckPublication(newer,GreenHalf);gpu.CheckPublication(held,RedHalf);
 Check(held->image->canvas.Get()!=newer->image->canvas.Get(),"new recording reused a held immutable snapshot");
 // A/B stay pinned by accepted readers. Failed attachment C must not become
 // a permanent bare published exclusion after its local read owner disappears.
 {
  ComPtr<ID3D12Fence> gate;Hr(gpu.device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&gate)));
  struct Unblock{ID3D12Fence* f;~Unblock(){f->Signal(1);}} unblock{gate.Get()};
  Hr(gpu.queue->Wait(gate.Get(),1));attachments.clear();rejectPublication=true;
  gpu.Begin();gpu.Draw(UiEditorDrawKind::Panel,Blue);gpu.Draw(UiEditorDrawKind::Portrait,Green);gpu.Submit(gpu.ClosePrepare(),false);
  rejectPublication=false;Check(rejectedPublications==1&&rejectedFrame.expired(),"failed publication still has a CPU reader");
  gpu.Begin();gpu.Draw(UiEditorDrawKind::Panel,Blue);gpu.Draw(UiEditorDrawKind::Portrait,Green);gpu.Submit(gpu.ClosePrepare(),false);
  Check(attachments.size()==1&&attachments.back().required&&!attachments.back().publication,"pending GPU candidate was recycled before completion");
  Hr(gate->Signal(1));gpu.Finish();attachments.clear();
  gpu.Begin();gpu.Draw(UiEditorDrawKind::Panel,Blue);gpu.Draw(UiEditorDrawKind::Portrait,Green);gpu.Submit(gpu.ClosePrepare());
  Check(attachments.size()==1&&attachments.back().publication,"failed attachment permanently excluded the only free UI slot");
  gpu.CheckPublication(attachments.back().publication,BlueHalf);gpu.CheckPublication(held,RedHalf);gpu.CheckPublication(newer,GreenHalf);
 }
 for(unsigned position=0;position<3;++position){
  attachments.clear();gpu.Begin();if(position==0)gpu.Draw(UiEditorDrawKind::Unknown,nullptr);
  gpu.Draw(UiEditorDrawKind::Panel,Red);if(position==1)gpu.Draw(UiEditorDrawKind::Unknown,nullptr);
  gpu.Draw(UiEditorDrawKind::Portrait,Blue);if(position==2)gpu.Draw(UiEditorDrawKind::Unknown,nullptr);gpu.Submit(gpu.ClosePrepare());
  Check(!attachments.empty(),"unknown target omitted required marker");for(const auto& a:attachments)Check(a.required&&!a.publication,"Unknown published unsafe UI or lost required marker");
 }
 for(const auto kind:{UiEditorDrawKind::Unknown,UiEditorDrawKind::Outside})for(unsigned position=0;position<3;++position){
  attachments.clear();gpu.Begin();if(position==0)gpu.UnqualifiedDraw(kind);
  gpu.Draw(UiEditorDrawKind::Panel,Red);if(position==1)gpu.UnqualifiedDraw(kind);
  gpu.Draw(UiEditorDrawKind::Portrait,Blue);if(position==2)gpu.UnqualifiedDraw(kind);gpu.Submit(gpu.ClosePrepare());
  Check(!attachments.empty(),"unqualified same-target draw omitted required marker");
  for(const auto& a:attachments)Check(a.required&&!a.publication,"unqualified prefix/middle/suffix published unsafe UI");
 }
 for(const auto only:{UiEditorDrawKind::Panel,UiEditorDrawKind::Portrait}){
  attachments.clear();gpu.Begin();gpu.Draw(only,only==UiEditorDrawKind::Panel?Red:Blue);gpu.Submit(gpu.ClosePrepare());
  Check(!attachments.empty(),"incomplete Panel/Portrait target omitted required marker");
  for(const auto& a:attachments)Check(a.required&&!a.publication,"missing Panel or Portrait still published");
 }
 attachments.clear();gpu.Begin();gpu.Draw(UiEditorDrawKind::Panel,Red);gpu.Draw(UiEditorDrawKind::Portrait,Blue);auto stale=gpu.ClosePrepare();
 gpu.Begin();Hr(gpu.list->Close());CloseEditorUiList(gpu.list.Get(),true);gpu.Submit(std::move(stale));
 for(const auto& a:attachments)Check(!a.publication,"Reset published a stale prepared recording");
 gpu.Finish();ClearEditorUiSurfaces();gpu.CheckPublication(held,RedHalf);
 attachments.clear();Gpu mipGpu(5);Check(mipGpu.target->GetDesc().MipLevels==5,"fixture did not create five native mips");
 mipGpu.Begin();mipGpu.Draw(UiEditorDrawKind::Panel,Red);mipGpu.Draw(UiEditorDrawKind::Portrait,Blue);mipGpu.Submit(mipGpu.ClosePrepare());
 Check(attachments.size()==1&&attachments.back().required,"mip0 native view did not publish required UI");
 auto mipHeld=attachments.back().publication;mipGpu.CheckPublication(mipHeld,RedHalf);
 Check(mipHeld->image->canvas->GetDesc().MipLevels==1,"owned snapshot inherited unrelated native mips");
 mipGpu.CheckColor(mipGpu.target.Get(),D3D12_RESOURCE_STATE_RENDER_TARGET,BlueHalf);
 attachments.clear();mipGpu.Begin();mipGpu.Draw(UiEditorDrawKind::Panel,Green);mipGpu.Draw(UiEditorDrawKind::Portrait,Blue);mipGpu.Submit(mipGpu.ClosePrepare());
 Check(attachments.size()==1,"second five-mip recording did not publish");mipGpu.CheckPublication(attachments.back().publication,GreenHalf);mipGpu.CheckPublication(mipHeld,RedHalf);
 for(const auto kind:{UiEditorDrawKind::Unknown,UiEditorDrawKind::Outside}){
  attachments.clear();mipGpu.Begin();for(UINT mip=1;mip<=4;++mip)mipGpu.AuxiliaryDraw(mip,kind);
  mipGpu.Draw(UiEditorDrawKind::Panel,Red);mipGpu.Draw(UiEditorDrawKind::Portrait,Blue);
  for(UINT mip=1;mip<=4;++mip)mipGpu.AuxiliaryDraw(mip,kind);mipGpu.Submit(mipGpu.ClosePrepare());
  Check(attachments.size()==1&&attachments.back().required&&attachments.back().publication,"fresh proved auxiliary mip writes tainted the mip0 packet");
  mipGpu.CheckPublication(attachments.back().publication,RedHalf);mipGpu.CheckColor(mipGpu.target.Get(),D3D12_RESOURCE_STATE_RENDER_TARGET,BlueHalf);
 }
 for(const auto state:{AuxProofState::Stale,AuxProofState::Pending,AuxProofState::Unproven})for(const auto kind:{UiEditorDrawKind::Unknown,UiEditorDrawKind::Outside})for(unsigned position=0;position<2;++position){
  attachments.clear();mipGpu.Begin();if(position==0)mipGpu.AuxiliaryDraw(1,kind,state);
  mipGpu.Draw(UiEditorDrawKind::Panel,Red);mipGpu.Draw(UiEditorDrawKind::Portrait,Blue);
  if(position==1)mipGpu.AuxiliaryDraw(1,kind,state);mipGpu.Submit(mipGpu.ClosePrepare());
  Check(!attachments.empty(),"unproven auxiliary view omitted required marker");
  for(const auto& a:attachments)Check(a.required&&!a.publication,"stale/pending/unproven auxiliary view escaped fail-closed taint");
 }
 for(unsigned position=0;position<2;++position){
  attachments.clear();mipGpu.Begin();if(position==1)mipGpu.Draw(UiEditorDrawKind::Panel,Red);
  mipGpu.UnqualifiedDraw(UiEditorDrawKind::Unknown);mipGpu.AuxiliaryDraw(1,UiEditorDrawKind::Outside);
  if(position==0)mipGpu.Draw(UiEditorDrawKind::Panel,Red);mipGpu.Draw(UiEditorDrawKind::Portrait,Blue);mipGpu.Submit(mipGpu.ClosePrepare());
  Check(!attachments.empty(),"earlier unsafe mip0 draw omitted required marker");
  for(const auto& a:attachments)Check(a.required&&!a.publication,"safe auxiliary exclusion cleared earlier mip0 taint");
 }
 attachments.clear();mipGpu.Begin();mipGpu.Draw(UiEditorDrawKind::Panel,Red);
 // Simulate an authoritative RTV overwrite after a private surface was cached.
 fixtureNativeView->mip=1;++descriptorWriteSequence;
 for(const auto kind:{UiEditorDrawKind::Panel,UiEditorDrawKind::Portrait}){
  const auto route=BeginEditorUiDraw(mipGpu.list.Get(),mipGpu.Proof(kind));
  Check(!route.redirected,"changed mip/descriptor version reused a cached private surface");EndEditorUiDraw(mipGpu.list.Get(),route);
 }
 mipGpu.Submit(mipGpu.ClosePrepare());Check(!attachments.empty(),"mid-record native view overwrite omitted required marker");
 for(const auto& a:attachments)Check(a.required&&!a.publication,"mid-record native view overwrite published stale surface");
 std::array<NativeView,4> unsafeViews{KnownNativeView,KnownNativeView,KnownNativeView,KnownNativeView};
 unsafeViews[0].mip=1;unsafeViews[1].format=DXGI_FORMAT_R8G8B8A8_UNORM;
 unsafeViews[2].dimension=D3D12_RTV_DIMENSION_TEXTURE2DARRAY;unsafeViews[3].known=false;
 mipGpu.RejectedView(nullptr);for(const auto& view:unsafeViews)mipGpu.RejectedView(&view);
 // Even the former one-mip path requires proof of its actual native RTV.
 ClearEditorUiSurfaces();Gpu noViewGpu;noViewGpu.RejectedView(nullptr);noViewGpu.Finish();ClearEditorUiSurfaces();
 Check(privateExecutes>0&&!EditorUiPrivateCommands(),"private Execute callback was not verified or leaked TLS");
 std::cout<<"PASS WARP FP16 Panel/Portrait routing, independent ledger generations, original restoration, fenced immutable snapshots, Unknown/unqualified prefix-middle-suffix fail-closed, required Panel+Portrait, reset/prepared-token rejection, native five-mip/mip0 normalization, safe auxiliary mip isolation, unsafe-view rejection, private Execute TLS identity (callbacks="<<privateExecutes<<")\n";return 0;
}catch(const std::exception& e){std::cerr<<"FAIL: "<<e.what()<<'\n';return 1;}
