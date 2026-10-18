#include "Runtimes/EditorSceneCapturePolicy.hpp"
#include <dxgi1_6.h>
#include <d3d12sdklayers.h>
#include <cstdio>
#include <stdexcept>
#include <vector>
#include <atomic>
using Microsoft::WRL::ComPtr;
using namespace cvr::menu_preview;
using namespace cvr::render;
void Log(const char*,...){}
static void Check(bool v,const char* m){if(!v)throw std::runtime_error(m);}
static void Hr(HRESULT h){Check(SUCCEEDED(h),"D3D12 failure");}
struct Token final:IUnknown{
 std::atomic<ULONG> refs{1};std::shared_ptr<int> owner;
 explicit Token(std::shared_ptr<int> o):owner(std::move(o)){}
 HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id,void** out)override{if(!out)return E_POINTER;*out=nullptr;if(id!=__uuidof(IUnknown))return E_NOINTERFACE;*out=this;AddRef();return S_OK;}
 ULONG STDMETHODCALLTYPE AddRef()override{return ++refs;}
 ULONG STDMETHODCALLTYPE Release()override{const auto n=--refs;if(!n)delete this;return n;}
};
static ComPtr<ID3D12Resource> Texture(ID3D12Device* d,UINT w,UINT h,DXGI_FORMAT format,D3D12_RESOURCE_STATES state){
 D3D12_HEAP_PROPERTIES hp{};hp.Type=D3D12_HEAP_TYPE_DEFAULT;D3D12_RESOURCE_DESC rd{};
 rd.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;rd.Width=w;rd.Height=h;rd.DepthOrArraySize=rd.MipLevels=1;
 rd.Format=format;rd.SampleDesc.Count=1;rd.Flags=D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
 ComPtr<ID3D12Resource> r;Hr(d->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&rd,state,nullptr,IID_PPV_ARGS(&r)));return r;
}
static ComPtr<ID3D12Resource> Buffer(ID3D12Device* d,UINT64 bytes,D3D12_HEAP_TYPE type){
 D3D12_HEAP_PROPERTIES hp{};hp.Type=type;D3D12_RESOURCE_DESC rd{};rd.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;
 rd.Width=bytes;rd.Height=1;rd.DepthOrArraySize=rd.MipLevels=1;rd.SampleDesc.Count=1;rd.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
 ComPtr<ID3D12Resource> r;Hr(d->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&rd,type==D3D12_HEAP_TYPE_UPLOAD?D3D12_RESOURCE_STATE_GENERIC_READ:D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&r)));return r;
}
static void Barrier(ID3D12GraphicsCommandList* c,ID3D12Resource* r,D3D12_RESOURCE_STATES a,D3D12_RESOURCE_STATES b){
 D3D12_RESOURCE_BARRIER v{};v.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;v.Transition={r,D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,a,b};c->ResourceBarrier(1,&v);
}
static void Complete(ID3D12Fence* f,UINT64 value){HANDLE e=CreateEventW(nullptr,FALSE,FALSE,nullptr);Check(e!=nullptr,"event");
 Hr(f->SetEventOnCompletion(value,e));const auto wait=WaitForSingleObject(e,15000);CloseHandle(e);Check(wait==WAIT_OBJECT_0&&f->GetCompletedValue()!=UINT64_MAX,"WARP completion");}
