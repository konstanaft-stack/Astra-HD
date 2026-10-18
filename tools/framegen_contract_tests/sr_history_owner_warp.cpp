#include "Framegen/SrHistoryOwner.hpp"
#include "Framegen/SrReferenceDefaults.hpp"
#include "SrOwnerNativeFixture.hpp"
#include "SrOwnerFaultQueue.hpp"
#define wmain SrOwnerUnusedOracleMain
#include "gpu_compare.cpp"
#undef wmain
#include "SrOwnerImageFixture.hpp"
using namespace cvr::framegen;
void Log(const char*,...){}
static SrSourceStamp Source(int64_t id){SrSourceStamp s;s.source={1,uint64_t(id+1),1,{1,1,1},id,(id&1)?AerV2Eye::Right:AerV2Eye::Left,1,1000+id,false};
    s.poseId=uint64_t(id+100);s.writeId=uint64_t(id+200);s.origin=s.captureClockGeneration=1;s.locatedPoseTimeNs=1000+id;s.nativeFrame=uint32_t(id+50);s.engineView=1;s.poseKnown=s.nativeFrameKnown=true;
    const int32_t centre[3]{};const float q[4]{0,0,0,1};s.appliedEye=cvr::camera::MakeAppliedEyeReference(centre,centre,q,cvr::camera::AppliedEyeRoute::Component,true,true);
    s.selectedFingerprint=s.appliedEye.writtenEye;return s;}
static CameraConstants Constants(){CameraConstants c;c.source=CameraConstantsSource::Streamline;c.version=2;c.valuePresent.set();c.flagPresent.set();c.flags={1,1,0,0,0,0,0};
    c.values[0]=c.values[5]=1;c.values[10]=-.02f/(16000-.02f);c.values[11]=1;c.values[14]=.02f*16000/(16000-.02f);
    c.values[16]=c.values[21]=1;c.values[27]=1/c.values[14];c.values[30]=1;c.values[31]=-c.values[10]/c.values[14];
    for(unsigned i=0;i<4;++i)c.values[48+i*5]=c.values[64+i*5]=1;
    c.values[91]=c.values[92]=c.values[96]=1;c.values[98]=.02f;c.values[99]=16000;return c;}
static void WaitFor(Device&gpu,ID3D12CommandQueue*queue=nullptr){if(!queue)queue=gpu.q.Get();Hr(queue->Signal(gpu.fence.Get(),++gpu.sequence),"fixture signal");HANDLE event=CreateEventW(nullptr,FALSE,FALSE,nullptr);
    Check(event!=nullptr,"fixture event");Hr(gpu.fence->SetEventOnCompletion(gpu.sequence,event),"fixture completion");const auto result=WaitForSingleObject(event,30000);CloseHandle(event);Check(result==WAIT_OBJECT_0,"fixture timeout");}
