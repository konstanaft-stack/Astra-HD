#include "Runtimes/HudQuad.hpp"
#include "Render/GpuStageProfile.hpp"
#include "Core/LiveControls.hpp"
#include <dxgi1_6.h>
#include <d3d12sdklayers.h>
#include <chrono>
#include <cstdio>
#include <stdexcept>
#include <iostream>
#include <thread>

using Microsoft::WRL::ComPtr;
using namespace cvr::hud;
LiveControls g_liveControls{};
extern "C" int CyberpunkVR_RuntimeDiagnostics=0;
extern "C" std::atomic<uint32_t> CyberpunkVR_HudSubmitRing;
void Log(const char*,...) {}
void Check(bool value,const char* message) {if(!value)throw std::runtime_error(message);}
void Hr(HRESULT value) {Check(SUCCEEDED(value),"D3D12 call failed");}
namespace cvr::gpu::profile {
BatchPtr Begin(ID3D12GraphicsCommandList*) {return {};}
Scope::Scope(const BatchPtr&,Stage) {}
Scope::~Scope() {}
void Submitted(BatchPtr,ID3D12CommandQueue*,ID3D12Fence*,UINT64) {}
}
namespace mock {
ID3D12Device* device{};
std::array<ComPtr<ID3D12Resource>,3> textures;
std::shared_ptr<Frame> source;
bool ready{},acquired{},waited{},timeout{};
uint32_t nextImage{},image{},acquires{},releases{},destroys{};
XrDuration lastTimeout{};
}
namespace cvr::hud {
std::shared_ptr<Frame> Latest(Channel) {return mock::source;}
void SetConsumerReady(bool ready,Channel) {mock::ready=ready;}
}
extern "C" {
XrResult XRAPI_CALL xrEnumerateSwapchainFormats(XrSession,uint32_t capacity,uint32_t* count,int64_t* values) {
    const int64_t formats[]={DXGI_FORMAT_R8G8B8A8_UNORM_SRGB,DXGI_FORMAT_R8G8B8A8_UNORM,DXGI_FORMAT_B8G8R8A8_UNORM,DXGI_FORMAT_R16G16B16A16_FLOAT};
    *count=4;if(capacity)std::copy(std::begin(formats),std::end(formats),values);return XR_SUCCESS;
}
XrResult XRAPI_CALL xrCreateSwapchain(XrSession,const XrSwapchainCreateInfo* ci,XrSwapchain* out) {
    D3D12_HEAP_PROPERTIES heap{};heap.Type=D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC desc{};desc.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width=ci->width;desc.Height=ci->height;desc.Format=static_cast<DXGI_FORMAT>(ci->format);
    desc.DepthOrArraySize=desc.MipLevels=1;desc.SampleDesc.Count=1;desc.Flags=D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    for(auto& texture:mock::textures)Hr(mock::device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&desc,
        D3D12_RESOURCE_STATE_RENDER_TARGET,nullptr,IID_PPV_ARGS(&texture)));
    *out=reinterpret_cast<XrSwapchain>(1);mock::nextImage=0;return XR_SUCCESS;
}
XrResult XRAPI_CALL xrEnumerateSwapchainImages(XrSwapchain,uint32_t capacity,uint32_t* count,XrSwapchainImageBaseHeader* out) {
    *count=3;if(capacity)for(unsigned i=0;i<3;++i)reinterpret_cast<XrSwapchainImageD3D12KHR*>(out)[i].texture=mock::textures[i].Get();return XR_SUCCESS;
}
XrResult XRAPI_CALL xrAcquireSwapchainImage(XrSwapchain,const XrSwapchainImageAcquireInfo*,uint32_t* image) {
    Check(!mock::acquired,"duplicate acquisition");mock::acquired=true;mock::waited=false;
    *image=mock::image=mock::nextImage++%3;++mock::acquires;return XR_SUCCESS;
}
XrResult XRAPI_CALL xrWaitSwapchainImage(XrSwapchain,const XrSwapchainImageWaitInfo* info) {
    Check(mock::acquired,"wait without acquire");mock::lastTimeout=info->timeout;
    if(mock::timeout)return XR_TIMEOUT_EXPIRED;mock::waited=true;return XR_SUCCESS;
}
XrResult XRAPI_CALL xrReleaseSwapchainImage(XrSwapchain,const XrSwapchainImageReleaseInfo*) {
    Check(mock::acquired && mock::waited,"unwaited image released");mock::acquired=false;++mock::releases;return XR_SUCCESS;
}
XrResult XRAPI_CALL xrDestroySwapchain(XrSwapchain) {
    Check(!mock::acquired,"acquired image destroyed");++mock::destroys;
    for(auto& texture:mock::textures)texture.Reset();return XR_SUCCESS;
}
}
int main() try {
    ComPtr<ID3D12Debug> debug;if(SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug))))debug->EnableDebugLayer();
    ComPtr<IDXGIFactory4> factory;Hr(CreateDXGIFactory1(IID_PPV_ARGS(&factory)));
    ComPtr<IDXGIAdapter> adapter;Hr(factory->EnumWarpAdapter(IID_PPV_ARGS(&adapter)));
    ComPtr<ID3D12Device> device;Hr(D3D12CreateDevice(adapter.Get(),D3D_FEATURE_LEVEL_11_0,IID_PPV_ARGS(&device)));mock::device=device.Get();
    ComPtr<ID3D12CommandQueue> queue;D3D12_COMMAND_QUEUE_DESC qd{};Hr(device->CreateCommandQueue(&qd,IID_PPV_ARGS(&queue)));
    ComPtr<ID3D12Fence> done,gate;Hr(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&done)));
    Hr(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&gate)));uint64_t doneValue{};
    auto drain=[&] {
        Hr(queue->Signal(done.Get(),++doneValue));HANDLE event=CreateEventW(nullptr,FALSE,FALSE,nullptr);
        Hr(done->SetEventOnCompletion(doneValue,event));auto status=WaitForSingleObject(event,5000);CloseHandle(event);Check(status==WAIT_OBJECT_0,"GPU timeout");
    };
    auto frame=[&](uint64_t serial,unsigned size=32,DXGI_FORMAT format=DXGI_FORMAT_R8G8B8A8_UNORM,
                   D3D12_RESOURCE_STATES state=D3D12_RESOURCE_STATE_COPY_SOURCE) {
        auto f=std::make_shared<Frame>();f->serial=serial;f->stamp=GetTickCount64();f->masked=true;
        D3D12_HEAP_PROPERTIES heap{};heap.Type=D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC desc{};desc.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;desc.Width=desc.Height=size;
        desc.DepthOrArraySize=desc.MipLevels=1;desc.Format=format;desc.SampleDesc.Count=1;
        if(state==D3D12_RESOURCE_STATE_RENDER_TARGET)desc.Flags=D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        f->restingState=state;
        Hr(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&desc,state,nullptr,IID_PPV_ARGS(&f->canvas)));
        return f;
    };
    g_liveControls.xrHudStereoDepth=1;g_liveControls.xrHudFollowMode=0;
    XrPosef head{{0,0,0,1},{0,1.7f,0}};XrTime clock=1000000000;
    Quad quad(Channel::Interaction);
    auto prepare=[&](bool enabled=true) {
        clock+=11111111;
        const auto begin=std::chrono::steady_clock::now();
        auto result=quad.Prepare(reinterpret_cast<XrSession>(1),reinterpret_cast<XrSpace>(1),device.Get(),queue.Get(),enabled,true,head,1,clock);
        if(CyberpunkVR_HudSubmitRing)Check(std::chrono::steady_clock::now()-begin<std::chrono::milliseconds(80),"pipelined prepare blocked");
        return reinterpret_cast<const XrCompositionLayerQuad*>(result);
    };
    // All three pending sources must survive until their own copy completes.
    Hr(queue->Wait(gate.Get(),1));
    std::array<std::weak_ptr<Frame>,3> old;
    for(unsigned i=0;i<3;++i){mock::source=frame(i+1);old[i]=mock::source;Check(prepare()!=nullptr,"free slot unavailable");}
    Check(mock::acquires==3 && mock::releases==3,"ring did not submit three copies");
    mock::source=frame(4);mock::source->masked=false;mock::source->lootVisible=true;
    // Last published image was masked, not loot. Busy fallback must use that
    // metadata, and continue head follow without a new acquisition.
    head.orientation={0,.3f,0,.9539392f};
    Check(prepare()!=nullptr && mock::ready,"busy fallback hid the prior image");
    Check(mock::acquires==3,"busy ring acquired another image");
    for(auto& f:old)Check(!f.expired(),"in-flight HUD source released early");
    Hr(gate->Signal(1));drain();
    Check(prepare()==nullptr && mock::ready,"first unmasked publication handshake changed");
    for(auto& f:old)Check(f.expired(),"completed sources were not retired");
    drain();mock::source=frame(5);Check(prepare()!=nullptr,"new masked publication missing");drain();
    const auto reused=mock::acquires;Check(prepare()!=nullptr && mock::acquires==reused,"same serial copied twice");

    // A nonblocking XR timeout keeps the acquisition and prior published image.
    mock::source=frame(6);mock::timeout=true;
    Check(prepare()!=nullptr && mock::acquired && mock::lastTimeout==0,"XR timeout fallback failed");
    auto pendingAcquires=mock::acquires,pendingReleases=mock::releases;
    Check(prepare()!=nullptr && mock::acquires==pendingAcquires && mock::releases==pendingReleases,"timeout duplicated acquisition/release");
    mock::timeout=false;Check(prepare()!=nullptr && !mock::acquired,"acquisition was not resumed");drain();
    Check(prepare(false)==nullptr && !mock::ready,"disable did not suppress the layer");
    mock::source=frame(7);Check(prepare()!=nullptr,"re-enable failed");drain();

    // Resize with a pending acquisition must wait/release it before destruction.
    mock::source=frame(8);mock::timeout=true;Check(prepare()!=nullptr && mock::acquired,"pending resize setup");
    mock::source=frame(9,64);const auto destroyedBefore=mock::destroys;
    Check(prepare()==nullptr && mock::destroys==destroyedBefore,"unready image destroyed on resize");
    mock::timeout=false;Check(prepare()!=nullptr && mock::destroys==destroyedBefore+1,"resize recovery failed");drain();
    mock::source=frame(10,64);CyberpunkVR_HudSubmitRing=0;
    mock::ready=true;
    const XrExtent2Df explicitSize{2,2};
    Check(!quad.PrepareExplicit(reinterpret_cast<XrSession>(1),reinterpret_cast<XrSpace>(1),
          device.Get(),queue.Get(),mock::source,head,explicitSize,3),"HUD instance admitted explicit preparation");
    Check(mock::ready,"wrong-mode explicit call changed HUD consumer readiness");
    Check(prepare()!=nullptr,"wrong-mode call disabled subsequent ordinary HUD preparation");drain();
    CyberpunkVR_HudSubmitRing=1;mock::source.reset();Check(prepare()==nullptr && !mock::ready,"missing source retained HUD");
    quad.Shutdown();Check(!mock::acquired && mock::acquires==mock::releases,"unbalanced XR lifecycle");

    {
        mock::ready=true;
        {Quad unusedEditor(Quad::Mode::Explicit);unusedEditor.Shutdown();}
        Check(mock::ready,"unused explicit instance changed main HUD consumer readiness");
        Quad editor(Quad::Mode::Explicit);
        const auto session=reinterpret_cast<XrSession>(1);
        const auto space=reinterpret_cast<XrSpace>(2);
        XrPosef pose{{0,0,0,1},{0,1.6f,-1}};XrExtent2Df size{2.5139f,2.8f};
        auto source=frame(100);source->masked=false;
        auto owner=std::make_shared<int>(42);std::weak_ptr<int> held=owner;
        source->retainedOwner=owner;source->producerFence=gate;source->producerValue=3;
        auto explicitPrepare=[&](const std::shared_ptr<Frame>& f) {
            return reinterpret_cast<const XrCompositionLayerQuad*>(editor.PrepareExplicit(session,space,device.Get(),queue.Get(),f,pose,size,3));
        };
        mock::ready=true;
        auto invalid=std::make_shared<Frame>(*source);invalid->retainedOwner.reset();
        const auto before=mock::acquires;
        Check(!explicitPrepare(invalid) && mock::acquires==before,"unowned external source admitted");invalid.reset();
        source->producerValue=UINT64_MAX;Check(!explicitPrepare(source),"sentinel producer value admitted");source->producerValue=3;
        pose.orientation.w=0;Check(!explicitPrepare(source),"invalid pose admitted");pose.orientation.w=1;
        auto layer=explicitPrepare(source);
        Check(layer && layer->space==space && layer->eyeVisibility==XR_EYE_VISIBILITY_BOTH &&
              !editor.SecondEye() && layer->layerFlags==3 && mock::ready,"explicit layer changed HUD handshake or eye policy");
        Check(!memcmp(&layer->pose,&pose,sizeof(pose)) && !memcmp(&layer->size,&size,sizeof(size)),"explicit placement changed");
        // This queue is blocked ONLY by the external producer dependency.
        Hr(queue->Signal(done.Get(),++doneValue));Sleep(20);
        Check(done->GetCompletedValue()<doneValue,"external producer fence was not queued before copy");
        owner.reset();source.reset();Check(!held.expired(),"producer owner dropped before copy completion");
        g_liveControls.xrHudPanel=0;g_liveControls.xrHudFollowMode=2;g_liveControls.xrHudDistance=5;
        Hr(gate->Signal(3));drain();
        explicitPrepare({});Check(held.expired(),"completed explicit copy retained producer owner");
        source=frame(101);source->retainedOwner=std::make_shared<int>(7);source->producerFence=gate;source->producerValue=3;
        layer=explicitPrepare(source);
        Check(layer && !memcmp(&layer->pose,&pose,sizeof(pose)) && !memcmp(&layer->size,&size,sizeof(size)),"world-fixed pose followed HUD/category controls");
        drain();
        ComPtr<ID3D12CommandQueue> otherQueue;Hr(device->CreateCommandQueue(&qd,IID_PPV_ARGS(&otherQueue)));
        Check(!editor.PrepareExplicit(session,space,device.Get(),otherQueue.Get(),source,pose,size,3),"different copy queue admitted");
        Check(!editor.PrepareExplicit(reinterpret_cast<XrSession>(2),space,device.Get(),queue.Get(),source,pose,size,3),"different session admitted");
        for(auto format:{DXGI_FORMAT_B8G8R8A8_UNORM,DXGI_FORMAT_R16G16B16A16_FLOAT}) {
            source=frame(++clock,32,format,D3D12_RESOURCE_STATE_RENDER_TARGET);
            source->retainedOwner=std::make_shared<int>(8);source->producerFence=gate;source->producerValue=3;
            Check(explicitPrepare(source)!=nullptr,"supported format/state copy rejected");drain();
        }
        source=frame(++clock,32,DXGI_FORMAT_R32_FLOAT);source->retainedOwner=std::make_shared<int>(9);
        source->producerFence=gate;source->producerValue=3;
        const auto acquiredAtUnsupported=mock::acquires;
        Check(!explicitPrepare(source) && mock::acquires==acquiredAtUnsupported,"unsupported source format was reinterpreted");
        drain();editor.Shutdown();
        g_liveControls.xrHudPanel=1;
    }
    ComPtr<ID3D12InfoQueue> info;
    if(SUCCEEDED(device.As(&info)))for(UINT64 i=0;i<info->GetNumStoredMessages();++i) {
        SIZE_T bytes{};info->GetMessage(i,nullptr,&bytes);std::vector<uint8_t> storage(bytes);auto* message=reinterpret_cast<D3D12_MESSAGE*>(storage.data());
        info->GetMessage(i,message,&bytes);
        if(message->Severity<=D3D12_MESSAGE_SEVERITY_ERROR){std::cerr<<message->pDescription<<'\n';throw std::runtime_error("D3D12 validation failed");}
    }
    // A removed-device sentinel does not prove a queued copy completed.
    Hr(queue->Wait(gate.Get(),4));
    std::weak_ptr<Frame> pending;
    std::weak_ptr<int> pendingOwner;
    const auto destroyedAtRemoval=mock::destroys;
    {
        Quad removed(Channel::Interaction);
        mock::source=frame(11);pending=mock::source;
        auto owner=std::make_shared<int>(11);pendingOwner=owner;mock::source->retainedOwner=owner;owner.reset();
        Check(removed.Prepare(reinterpret_cast<XrSession>(1),reinterpret_cast<XrSpace>(1),device.Get(),queue.Get(),true,true,head,1,clock)!=nullptr,"removal setup failed");
        mock::source.reset();
        ComPtr<ID3D12Device5> removal;Hr(device.As(&removal));removal->RemoveDevice();
        Check(done->GetCompletedValue()==UINT64_MAX,"RemoveDevice did not produce sentinel");
        removed.Prepare(reinterpret_cast<XrSession>(1),reinterpret_cast<XrSpace>(1),device.Get(),queue.Get(),false,true,head,1,clock);
        Check(!pending.expired(),"removed-device sentinel retired a pending HUD source");
        removed.Shutdown();
    }
    Check(!pending.expired() && !pendingOwner.expired() && mock::destroys==destroyedAtRemoval,"unsafe HUD teardown released pending copy ownership");
    std::cout<<"PASS healthy HUD contracts; explicit pose/input/format/state/producer wait/owner lifetime; removed-device retention through disable/shutdown/destructor\n";
    return 0;
} catch(const std::exception& e) {std::cerr<<e.what()<<'\n';return 1;}