struct Footprint {D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};UINT64 bytes{};};
static Footprint Layout(ID3D12Device* d,ID3D12Resource* r){Footprint out;const auto desc=r->GetDesc();d->GetCopyableFootprints(&desc,0,1,0,&out.fp,nullptr,nullptr,&out.bytes);return out;}
static ComPtr<ID3D12Resource> Upload(ID3D12Device* d,ID3D12GraphicsCommandList* c,ID3D12Resource* r,bool green){
 const auto f=Layout(d,r);auto upload=Buffer(d,f.bytes,D3D12_HEAP_TYPE_UPLOAD);uint8_t* p{};Hr(upload->Map(0,nullptr,reinterpret_cast<void**>(&p)));
 for(UINT y=0;y<f.fp.Footprint.Height;++y)for(UINT x=0;x<f.fp.Footprint.Width;++x){auto* q=p+y*f.fp.Footprint.RowPitch+x*4;
  const bool left=x<f.fp.Footprint.Width/2,top=y<f.fp.Footprint.Height/2;
  const uint8_t colors[4][4]={{255,0,0,255},{128,128,128,255},{0,0,255,255},{255,255,255,255}};
  if(green){q[0]=0;q[1]=255;q[2]=0;q[3]=255;}else std::memcpy(q,colors[(top?0:2)+(left?0:1)],4);}
 upload->Unmap(0,nullptr);D3D12_TEXTURE_COPY_LOCATION src{},dst{};src.pResource=upload.Get();src.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;src.PlacedFootprint=f.fp;
 dst.pResource=r;dst.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;c->CopyTextureRegion(&dst,0,0,0,&src,nullptr);
 Barrier(c,r,D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);return upload;
}
int main()try{
 ComPtr<ID3D12Debug> debug;Hr(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)));debug->EnableDebugLayer();
 ComPtr<IDXGIFactory4> factory;Hr(CreateDXGIFactory1(IID_PPV_ARGS(&factory)));ComPtr<IDXGIAdapter> adapter;Hr(factory->EnumWarpAdapter(IID_PPV_ARGS(&adapter)));
 ComPtr<ID3D12Device> device;Hr(D3D12CreateDevice(adapter.Get(),D3D_FEATURE_LEVEL_11_0,IID_PPV_ARGS(&device)));
 ComPtr<ID3D12CommandQueue> queue;D3D12_COMMAND_QUEUE_DESC qd{};Hr(device->CreateCommandQueue(&qd,IID_PPV_ARGS(&queue)));
 ComPtr<ID3D12CommandAllocator> allocator;ComPtr<ID3D12GraphicsCommandList> list;Hr(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&allocator)));
 Hr(device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,allocator.Get(),nullptr,IID_PPV_ARGS(&list)));
 ComPtr<ID3D12Fence> done,gate;Hr(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&done)));Hr(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&gate)));
 auto raw=Texture(device.Get(),1920,1440,DXGI_FORMAT_R8G8B8A8_UNORM_SRGB,D3D12_RESOURCE_STATE_COPY_DEST);
 auto bb=Texture(device.Get(),1920,1920,DXGI_FORMAT_R8G8B8A8_UNORM,D3D12_RESOURCE_STATE_COPY_DEST);
 auto upRaw=Upload(device.Get(),list.Get(),raw.Get(),false),upBB=Upload(device.Get(),list.Get(),bb.Get(),true);
 Hr(list->Close());ID3D12CommandList* lists[]{list.Get()};queue->ExecuteCommandLists(1,lists);Hr(queue->Signal(done.Get(),1));Complete(done.Get(),1);
 Hr(allocator->Reset());Hr(list->Reset(allocator.Get(),nullptr));
 auto app=std::make_shared<CameraApplication>();app->metadata.screen=2;app->metadata.screenEpoch=3;app->metadata.setupID=6;
 app->metadata.controllerIdentity=100;app->metadata.componentIdentity=200;app->metadata.virtualCameraNameHash=300;const auto now=PublicationNowMs();app->metadata.publishedAtMs=now;
 app->frame.key={3,6,200,300};app->frame.eye=1;app->frame.nativeFrame=50;app->frame.sourceSequence=60;app->localPoseRevision=70;
 app->editorControlRevision=1;
 app->projection.valid=true;app->projection.origin=2;app->projection.fov={-.7f,.7f,.7f,-.7f};
 UiFrameReceipt receipt;receipt.scope=app->metadata;receipt.eye=1;receipt.nativeFrame=50;receipt.sourceSequence=60;receipt.cameraRevision=70;receipt.uiSubmission=20;receipt.projection=app->projection;
 auto scene=std::make_shared<EditorSceneImage>();scene->application=app;scene->pixels.texture=raw;scene->producerFence=gate;scene->producerValue=1;scene->producerUiSubmission=10;scene->serial=80;scene->queue=uintptr_t(queue.Get());
 auto marker=std::make_shared<int>(1);std::weak_ptr<int> retained=marker;scene->pixels.lifetime.Attach(new Token(marker));marker.reset();
 Check(cvr::xr::SelectEditorSceneCapture(false,bb.Get(),receipt,{},now).texture==bb.Get(),"legacy BB path changed");
 Check(!cvr::xr::SelectEditorSceneCapture(true,bb.Get(),receipt,{},now).texture,"missing required raw fell back to BB");
 auto wrong=receipt;wrong.sourceSequence++;Check(!cvr::xr::SelectEditorSceneCapture(true,bb.Get(),wrong,scene,now).texture,"mismatched raw admitted");
 scene->producerUiSubmission=21;Check(!cvr::xr::SelectEditorSceneCapture(true,bb.Get(),receipt,scene,now).texture,"future submission admitted");scene->producerUiSubmission=10;
 Check(!cvr::xr::SelectEditorSceneCapture(true,bb.Get(),receipt,scene,now+501).texture,"stale application admitted");
 app->editorControlRevision=0;Check(!cvr::xr::SelectEditorSceneCapture(true,bb.Get(),receipt,scene,now).texture,"missing editor control revision admitted");app->editorControlRevision=1;
 wrong=receipt;wrong.scope.screenEpoch++;Check(!cvr::xr::SelectEditorSceneCapture(true,bb.Get(),wrong,scene,now).texture,"old editor scope admitted");
 wrong=receipt;wrong.projection.fov.angleLeft-=.1f;Check(!cvr::xr::SelectEditorSceneCapture(true,bb.Get(),wrong,scene,now).texture,"different projection admitted");
 {auto missingToken=std::make_shared<EditorSceneImage>(*scene);missingToken->pixels.lifetime.Reset();Check(!cvr::xr::SelectEditorSceneCapture(true,bb.Get(),receipt,missingToken,now).texture,"missing cell token admitted");}
 auto selected=cvr::xr::SelectEditorSceneCapture(true,bb.Get(),receipt,scene,now);Check(selected.texture==raw.Get(),"required selection is not raw");
