#include "Framegen/SrHistoryOwner.hpp"
#include "Framegen/SrReferenceDefaults.hpp"
#include "SrOwnerNativeFixture.hpp"
#define wmain UnusedSrOracleMain
#include "gpu_compare.cpp"
#undef wmain
#include "SrOwnerImageFixture.hpp"
#include "NativeFgFixture.hpp"
using namespace cvr::framegen;
void Log(const char*,...){}
static SrSourceStamp Source(int64_t id){
    SrSourceStamp s;s.source={1,uint64_t(id+1),1,{1,1,1},id,(id&1)?AerV2Eye::Right:AerV2Eye::Left,1,1000+id,false};
    s.poseId=uint64_t(id+100);s.writeId=uint64_t(id+200);s.origin=s.captureClockGeneration=1;
    s.locatedPoseTimeNs=1000+id;s.nativeFrame=uint32_t(id+50);s.engineView=1;s.poseKnown=s.nativeFrameKnown=true;
    const int32_t centre[3]{};const float q[4]{0,0,0,1};
    s.appliedEye=cvr::camera::MakeAppliedEyeReference(centre,centre,q,cvr::camera::AppliedEyeRoute::Component,true,true);
    s.selectedFingerprint=s.appliedEye.writtenEye;return s;
}
static CameraConstants Constants(){
    CameraConstants c;c.source=CameraConstantsSource::Streamline;c.version=2;c.valuePresent.set();c.flagPresent.set();c.flags={1,1,0,0,0,0,0};
    c.values[0]=c.values[5]=1;c.values[10]=-.02f/(16000-.02f);c.values[11]=1;c.values[14]=.02f*16000/(16000-.02f);
    c.values[16]=c.values[21]=1;c.values[27]=1/c.values[14];c.values[30]=1;c.values[31]=-c.values[10]/c.values[14];
    for(unsigned i=0;i<4;++i)c.values[48+i*5]=c.values[64+i*5]=1;
    c.values[91]=c.values[92]=c.values[96]=1;c.values[98]=.02f;c.values[99]=16000;return c;
}
template<class Owner>static void Observe(Owner& owner,SrTagCaptureToken token,ID3D12GraphicsCommandList* list,ID3D12Resource* to,ID3D12Resource* from){
    // The same fixture compiles against the pre-fix implementation; absence of
    // provenance then fails the real routing assertion, not compilation.
    if constexpr(requires{owner.ObserveTaggedCopy(token,list,to,from);})owner.ObserveTaggedCopy(token,list,to,from);
}
template<class Owner>static void CheckGeneratedGuideLease(Owner& owner,const SrSourceStamp& source){
    if constexpr(requires{owner.AcquireFrameGenerationGuide(source);}){
        auto guide=owner.AcquireFrameGenerationGuide(source);
        Check(bool(guide),"corrected SR guide was not exported after GPU submission");
        Check(guide->history[2].key.source==source.source&&guide->history[0].key.source.sourceId==source.source.sourceId-2,
            "guide history is not the exact same-eye predecessor");
        Check(guide->history[0].key.source.physicalEye==source.source.physicalEye,"guide crossed eyes");
        Check(guide->motion&&guide->depth&&guide->fence&&guide->queue&&guide->fence->GetCompletedValue()>=guide->fenceValue,
            "guide lacks its real producer completion");
        auto wrong=source;++wrong.poseId;Check(!owner.AcquireFrameGenerationGuide(wrong),"guide matched only a source counter");
        Check(!owner.AcquireFrameGenerationGuide(Source(1)),"warmup source falsely exported corrected motion");
        Check(owner.BeforeHostShutdown()==SrNativeRetireResult::NotReady,"retirement ignored a live guide consumer");
        guide.reset();
    }else Check(false,"SR has no exact-source retained FG guide export");
}
static void Wait(Device&gpu){
    Hr(gpu.q->Signal(gpu.fence.Get(),++gpu.sequence),"signal");HANDLE event=CreateEventW(nullptr,FALSE,FALSE,nullptr);
    Check(event!=nullptr,"event");Hr(gpu.fence->SetEventOnCompletion(gpu.sequence,event),"completion");
    const auto wait=WaitForSingleObject(event,30000);CloseHandle(event);Check(wait==WAIT_OBJECT_0,"GPU timeout");
}
enum class TaggedCopyCase {Observed,Unobserved,Overwritten,Expired,FailedTag};
static void Exercise(Device&gpu,TaggedCopyCase which,bool fg=false){
    SrHistoryOwner owner([](IUnknown*p,REFIID iid,void**out){return p?p->QueryInterface(iid,out):E_POINTER;});
    SrSourceStamp fgSource;if(fg)ConfigureFg(owner,[&](const NVSDK_NGX_Parameter*){return fgSource;});
    owner.SetRequested(true);owner.SetConfiguration(sr::ReferenceCp2077SuperSamplingDefaults(true),{true,false,false});
    ownerCreates=0;ownerCalls.clear();ownerConsumer={};const auto context=OwnerContext();OwnerParameters params;NVSDK_NGX_Handle* handle{};
    ComPtr<ID3D12CommandAllocator> allocator;ComPtr<ID3D12GraphicsCommandList> list;
    Hr(gpu.d->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&allocator)),"allocator");
    Hr(gpu.d->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,allocator.Get(),nullptr,IID_PPV_ARGS(&list)),"list");
    auto submit=[&]{Hr(list->Close(),"close");ID3D12CommandList* lists[]{list.Get()};const auto claim=owner.BeforeSubmit(gpu.q.Get(),1,lists);
        gpu.q->ExecuteCommandLists(1,lists);owner.Submitted(claim,gpu.q.Get(),1,lists);Wait(gpu);owner.Collect();
        Hr(allocator->Reset(),"allocator reset");Hr(list->Reset(allocator.Get(),nullptr),"list reset");owner.ResetList(list.Get());};
    Check(owner.Create(context,list.Get(),NVSDK_NGX_Feature_SuperSampling,&params,&handle)==NVSDK_NGX_Result_Success,"create");submit();
    auto fgContext=context;fgContext.featureId=11;fgContext.createOrdinal=5;fgContext.originals={FgCreate,FgEvaluate,OwnerRelease};
    NVSDK_NGX_Handle* fgHandle{};FgParameters fgParams;
    if(fg){fgCreates=fgCalls=fgEyeCalls[0]=fgEyeCalls[1]=0;
        Check(owner.Create(fgContext,list.Get(),static_cast<NVSDK_NGX_Feature>(11),&fgParams,&fgHandle)==NVSDK_NGX_Result_Success&&fgCreates==2,"FG companion context missing");submit();}
    OwnerImage depth(gpu.d.Get(),1,false),motion(gpu.d.Get(),2,false),color(gpu.d.Get(),4,false),output(gpu.d.Get(),4,true,true);
    OwnerImage depthClone(gpu.d.Get(),1,false),motionClone(gpu.d.Get(),2,false),unrelated(gpu.d.Get(),2,false);
    params.color=color.resource.Get();params.output=output.resource.Get();params.depth=depthClone.resource.Get();params.motion=motionClone.resource.Get();
    const unsigned frames=fg?6:3;
    for(unsigned frame=0;frame<frames;++frame){
        if(frame)for(auto* r:{&depth,&motion,&color,&output,&depthClone,&motionClone,&unrelated})
            Barrier(list.Get(),r->resource.Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_COPY_DEST);
        for(auto* r:{&depth,&motion,&color,&output,&depthClone,&motionClone,&unrelated})
            r->Upload(list.Get(),[&](unsigned,unsigned,unsigned){return r==&depth?.5f:r==&motion?.03f+.01f*frame:.25f;},D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        const auto source=Source(frame);owner.ObserveConstants({Constants(),source,0,700+frame,0,true,true,true});
        const SrSnapshotTag tags[]{{depth.resource.Get(),0,64,0,0,0,0,1,true},{motion.resource.Get(),1,64,0,0,0,0,1,true},
            {color.resource.Get(),3,64,0,0,0,0,1,true},{output.resource.Get(),4,64,0,0,0,0,1,true}};
        const auto token=owner.BeginTags(list.Get(),0,true,source,tags);Check(bool(token),"capture");
        if(which==TaggedCopyCase::Expired)owner.EndTags(token,0);
        auto copy=[&](ID3D12Resource* to,ID3D12Resource* from){
            D3D12_RESOURCE_BARRIER b[2]{};for(auto& x:b){x.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;x.Transition.Subresource=D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;x.Transition.StateBefore=D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;}
            b[0].Transition.pResource=from;b[0].Transition.StateAfter=D3D12_RESOURCE_STATE_COPY_SOURCE;
            b[1].Transition.pResource=to;b[1].Transition.StateAfter=D3D12_RESOURCE_STATE_COPY_DEST;
            list->ResourceBarrier(2,b);list->CopyResource(to,from);
            if(which!=TaggedCopyCase::Unobserved)Observe(owner,token,list.Get(),to,from);
            for(auto& x:b)std::swap(x.Transition.StateBefore,x.Transition.StateAfter);list->ResourceBarrier(2,b);
        };
        copy(depthClone.resource.Get(),depth.resource.Get());copy(motionClone.resource.Get(),motion.resource.Get());
        if(which==TaggedCopyCase::Overwritten)copy(motionClone.resource.Get(),unrelated.resource.Get());
        if(which!=TaggedCopyCase::Expired)owner.EndTags(token,which==TaggedCopyCase::FailedTag?1u:0u);
        const auto scope=owner.BeginSlEvaluation({source,0,700+frame,0,true,true,true,list.Get(),SrSlStateContract::StreamlineOriginalEvaluate});
        Check(owner.Evaluate(context,list.Get(),handle,&params,nullptr)==NVSDK_NGX_Result_Success,"evaluate");owner.EndSlEvaluation(scope,0);
        Check(params.motion==motionClone.resource.Get()&&params.depth==depthClone.resource.Get(),"host params changed");submit();
        if(fg&&frame>=2){
            fgSource=source;fgParams.pointers["DLSSG.Backbuffer"]=color.resource.Get();
            fgParams.pointers["DLSSG.MVecs"]=motionClone.resource.Get();fgParams.pointers["DLSSG.Depth"]=depthClone.resource.Get();
            const auto constants=Constants();const char* keys[]{"DLSSG.CameraViewToClip","DLSSG.ClipToCameraView","DLSSG.ClipToLensClip","DLSSG.ClipToPrevClip","DLSSG.PrevClipToClip"};
            for(unsigned i=0;i<5;++i){std::copy_n(constants.values.data()+16*i,16,fgParams.matrices[i].data());fgParams.pointers[keys[i]]=fgParams.matrices[i].data();}
            const auto original=fgParams.pointers;
            Check(owner.Evaluate(fgContext,list.Get(),fgHandle,&fgParams,nullptr)==NVSDK_NGX_Result_Success,"native FG result");
            Check(fgParams.pointers==original&&fgParams.integers["DLSSG.Reset"]==0&&fgParams.floats["DLSSG.CameraFOV"]==90,"FG parameter bag not restored");submit();
        }
    }
    const auto status=owner.Status();
    std::cout<<"case="<<unsigned(which)<<" corrected="<<status.correctedEvaluations<<" left="<<status.successfulNativeByEye[0]<<" right="<<status.successfulNativeByEye[1]<<'\n';
    if(which==TaggedCopyCase::Observed){Check(status.correctedEvaluations==frames-2,"observed full tag copies did not preserve SR correction");
        Check(status.successfulNativeByEye==std::array<uint64_t,2>{(frames+1)/2,frames/2},"tag clones bypassed eye routing");
        if(fg)Check(fgCalls==4&&fgEyeCalls[0]==2&&fgEyeCalls[1]==2,"FG eye counts");else CheckGeneratedGuideLease(owner,Source(2));}
    else Check(status.correctedEvaluations==0,"unproven or overwritten clone was accepted");
    if(fg)Check(owner.Release(fgContext,fgHandle)==NVSDK_NGX_Result_Success,"FG release");
    Check(owner.Release(context,handle)==NVSDK_NGX_Result_Success,"release");
    Check(owner.BeforeHostShutdown()==SrNativeRetireResult::Retired,"retirement");
}
int wmain(){try{Device gpu;Exercise(gpu,TaggedCopyCase::Observed);Exercise(gpu,TaggedCopyCase::Unobserved);Exercise(gpu,TaggedCopyCase::Overwritten);Exercise(gpu,TaggedCopyCase::Expired);Exercise(gpu,TaggedCopyCase::FailedTag);Exercise(gpu,TaggedCopyCase::Observed,true);
    ComPtr<ID3D12InfoQueue> info;if(SUCCEEDED(gpu.d.As(&info)))for(UINT64 i=0;i<info->GetNumStoredMessages();++i){SIZE_T size{};info->GetMessage(i,nullptr,&size);std::vector<unsigned char> data(size);auto* message=reinterpret_cast<D3D12_MESSAGE*>(data.data());info->GetMessage(i,message,&size);if(message->Severity<=D3D12_MESSAGE_SEVERITY_ERROR){std::cerr<<message->pDescription<<'\n';throw std::runtime_error("D3D12 debug error");}}
    std::cout<<"PASS tag-copy provenance and rejection controls; GPU debug errors=0\n";return 0;
}catch(const std::exception&e){std::cerr<<e.what()<<'\n';return 1;}}
