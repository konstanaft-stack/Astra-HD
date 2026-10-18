#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <d3d12sdklayers.h>
#include <wrl/client.h>
#include <atomic>
#include <iostream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>
#include "Runtimes/CaptureBufferLeases.hpp"
#include "submit_config.inc"
#if HAS_SUBMIT_TRACKER
#include "Runtimes/SubmitFenceRetirement.hpp"
#endif
using Microsoft::WRL::ComPtr;
static unsigned checks{}, failures{};
static void Check(bool ok,const char* message) {
    ++checks;if(!ok){++failures;std::cout<<"FAIL "<<message<<'\n';}
}
static void Require(bool ok,const char* message) {if(!ok)throw std::runtime_error(message);}
static void Log(const char*,...) {}
static unsigned destroyedSwapchains{};
using XrSwapchain=uint64_t;
static constexpr XrSwapchain XR_NULL_HANDLE=0;
static void xrDestroySwapchain(XrSwapchain) {++destroyedSwapchains;}
struct SrgbToLinearPass {bool EnsureInitialized(ID3D12Device*,DXGI_FORMAT,uint32_t,uint32_t){return true;}};
struct OpenXRManager {
    ID3D12Device* m_d3dDevice{};
    ID3D12Fence* m_fence{};HANDLE m_fenceEvent{};uint64_t m_fenceValue{};
    std::mutex m_presentMutex;cvr::capture::BufferLeases m_captureLeases;
#if HAS_SUBMIT_TRACKER
    cvr::capture::SubmitFenceRetirement m_submitFenceRetirement;
#endif
#include "submit_retired.inc"
    std::vector<RetiredCaptureRead> m_retiredCaptureReads;
    struct Eye {XrSwapchain handle{},depthHandle{};};std::vector<Eye> m_eyeSwapchains;
    bool m_lastGoodValid{};ComPtr<ID3D12Resource> m_lastGoodEye[2];bool m_lastGoodEyeInited[2]{};
    ID3D12GraphicsCommandList* m_cmdLists[3]{};ID3D12CommandAllocator* m_cmdAllocators[3]{};
    std::unique_ptr<SrgbToLinearPass> m_srgbToLinearPass;bool m_decodeSrgbForUnormSwapchain{};
    void ReleaseCapturedRead(ID3D12Resource*,uint64_t);
#if HAS_SUBMIT_TRACKER
    void CollectCapturedReadsLocked(std::vector<ID3D12Resource*>&);
    bool PollSubmitResources(bool);
    void RecordSubmitExecuted(bool);
    bool CreateSubmitFence();
#else
    bool PollSubmitResources(bool){return true;}
    void RecordSubmitExecuted(bool){}
#endif
    bool Teardown() {
        struct {bool decodeSrgbToLinear{};DXGI_FORMAT format=DXGI_FORMAT_R8G8B8A8_UNORM;} formatSelection;
        uint32_t width=8,height=8;
#include "submit_teardown.inc"
        return true;
    }
    bool Create() {
#include "submit_create.inc"
        return true;
    }
};
#include "submit_methods.inc"

