#include "Render/SrTemporalMotionPass.hpp"
// Reuse the frozen independent input/CB case definitions and actual DXIL oracle.
// Only the old fixture entry point is renamed; no audit file is modified.
#define wmain SrAuditUnusedMain
#include "gpu_compare.cpp"
#undef wmain
#include <chrono>
#pragma warning(push)
#pragma warning(disable:4100)
#include "CpuD3d.hpp"
#pragma warning(pop)
using namespace cvr::render;
void Log(const char*,...) {}
static unsigned assertions{},recordCalls{};
static void Assert(bool condition,const char* message){++assertions;Check(condition,message);}
struct Image {
    ComPtr<ID3D12Resource> resource,upload,readback;D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    uint64_t bytes{};unsigned width,height,channels;
    Image(Device& gpu,unsigned w,unsigned h,unsigned c,bool output):width(w),height(h),channels(c){
        D3D12_RESOURCE_DESC d{};d.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;d.Width=w;d.Height=h;d.DepthOrArraySize=d.MipLevels=1;
        d.SampleDesc.Count=1;d.Format=c==1?DXGI_FORMAT_R32_FLOAT:c==2?DXGI_FORMAT_R32G32_FLOAT:DXGI_FORMAT_R32G32B32A32_FLOAT;
        d.Flags=output?D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS:D3D12_RESOURCE_FLAG_NONE;D3D12_HEAP_PROPERTIES hp{};hp.Type=D3D12_HEAP_TYPE_DEFAULT;
        Hr(gpu.d->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&d,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&resource)),"image");
        gpu.d->GetCopyableFootprints(&d,0,1,0,&footprint,nullptr,nullptr,&bytes);upload=Buffer(gpu.d.Get(),bytes,D3D12_HEAP_TYPE_UPLOAD);
        if(output)readback=Buffer(gpu.d.Get(),bytes,D3D12_HEAP_TYPE_READBACK);
    }
    template<class Pixel>void Upload(ID3D12GraphicsCommandList* list,Pixel pixel,D3D12_RESOURCE_STATES state){
        void* mapped{};D3D12_RANGE none{};Hr(upload->Map(0,&none,&mapped),"image upload map");std::memset(mapped,0,size_t(bytes));
        for(unsigned y=0;y<height;++y)for(unsigned x=0;x<width;++x)for(unsigned channel=0;channel<channels;++channel){
            const float value=pixel(x,y,channel);std::memcpy(static_cast<char*>(mapped)+y*footprint.Footprint.RowPitch+(x*channels+channel)*4,&value,4);
        }
        upload->Unmap(0,nullptr);D3D12_TEXTURE_COPY_LOCATION from{},to{};from.pResource=upload.Get();from.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;from.PlacedFootprint=footprint;
        to.pResource=resource.Get();to.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;list->CopyTextureRegion(&to,0,0,0,&from,nullptr);Barrier(list,resource.Get(),D3D12_RESOURCE_STATE_COPY_DEST,state);
    }
    void CopyBack(ID3D12GraphicsCommandList* list,D3D12_RESOURCE_STATES state){
        Barrier(list,resource.Get(),state,D3D12_RESOURCE_STATE_COPY_SOURCE);D3D12_TEXTURE_COPY_LOCATION from{},to{};
        from.pResource=resource.Get();from.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;to.pResource=readback.Get();to.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;to.PlacedFootprint=footprint;list->CopyTextureRegion(&to,0,0,0,&from,nullptr);
    }
    std::vector<float> Values(){
        void* mapped{};D3D12_RANGE all{0,size_t(bytes)};Hr(readback->Map(0,&all,&mapped),"image readback");std::vector<float> result(width*height*channels);
        for(unsigned y=0;y<height;++y)std::memcpy(result.data()+y*width*channels,static_cast<char*>(mapped)+y*footprint.Footprint.RowPitch,width*channels*4);
        D3D12_RANGE none{};readback->Unmap(0,&none);return result;
    }
};
struct Batch {
    std::vector<Image> inputs,outputs;ComPtr<ID3D12CommandAllocator> allocator;ComPtr<ID3D12GraphicsCommandList> list;
    SrTemporalMotionRequest request;SrTemporalTicket ticket;
    Batch(Device& gpu,const Case& test,unsigned width=32,unsigned height=24,bool clipped=false,bool generic=false,
          D3D12_RESOURCE_STATES outputState=D3D12_RESOURCE_STATE_UNORDERED_ACCESS){
        Hr(gpu.d->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&allocator)),"batch allocator");
        Hr(gpu.d->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,allocator.Get(),nullptr,IID_PPV_ARGS(&list)),"batch list");
        request.constants.words=test.cb.raw;
        for(unsigned i=0;i<8;++i){inputs.emplace_back(gpu,width,height,i<2?2:i<5?1:4,false);
            const auto state=generic?D3D12_RESOURCE_STATE_GENERIC_READ:D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
            inputs.back().Upload(list.Get(),[&](unsigned x,unsigned y,unsigned c){return Input(i,test.pattern,x,y,c);},state);
            request.inputs[i]={inputs.back().resource,inputs.back().resource->GetDesc().Format,width,height,state,{}};
        }
        for(unsigned i=0;i<4;++i){const auto w=clipped&&i>=2?width/2:width,h=clipped&&i>=2?height/2:height;
            outputs.emplace_back(gpu,w,h,i<2?2:4,true);outputs.back().Upload(list.Get(),[](unsigned,unsigned,unsigned c){return 16.5f+float(c);},outputState);
            request.outputs[i]={outputs.back().resource,outputs.back().resource->GetDesc().Format,w,h,outputState,{}};
        }
        request.allowClippedColorStores=clipped;request.completion={gpu.fence.Get(),gpu.sequence+1};
    }
    void Back(D3D12_RESOURCE_STATES state){for(auto& output:outputs)output.CopyBack(list.Get(),state);}
    std::array<std::vector<float>,4> Values(){std::array<std::vector<float>,4> result;for(unsigned i=0;i<4;++i)result[i]=outputs[i].Values();return result;}
};
static Case Sized(Case test,unsigned width,unsigned height){
    test.cb.U(256,width);test.cb.U(260,height);test.cb.V2(264,float(width),float(height));test.cb.V2(272,1.f/width,1.f/height);
    test.cb.V2(288,float(width),float(height));test.cb.V2(296,1.f/width,1.f/height);return test;
}
static std::array<std::vector<float>,4> Oracle(Device& gpu,ID3D12PipelineState* pso,const Case& test,unsigned w=32,unsigned h=24,bool clipped=false){
    Batch batch(gpu,test,w,h,clipped);auto cb=Buffer(gpu.d.Get(),512,D3D12_HEAP_TYPE_UPLOAD);void* mapped{};D3D12_RANGE none{};Hr(cb->Map(0,&none,&mapped),"oracle cb");std::memcpy(mapped,&test.cb,480);cb->Unmap(0,nullptr);
    ComPtr<ID3D12DescriptorHeap> heap;D3D12_DESCRIPTOR_HEAP_DESC hd{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,13,D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE,0};Hr(gpu.d->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&heap)),"oracle heap");const auto stride=gpu.d->GetDescriptorHandleIncrementSize(hd.Type);
    auto cpu=heap->GetCPUDescriptorHandleForHeapStart();for(unsigned i=0;i<9;++i){const auto& r=batch.inputs[std::min(i,7u)].resource;D3D12_SHADER_RESOURCE_VIEW_DESC d{};d.Format=r->GetDesc().Format;d.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D;d.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;d.Texture2D.MipLevels=1;gpu.d->CreateShaderResourceView(r.Get(),&d,cpu);cpu.ptr+=stride;}
    for(const auto& image:batch.outputs){D3D12_UNORDERED_ACCESS_VIEW_DESC d{};d.Format=image.resource->GetDesc().Format;d.ViewDimension=D3D12_UAV_DIMENSION_TEXTURE2D;gpu.d->CreateUnorderedAccessView(image.resource.Get(),nullptr,&d,cpu);cpu.ptr+=stride;}
    ID3D12DescriptorHeap* heaps[]{heap.Get()};batch.list->SetDescriptorHeaps(1,heaps);batch.list->SetComputeRootSignature(gpu.root.Get());batch.list->SetPipelineState(pso);batch.list->SetComputeRootConstantBufferView(0,cb->GetGPUVirtualAddress());auto handle=heap->GetGPUDescriptorHandleForHeapStart();batch.list->SetComputeRootDescriptorTable(1,handle);handle.ptr+=uint64_t(stride)*9;batch.list->SetComputeRootDescriptorTable(2,handle);batch.list->Dispatch((w+7)/8,(h+7)/8,1);batch.Back(D3D12_RESOURCE_STATE_UNORDERED_ACCESS);gpu.Finish(batch.list.Get());return batch.Values();
}
static std::array<std::vector<float>,4> Production(Device& gpu,SrTemporalMotionPass& pass,const Case& test,bool negative=false,unsigned w=32,unsigned h=24,bool clipped=false,bool generic=false,
    D3D12_RESOURCE_STATES outputState=D3D12_RESOURCE_STATE_UNORDERED_ACCESS,bool optionalT8=false){
    Batch batch(gpu,test,w,h,clipped,generic,outputState);if(optionalT8)batch.request.unusedT8=batch.request.inputs[7];
    if(!negative){batch.ticket=pass.Record(batch.list.Get(),batch.request);++recordCalls;Assert(bool(batch.ticket),"actual production Record rejected");}
    batch.Back(negative?D3D12_RESOURCE_STATE_UNORDERED_ACCESS:D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);gpu.Finish(batch.list.Get());
    if(!negative){Assert(pass.SubmissionReported(batch.ticket,true,true),"successful signal report");Assert(pass.Collect()==1,"retire exact successful fence");}
    return batch.Values();
}
static uint64_t Compare(const std::array<std::vector<float>,4>& a,const std::array<std::vector<float>,4>& b,float& maximum){
    uint64_t failures=0;
    for(unsigned out=0;out<4;++out){Assert(a[out].size()==b[out].size(),"readback extent mismatch");for(size_t i=0;i<a[out].size();++i){
        const auto x=a[out][i],y=b[out][i];bool same;
        if(std::isnan(x)||std::isnan(y))same=std::isnan(x)&&std::isnan(y);else if(std::isinf(x)||std::isinf(y))same=x==y;
        else{const auto delta=std::abs(x-y);maximum=std::max(maximum,delta);same=delta<=1e-5f+2e-5f*std::max(std::abs(x),std::abs(y));}failures+=!same;
    }}return failures;
}
static void Guards(Device& gpu,SrTemporalMotionPass& pass){
    Batch batch(gpu,Cases()[0]);SrTemporalMotionPass notPrepared;
    Assert(notPrepared.Record(batch.list.Get(),batch.request).status==SrTemporalRecordStatus::NotPrepared,"Record before Prepare");
    auto invalid=[&](const SrTemporalMotionRequest& request,SrTemporalRecordStatus status){Assert(pass.Record(batch.list.Get(),request).status==status,"invalid request status");Assert(pass.BusySlots()==0,"rejected request consumes no slot");};
    auto r=batch.request;r.inputs[0].width++;invalid(r,SrTemporalRecordStatus::InvalidDescriptor);
    r=batch.request;r.inputs[0].viewFormat=DXGI_FORMAT_R8G8B8A8_UNORM;invalid(r,SrTemporalRecordStatus::InvalidDescriptor);
    r=batch.request;r.inputs[0].beforeState=D3D12_RESOURCE_STATE_COPY_DEST;invalid(r,SrTemporalRecordStatus::InvalidState);
    r=batch.request;r.outputs[0]=r.inputs[0];invalid(r,SrTemporalRecordStatus::InvalidDescriptor);
    r=batch.request;r.inputs[0]=r.outputs[0];r.inputs[0].beforeState=D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;invalid(r,SrTemporalRecordStatus::Aliasing);
    r=batch.request;r.outputs[1]=r.outputs[0];invalid(r,SrTemporalRecordStatus::Aliasing);
    r=batch.request;r.inputs[1]=r.inputs[0];r.inputs[1].beforeState=D3D12_RESOURCE_STATE_GENERIC_READ;invalid(r,SrTemporalRecordStatus::Aliasing);
    r=batch.request;r.constants.words[64]=0;invalid(r,SrTemporalRecordStatus::InvalidConstants);
    r=batch.request;r.constants.words[64]=31;invalid(r,SrTemporalRecordStatus::InvalidDescriptor);
    r=batch.request;r.completion.value=gpu.fence->GetCompletedValue();invalid(r,SrTemporalRecordStatus::InvalidCompletion);
    r=batch.request;r.completion.value=UINT64_MAX;invalid(r,SrTemporalRecordStatus::InvalidCompletion);
    Batch clip(gpu,Cases()[0],32,24,true);clip.request.allowClippedColorStores=false;invalid(clip.request,SrTemporalRecordStatus::InvalidDescriptor);
    // Never-executed cancellations beyond capacity; values remain monotonic.
    for(unsigned i=0;i<12;++i){Batch never(gpu,Cases()[0]);never.request.completion.value=++gpu.sequence;
        const auto ticket=pass.Record(never.list.Get(),never.request);Assert(bool(ticket),"cancel fixture record");
        Assert(!pass.SubmissionReported(ticket,false,false),"false execute is not submission");Assert(pass.BusySlots()==1,"recorded retains");
        Assert(pass.CancelNeverExecuted(ticket),"explicit never execute cancel");Assert(!pass.CancelNeverExecuted(ticket),"stale ticket rejected");
        Assert(pass.BusySlots()==0,"cancel frees slot");Assert(pass.Record(never.list.Get(),never.request).status==SrTemporalRecordStatus::InvalidCompletion,"cancel burns fence value");
    }
    Assert(pass.PipelineStats().shaderCompileCalls==1&&pass.PipelineStats().pipelineCreateCalls==1,"Record cannot compile/create PSO");
    // CPU-fence fault injection at a real device/list: UINT64_MAX is removal,
    // never normal completion. The list is explicitly never executed.
    SrTemporalMotionPass removed;Assert(removed.Prepare(gpu.d.Get())==SrTemporalPrepareStatus::Ready,"removal fixture prepare");
    ComPtr<CpuFence> fence;fence.Attach(new CpuFence(nullptr));fence->device=gpu.d;
    Batch never(gpu,Cases()[0]);never.request.completion={fence.Get(),1};const auto ticket=removed.Record(never.list.Get(),never.request);
    Assert(bool(ticket),"removal fixture record");fence->completed=UINT64_MAX;
    Assert(removed.Collect()==0&&removed.DeviceRemoved()&&removed.BusySlots()==1,"removal sentinel cannot free slot");
    Assert(removed.CancelNeverExecuted(ticket)&&removed.TryShutdown(),"never executed removal record explicit cancellation");
}
static void Wait(ID3D12Fence* fence,uint64_t value){
    HANDLE event=CreateEventW(nullptr,FALSE,FALSE,nullptr);Check(event!=nullptr,"fixture wait event");Hr(fence->SetEventOnCompletion(value,event),"fixture wait fence");
    const auto result=WaitForSingleObject(event,30000);CloseHandle(event);Check(result==WAIT_OBJECT_0&&fence->GetCompletedValue()!=UINT64_MAX,"fixture wait timeout");
}
static void Lifetime(Device& gpu,SrTemporalMotionPass& pass,ID3D12PipelineState* oracle){
    struct Gate {ComPtr<ID3D12Fence> fence;~Gate(){if(fence)fence->Signal(1);}} gate;
    Hr(gpu.d->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&gate.fence)),"queue gate");Hr(gpu.q->Wait(gate.fence.Get(),1),"test-only queue gate");
    std::array<std::unique_ptr<Batch>,4> batches;std::array<std::weak_ptr<int>,4> ownerRefs;std::array<ComPtr<ID3D12Fence>,4> completions;
    const unsigned widths[]{32,40,16,24},heights[]{24,32,16,16};const auto all=Cases();
    std::array<Case,4> tests{Sized(all.back(),32,24),Sized(all[61],40,32),Sized(all[54],16,16),Sized(all[59],24,16)};
    for(unsigned i=0;i<4;++i){batches[i]=std::make_unique<Batch>(gpu,tests[i],widths[i],heights[i]);auto& batch=*batches[i];
        auto owner=std::make_shared<int>(int(i));ownerRefs[i]=owner;for(auto& in:batch.request.inputs)in.owner=owner;for(auto& out:batch.request.outputs)out.owner=owner;
        Hr(gpu.d->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&completions[i])),"independent inflight fence");
        batch.request.completion={completions[i].Get(),1};batch.ticket=pass.Record(batch.list.Get(),batch.request);Assert(bool(batch.ticket),"inflight record");
        for(auto& in:batch.request.inputs)in.owner.reset();for(auto& out:batch.request.outputs)out.owner.reset();owner.reset();Assert(!ownerRefs[i].expired(),"slot pins immutable input/output owners");
        batch.Back(D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);Hr(batch.list->Close(),"inflight close");ID3D12CommandList* lists[]{batch.list.Get()};gpu.q->ExecuteCommandLists(1,lists);
        if(i<3)Hr(gpu.q->Signal(completions[i].Get(),batch.request.completion.value),"inflight signal");
        Assert(pass.SubmissionReported(batch.ticket,true,i<3),"inflight submit/unknown report");Assert(!pass.CancelNeverExecuted(batch.ticket),"submitted/unknown cannot cancel");
    }
    Assert(pass.BusySlots()==4&&pass.Collect()==0,"four independently pinned inflight slots");
    Batch fifth(gpu,Cases()[0]);Assert(pass.Record(fifth.list.Get(),fifth.request).status==SrTemporalRecordStatus::NoCapacity,"fifth is NoCapacity");
    auto overlap=fifth.request;overlap.outputs[0]=batches[0]->request.outputs[0];
    Assert(pass.Record(fifth.list.Get(),overlap).status==SrTemporalRecordStatus::Aliasing,"live output cannot be reused for writes");
    overlap=fifth.request;overlap.inputs[0]=batches[0]->request.outputs[0];overlap.inputs[0].beforeState=D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    Assert(pass.Record(fifth.list.Get(),overlap).status==SrTemporalRecordStatus::Aliasing,"live output is not immutable new input");
    auto reused=fifth.request;reused.completion={completions[0].Get(),2};
    Assert(pass.Record(fifth.list.Get(),reused).status==SrTemporalRecordStatus::InvalidCompletion,"submitted live fence cannot be reused");
    reused.completion={completions[3].Get(),2};Assert(pass.Record(fifth.list.Get(),reused).status==SrTemporalRecordStatus::InvalidCompletion,"unknown live fence cannot be reused");
    Assert(!pass.TryShutdown(),"inflight shutdown refuses without waiting");
    Assert(pass.Prepare(gpu.d.Get())==SrTemporalPrepareStatus::Ready,"repeat prepare preserves busy slots");
    // D3D12CreateDevice caches the same WARP adapter's device identity. A CPU
    // identity is enough for this guard: Busy must return before device access.
    CpuDevice other;Assert(pass.Prepare(other.Native())==SrTemporalPrepareStatus::Busy,"device replacement refuses live pool");
    Hr(gate.fence->Signal(1),"release test gate");Wait(completions[2].Get(),batches[2]->request.completion.value);
    Assert(pass.Collect()==3&&pass.BusySlots()==1,"only known successful timelines retire");
    for(unsigned i=0;i<3;++i)Assert(ownerRefs[i].expired(),"completed owner released");Assert(!ownerRefs[3].expired(),"unknown owner retained");
    Hr(gpu.q->Signal(completions[3].Get(),batches[3]->request.completion.value),"successful exact signal retry");Wait(completions[3].Get(),batches[3]->request.completion.value);
    Assert(pass.Collect()==0&&pass.BusySlots()==1,"completed but unreported unknown remains owned");
    Assert(pass.SubmissionReported(batches[3]->ticket,true,true),"report exact successful retry");Assert(pass.Collect()==1&&ownerRefs[3].expired(),"retry retires exact point");
    for(unsigned i=0;i<4;++i){float maximum=0;Assert(Compare(Oracle(gpu,oracle,tests[i],widths[i],heights[i]),batches[i]->Values(),maximum)==0,"inflight CB/heap/resize readback vs original");}
    Assert(pass.PipelineStats().shaderCompileCalls==1&&pass.PipelineStats().pipelineCreateCalls==1,"resize did not rebuild pipeline");
    // Failure-only destructor is nonblocking and retains a recorded unknown pool.
    std::weak_ptr<int> retained;auto start=std::chrono::steady_clock::now();
    {SrTemporalMotionPass uncertain;Assert(uncertain.Prepare(gpu.d.Get())==SrTemporalPrepareStatus::Ready,"uncertain prepare");Batch never(gpu,Cases()[0]);auto owner=std::make_shared<int>(1);retained=owner;never.request.inputs[0].owner=owner;
        Assert(bool(uncertain.Record(never.list.Get(),never.request)),"uncertain record");never.request.inputs[0].owner.reset();owner.reset();}
    Assert(!retained.expired(),"failure destructor retains owner");Assert(std::chrono::steady_clock::now()-start<std::chrono::seconds(5),"destructor never waits for unsubmitted fence");
}
static void OutOfOrder(Device& gpu,SrTemporalMotionPass& pass,ID3D12PipelineState* oracle){
    Batch first(gpu,Cases()[61]),second(gpu,Cases()[62]);ComPtr<ID3D12Fence> a,b;
    Hr(gpu.d->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&a)),"order fenceA");Hr(gpu.d->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&b)),"order fenceB");
    first.request.completion={a.Get(),1};second.request.completion={b.Get(),100};
    auto owner=std::make_shared<int>(3);std::weak_ptr<int> retained=owner;first.request.inputs[0].owner=owner;
    first.ticket=pass.Record(first.list.Get(),first.request);Assert(bool(first.ticket),"first recorded before second");first.request.inputs[0].owner.reset();owner.reset();
    auto collision=second.request;collision.completion={a.Get(),2};
    Assert(pass.Record(second.list.Get(),collision).status==SrTemporalRecordStatus::InvalidCompletion,"same live fence cannot overtake");
    second.ticket=pass.Record(second.list.Get(),second.request);Assert(bool(second.ticket),"distinct higher foreign fence accepted");
    first.Back(D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);second.Back(D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    Hr(first.list->Close(),"order closeA");Hr(second.list->Close(),"order closeB");ID3D12CommandList* listsB[]{second.list.Get()};
    gpu.q->ExecuteCommandLists(1,listsB);Hr(gpu.q->Signal(b.Get(),100),"order signalB");Assert(pass.SubmissionReported(second.ticket,true,true),"B submitted first");Wait(b.Get(),100);
    Assert(pass.Collect()==1&&pass.BusySlots()==1&&!retained.expired()&&a->GetCompletedValue()==0,"foreign100 never retires unexecuted A1");
    ID3D12CommandList* listsA[]{first.list.Get()};gpu.q->ExecuteCommandLists(1,listsA);Hr(gpu.q->Signal(a.Get(),1),"order signalA");
    Assert(pass.SubmissionReported(first.ticket,true,true),"A submitted after B");Wait(a.Get(),1);Assert(pass.Collect()==1&&retained.expired(),"A exact fence retires owner");
    float maximum=0;Assert(Compare(Oracle(gpu,oracle,Cases()[61]),first.Values(),maximum)==0,"out-of-order A constants/descriptors survive");
    Assert(Compare(Oracle(gpu,oracle,Cases()[62]),second.Values(),maximum)==0,"out-of-order B correct");
}
int wmain(int argc,wchar_t** argv){try{
    Check(argc>=3,"usage: sr_temporal_motion_warp original.dxbc output-directory [negative]");const fs::path output(argv[2]);fs::create_directories(output);const bool negative=argc>3;
    Device gpu;auto original=gpu.Pipeline(argv[1]);SrTemporalMotionPass pass;
    Assert(pass.Prepare(gpu.d.Get())==SrTemporalPrepareStatus::Ready,"production Prepare");
    Assert(pass.Prepare(gpu.d.Get())==SrTemporalPrepareStatus::Ready,"cached Prepare");Assert(pass.PipelineStats().shaderCompileCalls==1&&pass.PipelineStats().pipelineCreateCalls==1,"single compile/PSO");
    std::ofstream rows(output/"cases.jsonl"),raw(output/"values.f32",std::ios::binary);unsigned cases=0,failed=0;float globalMaximum=0;
    for(const auto& test:Cases()){
        const auto expected=Run(gpu,original.Get(),test);const auto actual=Production(gpu,pass,test,negative);float maximum=0;const auto bad=Compare(expected,actual,maximum);
        rows<<"{\"name\":\""<<test.name<<"\",\"mismatches\":"<<bad<<",\"max_absolute_error\":"<<maximum<<"}\n";
        for(unsigned i=0;i<4;++i){raw.write(reinterpret_cast<const char*>(expected[i].data()),std::streamsize(expected[i].size()*4));raw.write(reinterpret_cast<const char*>(actual[i].data()),std::streamsize(actual[i].size()*4));}
        ++cases;failed+=bad!=0;globalMaximum=std::max(globalMaximum,maximum);
    }
    if(!negative){Assert(recordCalls==63,"all63 comparisons invoke production Record");Guards(gpu,pass);
        // Actual clipped optional UAVs and alternate allowed input read state.
        for(bool clipped:{false,true}){const auto test=Sized(Cases()[44],32,24);float maximum=0;
            Assert(Compare(Oracle(gpu,original.Get(),test,32,24,clipped),Production(gpu,pass,test,false,32,24,clipped,true),maximum)==0,"clipped/generic read oracle");}
        for(auto state:{D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_COMMON}){
            float maximum=0;const auto test=Cases()[60];Assert(Compare(Oracle(gpu,original.Get(),test),Production(gpu,pass,test,false,32,24,false,false,state,true),maximum)==0,"output state/optional t8 original oracle");}
        Lifetime(gpu,pass,original.Get());OutOfOrder(gpu,pass,original.Get());Assert(pass.TryShutdown(),"successful drained shutdown");
    }
    for(UINT64 i=0;i<gpu.info->GetNumStoredMessages();++i){SIZE_T size{};Hr(gpu.info->GetMessage(i,nullptr,&size),"final message size");std::vector<char> data(size);auto* message=reinterpret_cast<D3D12_MESSAGE*>(data.data());Hr(gpu.info->GetMessage(i,message,&size),"final message");Assert(message->Severity>D3D12_MESSAGE_SEVERITY_ERROR,"final D3D12 debug error");}
    std::cout<<"cases="<<cases<<" failures="<<failed<<" max_absolute_error="<<globalMaximum<<" assertions="<<assertions<<" production_record_calls="<<recordCalls<<" debug_errors=0 adapter=WARP game=not-used\n";
    return failed?1:0;
}catch(const std::exception& error){std::cerr<<"ERROR "<<error.what()<<'\n';return 2;}}
