// Real MAIN4 registry + production worker private snapshot on WARP.
// Game hooks are not invoked; no NGX output or headset acceptance is claimed.
#include "../../src/Framegen/Inputs.cpp"
#include "cpvr/generation.hpp"
#include <dxgi1_6.h>
#include <d3d12sdklayers.h>
#include <iostream>
#include <stdexcept>
#include <cstdlib>
extern "C" {__declspec(dllexport) int CyberpunkVR_RuntimeDiagnostics=0;int32_t CyberpunkVR_VrcamDlssViewport=1;}
void Log(const char*,...){}
namespace cvr::detail {uint8_t* g_exe_base{};}
namespace cvr::camera {PoseIdentity CurrentNodePoseIdentity(){std::abort();}}
extern "C" {
MH_STATUS WINAPI MH_CreateHook(LPVOID,LPVOID,LPVOID*){std::abort();}
MH_STATUS WINAPI MH_RemoveHook(LPVOID){std::abort();}
MH_STATUS WINAPI MH_EnableHook(LPVOID){std::abort();}
MH_STATUS WINAPI MH_DisableHook(LPVOID){std::abort();}
}
namespace cvr::framegen {
Settings GetSettings(){Settings s;s.aerV2=true;return s;}
void OnInput(unsigned,uint32_t,uint32_t){}
void RecordProducerTrace(const ProducerTraceRecord&) noexcept{}
uint64_t SrTraceQpc() noexcept {std::abort();}
void SnapshotSrConstants(SrTraceRecord&,const sl::Constants*,const sl::FrameToken*,const sl::ViewportHandle*) noexcept {std::abort();}
void SnapshotSrTags(SrTraceRecord&,const void*,const void*,uint32_t,void*) noexcept {std::abort();}
void SnapshotSrEvaluate(SrTraceRecord&,uint32_t,const sl::FrameToken*,const sl::BaseStructure*const*,uint32_t,void*) noexcept {std::abort();}
void RecordSrIdentity(const SrTraceRecord&) noexcept {std::abort();}
bool InstallSrSlHooks(){std::abort();}bool EnsureSrNativeHooks(){std::abort();}
SrConstantsCall BeginSrConstants(const sl::Constants&,const sl::FrameToken&,const sl::ViewportHandle&) noexcept {std::abort();}
void EndSrConstants(SrConstantsCall&,uint32_t,bool) noexcept {std::abort();}
SrEvaluationCall BeginSrEvaluation(uint32_t,const sl::FrameToken&,const sl::BaseStructure*const*,uint32_t,void*) noexcept {std::abort();}
void EndSrEvaluation(SrEvaluationCall&,uint32_t,bool) noexcept {std::abort();}
bool CopyInput(ID3D12GraphicsCommandList*,const InputTag&,Inputs&){std::abort();}
bool CaptureInputsEnabled(){std::abort();}void OnSkip(){std::abort();}
}
using Microsoft::WRL::ComPtr;
using namespace cvr::framegen;
void check(bool ok,const char* why){if(!ok)throw std::runtime_error(why);}
struct Gpu {
 ComPtr<ID3D12Device> device;ComPtr<ID3D12CommandQueue> queue;ComPtr<ID3D12Fence> fence;
 ComPtr<ID3D12CommandAllocator> allocator;ComPtr<ID3D12GraphicsCommandList> list;ComPtr<ID3D12DescriptorHeap> heap,cpuHeap;
 std::array<ComPtr<ID3D12Resource>,3> images;
 Gpu(){
  ComPtr<ID3D12Debug> debug;check(SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug))),"debug layer missing");debug->EnableDebugLayer();
  ComPtr<IDXGIFactory4> factory;ComPtr<IDXGIAdapter> warp;
  check(SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))&&SUCCEEDED(factory->EnumWarpAdapter(IID_PPV_ARGS(&warp)))&&
      SUCCEEDED(D3D12CreateDevice(warp.Get(),D3D_FEATURE_LEVEL_12_0,IID_PPV_ARGS(&device))),"WARP unavailable");
  D3D12_COMMAND_QUEUE_DESC q{};
  check(SUCCEEDED(device->CreateCommandQueue(&q,IID_PPV_ARGS(&queue)))&&
      SUCCEEDED(device->CreateCommandAllocator(q.Type,IID_PPV_ARGS(&allocator)))&&
      SUCCEEDED(device->CreateCommandList(0,q.Type,allocator.Get(),nullptr,IID_PPV_ARGS(&list)))&&
      SUCCEEDED(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&fence))),"GPU setup failed");
  D3D12_DESCRIPTOR_HEAP_DESC hd{};hd.NumDescriptors=3;hd.Flags=D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
  check(SUCCEEDED(device->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&heap))),"descriptor heap failed");
  hd.Flags=D3D12_DESCRIPTOR_HEAP_FLAG_NONE;check(SUCCEEDED(device->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&cpuHeap))),"CPU descriptor heap failed");
  const auto inc=device->GetDescriptorHandleIncrementSize(hd.Type);
  auto cpu=cpuHeap->GetCPUDescriptorHandleForHeapStart();auto visible=heap->GetCPUDescriptorHandleForHeapStart();auto gpu=heap->GetGPUDescriptorHandleForHeapStart();
  ID3D12DescriptorHeap* heaps[]{heap.Get()};list->SetDescriptorHeaps(1,heaps);
  const DXGI_FORMAT formats[]{DXGI_FORMAT_R8G8B8A8_UNORM,DXGI_FORMAT_R16G16_FLOAT,DXGI_FORMAT_R32_FLOAT};
  for(unsigned i=0;i<3;++i){
   D3D12_RESOURCE_DESC d{};d.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;d.Width=d.Height=8;
   d.DepthOrArraySize=d.MipLevels=1;d.SampleDesc.Count=1;d.Format=formats[i];d.Flags=D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
   D3D12_HEAP_PROPERTIES h{};h.Type=D3D12_HEAP_TYPE_DEFAULT;
   check(SUCCEEDED(device->CreateCommittedResource(&h,D3D12_HEAP_FLAG_NONE,&d,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,nullptr,IID_PPV_ARGS(&images[i]))),"texture failed");
   D3D12_UNORDERED_ACCESS_VIEW_DESC u{};u.Format=formats[i];u.ViewDimension=D3D12_UAV_DIMENSION_TEXTURE2D;
   device->CreateUnorderedAccessView(images[i].Get(),nullptr,&u,cpu);
   device->CopyDescriptorsSimple(1,visible,cpu,D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
   const float values[4]{i==1?0.f:.5f,0,0,1};list->ClearUnorderedAccessViewFloat(gpu,cpu,images[i].Get(),values,0,nullptr);
   D3D12_RESOURCE_BARRIER b{};b.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
   b.Transition={images[i].Get(),D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
     D3D12_RESOURCE_STATES(D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE|D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE|D3D12_RESOURCE_STATE_COPY_SOURCE)};
   list->ResourceBarrier(1,&b);cpu.ptr+=inc;visible.ptr+=inc;gpu.ptr+=inc;
  }
  check(SUCCEEDED(list->Close()),"initial close failed");ID3D12CommandList* lists[]{list.Get()};queue->ExecuteCommandLists(1,lists);
  const auto done=CreateEventW(nullptr,FALSE,FALSE,nullptr);
  check(done&&SUCCEEDED(queue->Signal(fence.Get(),1))&&SUCCEEDED(fence->SetEventOnCompletion(1,done))&&WaitForSingleObject(done,5000)==WAIT_OBJECT_0,"initial fence failed");CloseHandle(done);
 }
 void no_errors(){
  ComPtr<ID3D12InfoQueue> info;check(SUCCEEDED(device.As(&info)),"info queue unavailable");
  unsigned errors{};for(UINT64 i=0;i<info->GetNumStoredMessages();++i){SIZE_T bytes{};info->GetMessage(i,nullptr,&bytes);
   std::vector<char> storage(bytes);auto* m=reinterpret_cast<D3D12_MESSAGE*>(storage.data());info->GetMessage(i,m,&bytes);
   if(m->Severity<=D3D12_MESSAGE_SEVERITY_ERROR){++errors;std::cerr<<m->pDescription<<'\n';}}
  check(!errors,"D3D12 validation error");
 }
};
int main(){try{
 Gpu gpu;cpvr::GenerationWorker worker;std::array<cpvr::GenerationInput,2> history;
 unsigned admitted{};
 for(uint64_t source=1;source<=12;++source){
  auto record=ReusableRecord(0);
  if(!record){std::cerr<<"MAIN InputPoolBusy at source="<<source<<" completed_snapshots="<<admitted<<'\n';throw std::runtime_error("completed private snapshots exhausted the actual four MAIN records");}
  auto& d=record->data;d.motion=gpu.images[1];d.depth=gpu.images[2];d.readyFence=gpu.fence;d.fenceValue=1;
  d.queue=gpu.queue.Get();d.view=1;d.frameId=uint32_t(source);d.poseId=source;d.origin=1;
  d.hasMotion=d.hasDepth=d.submitted=d.evaluated=true;record->expected=record->submitted=7;record->claimedMask=0;
  std::shared_ptr<const Inputs> alias(record,&d);
  auto frame=std::make_shared<cpvr::EyeFrame>();frame->source_id=source;frame->eye=cpvr::Eye((source-1)&1);
  frame->epoch=frame->space_epoch=1;frame->content_time_ns=1'000'000'000+int64_t(source)*10'000'000;frame->render_pose.orientation[3]=1;
  frame->fov={-.8f,.8f,.8f,-.8f};frame->color.texture=gpu.images[0];frame->color.ready_fence=gpu.fence;frame->color.ready_value=1;
  frame->color.resting_state=CaptureColorReadState;frame->color.transfer=cpvr::ColorTransfer::SrgbEncoded;
  auto guides=std::make_shared<cpvr::SourceGuides>();guides->camera_valid=true;
  guides->motion.texture=d.motion;guides->depth.texture=d.depth;
  for(auto* image:{&guides->motion,&guides->depth}){image->ready_fence=gpu.fence;image->ready_value=1;image->resting_state=CaptureColorReadState;image->lifetime=std::const_pointer_cast<Inputs>(alias);}
  for(unsigned base:{0u,16u,32u,48u,64u})for(unsigned k=0;k<16;++k)guides->camera[base+k]=(k%5)==0?1.f:0.f;
  guides->camera[80]=float(source%3)*.125f;guides->camera[81]=-.25f;guides->camera[82]=guides->camera[83]=1;
  guides->camera[102]=65504;guides->camera[103]=40;guides->camera_flags={0,1,0,0,0,0,1};frame->guides=guides;
  cpvr::GenerationInput input;input.frame=frame;input.motion_convention=cpvr::MotionConvention::NativeStreamlineBackwardUvScaled;
  input.native_frame_token=source;input.previous_native_frame_token=source-1;input.guides_previous_source_id=source-1;
  input.guides_previous_time_ns=frame->content_time_ns-10'000'000;
  if(source>2){const auto& m=history[unsigned(frame->eye)^1];input.immediate_previous={m.frame,m.native_frame_token,m.previous_native_frame_token,m.guides_previous_source_id,m.guides_previous_time_ns,m.motion_convention};}
  check(cpvr::GenerationWorkerTestAccess::snapshot_only(worker,gpu.device.Get(),gpu.queue.Get(),input),"production private snapshot failed");
  ++admitted;
  auto retained=cpvr::GenerationWorkerTestAccess::snapshot_history(worker,frame->eye);
  check(retained.frame&&retained.frame->guides&&retained.frame->guides->camera==guides->camera&&retained.frame->guides->camera_flags==guides->camera_flags,"raw normalization metadata changed");
  history[unsigned(frame->eye)]=input;history[unsigned(frame->eye)].immediate_previous={};
 }
 history={};
 for(const auto& record:pools[0])check(!record||record.use_count()==1,"completed idle copy recording still pins a producer record until the next input");
 worker.stop();
 for(const auto& record:pools[0])check(!record||record.use_count()==1,"worker shutdown retains MAIN producer record");
 gpu.no_errors();std::cout<<"PASS actual MAIN4: "<<admitted<<" alternating native delta2 snapshots; raw jitter preserved; D3D12 errors=0; NGX/HMD not exercised\n";return 0;
}catch(const std::exception& e){std::cerr<<"FAIL "<<e.what()<<'\n';return 1;}}