static constexpr GUID WitnessKey={0x18a8a0b2,0xa318,0x43a4,{0xa7,0xc2,0xd6,0x53,0x85,0x1c,0x9c,0x4a}};
struct Witness final:IUnknown {
    std::atomic<ULONG> refs{1};std::shared_ptr<std::atomic<unsigned>> deaths;
    explicit Witness(std::shared_ptr<std::atomic<unsigned>> d):deaths(std::move(d)){}
    ~Witness(){++*deaths;}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id,void** out) override {
        if(!out)return E_POINTER;*out=nullptr;if(id!=__uuidof(IUnknown))return E_NOINTERFACE;
        *out=static_cast<IUnknown*>(this);AddRef();return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override{return ++refs;}
    ULONG STDMETHODCALLTYPE Release() override{auto n=--refs;if(!n)delete this;return n;}
};
struct Context {
    ComPtr<ID3D12Device> device;ComPtr<ID3D12CommandQueue> queue;ComPtr<ID3D12InfoQueue> info;
    ComPtr<ID3D12CommandAllocator> allocator;ComPtr<ID3D12GraphicsCommandList> list;
    ComPtr<ID3D12Resource> upload;
    Context() {
        ComPtr<ID3D12Debug> debug;Require(SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug))),"NOT_RUN debug layer unavailable");debug->EnableDebugLayer();
        ComPtr<IDXGIFactory4> factory;ComPtr<IDXGIAdapter1> adapter;DXGI_ADAPTER_DESC1 desc{};
        Require(SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))&&SUCCEEDED(factory->EnumWarpAdapter(IID_PPV_ARGS(&adapter)))&&
            SUCCEEDED(adapter->GetDesc1(&desc))&&(desc.Flags&DXGI_ADAPTER_FLAG_SOFTWARE),"WARP required");
        Require(SUCCEEDED(D3D12CreateDevice(adapter.Get(),D3D_FEATURE_LEVEL_11_0,IID_PPV_ARGS(&device))),"WARP device failed");
        D3D12_COMMAND_QUEUE_DESC q{};q.Type=D3D12_COMMAND_LIST_TYPE_DIRECT;
        Require(SUCCEEDED(device->CreateCommandQueue(&q,IID_PPV_ARGS(&queue)))&&SUCCEEDED(device.As(&info)),"queue/info failed");
        Require(SUCCEEDED(device->CreateCommandAllocator(q.Type,IID_PPV_ARGS(&allocator)))&&
            SUCCEEDED(device->CreateCommandList(0,q.Type,allocator.Get(),nullptr,IID_PPV_ARGS(&list))),"commands failed");
        upload=Buffer(D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_STATE_GENERIC_READ);
        void* bytes{};Require(SUCCEEDED(upload->Map(0,nullptr,&bytes)),"map failed");memset(bytes,0x5a,256);upload->Unmap(0,nullptr);
    }
    ComPtr<ID3D12Resource> Buffer(D3D12_HEAP_TYPE type,D3D12_RESOURCE_STATES state) {
        D3D12_HEAP_PROPERTIES heap{};heap.Type=type;D3D12_RESOURCE_DESC d{};
        d.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;d.Width=256;d.Height=d.DepthOrArraySize=d.MipLevels=d.SampleDesc.Count=1;d.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        ComPtr<ID3D12Resource> result;Require(SUCCEEDED(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&d,state,nullptr,IID_PPV_ARGS(&result))),"buffer failed");return result;
    }
    ComPtr<ID3D12Fence> Fence(uint64_t initial=0) {
        ComPtr<ID3D12Fence> f;Require(SUCCEEDED(device->CreateFence(initial,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&f))),"fence failed");return f;
    }
    void Write(ID3D12Resource* resource) {
        list->CopyBufferRegion(resource,0,upload.Get(),0,256);Require(SUCCEEDED(list->Close()),"close failed");
        ID3D12CommandList* lists[]{list.Get()};queue->ExecuteCommandLists(1,lists);
    }
    void Wait(ID3D12Fence* fence,uint64_t value) {
        HANDLE event=CreateEventW(nullptr,FALSE,FALSE,nullptr);Require(event!=nullptr,"event failed");
        const bool ready=SUCCEEDED(fence->SetEventOnCompletion(value,event))&&WaitForSingleObject(event,5000)==WAIT_OBJECT_0&&
            fence->GetCompletedValue()!=UINT64_MAX&&fence->GetCompletedValue()>=value;
        CloseHandle(event);Require(ready,"GPU drain failed");
    }
    void Debug() {
        for(UINT64 i=0;i<info->GetNumStoredMessages();++i){SIZE_T size{};info->GetMessage(i,nullptr,&size);std::vector<unsigned char> bytes(size);
            auto* message=reinterpret_cast<D3D12_MESSAGE*>(bytes.data());info->GetMessage(i,message,&size);
            Check(message->Severity!=D3D12_MESSAGE_SEVERITY_ERROR&&message->Severity!=D3D12_MESSAGE_SEVERITY_CORRUPTION,message->pDescription);
        }
    }
};
int main() {
    try {
        Context c;OpenXRManager manager;manager.m_d3dDevice=c.device.Get();Require(manager.Create(),"initial fence create failed");
        auto oldFence=ComPtr<ID3D12Fence>(manager.m_fence);auto blocker=c.Fence();
        struct Drain {ID3D12Fence* blocker;~Drain(){blocker->Signal(1);}} drain{blocker.Get()};
        auto color=c.Buffer(D3D12_HEAP_TYPE_DEFAULT,D3D12_RESOURCE_STATE_COPY_DEST);
        auto deaths=std::make_shared<std::atomic<unsigned>>(0);auto* witness=new Witness(deaths);
        Require(SUCCEEDED(color->SetPrivateDataInterface(WitnessKey,witness)),"witness failed");witness->Release();
        const auto key=reinterpret_cast<uintptr_t>(color.Get());Require(manager.m_captureLeases.Pin(key),"pin failed");
        color->AddRef(); // ownership passed to ReleaseCapturedRead
        Require(SUCCEEDED(c.queue->Wait(blocker.Get(),1)),"queue wait failed");c.Write(color.Get());
        manager.m_fenceValue=7; // Long-running monotonic timeline, not the first three frames.
        Require(SUCCEEDED(c.queue->Signal(manager.m_fence,7)),"Signal failed");manager.RecordSubmitExecuted(true);
        manager.ReleaseCapturedRead(color.Get(),7);
        const bool overwritable=manager.m_captureLeases.BeginWrite(key);
        Check(!overwritable,"pending XR read must keep capture pin until actual GPU completion");
        if(overwritable)manager.m_captureLeases.EndWrite(key);
        Check(manager.m_retiredCaptureReads.size()==1&&deaths->load()==0,"pending read resource was lost");
        manager.m_eyeSwapchains={{1,2},{3,4}};
        const bool resized=manager.Teardown();
        Check(!resized&&destroyedSwapchains==0,"resize must not destroy XR swapchains while submitted app GPU work is blocked");
        // The RED fixture's XR handles are stand-ins. Keep actual GPU objects
        // independently alive so observing the bug never submits a use-after-free.
        Require(SUCCEEDED(blocker->Signal(1)),"unblock failed");c.Wait(oldFence.Get(),7);
        if(!resized)Require(manager.Teardown(),"completed resize remained blocked");
        Check(destroyedSwapchains==4,"completed resize did not retire the four XR handles");
        Require(manager.Create(),"replacement fence create failed");
        Check(manager.m_fence->GetCompletedValue()==7,"replacement fence must start at proven monotonic highwater");
        auto late=c.Buffer(D3D12_HEAP_TYPE_DEFAULT,D3D12_RESOURCE_STATE_COPY_DEST);const auto lateKey=reinterpret_cast<uintptr_t>(late.Get());
        Require(manager.m_captureLeases.Pin(lateKey),"late pin failed");late->AddRef();
        manager.ReleaseCapturedRead(late.Get(),7);
        Check(manager.m_retiredCaptureReads.empty(),"late old-epoch read must use old completion proof, not wait on new fence zero");
        Check(manager.m_captureLeases.BeginWrite(lateKey),"completed old read pin did not release");manager.m_captureLeases.EndWrite(lateKey);
        Check(manager.m_captureLeases.BeginWrite(key),"completed pending read pin did not release");manager.m_captureLeases.EndWrite(key);
        color.Reset();Check(deaths->load()==1,"completed capture COM reference leaked");
#if HAS_SUBMIT_TRACKER
        auto threaded=c.Buffer(D3D12_HEAP_TYPE_DEFAULT,D3D12_RESOURCE_STATE_COPY_DEST);
        const auto threadedKey=reinterpret_cast<uintptr_t>(threaded.Get());
        for(unsigned i=0;i<256;++i){Require(manager.m_captureLeases.Pin(threadedKey),"threaded pin failed");threaded->AddRef();}
        std::atomic<bool> start{};
        std::thread callback([&]{while(!start.load(std::memory_order_acquire))std::this_thread::yield();
            for(unsigned i=0;i<256;++i)manager.ReleaseCapturedRead(threaded.Get(),7);});
        start.store(true,std::memory_order_release);bool replacements=true;
        for(unsigned i=0;i<256;++i)replacements=manager.Teardown()&&manager.Create()&&replacements;
        callback.join();Check(replacements&&manager.m_retiredCaptureReads.empty(),"late worker callbacks raced completed fence replacement");
        Check(manager.m_captureLeases.BeginWrite(threadedKey),"threaded late read left capture pins");manager.m_captureLeases.EndWrite(threadedKey);
#endif
        Require(manager.Teardown(),"final known teardown failed");
#if HAS_SUBMIT_TRACKER
        // A retirement owns the exact fence whose value it checks. Completing
        // an unrelated replacement timeline cannot retire this point.
        cvr::capture::SubmitReadCompletion read;
        auto fenceDeaths=std::make_shared<std::atomic<unsigned>>(0);
        {
            cvr::capture::SubmitFenceRetirement timeline;auto own=c.Fence();
            auto* fenceWitness=new Witness(fenceDeaths);Require(SUCCEEDED(own->SetPrivateDataInterface(WitnessKey,fenceWitness)),"fence witness failed");fenceWitness->Release();
            timeline.Executed(own.Get(),1,true);read=timeline.ResolveRead(1);
        }
        auto unrelated=c.Fence(1000);
        Check(fenceDeaths->load()==0&&!read.Complete(),"retired read lost its strong original fence or used replacement high value");
        Require(SUCCEEDED(read.fence->Signal(1)),"original fence signal failed");
        Check(read.Complete(),"retired read ignored its own completed fence");read={};
        Check(fenceDeaths->load()==1,"completed retired fence reference leaked");
        Check(!cvr::capture::SubmitFenceValueComplete(UINT64_MAX,1),"device removal must not be completion");
        Check(!cvr::capture::SubmitFenceValueComplete(100,UINT64_MAX),"unknown scalar must not be completion");

        // Submit actual WARP work with no successful Signal receipt. A later
        // independently observed drain is only test cleanup, not permission for
        // the production owner to erase the sticky uncertainty.
        Require(manager.Create(),"unknown-case fence create failed");
        Require(SUCCEEDED(c.allocator->Reset())&&SUCCEEDED(c.list->Reset(c.allocator.Get(),nullptr)),"unknown-case reset failed");
        auto unknownColor=c.Buffer(D3D12_HEAP_TYPE_DEFAULT,D3D12_RESOURCE_STATE_COPY_DEST);
        const auto unknownKey=reinterpret_cast<uintptr_t>(unknownColor.Get());
        auto unknownDeaths=std::make_shared<std::atomic<unsigned>>(0);auto* unknownWitness=new Witness(unknownDeaths);
        Require(SUCCEEDED(unknownColor->SetPrivateDataInterface(WitnessKey,unknownWitness)),"unknown witness failed");unknownWitness->Release();
        Require(manager.m_captureLeases.Pin(unknownKey),"unknown pin failed");unknownColor->AddRef();
        auto unknownBlocker=c.Fence();Require(SUCCEEDED(c.queue->Wait(unknownBlocker.Get(),1)),"unknown queue wait failed");
        c.Write(unknownColor.Get());manager.m_fenceValue=8;manager.RecordSubmitExecuted(false);
        manager.ReleaseCapturedRead(unknownColor.Get(),8);unknownColor.Reset();
        Check(!manager.PollSubmitResources(false)&&!manager.Teardown(),"unknown Signal must disable both subsequent submits and resize");
        unsigned extraSubmits{};for(unsigned i=0;i<100;++i)extraSubmits+=manager.PollSubmitResources(false)?1:0;
        Check(extraSubmits==0&&manager.m_retiredCaptureReads.size()==1&&unknownDeaths->load()==0,"unknown failure did not bound subsequent work/retirement");
        Require(SUCCEEDED(unknownBlocker->Signal(1)),"unknown unblock failed");
        auto cleanupFence=c.Fence();Require(SUCCEEDED(c.queue->Signal(cleanupFence.Get(),1)),"independent drain failed");c.Wait(cleanupFence.Get(),1);
        Require(SUCCEEDED(c.queue->Signal(manager.m_fence,9)),"later Signal failed");c.Wait(manager.m_fence,9);
        Check(!manager.PollSubmitResources(true)&&!manager.PollSubmitResources(false)&&unknownDeaths->load()==0,
            "higher fence completion must not erase earlier unknown Signal");
        const bool unknownWritable=manager.m_captureLeases.BeginWrite(unknownKey);
        Check(!unknownWritable,"unknown read pin released after unrelated/higher completion");if(unknownWritable)manager.m_captureLeases.EndWrite(unknownKey);
        auto independentlyDone=c.Buffer(D3D12_HEAP_TYPE_DEFAULT,D3D12_RESOURCE_STATE_COPY_DEST);
        const auto independentlyDoneKey=reinterpret_cast<uintptr_t>(independentlyDone.Get());
        Require(manager.m_captureLeases.Pin(independentlyDoneKey),"zero pin failed");independentlyDone->AddRef();
        manager.ReleaseCapturedRead(independentlyDone.Get(),0);
        Check(manager.m_captureLeases.BeginWrite(independentlyDoneKey),"zero-fence owner completion should unpin even after unrelated XR failure");manager.m_captureLeases.EndWrite(independentlyDoneKey);
        // Explicit fixture-only cleanup after the independent drain above. The
        // production process-lifetime owner deliberately has no recovery here.
        for(auto& item:manager.m_retiredCaptureReads)item.resource->Release();manager.m_retiredCaptureReads.clear();
        manager.m_fence->Release();manager.m_fence=nullptr;
#endif
        c.Debug();std::cout<<"RESULT "<<checks<<" checks, "<<failures<<" failures (WARP + exact production fragments)\n";
        return failures?1:0;
    } catch(const std::exception& error) {std::cerr<<"ERROR "<<error.what()<<'\n';return 2;}
}