static float OriginalOracle(Device&gpu,float currentMotion,float previousMotion,unsigned sourceId,bool actual=false){
    auto parameters=sr::ReferenceCp2077SuperSamplingDefaults(true);auto sample=[&](int64_t id){sr::Sample s;s.sourceId=s.matrixSourceId=id;s.featureKey=s.basisId=1;s.parametersKey=parameters.key;s.epochs={1,1,1,1};
        s.explicitReference=sr::Pose{};s.projection={1,1,1,1};s.nearZ=.02f;s.farZ=16000;s.mvScale=actual?sr::V2{960,960}:sr::V2{32,24};s.depthSize=s.motionSize=s.renderSubrect=actual?sr::U2{960,960}:sr::U2{32,24};s.outputSize=actual?sr::U2{1920,1920}:sr::U2{32,24};s.lowResMv=true;return s;};
    const auto constants=sr::Build({sample(sourceId),sample(int64_t(sourceId)-1),sample(int64_t(sourceId)-2)},parameters);
    Check(constants.status==sr::Status::Ready,"oracle scalar/matrix fixture");
    auto pipeline=gpu.Pipeline(L"<WORKSPACE_ROOT>/research/2026-09-30-cpvr-open-nvidia/audit/luke-eye-cycle-20261003/sr-motion-shader/combined-mv-110-1024.dxbc");
    ComPtr<ID3D12CommandAllocator> allocator;ComPtr<ID3D12GraphicsCommandList> list;Hr(gpu.d->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&allocator)),"oracle allocator");
    Hr(gpu.d->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,allocator.Get(),pipeline.Get(),IID_PPV_ARGS(&list)),"oracle list");
    std::vector<OwnerImage> inputs,outputs;for(unsigned i=0;i<8;++i)inputs.emplace_back(gpu.d.Get(),i<2?2:i<5?1:4,false,false,actual);for(unsigned i=0;i<4;++i)outputs.emplace_back(gpu.d.Get(),i<2?2:4,true,false,actual);
    for(unsigned i=0;i<8;++i)inputs[i].Upload(list.Get(),[&](unsigned,unsigned,unsigned channel){return i==0?currentMotion:i==1?previousMotion:i<5?.5f:channel==3?1.f:.25f;},D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    for(unsigned i=0;i<4;++i)outputs[i].Upload(list.Get(),[&](unsigned,unsigned,unsigned channel){return i<2?currentMotion:channel==3?1.f:.25f;},D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    auto cb=Buffer(gpu.d.Get(),512,D3D12_HEAP_TYPE_UPLOAD);void* mapped{};D3D12_RANGE none{};Hr(cb->Map(0,&none,&mapped),"oracle CB");std::memcpy(mapped,&constants.constants,480);cb->Unmap(0,nullptr);
    ComPtr<ID3D12DescriptorHeap> heap;D3D12_DESCRIPTOR_HEAP_DESC hd{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,13,D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE,0};Hr(gpu.d->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&heap)),"oracle heap");
    const auto stride=gpu.d->GetDescriptorHandleIncrementSize(hd.Type);auto cpu=heap->GetCPUDescriptorHandleForHeapStart();
    for(unsigned i=0;i<9;++i){auto*r=inputs[(std::min)(i,7u)].resource.Get();D3D12_SHADER_RESOURCE_VIEW_DESC v{};v.Format=inputs[(std::min)(i,7u)].View();v.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D;v.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;v.Texture2D.MipLevels=1;gpu.d->CreateShaderResourceView(r,&v,cpu);cpu.ptr+=stride;}
    for(auto& output:outputs){D3D12_UNORDERED_ACCESS_VIEW_DESC v{};v.Format=output.resource->GetDesc().Format;v.ViewDimension=D3D12_UAV_DIMENSION_TEXTURE2D;gpu.d->CreateUnorderedAccessView(output.resource.Get(),nullptr,&v,cpu);cpu.ptr+=stride;}
    ID3D12DescriptorHeap* heaps[]{heap.Get()};list->SetDescriptorHeaps(1,heaps);list->SetComputeRootSignature(gpu.root.Get());list->SetComputeRootConstantBufferView(0,cb->GetGPUVirtualAddress());auto gpuHandle=heap->GetGPUDescriptorHandleForHeapStart();list->SetComputeRootDescriptorTable(1,gpuHandle);gpuHandle.ptr+=uint64_t(stride)*9;list->SetComputeRootDescriptorTable(2,gpuHandle);list->Dispatch(actual?120:4,actual?120:3,1);
    outputs[0].CopyBack(list.Get());gpu.Finish(list.Get());return outputs[0].ReadMotion(4,4);
}
enum class DeltaFixture {Supplied,Absent,Gap};
static std::filesystem::path evidenceTestRoot;
static void FullCorrection(Device&gpu,bool crossQueue=false,bool failWait=false,bool actual=false,bool voidResources=false,DeltaFixture deltaFixture=DeltaFixture::Supplied){SrHistoryOwner owner([](IUnknown*p,REFIID iid,void**out){return p?p->QueryInterface(iid,out):E_POINTER;});owner.SetRequested(true);
    owner.SetConfiguration(sr::ReferenceCp2077SuperSamplingDefaults(true),{true,false,false});
    if(actual){D3D12_FEATURE_DATA_FORMAT_SUPPORT support{DXGI_FORMAT_R11G11B10_FLOAT};Hr(gpu.d->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT,&support,sizeof(support)),"R11 capability");std::cout<<"R11 typedUav="<<bool(support.Support1&D3D12_FORMAT_SUPPORT1_TYPED_UNORDERED_ACCESS_VIEW)<<" store="<<bool(support.Support2&D3D12_FORMAT_SUPPORT2_UAV_TYPED_STORE)<<" load="<<bool(support.Support2&D3D12_FORMAT_SUPPORT2_UAV_TYPED_LOAD)<<" originalOracle="<<OriginalOracle(gpu,.05f,.04f,2,true)<<'\n';}
    ownerCreates=0;ownerCalls.clear();const auto context=OwnerContext();OwnerParameters params;params.voidResources=voidResources;NVSDK_NGX_Handle* handle{};
    ComPtr<ID3D12CommandAllocator> allocator;ComPtr<ID3D12GraphicsCommandList> list;Hr(gpu.d->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&allocator)),"full allocator");
    Hr(gpu.d->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,allocator.Get(),nullptr,IID_PPV_ARGS(&list)),"full list");
    Check(owner.Create(context,list.Get(),NVSDK_NGX_Feature_SuperSampling,&params,&handle)==NVSDK_NGX_Result_Success&&ownerCreates==2,"full owner pair creation");
    ComPtr<ID3D12CommandQueue> queue2;D3D12_COMMAND_QUEUE_DESC qd{};qd.Type=D3D12_COMMAND_LIST_TYPE_DIRECT;Hr(gpu.d->CreateCommandQueue(&qd,IID_PPV_ARGS(&queue2)),"second queue");
    ID3D12CommandQueue* activeQueue=gpu.q.Get();
    ComPtr<SrFaultQueue> faulty;faulty.Attach(new SrFaultQueue(nullptr));faulty->device=gpu.d;faulty->target=queue2;faulty->failWait=failWait;
    auto submit=[&](){Hr(list->Close(),"full close");ID3D12CommandList* lists[]{list.Get()};const auto submitted=owner.BeforeSubmit(activeQueue,1,lists);activeQueue->ExecuteCommandLists(1,lists);owner.Submitted(submitted,activeQueue,1,lists);WaitFor(gpu,activeQueue);owner.Collect();};
    submit();Hr(allocator->Reset(),"full reset allocator");Hr(list->Reset(allocator.Get(),nullptr),"full reset list");owner.ResetList(list.Get());
    OwnerImage depth(gpu.d.Get(),1,false,false,actual),motion(gpu.d.Get(),2,false,false,actual),color(gpu.d.Get(),4,false,false,actual),output(gpu.d.Get(),4,true,true,actual);
    params.width=depth.width;params.height=depth.height;params.scaleX=float(depth.width);params.scaleY=float(depth.height);
    params.color=color.resource.Get();params.depth=depth.resource.Get();params.motion=motion.resource.Get();params.output=output.resource.Get();
    auto readback=Buffer(gpu.d.Get(),uint64_t(depth.width)*depth.height*16,D3D12_HEAP_TYPE_READBACK);DXGI_FORMAT readbackFormat{};D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};uint64_t readbackBytes{};
    ownerConsumer=[&](ID3D12GraphicsCommandList*cl,ID3D12Resource*resource){if(resource==motion.resource.Get())return;const auto desc=resource->GetDesc();readbackFormat=desc.Format;
        gpu.d->GetCopyableFootprints(&desc,0,1,0,&footprint,nullptr,nullptr,&readbackBytes);Barrier(cl,resource,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_COPY_SOURCE);
        D3D12_TEXTURE_COPY_LOCATION a{},b{};a.pResource=resource;a.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;b.pResource=readback.Get();b.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;b.PlacedFootprint=footprint;
        cl->CopyTextureRegion(&b,0,0,0,&a,nullptr);Barrier(cl,resource,D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);};
    for(unsigned frame=0;frame<(failWait?3u:7u);++frame){if(frame==4){ownerConsumer={};owner.SetRequested(false);
            for(unsigned i=0;i<3;++i){owner.Evaluate(context,nullptr,handle,&params,nullptr);const auto call=ownerCalls.back();Check(call.handle==0x1110&&call.reset==(i==0?1:0)&&params.reset==0,"off transition did not reset plain original exactly once");}
            owner.SetRequested(true);}
        if(frame){Hr(allocator->Reset(),"frame allocator");Hr(list->Reset(allocator.Get(),nullptr),"frame list");owner.ResetList(list.Get());
            for(auto* image:{&depth,&motion,&color,&output})Barrier(list.Get(),image->resource.Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_COPY_DEST);}
        activeQueue=crossQueue&&frame>=2?queue2.Get():gpu.q.Get();
        if(failWait&&frame>=2)activeQueue=faulty.Get();
        // Distinct supplied values expose whether absence incorrectly overwrites
        // the reference's last-success cache with zero/a previous-source guess.
        constexpr float supplied[]{11,13,999,17,21,23,29};
        params.deltaPresent=deltaFixture!=DeltaFixture::Absent&&(deltaFixture!=DeltaFixture::Gap||frame!=2);
        params.delta=deltaFixture==DeltaFixture::Gap?supplied[frame]:16.f;
        const auto deltaSetsBefore=params.deltaSets;
        depth.Upload(list.Get(),[](unsigned,unsigned,unsigned){return .5f;},D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        motion.Upload(list.Get(),[&](unsigned,unsigned,unsigned){return .03f+.01f*frame;},D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        for(auto* image:{&color,&output})image->Upload(list.Get(),[](unsigned,unsigned,unsigned c){return c==3?1.f:.25f;},D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        const auto source=Source(frame);owner.ObserveConstants({Constants(),source,0,700+frame,0,true,true,true});
        const SrSnapshotTag tags[]{{depth.resource.Get(),0,64,0,0,0,0,1,true},{motion.resource.Get(),1,64,0,0,0,0,1,true},
            {color.resource.Get(),3,64,0,0,0,0,1,true},{output.resource.Get(),4,64,0,0,0,0,1,true}};
        const auto capture=owner.BeginTags(list.Get(),0,true,source,tags);Check(bool(capture),"full source capture");owner.EndTags(capture,0);
        const auto scope=owner.BeginSlEvaluation({source,0,700+frame,0,true,true,true,list.Get(),SrSlStateContract::StreamlineOriginalEvaluate});
        Check(owner.Evaluate(context,list.Get(),handle,&params,nullptr)==NVSDK_NGX_Result_Success,"full native result");
        const auto historyBeforeEnd=owner.Status().historyEpoch;
        // Real game trace: native NGX succeeds, then SL reports budget warning39.
        // The next eye must retain the three-source history and corrected GPU MV.
        owner.EndSlEvaluation(scope,frame==2?39u:0u);
        Check(owner.Status().historyEpoch==historyBeforeEnd,"successful evaluation budget warning discarded live stereo history");
        const auto call=ownerCalls.back();std::cout<<"native source="<<frame<<" handle="<<std::hex<<call.handle<<std::dec<<" reset="<<call.reset<<" deltaKnown="<<call.deltaKnown<<" delta="<<call.delta<<" private="<<(call.motion!=motion.resource.Get())<<" stage="<<uint32_t(owner.Status().stage)<<'\n';
        Check(call.handle==uintptr_t(frame&1?0x2220:0x1110),"native handle not selected from physical source eye");
        const bool warm=frame<2||frame==4||frame==5;
        const float expectedDelta=deltaFixture==DeltaFixture::Gap?(warm?supplied[frame]:(frame==3?30.f:52.f)):(warm?16.f:32.f);
        Check(call.reset==(warm?1:0)&&call.deltaKnown==params.deltaPresent&&(!call.deltaKnown||call.delta==expectedDelta),"warmup/reset or optional delta success-only cache wrong");
        Check(owner.Status().lastDeltaKnown==call.deltaKnown,"status invented a delta for absent native key");
        Check(params.motion==motion.resource.Get()&&params.reset==0&&params.delta==(deltaFixture==DeltaFixture::Gap?supplied[frame]:16.f),"native parameter bag was not restored");
        Check(params.deltaSets==deltaSetsBefore+(call.deltaKnown&&!warm?2u:0u),"absent delta synthesized or present delta restoration count wrong");
        Check((call.motion!=motion.resource.Get())==!warm,"three actual sources did not produce correction");
        if(!evidenceTestRoot.empty()&&(frame==2||frame==3))
            Check(!std::filesystem::exists(evidenceTestRoot/("source"+std::to_string(frame)+"-eye"+std::to_string(frame&1)+".json")),"SR evidence published before GPU submission/completion");
        submit();
        if(frame>=2&&frame<4){void* bytes{};D3D12_RANGE range{0,size_t(readbackBytes)};Hr(readback->Map(0,&range,&bytes),"composite readback");float mv{};
            if(readbackFormat==DXGI_FORMAT_R16G16_FLOAT){uint16_t half{};std::memcpy(&half,static_cast<char*>(bytes)+4*footprint.Footprint.RowPitch+4*4,2);mv=OwnerFloat(half);}else std::memcpy(&mv,static_cast<char*>(bytes)+4*footprint.Footprint.RowPitch+4*8,sizeof(mv));D3D12_RANGE none{};readback->Unmap(0,&none);
            const float expected=OriginalOracle(gpu,.03f+.01f*frame,.03f+.01f*(frame-1),frame,actual);std::cout<<"combinedMV="<<mv<<" originalDXIL="<<expected<<'\n';Check(std::abs(mv-expected)<.00002f,"actual private-copy/adapter/pass/native-consumer pipeline differs from original DXIL");}
    }
    if(failWait){ownerConsumer={};Check(faulty->executes==1&&faulty->waits>=2,"Wait failure skipped original Execute or did not exercise cross queue dependencies");
        Check(owner.Status().quiescing&&owner.Status().submissionFailures>=2&&FAILED(owner.Status().lastSubmissionResult),"Wait failure disappeared behind successful native/Signal results");
        Check(owner.Release(context,handle)==NVSDK_NGX_Result_Success&&owner.BeforeHostShutdown()==SrNativeRetireResult::Retired,"completed failed-Wait work did not retire");return;}
    ownerConsumer={};Check(owner.Status().correctedEvaluations==3,"full correction count");
    Check(owner.Status().successfulNativeByEye==std::array<uint64_t,2>{4,3},"successful native routing counters do not reflect actual source eyes");
    Check(owner.Status().lastSelectedHandle==0x1110&&owner.Status().lastReset==0&&owner.Status().lastDeltaKnown==(deltaFixture!=DeltaFixture::Absent)&&
        (deltaFixture==DeltaFixture::Absent||owner.Status().lastDeltaMsec==(deltaFixture==DeltaFixture::Gap?52.f:32.f))&&owner.Status().lastPhysicalEye==0,"last routing snapshot not tied to observed native call");
    Check(crossQueue?owner.Status().crossQueueWaits>=2:owner.Status().crossQueueWaits==0,"same/cross queue dependency protocol");
    for(unsigned invalid=0;invalid<8;++invalid){OwnerParameters wrong=params;wrong.deltaPresent=true;switch(invalid){case 0:wrong.delta=std::numeric_limits<float>::quiet_NaN();break;
        case 1:wrong.delta=std::numeric_limits<float>::infinity();break;case 2:wrong.jitterX=std::numeric_limits<float>::quiet_NaN();break;
        case 3:wrong.scaleX=0;break;case 4:wrong.reset=2;break;case 5:wrong.width=0;break;case 6:wrong.height=0;break;case 7:wrong.scaleX=(std::numeric_limits<float>::max)();break;}
        const auto scope=owner.BeginSlEvaluation({Source(6),0,706,0,true,true,true,list.Get(),SrSlStateContract::StreamlineOriginalEvaluate});
        const auto before=owner.Status().correctedEvaluations;owner.Evaluate(context,list.Get(),handle,&wrong,nullptr);owner.EndSlEvaluation(scope,0);
        Check(owner.Status().stage==SrHistoryStage::NativeParametersUnknown&&owner.Status().missingNativeParameters==(1ull<<63),"all-success Get with invalid values passed admission");
        Check(owner.Status().correctedEvaluations==before&&wrong.motion==params.motion&&ownerCalls.back().reset==wrong.reset,"invalid numeric changed native parameters or recorded correction");
    }
    Check(owner.Release(context,handle)==NVSDK_NGX_Result_Success,"full pair release");Check(owner.BeforeHostShutdown()==SrNativeRetireResult::Retired,"full pipeline retirement");
}
static void SubmitResetClaim(Device&gpu,bool failSignal=false,bool failList=false,bool failReset=false){
    bool failListResolve=false;
    SrHistoryOwner owner([&](IUnknown*p,REFIID iid,void**out){if(failListResolve&&iid==__uuidof(ID3D12GraphicsCommandList)){*out=nullptr;return E_NOINTERFACE;}return p?p->QueryInterface(iid,out):E_POINTER;});owner.SetRequested(true);
    owner.SetConfiguration(sr::ReferenceCp2077SuperSamplingDefaults(true),{true,false,false});
    ComPtr<ID3D12CommandAllocator> first,second,third;ComPtr<ID3D12GraphicsCommandList> list;
    for(auto*p:{std::addressof(first),std::addressof(second),std::addressof(third)})Hr(gpu.d->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(p->GetAddressOf())),"claim allocator");
    Hr(gpu.d->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,first.Get(),nullptr,IID_PPV_ARGS(&list)),"claim list");
    Texture depth(gpu.d.Get(),1,false),motion(gpu.d.Get(),2,false),color(gpu.d.Get(),4,false),output(gpu.d.Get(),4,true);
    Texture depth2(gpu.d.Get(),1,false),motion2(gpu.d.Get(),2,false),color2(gpu.d.Get(),4,false),output2(gpu.d.Get(),4,true);
    const auto capture=[&](unsigned frame,Texture&d,Texture&m,Texture&c,Texture&o){
        for(auto*image:{&d,&m,&c,&o})image->Upload(list.Get(),[](unsigned,unsigned,unsigned){return .2f;},D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        const auto source=Source(frame);
        if(frame==0){for(unsigned i=0;i<63;++i)owner.ObserveConstants({Constants(),Source(1000+i),0,10000+i,0,true,true,true});
            owner.ObserveConstants({Constants(),source,0,699,0,true,true,true});}
        owner.ObserveConstants({Constants(),source,0,700+frame,0,true,true,true});
        const SrSnapshotTag tags[]{{d.resource.Get(),0,64,0,0,32,24,1,true},{m.resource.Get(),1,64,0,0,32,24,1,true},{c.resource.Get(),3,64,0,0,32,24,1,true},{o.resource.Get(),4,64,0,0,32,24,1,true}};
        const auto token=owner.BeginTags(list.Get(),0,true,source,tags);Check(bool(token),"claim source capture");owner.EndTags(token,0);
    };
    capture(0,depth,motion,color,output);Hr(list->Close(),"claim close");
    if(failReset){Hr(list->Reset(second.Get(),nullptr),"host reset before failing owner resolver");failListResolve=true;owner.ResetList(list.Get());failListResolve=false;
        Hr(list->Close(),"close new empty recording");ID3D12CommandList*fresh[]{list.Get()};const auto token=owner.BeforeSubmit(gpu.q.Get(),1,fresh);gpu.q->ExecuteCommandLists(1,fresh);owner.Submitted(token,gpu.q.Get(),1,fresh);WaitFor(gpu);owner.Collect();
        Check(owner.Status().pendingProducers==1&&owner.Status().quiescing&&owner.BeforeHostShutdown()==SrNativeRetireResult::NotReady,"failed Reset resolver later published discarded old recording");return;}
    ComPtr<ID3D12Fence> gate;Hr(gpu.d->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&gate)),"claim gate");Hr(gpu.q->Wait(gate.Get(),1),"block old execution");
    ComPtr<SrFaultQueue> fault;fault.Attach(new SrFaultQueue(nullptr));fault->device=gpu.d;fault->target=gpu.q;fault->failSignal=failSignal;
    ID3D12CommandQueue* queue=failSignal?static_cast<ID3D12CommandQueue*>(fault.Get()):gpu.q.Get();ID3D12CommandList* lists[]{list.Get()};
    failListResolve=failList;const auto submitted=owner.BeforeSubmit(queue,1,lists);failListResolve=false;Check(bool(submitted),"submit claim missing");queue->ExecuteCommandLists(1,lists);
    Hr(list->Reset(second.Get(),nullptr),"legal reset after Execute before Submitted");owner.ResetList(list.Get());
    Check(owner.Status().pendingProducers==1,"Reset cancelled claimed generation while GPU was blocked");
    if(failList){owner.Submitted(submitted,queue,1,lists);Hr(gate->Signal(1),"release unresolved-list work");WaitFor(gpu);owner.Collect();
        Check(owner.Status().pendingProducers==1&&owner.Status().quiescing&&owner.Status().lastSubmissionResult==E_NOINTERFACE,"recovered resolver/Reset freed uncertain executed source");
        Check(owner.BeforeHostShutdown()==SrNativeRetireResult::NotReady,"unresolved list submission falsely retired");return;}
    capture(1,depth2,motion2,color2,output2);Check(owner.Status().pendingProducers==2,"new generation not independently recorded");
    owner.Submitted(submitted,queue,1,lists);Hr(gate->Signal(1),"release old GPU work");WaitFor(gpu);owner.Collect();
    Check(owner.Status().pendingProducers==(failSignal?2u:1u),"old post callback signaled newer unexecuted generation");
    Hr(list->Close(),"new unexecuted close");Hr(list->Reset(third.Get(),nullptr),"new generation discard");owner.ResetList(list.Get());
    if(failSignal){Check(owner.Status().pendingProducers==1&&owner.Status().quiescing&&FAILED(owner.Status().lastSubmissionResult),"unknown Signal freed lifetime after Reset/completion");
        Check(owner.BeforeHostShutdown()==SrNativeRetireResult::NotReady,"missing Signal falsely retired host owner");}
    else Check(owner.BeforeHostShutdown()==SrNativeRetireResult::Retired,"claimed old completion plus cancelled new work failed retirement");
}
static void MoreThan64RetiredLists(Device&gpu){SrHistoryOwner owner([](IUnknown*p,REFIID iid,void**out){return p?p->QueryInterface(iid,out):E_POINTER;});owner.SetRequested(true);
    owner.SetConfiguration(sr::ReferenceCp2077SuperSamplingDefaults(true),{true,false,false});std::vector<ComPtr<ID3D12CommandAllocator>> allocators;std::vector<ComPtr<ID3D12GraphicsCommandList>> lists;
    for(unsigned i=0;i<96;++i){ComPtr<ID3D12CommandAllocator>a;ComPtr<ID3D12GraphicsCommandList>l;Hr(gpu.d->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&a)),"many allocator");
        Hr(gpu.d->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,a.Get(),nullptr,IID_PPV_ARGS(&l)),"many list");const auto scope=owner.BeginSlEvaluation({Source(i),0,700+i,0,true,true,true,l.Get(),SrSlStateContract::StreamlineOriginalEvaluate});
        Check(bool(scope),"bounded valid scope missing");owner.EndSlEvaluation(scope,0);allocators.push_back(a);lists.push_back(l);}
    auto&list=lists.back();Texture d(gpu.d.Get(),1,false),m(gpu.d.Get(),2,false),c(gpu.d.Get(),4,false),o(gpu.d.Get(),4,true);
    for(auto*image:{&d,&m,&c,&o})image->Upload(list.Get(),[](unsigned,unsigned,unsigned){return .2f;},D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    const auto source=Source(96);owner.ObserveConstants({Constants(),source,0,796,0,true,true,true});const SrSnapshotTag tags[]{{d.resource.Get(),0,64,0,0,32,24,1,true},{m.resource.Get(),1,64,0,0,32,24,1,true},{c.resource.Get(),3,64,0,0,32,24,1,true},{o.resource.Get(),4,64,0,0,32,24,1,true}};
    const auto tag=owner.BeginTags(list.Get(),0,true,source,tags);Check(bool(tag),"retired list identities permanently exhausted table64");owner.EndTags(tag,0);
    ComPtr<ID3D12CommandAllocator> discard;Hr(gpu.d->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&discard)),"many discard allocator");
    Hr(list->Close(),"many close");Hr(list->Reset(discard.Get(),nullptr),"many discard");owner.ResetList(list.Get());Check(owner.BeforeHostShutdown()==SrNativeRetireResult::Retired,"many list cleanup");
}
static void CheckDebugQueue(Device&gpu){
    for(UINT64 i=0;i<gpu.info->GetNumStoredMessages();++i){SIZE_T size{};gpu.info->GetMessage(i,nullptr,&size);std::vector<char> bytes(size);auto* message=reinterpret_cast<D3D12_MESSAGE*>(bytes.data());gpu.info->GetMessage(i,message,&size);
        if(message->Severity<=D3D12_MESSAGE_SEVERITY_ERROR){std::cerr<<message->pDescription<<'\n';throw std::runtime_error("D3D12 debug error");}}
}
#ifdef CVR_SR_EVIDENCE_TEST
static void EvidenceCapture(Device&gpu){
    evidenceTestRoot=std::filesystem::current_path()/("evidence-"+std::to_string(GetCurrentProcessId()));
    SetEnvironmentVariableW(L"CPVR_SR_PIXEL_CAPTURE_DIR",evidenceTestRoot.c_str());
    SetEnvironmentVariableW(L"CPVR_SR_PIXEL_CAPTURE_FIRST_SOURCE",L"2");
    FullCorrection(gpu,false,false,true,true);
    SetEnvironmentVariableW(L"CPVR_SR_PIXEL_CAPTURE_DIR",nullptr);
    SetEnvironmentVariableW(L"CPVR_SR_PIXEL_CAPTURE_FIRST_SOURCE",nullptr);
    for(unsigned source=2;source<=3;++source){
        const auto prefix=evidenceTestRoot/("source"+std::to_string(source)+"-eye"+std::to_string(source&1));
        Check(std::filesystem::exists(prefix.wstring()+L".json"),"SR completed input/output evidence missing");
        Check(std::filesystem::file_size(prefix.wstring()+L".cb480")==480,"SR constant evidence is incomplete");
        for(const auto* suffix:{L"-cur_mv_snapshot.raw",L"-prev_mv_snapshot.raw",L"-cur_depth_snapshot.raw",L"-prev_depth_snapshot.raw",L"-old_depth_snapshot.raw",L"-corrected_mv.raw",L"-current_color.raw",L"-native_mv.raw",L"-native_depth.raw"})
            Check(std::filesystem::file_size(prefix.wstring()+suffix)==960ull*960*4,"SR evidence is padded, partial or wrong extent");
        const auto load=[&](const wchar_t* suffix){std::ifstream f(prefix.wstring()+suffix,std::ios::binary);return std::vector<char>(std::istreambuf_iterator<char>(f),{});};
        Check(load(L"-cur_mv_snapshot.raw")==load(L"-native_mv.raw"),"tag/native MV capture mismatch in identical-input fixture");
        Check(load(L"-cur_depth_snapshot.raw")==load(L"-native_depth.raw"),"tag/native depth capture mismatch in identical-input fixture");
        const auto motionBytes=load(L"-corrected_mv.raw");uint16_t bits{};std::memcpy(&bits,motionBytes.data()+(4*960+4)*4,2);
        const auto expected=OriginalOracle(gpu,.03f+.01f*source,.03f+.01f*(source-1),source,true);
        Check(std::abs(OwnerFloat(bits)-expected)<.00002f,"saved SR output is not the native consumer's corrected image");
    }
    unsigned manifests=0;for(const auto&entry:std::filesystem::directory_iterator(evidenceTestRoot))if(entry.path().extension()==".json")++manifests;
    Check(manifests==2,"SR diagnostic exceeded one attempt per eye");
    std::cout<<"PASS source-keyed SR GPU evidence, nine actual-format surfaces per eye, completed fence, bounded capture\n";
    evidenceTestRoot.clear();
}
#endif
static ID3D12GraphicsCommandList* fgHostInitList{};
static bool fgPrivateInitSeen{};
static NVSDK_NGX_Result NVSDK_CONV InitFixtureCreate(ID3D12GraphicsCommandList* list,NVSDK_NGX_Feature feature,NVSDK_NGX_Parameter* p,NVSDK_NGX_Handle** h){
    if(!ownerCreates)fgHostInitList=list;else fgPrivateInitSeen=list&&list!=fgHostInitList;
    return OwnerCreate(list,feature,p,h);
}
static void FgCompanionDoesNotAwaitUnsubmittedHostCreate(Device& gpu){
    ownerCreates=0;fgPrivateInitSeen=false;OwnerParameters params;
    SrHistoryOwner owner([](IUnknown* p,REFIID iid,void** out){return p?p->QueryInterface(iid,out):E_POINTER;});
    Check(owner.ConfigureFrameGeneration([](const NVSDK_NGX_Parameter*){return SrSourceStamp{};}),"configure FG owner");
    auto context=OwnerContext();context.featureId=11;context.originals.create=InitFixtureCreate;
    ComPtr<ID3D12CommandAllocator> allocator;ComPtr<ID3D12GraphicsCommandList> list;
    Hr(gpu.d->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&allocator)),"FG host allocator");
    Hr(gpu.d->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,allocator.Get(),nullptr,IID_PPV_ARGS(&list)),"FG host list");
    NVSDK_NGX_Handle* handle{};
    Check(owner.Create(context,list.Get(),static_cast<NVSDK_NGX_Feature>(11),&params,&handle)==NVSDK_NGX_Result_Success,"FG create");
    Check(ownerCreates==2&&fgPrivateInitSeen,"FG companion borrowed unsubmitted host initialization list");
    Check(owner.Status().livePasses==0,"FG initialization falsely remains in flight");
    Check(owner.Release(context,handle)==NVSDK_NGX_Result_Success,"FG cannot retire before host Create list submission");
    Check(owner.BeforeHostShutdown()==SrNativeRetireResult::Retired,"FG init resources did not retire");
    Hr(list->Close(),"close deliberately unsubmitted host list");
}
int wmain(){try{Device gpu;FgCompanionDoesNotAwaitUnsubmittedHostCreate(gpu);
#ifdef CVR_SR_EVIDENCE_TEST
EvidenceCapture(gpu);CheckDebugQueue(gpu);return 0;
#endif
#ifdef CVR_SR_ACTUAL_FORMATS
FullCorrection(gpu,false,false,true,true);FullCorrection(gpu,false,false,true,true,DeltaFixture::Absent);FullCorrection(gpu,false,false,true,true,DeltaFixture::Gap);CheckDebugQueue(gpu);std::cout<<"PASS actual CP formats39/34/26 960x960->1920x1920 void-resource API supplied/absent/gap delta debug_errors=0\n";return 0;
#endif
FullCorrection(gpu);FullCorrection(gpu,true,false,false,true);FullCorrection(gpu,true,true);SubmitResetClaim(gpu);SubmitResetClaim(gpu,true);SubmitResetClaim(gpu,false,true);SubmitResetClaim(gpu,false,false,true);MoreThan64RetiredLists(gpu);SrHistoryOwner owner([](IUnknown* p,REFIID iid,void**out){return p?p->QueryInterface(iid,out):E_POINTER;});
    owner.SetRequested(true);owner.SetConfiguration(sr::ReferenceCp2077SuperSamplingDefaults(true),{true,false,false});
    ComPtr<ID3D12CommandAllocator> allocator;ComPtr<ID3D12GraphicsCommandList> list;Hr(gpu.d->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&allocator)),"allocator");
    Hr(gpu.d->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,allocator.Get(),nullptr,IID_PPV_ARGS(&list)),"list");
    Texture depth(gpu.d.Get(),1,false),motion(gpu.d.Get(),2,false),color(gpu.d.Get(),4,false),output(gpu.d.Get(),4,true);
    for(auto* image:{&depth,&motion,&color,&output})image->Upload(list.Get(),[](unsigned,unsigned,unsigned){return .1f;},D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    const auto source=Source(0);owner.ObserveConstants({Constants(),source,0,700,0,true,true,true});
    const SrSnapshotTag unknownState{motion.resource.Get(),1,UINT32_MAX,0,0,32,24,1,true};
    Check(!owner.BeginTags(list.Get(),0,true,source,std::span(&unknownState,1)),"unknown source state accepted before private GPU copy");
    Check(owner.Status().pendingProducers==0,"unknown state recorded a producer command");
    const SrSnapshotTag tags[]{{depth.resource.Get(),0,64,0,0,32,24,1,true},{motion.resource.Get(),1,64,0,0,32,24,1,true},
        {color.resource.Get(),3,64,0,0,32,24,1,true},{output.resource.Get(),4,64,0,0,32,24,1,true}};
    const auto token=owner.BeginTags(list.Get(),0,true,source,tags);Check(bool(token),"private SR snapshots were not staged on actual tag list");
    Check(owner.Status().capturedSources==0,"capture published before original tag success");owner.EndTags(token,0);
    Check(owner.Status().capturedSources==1&&owner.Status().snapshotCells==1,"successful tags did not publish bounded private source snapshot");
    Hr(list->Close(),"close");ID3D12CommandList* lists[]{list.Get()};const auto submitted=owner.BeforeSubmit(gpu.q.Get(),1,lists);gpu.q->ExecuteCommandLists(1,lists);owner.Submitted(submitted,gpu.q.Get(),1,lists);WaitFor(gpu);owner.Collect();
    const auto rotationEpoch=owner.Status().historyEpoch;
    ComPtr<ID3D12CommandAllocator> allocator2;ComPtr<ID3D12GraphicsCommandList> list2;Hr(gpu.d->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&allocator2)),"allocator2");
    Hr(gpu.d->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,allocator2.Get(),nullptr,IID_PPV_ARGS(&list2)),"list2");
    Texture depth2(gpu.d.Get(),1,false),motion2(gpu.d.Get(),2,false),color2(gpu.d.Get(),4,false),output2(gpu.d.Get(),4,true);
    for(auto* image:{&depth2,&motion2,&color2,&output2})image->Upload(list2.Get(),[](unsigned,unsigned,unsigned){return .2f;},D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    const auto source2=Source(1);owner.ObserveConstants({Constants(),source2,0,701,0,true,true,true});
    const SrSnapshotTag rotated[]{{depth2.resource.Get(),0,64,0,0,32,24,1,true},{motion2.resource.Get(),1,64,0,0,32,24,1,true},
        {color2.resource.Get(),3,64,0,0,32,24,1,true},{output2.resource.Get(),4,64,0,0,32,24,1,true}};
    const auto rotatedToken=owner.BeginTags(list2.Get(),0,true,source2,rotated);owner.EndTags(rotatedToken,0);
    Check(owner.Status().capturedSources==2&&owner.Status().historyEpoch==rotationEpoch,"rotating resources with identical descriptors reset temporal history");
    Hr(list2->Close(),"close2");ID3D12CommandList* lists2[]{list2.Get()};const auto submitted2=owner.BeforeSubmit(gpu.q.Get(),1,lists2);gpu.q->ExecuteCommandLists(1,lists2);owner.Submitted(submitted2,gpu.q.Get(),1,lists2);WaitFor(gpu);owner.Collect();
    const auto epoch=owner.Status().historyEpoch;const SrSnapshotTag revoke{nullptr,1,0,0,0,0,0,1,true};
    auto revocation=owner.BeginTags(nullptr,0,true,{},std::span(&revoke,1));Check(bool(revocation),"CPU-only no-list revoke was discarded");
    Check(owner.Status().historyEpoch==epoch,"revoke committed before original result");owner.EndTags(revocation,1);Check(owner.Status().historyEpoch==epoch,"failed original revoke invalidated mapping");
    revocation=owner.BeginTags(nullptr,0,true,{},std::span(&revoke,1));owner.EndTags(revocation,0);Check(owner.Status().historyEpoch>epoch,"successful no-list revoke retained old source mapping");
    Check(owner.BeforeHostShutdown()==SrNativeRetireResult::Retired,"completed private capture prevented clean host retirement");
    CheckDebugQueue(gpu);
    std::cout<<"PASS SR owner private capture/revoke WARP; debug_errors=0\n";return 0;
}catch(const std::exception&e){std::cerr<<"FAIL SR owner WARP: "<<e.what()<<'\n';return 1;}}
