#include "Framegen/NativeFgOutput.hpp"
#include "Framegen/ColorRead.hpp"
#include "Render/CommandResources.hpp"
#include "Render/ResourceAllocation.hpp"
#include "../src/Framegen/NativeFgPairedParameters.hpp"
#include "NativeFgParameterMap.hpp"
#define wmain UnusedGpuOracle
#include "gpu_compare.cpp"
#undef wmain
#include "SrOwnerImageFixture.hpp"
#include <thread>
using namespace cvr::framegen;
void Log(const char*,...){}
static SrSourceStamp Source(int64_t id){SrSourceStamp s;s.source={1,uint64_t(id+1),1,{1,1,1},id,(id&1)?AerV2Eye::Right:AerV2Eye::Left,1,1000+id,false};
    s.poseKnown=s.nativeFrameKnown=true;s.poseId=id+1;s.writeId=id+20;s.nativeFrame=uint32_t(id+50);s.engineView=1;s.origin=s.captureClockGeneration=1;return s;}
static NativeFgReceipt Receipt(int64_t id){NativeFgReceipt r;r.previous=Source(id-2);r.current=Source(id);r.moduleGeneration=r.lifecycleGeneration=r.createOrdinal=r.historyEpoch=r.resourceEpoch=1;r.count=r.index=1;return r;}
static void Wait(Device&gpu){Hr(gpu.q->Signal(gpu.fence.Get(),++gpu.sequence),"signal");HANDLE event=CreateEventW(nullptr,FALSE,FALSE,nullptr);Check(event!=nullptr,"event");Hr(gpu.fence->SetEventOnCompletion(gpu.sequence,event),"completion");Check(WaitForSingleObject(event,30000)==WAIT_OBJECT_0,"GPU timeout");CloseHandle(event);}
static void PairedReceipts(){
    auto r=Receipt(4);r.paired=true;r.pairedEye=1;r.previousPairSerial=20;r.currentPairSerial=21;r.gameWorldEpoch=7;
    r.previous.source={};r.current.source={};r.previous.nativeFrame=100;r.current.nativeFrame=101;
    for(unsigned view=1;view<=2;++view){r.previous.engineView=r.current.engineView=view;
        Check(ValidNativeFgReceipt(r),"valid paired native FG receipt rejected");
        auto wrong=r;wrong.previous.engineView=view==1?2:1;Check(!ValidNativeFgReceipt(wrong),"mixed camera history admitted");
        wrong=r;wrong.current.nativeFrame++;Check(!ValidNativeFgReceipt(wrong),"skipped native frame admitted as consecutive history");
        wrong=r;wrong.currentPairSerial=wrong.previousPairSerial;Check(!ValidNativeFgReceipt(wrong),"duplicate source admitted");
        wrong=r;wrong.pairedEye=2;Check(!ValidNativeFgReceipt(wrong),"invalid physical eye admitted");
        wrong=r;wrong.gameWorldEpoch=0;Check(!ValidNativeFgReceipt(wrong),"unknown game world admitted");
        wrong=r;wrong.current.source=Source(4).source;Check(!ValidNativeFgReceipt(wrong),"AER label mixed into paired history");
        for(unsigned count=1;count<=3;++count)for(unsigned index=1;index<=count;++index){auto m=r;m.count=count;m.index=index;Check(ValidNativeFgReceipt(m),"valid X2-X4 ordinal rejected");}
    }
}
static void PairedParameterRestoration(ID3D12Resource* oldResource,ID3D12Resource* newResource){
    FgParameters p;p.integers["DLSSG.MvecDilated"]=0;
    for(const auto* key:{"DLSSG.Backbuffer","DLSSG.MVecs","DLSSG.Depth","DLSSG.OutputInterpolated","DLSSG.OutputReal","DLSSG.OutputDisableInterpolation","DLSSG.HUDLess"})p.pointers[key]=oldResource;
    const char* matrices[]{"DLSSG.CameraViewToClip","DLSSG.ClipToCameraView","DLSSG.ClipToLensClip","DLSSG.ClipToPrevClip","DLSSG.PrevClipToClip"};
    for(unsigned i=0;i<5;++i)p.pointers[matrices[i]]=p.matrices[i].data();
    Inputs g;g.depth=newResource;g.motion=newResource;g.submitted=g.evaluated=true;g.width=g.motionWidth=32;g.height=g.motionHeight=24;
    g.camera.verticalFov=1.2f;g.camera.motionScale[0]=32;g.camera.motionScale[1]=24;
    auto& c=g.camera.constants;c.source=CameraConstantsSource::NativeStaging;c.version=2;c.flagPresent.set();c.valuePresent.set();c.flags[0]=c.flags[1]=1;
    for(unsigned m=0;m<5;++m)for(unsigned i=0;i<16;++i)c.values[m*16+i]=i%5==0?1.f:0.f;
    const auto integers=p.integers;const auto floats=p.floats;const auto pointers=p.pointers;
    {native_fg::PairedOverride scoped(&p,g,newResource,newResource,newResource,newResource,1);
        Check(bool(scoped),"verified native-staging camera rejected");
        Check(p.pointers["DLSSG.Backbuffer"]==newResource&&p.pointers["DLSSG.Depth"]==newResource,"companion uses host resources");
        Check(p.pointers["DLSSG.HUDLess"]==nullptr,"companion borrows MAIN HUDless image");
        Check(p.integers["DLSSG.Reset"]==1&&p.floats["DLSSG.CameraFOV"]==1.2f,"companion camera/reset not applied");}
    Check(p.integers==integers&&p.floats==floats&&p.pointers==pointers,"host parameters not exactly restored");
    p.pointers.erase("DLSSG.PrevClipToClip");const auto rejected=p.pointers;
    {native_fg::PairedOverride scoped(&p,g,newResource,newResource,newResource,newResource,1);Check(!scoped,"missing required projection accepted");}
    Check(p.integers==integers&&p.floats==floats&&p.pointers==rejected,"rejected override mutated host parameters");
}
int wmain(){try{PairedReceipts();Device gpu;
    ComPtr<ID3D12CommandAllocator> allocator;ComPtr<ID3D12GraphicsCommandList> list;
    Hr(gpu.d->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&allocator)),"allocator");
    Hr(gpu.d->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,allocator.Get(),nullptr,IID_PPV_ARGS(&list)),"list");
    OwnerImage real(gpu.d.Get(),4,false),generated(gpu.d.Get(),4,true);
    PairedParameterRestoration(real.resource.Get(),generated.resource.Get());
    D3D12_RESOURCE_ALIASING_BARRIER globalAlias{};
    Check(cvr::gpu::AliasingInvalidatesResource(real.resource.Get(),globalAlias),"unknown allocation trusted");
    Hr(cvr::gpu::MarkResourceAllocation(real.resource.Get(),true),"committed provenance");
    Check(!cvr::gpu::AliasingInvalidatesResource(real.resource.Get(),globalAlias),"global alias incorrectly invalidated proven committed FG output");
    D3D12_RESOURCE_ALIASING_BARRIER explicitAlias{real.resource.Get(),nullptr};
    Check(cvr::gpu::AliasingInvalidatesResource(real.resource.Get(),explicitAlias),"explicit alias ignored");
    Hr(cvr::gpu::MarkResourceAllocation(generated.resource.Get(),false),"placed provenance");
    Check(cvr::gpu::AliasingInvalidatesResource(generated.resource.Get(),globalAlias),"placed allocation trusted across alias");
    real.Upload(list.Get(),[](unsigned,unsigned,unsigned c){return c==0?.25f:1.f;},D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    generated.Upload(list.Get(),[](unsigned,unsigned,unsigned c){return c==1?.75f:1.f;},D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    auto upload=Buffer(gpu.d.Get(),4,D3D12_HEAP_TYPE_UPLOAD);void* mapped{};D3D12_RANGE none{};Hr(upload->Map(0,&none,&mapped),"map flag");std::memset(mapped,0,4);upload->Unmap(0,nullptr);
    D3D12_RESOURCE_DESC desc{};desc.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;desc.Width=4;desc.Height=1;desc.DepthOrArraySize=desc.MipLevels=1;desc.SampleDesc.Count=1;desc.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    D3D12_HEAP_PROPERTIES heap{};heap.Type=D3D12_HEAP_TYPE_DEFAULT;ComPtr<ID3D12Resource> flag;Hr(gpu.d->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&desc,D3D12_RESOURCE_STATE_COMMON,nullptr,IID_PPV_ARGS(&flag)),"flag");
    auto submit=[&]{Hr(list->Close(),"close");ID3D12CommandList* lists[]{list.Get()};auto resources=cvr::gpu::PrepareCommandResources(1,lists);
        NativeFgOutputBeforeSubmit(gpu.q.Get(),1,lists);gpu.q->ExecuteCommandLists(1,lists);NativeFgOutputSubmitted(gpu.q.Get());cvr::gpu::SubmitCommandResources(gpu.q.Get(),std::move(resources));Wait(gpu);
        Hr(allocator->Reset(),"allocator reset");Hr(list->Reset(allocator.Get(),nullptr),"list reset");NativeFgOutputReset(list.Get());cvr::gpu::ResetCommandResources(list.Get());cvr::gpu::CollectCommandResources();};
    const auto record=[&](unsigned n,bool denied=false){Hr(upload->Map(0,&none,&mapped),"flag map");std::memset(mapped,denied?1:0,4);upload->Unmap(0,nullptr);
        Barrier(list.Get(),flag.Get(),D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_COPY_DEST);list->CopyBufferRegion(flag.Get(),0,upload.Get(),0,4);
        RecordNativeFgOutput(list.Get(),real.resource.Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,generated.resource.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,flag.Get(),D3D12_RESOURCE_STATE_COPY_DEST,Receipt(2*n),n,n);};
    auto wrong=Receipt(2);wrong.previous.source.physicalEye=AerV2Eye::Right;
    RecordNativeFgOutput(list.Get(),real.resource.Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,generated.resource.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,flag.Get(),D3D12_RESOURCE_STATE_COMMON,wrong,99,99);
    Check(ReadNativeFgOutputStatus().recorded==0,"wrong eye accepted");
    record(1);Check(CollectNativeFgOutputs().empty(),"unsubmitted G exposed");submit();
    auto packets=CollectNativeFgOutputs();Check(packets.size()==1&&packets[0]->interpolationAllowed&&packets[0]->real&&packets[0]->color,"ready R/G not exposed");
    auto original=packets[0];const auto originalColor=original->color.Get();
    const auto pixel=[&](ID3D12Resource* texture,D3D12_RESOURCE_STATES state){
        const auto shape=texture->GetDesc();D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};UINT64 bytes{};
        gpu.d->GetCopyableFootprints(&shape,0,1,0,&footprint,nullptr,nullptr,&bytes);auto readback=Buffer(gpu.d.Get(),bytes,D3D12_HEAP_TYPE_READBACK);
        Barrier(list.Get(),texture,state,D3D12_RESOURCE_STATE_COPY_SOURCE);D3D12_TEXTURE_COPY_LOCATION from{},to{};
        from.pResource=texture;from.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;to.pResource=readback.Get();to.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;to.PlacedFootprint=footprint;
        list->CopyTextureRegion(&to,0,0,0,&from,nullptr);Barrier(list.Get(),texture,D3D12_RESOURCE_STATE_COPY_SOURCE,state);submit();
        void* data{};D3D12_RANGE range{0,16};Hr(readback->Map(0,&range,&data),"pixel map");std::array<float,4> rgba{};std::memcpy(rgba.data(),data,16);readback->Unmap(0,&none);return rgba;
    };
    const auto realPixel=pixel(original->real.Get(),CaptureColorReadState),generatedPixel=pixel(original->color.Get(),D3D12_RESOURCE_STATE_COMMON);
    Check(realPixel[0]==.25f&&realPixel[1]==1.f&&generatedPixel[0]==1.f&&generatedPixel[1]==.75f,"R/G copied the wrong pixels");
    record(2,true);submit();packets=CollectNativeFgOutputs();
    Check(std::count_if(packets.begin(),packets.end(),[](const auto&p){return !p->interpolationAllowed;})==1,"NVIDIA suppression ignored");
    Check(original->color.Get()==originalColor&&original->valid->load(),"live product invalidated by another record");
    for(unsigned n=3;n<=8;++n){record(n);submit();packets=CollectNativeFgOutputs();}
    Check(packets.size()==8,"bounded producer slots missing");record(9);Check(ReadNativeFgOutputStatus().poolBusy==1,"live consumer allowed pool overwrite");submit();
    const auto r=ReadNativeFgOutputStatus();Check(r.ready==7&&r.suppressed==1&&r.unknown==0,"incorrect readiness counters");
    packets.clear();original.reset();record(10);Hr(list->Close(),"cancel close");Hr(allocator->Reset(),"cancel allocator");Hr(list->Reset(allocator.Get(),nullptr),"cancel reset");NativeFgOutputReset(list.Get());cvr::gpu::ResetCommandResources(list.Get());
    Check(ReadNativeFgOutputStatus().cancelled==1,"discarded commands published a product");
    ComPtr<ID3D12InfoQueue> info;if(SUCCEEDED(gpu.d.As(&info)))for(UINT64 i=0;i<info->GetNumStoredMessages();++i){SIZE_T size{};info->GetMessage(i,nullptr,&size);std::vector<unsigned char> bytes(size);auto*m=reinterpret_cast<D3D12_MESSAGE*>(bytes.data());info->GetMessage(i,m,&size);if(m->Severity<=D3D12_MESSAGE_SEVERITY_ERROR){std::cerr<<m->pDescription<<'\n';throw std::runtime_error("D3D12 debug error");}}
    ObserveNativeFgResizeQueue(gpu.q.Get());
    ComPtr<ID3D12Fence> resizeBlock;Hr(gpu.d->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&resizeBlock)),"resize test fence");
    Hr(gpu.q->Wait(resizeBlock.Get(),1),"block resize queue");
    Check(!DrainNativeFgResizeQueues(20),"resize proceeded with pending GPU work");
    Hr(resizeBlock->Signal(1),"release timed-out resize queue");
    Check(DrainNativeFgResizeQueues(1000),"completed resize queue was not drained");
    Hr(gpu.q->Wait(resizeBlock.Get(),2),"block second resize");
    std::thread finish([&]{Sleep(50);resizeBlock->Signal(2);});
    const bool drained=DrainNativeFgResizeQueues(1000);finish.join();
    Check(drained,"resize did not await GPU completion");
    std::cout<<"PASS native GPU R/G copy, permission, completion, cancellation, bounded leases and resize drain/timeout; debug errors0\n";return 0;
}catch(const std::exception&e){std::cerr<<e.what()<<'\n';return 1;}}