#ifdef CPVR_TEST_BACKBUFFER_SOURCE
 selected.texture=bb.Get(); // Prior consumer selected the green BB, not the native raw eye.
#endif
#ifdef CPVR_TEST_MAIN_FP16
 const auto mainFormat=DXGI_FORMAT_R16G16B16A16_FLOAT;
#else
 const auto mainFormat=DXGI_FORMAT_R8G8B8A8_UNORM;
#endif
 auto main=Texture(device.Get(),1920,1920,mainFormat,cvr::framegen::CaptureColorReadState);
 auto eye=Texture(device.Get(),1782,1920,DXGI_FORMAT_R8G8B8A8_UNORM,D3D12_RESOURCE_STATE_RENDER_TARGET);
 AerReconstructionPass capture,output;Check(capture.Initialize(device.Get())&&output.Initialize(device.Get()),"pass initialization");
 Check(capture.Prepare(selected.texture->GetDesc().Format,main->GetDesc().Format,cvr::xr::EditorSceneColorRoute(main->GetDesc().Format)),"capture prepare");
 const auto first=cvr::xr::RecordEditorSceneCapture(capture,list.Get(),selected,main.Get(),done.Get(),2);Check(bool(first),"capture record");
 const AerColorRoute route{cvr::xr::EditorSceneColorRoute(main->GetDesc().Format).output,AerEncoding::EncodedSrgb};
 Check(output.Prepare(main->GetDesc().Format,eye->GetDesc().Format,route),"eye prepare");
 AerRecordRequest request;request.source=main.Get();request.destination=eye.Get();request.sourceOwner=std::shared_ptr<void>(scene,main.Get());request.sourceState=cvr::framegen::CaptureColorReadState;
 request.completion={done.Get(),2};request.warp.crop={0,0,1920,1920};request.warp.sourceEpoch=request.warp.referenceEpoch=3;request.warp.sourceSpace=request.warp.referenceSpace=2;request.filter={0,0,0,0};request.color=route;
 const auto second=output.Record(list.Get(),request);Check(bool(second),"eye record");request.sourceOwner.reset();
 const auto f=Layout(device.Get(),eye.Get());auto readback=Buffer(device.Get(),f.bytes,D3D12_HEAP_TYPE_READBACK);
 Barrier(list.Get(),eye.Get(),D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_COPY_SOURCE);
 D3D12_TEXTURE_COPY_LOCATION src{},dst{};src.pResource=eye.Get();src.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;dst.pResource=readback.Get();dst.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;dst.PlacedFootprint=f.fp;list->CopyTextureRegion(&dst,0,0,0,&src,nullptr);
 Barrier(list.Get(),eye.Get(),D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_RENDER_TARGET);Hr(list->Close());
 auto resources=cvr::gpu::PrepareCommandResources(1,lists);Hr(queue->Wait(gate.Get(),1));queue->ExecuteCommandLists(1,lists);Hr(queue->Signal(done.Get(),2));
 capture.SubmissionReported(first,true,true);output.SubmissionReported(second,true,true);cvr::gpu::SubmitCommandResources(queue.Get(),std::move(resources));
 cvr::gpu::ResetCommandResources(list.Get());selected.scene.reset();scene.reset();Check(!retained.expired(),"source cell token lost before late copy");
 Hr(gate->Signal(1));Hr(queue->Signal(done.Get(),3));Complete(done.Get(),3);capture.Collect();output.Collect();cvr::gpu::CollectCommandResources();Check(retained.expired(),"source cell token not retired after GPU completion");
 uint8_t* bytes{};Hr(readback->Map(0,nullptr,reinterpret_cast<void**>(&bytes)));
 const uint8_t expected[4][3]={{255,0,0},{128,128,128},{0,0,255},{255,255,255}};
 for(unsigned y=0;y<2;++y)for(unsigned x=0;x<2;++x){const auto* p=bytes+(480+y*960)*f.fp.Footprint.RowPitch+(445+x*891)*4;
  for(unsigned c=0;c<3;++c)Check(std::abs(int(p[c])-int(expected[y*2+x][c]))<=2,"raw quadrants/midgray128 lost; BB or crop/encoding selected");}
 readback->Unmap(0,nullptr);Check(capture.TryShutdown()&&output.TryShutdown(),"pass retirement");
 ComPtr<ID3D12InfoQueue> info;Hr(device.As(&info));for(UINT64 i=0;i<info->GetNumStoredMessages();++i){SIZE_T size{};info->GetMessage(i,nullptr,&size);std::vector<uint8_t> storage(size);auto* m=reinterpret_cast<D3D12_MESSAGE*>(storage.data());Hr(info->GetMessage(i,m,&size));if(m->Severity<=D3D12_MESSAGE_SEVERITY_ERROR){std::fprintf(stderr,"%s\n",m->pDescription);Check(false,"D3D12 validation error");}}
 std::puts("PASS WARP raw-vs-green-BB, strict receipt gates, full1920x1440->1920sq->1782x1920/midgray128 and late-copy token lifetime");return 0;
}catch(const std::exception& e){std::fprintf(stderr,"FAIL %s\n",e.what());return 1;}
