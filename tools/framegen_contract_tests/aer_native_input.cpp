#include "Framegen/NativeGenerationBridge.hpp"
#include "CpuD3d.hpp"
#include "Render/SrgbToLinearPass.hpp"
#include <dxgi1_4.h>
#include <d3d12sdklayers.h>
#include <cmath>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string_view>
#include <vector>
using namespace cvr::framegen;
using Microsoft::WRL::ComPtr;
void Log(const char*,...) {} // Render helper diagnostics; validation uses the D3D12 info queue.
static unsigned checks{};
static void Check(bool value,const char* why){++checks;if(!value)throw std::runtime_error(why);}
static void Barrier(ID3D12GraphicsCommandList* list,ID3D12Resource* resource,D3D12_RESOURCE_STATES before,D3D12_RESOURCE_STATES after){
    D3D12_RESOURCE_BARRIER b{};b.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;b.Transition={resource,D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,before,after};list->ResourceBarrier(1,&b);
}
struct Commands {
    ComPtr<ID3D12CommandAllocator> allocator;ComPtr<ID3D12GraphicsCommandList> list;
    explicit Commands(ID3D12Device* device){
        Check(SUCCEEDED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&allocator))),"allocator failed");
        Check(SUCCEEDED(device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,allocator.Get(),nullptr,IID_PPV_ARGS(&list))),"command list failed");
    }
};
struct Context {
    bool warp{};ComPtr<CpuDevice> cpu;
    ComPtr<ID3D12Device> device;ComPtr<ID3D12CommandQueue> queue;ComPtr<ID3D12Fence> fence;
    ComPtr<ID3D12InfoQueue> info;uint64_t sequence=5;unsigned releasedColors{};
    explicit Context(bool useWarp=false):warp(useWarp){
        if(!warp){
            cpu.Attach(new CpuDevice);device=cpu->Native();queue.Attach(new CpuQueue(cpu.Get()));
            fence.Attach(new CpuFence(cpu.Get()));fence->Signal(sequence);return;
        }
        ComPtr<ID3D12Debug> debug;
        if(FAILED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug))))throw std::runtime_error("NOT_RUN D3D12 debug layer unavailable");
        debug->EnableDebugLayer();ComPtr<IDXGIFactory4> factory;ComPtr<IDXGIAdapter1> adapter;
        Check(SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))&&SUCCEEDED(factory->EnumWarpAdapter(IID_PPV_ARGS(&adapter))),"WARP adapter missing");
        DXGI_ADAPTER_DESC1 desc{};Check(SUCCEEDED(adapter->GetDesc1(&desc))&&(desc.Flags&DXGI_ADAPTER_FLAG_SOFTWARE),"hardware adapter refused");
        Check(SUCCEEDED(D3D12CreateDevice(adapter.Get(),D3D_FEATURE_LEVEL_11_0,IID_PPV_ARGS(&device))),"WARP device failed");
        D3D12_COMMAND_QUEUE_DESC q{};q.Type=D3D12_COMMAND_LIST_TYPE_DIRECT;
        Check(SUCCEEDED(device->CreateCommandQueue(&q,IID_PPV_ARGS(&queue))),"WARP queue failed");
        Check(SUCCEEDED(device->CreateFence(sequence,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&fence))),"WARP fence failed");
        Check(SUCCEEDED(device.As(&info)),"WARP info queue unavailable");
    }
    void Submit(Commands& commands){
        Check(SUCCEEDED(commands.list->Close()),"command close failed");ID3D12CommandList* lists[]{commands.list.Get()};
        queue->ExecuteCommandLists(1,lists);++sequence;Check(SUCCEEDED(queue->Signal(fence.Get(),sequence)),"producer signal failed");
        HANDLE event=CreateEventW(nullptr,FALSE,FALSE,nullptr);Check(event!=nullptr,"fence event failed");
        if(fence->GetCompletedValue()<sequence)Check(SUCCEEDED(fence->SetEventOnCompletion(sequence,event))&&WaitForSingleObject(event,5000)==WAIT_OBJECT_0,"fence completion failed");
        CloseHandle(event);Check(fence->GetCompletedValue()!=UINT64_MAX&&fence->GetCompletedValue()>=sequence,"device removed/incomplete");
    }
    ComPtr<ID3D12Resource> Buffer(uint64_t size,D3D12_HEAP_TYPE type){
        D3D12_HEAP_PROPERTIES heap{};heap.Type=type;D3D12_RESOURCE_DESC d{};d.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;
        d.Width=size;d.Height=d.DepthOrArraySize=d.MipLevels=d.SampleDesc.Count=1;d.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        ComPtr<ID3D12Resource> out;Check(SUCCEEDED(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&d,
            type==D3D12_HEAP_TYPE_UPLOAD?D3D12_RESOURCE_STATE_GENERIC_READ:D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&out))),"buffer failed");return out;
    }
    ComPtr<ID3D12Resource> Texture(DXGI_FORMAT format,uint32_t width,uint32_t height,D3D12_RESOURCE_STATES state,float motion=.0f){
        D3D12_RESOURCE_DESC d{};d.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;d.Width=width;d.Height=height;
        d.DepthOrArraySize=d.MipLevels=d.SampleDesc.Count=1;d.Format=format;ComPtr<ID3D12Resource> out;
        if(!warp){auto* p=new CpuResource(cpu.Get());p->descriptor=d;out.Attach(p);return out;}
        D3D12_HEAP_PROPERTIES heap{};heap.Type=D3D12_HEAP_TYPE_DEFAULT;
        Check(SUCCEEDED(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&d,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&out))),"texture failed");
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};UINT rows{};UINT64 rowBytes{},bytes{};device->GetCopyableFootprints(&d,0,1,0,&fp,&rows,&rowBytes,&bytes);
        auto upload=Buffer(bytes,D3D12_HEAP_TYPE_UPLOAD);void* mapped{};D3D12_RANGE empty{};
        Check(SUCCEEDED(upload->Map(0,&empty,&mapped)),"upload map failed");
        for(unsigned y=0;y<height;++y)for(unsigned x=0;x<width;++x){
            auto* pixel=static_cast<unsigned char*>(mapped)+fp.Offset+y*fp.Footprint.RowPitch+x*(format==DXGI_FORMAT_R32G32_FLOAT?8:4);
            if(format==DXGI_FORMAT_R32G32_FLOAT){const float value[]{motion,0};std::memcpy(pixel,value,8);}
            else if(format==DXGI_FORMAT_R32_FLOAT){const float value=.5f;std::memcpy(pixel,&value,4);}
            else{pixel[0]=128;pixel[1]=64;pixel[2]=32;pixel[3]=255;}
        }
        upload->Unmap(0,nullptr);Commands cmd(device.Get());D3D12_TEXTURE_COPY_LOCATION src{},dst{};
        src.pResource=upload.Get();src.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;src.PlacedFootprint=fp;
        dst.pResource=out.Get();dst.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        cmd.list->CopyTextureRegion(&dst,0,0,0,&src,nullptr);Barrier(cmd.list.Get(),out.Get(),D3D12_RESOURCE_STATE_COPY_DEST,state);Submit(cmd);return out;
    }
    void CheckDebug(){
        if(!info)return;
        for(UINT64 i=0;i<info->GetNumStoredMessages();++i){SIZE_T bytes{};info->GetMessage(i,nullptr,&bytes);std::vector<unsigned char> storage(bytes);
            auto* m=reinterpret_cast<D3D12_MESSAGE*>(storage.data());info->GetMessage(i,m,&bytes);
            if(m->Severity==D3D12_MESSAGE_SEVERITY_ERROR||m->Severity==D3D12_MESSAGE_SEVERITY_CORRUPTION)throw std::runtime_error(m->pDescription);
        }
    }
};
struct Capture {
    Context& context;Frame frame;std::shared_ptr<Inputs> input;ComPtr<ID3D12Resource> color,depth,motion;unsigned eye{};
    Capture(Context& c,int64_t source,uint32_t token):context(c),eye(unsigned(source&1)){
        color=c.Texture(DXGI_FORMAT_R8G8B8A8_UNORM_SRGB,32,16,CaptureColorReadState);
        depth=c.Texture(DXGI_FORMAT_R32_FLOAT,16,8,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        motion=c.Texture(DXGI_FORMAT_R32G32_FLOAT,16,8,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,.01f*float(source+1));
        frame.identity.serial=50+source;frame.identity.origin=7;frame.identity.nativeFrame=token;
        // Intentionally not a stereo pair, and no fabricated second inputs flag.
        frame.identity.stereo=frame.identity.inputs=false;frame.color[eye]=color.Get();frame.engineView[eye]=1;
        frame.identity.pose[eye]=100+source;frame.pose[eye].orientation.w=1;frame.pose[eye].position.x=eye?.032f:-.032f;
        frame.fov[eye]={-.8f,.8f,.8f,-.8f};frame.nativeStamp.resourceEpoch=3;frame.nativeStamp.transfer=CaptureTransfer::SrgbEncoded;
        frame.lease=std::make_shared<ReadLease>();frame.lease->owner=&c;frame.lease->resources[eye]=color.Get();color->AddRef();
        frame.lease->release=[](void* owner,ID3D12Resource* resource,uint64_t){++static_cast<Context*>(owner)->releasedColors;resource->Release();};
        LARGE_INTEGER captureQpc{};captureQpc.QuadPart=2000+source*100;
        frame.colorCompletion=CaptureCompletion::AfterSignal(c.fence.Get(),c.sequence,S_OK,captureQpc,1000);
        input=std::make_shared<Inputs>();frame.inputs[eye]=input;input->depth=depth;input->motion=motion;input->readyFence=c.fence;input->fenceValue=c.sequence;
        input->queue=c.queue.Get();input->poseId=100+source;input->poseWriteId=200+source;input->origin=7;input->frameId=token;input->view=1;
        input->width=input->motionWidth=16;input->height=input->motionHeight=8;input->hasDepth=input->hasMotion=input->submitted=input->evaluated=true;
        input->clockGeneration=11;input->cameraApplicationQpc=1000+source*100;input->contentTimeNs=1'000'000'000+source*10'000'000;
        input->posePredictionNs=input->contentTimeNs+20'000'000;
        auto& stamp=frame.nativeStamp.eyes[eye];stamp={frame.identity.serial,input->poseId,input->poseWriteId,7,11,input->cameraApplicationQpc,
            input->contentTimeNs,input->posePredictionNs,1,token,true};
        // Source/display fit owner generation is NOT the QPC->XR mapping generation.
        stamp.mainEyeSource={1,uint64_t(source+1),2,{3,9,7},source,AerV2Eye(eye),23,input->posePredictionNs,true};
        auto& camera=input->camera.constants;camera.source=CameraConstantsSource::Streamline;camera.version=2;
        camera.valuePresent.set();camera.flagPresent.set();camera.flags={0,1,0,0,0,0,1};auto& p=camera.values;
        p[0]=p[5]=1;p[10]=1.0001f;p[11]=1;p[14]=-.10001f;p[16]=p[21]=1;p[27]=-9.999f;p[30]=1;p[31]=10;
        for(unsigned base:{32u,48u,64u})for(unsigned i=0;i<16;++i)p[base+i]=i%5==0?1.f:0.f;
        p[82]=p[83]=1;p[86]=frame.pose[eye].position.x;p[90]=p[92]=p[97]=1;p[98]=.1f;p[99]=1000;p[100]=113.7f;p[101]=1;p[103]=40;
    }
    NativeAlternatingInputResult Adapt(const NativeAlternatingInputResult& same={},const NativeAlternatingInputResult& immediate={},uint64_t epoch=90){
        return AdaptAlternatingMainInput(context.device.Get(),context.queue.Get(),frame,cpvr::Eye(eye),same,immediate,epoch);
    }
};
static cpvr::MotionGuideSample Sample(const cpvr::GenerationInput& input){return {input.frame->guides,input.frame->source_id,
    input.native_frame_token,input.previous_native_frame_token,input.guides_previous_source_id,input.frame->content_time_ns,input.guides_previous_time_ns,input.motion_convention};}
static cpvr::MotionRecordRequest Motion(const NativeAlternatingInputResult& previous,const NativeAlternatingInputResult& current){
    cpvr::MotionRecordRequest out;out.previous=Sample(previous.input);out.current=Sample(current.input);
    if(const auto& m=current.input.immediate_previous;m.frame)out.middle=cpvr::MotionGuideSample{m.frame->guides,m.frame->source_id,
        m.native_frame_token,m.previous_native_frame_token,m.guides_previous_source_id,m.frame->content_time_ns,m.guides_previous_time_ns,m.motion_convention};
    return out;
}
static void Admission(Context& c){
    for(int64_t id:{0,1}){
        Capture f(c,id,0);const auto result=f.Adapt();Check(bool(result),"one stamped MAIN eye rejected");
        Check(result.input.frame->eye==cpvr::Eye(id)&&result.input.frame->source_id==uint64_t(id+1),"worker source ID/physical eye mapping changed");
        Check(result.sourceStamp.mainEyeSource==f.frame.nativeStamp.eyes[f.eye].mainEyeSource,"immutable AER receipt changed");
        Check(result.input.native_frame_token==0&&result.input.frame->stereo_pair_id==0,"native token changed or fake stereo pair minted");
        Check(result.input.frame->content_time_ns==f.input->contentTimeNs&&result.sourceStamp.posePredictionNs==f.input->posePredictionNs&&
            result.input.frame->content_time_ns!=result.sourceStamp.posePredictionNs,"prediction/capture time substituted for source content time");
        Check(result.input.frame->color.texture.Get()==f.color.Get()&&result.input.frame->color.lifetime==f.frame.lease&&
            result.input.frame->color.ready_fence.Get()==c.fence.Get()&&result.input.frame->color.ready_value==f.frame.colorCompletion.fenceValue,"color fence/lease lost");
        Check(result.input.frame->guides->camera_flags==f.input->camera.constants.flags,"measured camera flags changed");
        for(unsigned i=0;i<80;++i)Check(result.input.frame->guides->camera[i]==f.input->camera.constants.values[i],"measured matrices changed");
        Check(!AdaptNativeInputs(c.device.Get(),c.queue.Get(),f.frame),"paired adapter accepted a one-eye source");
        cpvr::GenerationWorker worker;cpvr::GenerationPolicy policy;policy.generated_per_interval=0;policy.cadence=cpvr::Cadence::AlternateEyes;
        Check(cpvr::GenerationWorkerTestAccess::configure(worker,policy),"real-only worker policy failed");
        cpvr::GenerationWorkerTestAccess::enable_submission(worker,c.device.Get(),90,7,true);
        Check(worker.submit(result.input)&&worker.status().accepted_inputs==1,"actual worker submit rejected valid single eye/pair_id0");
        worker.request_history_reset();Check(cpvr::GenerationWorkerTestAccess::apply_history_reset(worker),"worker test ingress did not retire");
    }
}
static void CpuGuards(){
    Context c;
    auto reject=[&](auto change,NativeBridgeCode code,const char* why){Capture f(c,0,7);change(f);auto result=f.Adapt();Check(!result&&result.code==code,why);};
    reject([](Capture& f){f.frame.nativeStamp.eyes[f.eye].mainEyeSource={};},NativeBridgeCode::MissingIdentity,"missing MAIN latch admitted");
    reject([](Capture& f){f.frame.nativeStamp.eyes[f.eye].mainEyeSource.physicalEye=AerV2Eye::Right;},NativeBridgeCode::MissingIdentity,"wrong physical eye admitted");
    reject([](Capture& f){f.frame.nativeStamp.eyes[f.eye].mainEyeSource.epochs.gameWorld=0;},NativeBridgeCode::MissingIdentity,"unknown source owner admitted");
    reject([](Capture& f){f.input->view=f.frame.engineView[f.eye]=f.frame.nativeStamp.eyes[f.eye].engineView=2;},NativeBridgeCode::MissingIdentity,"VRCAM relabeled MAIN");
    reject([](Capture& f){f.frame.nativeStamp.eyes[f.eye].contentTimeNs=0;},NativeBridgeCode::MissingCameraTime,"unknown content time admitted");
    reject([](Capture& f){f.input->posePredictionNs=f.frame.nativeStamp.eyes[f.eye].posePredictionNs=0;},NativeBridgeCode::MissingPoseTime,"unknown pose time admitted");
    reject([](Capture& f){f.input->camera.constants.valuePresent.reset(48);},NativeBridgeCode::IncompleteCamera,"unknown temporal matrix admitted");
    reject([](Capture& f){f.frame.colorCompletion=CaptureCompletion::AfterSignal(f.context.fence.Get(),5,E_FAIL,{1},1000);},NativeBridgeCode::MissingProducerFence,"failed color Signal admitted");
    reject([](Capture& f){f.input->fenceValue=0;},NativeBridgeCode::MissingProducerFence,"missing guide fence value admitted");
    reject([](Capture& f){f.frame.lease->resources[f.eye]->Release();f.frame.lease->resources[f.eye]=nullptr;},NativeBridgeCode::MissingIdentity,"unreserved color admitted");
    reject([](Capture& f){static_cast<CpuResource*>(f.motion.Get())->descriptor.Width=17;},NativeBridgeCode::UnsupportedResource,"actual motion size mismatch admitted");
    reject([](Capture& f){static_cast<CpuResource*>(f.color.Get())->descriptor.Format=DXGI_FORMAT_B8G8R8A8_UNORM;},NativeBridgeCode::UnsupportedResource,"unsupported color format admitted");
    reject([](Capture& f){f.input->queue=nullptr;},NativeBridgeCode::WrongQueue,"unproven source queue admitted");
    {Capture f(c,0,7);ComPtr<CpuQueue> other;other.Attach(new CpuQueue(c.cpu.Get()));
     Check(AdaptAlternatingMainInput(c.device.Get(),other.Get(),f.frame,cpvr::Eye::Left).code==NativeBridgeCode::WrongQueue,"same-device foreign queue admitted");}
    {Capture f(c,0,7);Context other;f.input->depth=other.Texture(DXGI_FORMAT_R32_FLOAT,16,8,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
     Check(f.Adapt().code==NativeBridgeCode::UnsupportedResource,"foreign-device guide admitted");}
    {Capture f(c,0,7);static_cast<CpuFence*>(c.fence.Get())->completed=0;Check(bool(f.Adapt()),"scheduled fence was confused with GPU completion");}
}
static void History(Context& c){
    Capture old(c,0,7),middle(c,1,8),current(c,2,9);
    auto a=old.Adapt(),b=middle.Adapt({},a),d=current.Adapt(a,b);
    Check(bool(a)&&bool(b)&&bool(d),"coherent alternating inputs rejected");
    Check(d.history==NativeAlternatingHistory::SameEyeDelta2&&d.input.immediate_previous.frame,"exact two-native-frame span not linked");
    Check(cpvr::validate_motion_request(Motion(a,d)).valid,"actual worker MotionRecorder contract rejects adapter span");
    Check(d.input.guides_previous_source_id==2&&d.input.immediate_previous.guides_previous_source_id==1&&
          d.input.native_frame_token==9&&d.input.immediate_previous.native_frame_token==8&&d.input.immediate_previous.previous_native_frame_token==7,
          "current-middle-previous IDs/native tokens were fabricated");
    auto changed=a;changed.sourceStamp.mainEyeSource.epochs.gameWorld++;
    auto reset=current.Adapt(changed,b);Check(bool(reset)&&reset.history!=NativeAlternatingHistory::SameEyeDelta2,"world owner mismatch joined history");
    {auto changedFit=b;changedFit.sourceStamp.mainEyeSource.clockGeneration++;
     Check(current.Adapt(a,changedFit).history!=NativeAlternatingHistory::SameEyeDelta2,"changed source/display fit clock joined history");}
    {auto changedQpc=b;changedQpc.sourceStamp.clockGeneration++;
     Check(current.Adapt(a,changedQpc).history!=NativeAlternatingHistory::SameEyeDelta2,"changed QPC->XR clock joined history");}
    {auto missingTime=b;missingTime.sourceStamp.contentTimeNs=0;
     Check(current.Adapt(a,missingTime).history!=NativeAlternatingHistory::SameEyeDelta2,"unknown middle content time joined history");}
    auto resized=a;resized.captureResourceEpoch++;
    Check(current.Adapt(resized,b).history!=NativeAlternatingHistory::SameEyeDelta2,"different actual resource epoch joined history");
    std::weak_ptr<ReadLease> middleLease=middle.frame.lease;
    std::weak_ptr<const Inputs> middleGuides=middle.input;
    b={};middle.frame={};middle.input.reset();
    Check(middleLease.expired()&&!middleGuides.expired()&&!d.input.immediate_previous.frame->color.texture&&
          !d.input.immediate_previous.frame->color.lifetime,"retained middle guides pinned color capture pool or lost guide ownership");
    Capture gap(c,4,11);auto warm=gap.Adapt(a,d);
    Check(bool(warm)&&warm.history!=NativeAlternatingHistory::SameEyeDelta2&&!warm.input.immediate_previous.frame,"gapped motion span fabricated");
    Capture zero(c,0,0),one(c,1,1),two(c,2,2);auto z=zero.Adapt(),o=one.Adapt({},z),t=two.Adapt(z,o);
    Check(bool(z)&&bool(o)&&bool(t)&&t.input.native_frame_token==2&&t.history!=NativeAlternatingHistory::SameEyeDelta2,
          "native zero token remapped to fake MotionRecorder predecessor");
    Capture wrapOld(c,0,0xFFFFFFFEu),wrapMiddle(c,1,0xFFFFFFFFu),wrapCurrent(c,2,0);
    auto wo=wrapOld.Adapt(),wm=wrapMiddle.Adapt({},wo),wc=wrapCurrent.Adapt(wo,wm);
    Check(bool(wc)&&wc.input.native_frame_token==0&&wc.history==NativeAlternatingHistory::SameEyeDelta2,
          "known current native token zero lost a valid modulo32 span");
}
static constexpr PresentationTime Target{1'040'000'000,11'111'111};
static void GuideOnlyHistory(Context& c){
    Capture old(c,0,7),middle(c,1,8),current(c,2,9);
    auto a=old.Adapt(),b=middle.Adapt({},a);std::weak_ptr<ReadLease> oldLease=old.frame.lease,midLease=middle.frame.lease;
    auto previous=AlternatingMainGuideHistory(a),immediate=AlternatingMainGuideHistory(b);
    Check(bool(previous)&&bool(immediate)&&previous.guideHistoryOnly,"history conversion lost proven source receipt");
    a={};b={};old.frame={};middle.frame={};
    Check(oldLease.expired()&&midLease.expired(),"owner history pinned old MAIN capture colors");
    auto d=current.Adapt(previous,immediate);
    Check(bool(d)&&d.history==NativeAlternatingHistory::SameEyeDelta2,"guide-only history cannot prove exact alternating span");
    Check(cpvr::validate_motion_request(Motion(previous,d)).valid,"guide-only old endpoint rejected by MotionRecorder");
}
static void BridgeLifecycle(){
    Context c;cpvr::GenerationWorker worker;NativeGenerationBridge bridge(worker);cpvr::GenerationPolicy policy;
    policy.generated_per_interval=0;policy.cadence=cpvr::Cadence::AlternateEyes;policy.maximum_eye_skew_ns=50'000'000;
    Check(cpvr::GenerationWorkerTestAccess::configure(worker,policy),"bridge fixture policy failed");
    cpvr::GenerationWorkerTestAccess::enable_submission(worker,c.device.Get(),bridge.HistoryEpoch(),7,true);
    Capture left(c,0,7),right(c,1,8);Selection selected;
    Check(!bridge.SelectAlternating(c.device.Get(),c.queue.Get(),left.frame,cpvr::Eye::Left,Target,selected)&&worker.status().accepted_inputs==1,
          "single input did not enter worker or fabricated opposite eye");
    Check(bridge.SelectAlternating(c.device.Get(),c.queue.Get(),right.frame,cpvr::Eye::Right,Target,selected),"worker's actual measured pair not selected");
    Check(worker.status().accepted_inputs==2&&selected.native&&selected.native->selected.frame->eyes[0]->source_id==1&&
          selected.native->selected.frame->eyes[1]->source_id==2,"bridge changed AER worker source identities");
    bridge.Submitted(selected,nullptr,0,false,false,false,0);selected={};
    Check(bridge.SelectAlternating(c.device.Get(),c.queue.Get(),right.frame,cpvr::Eye::Right,Target,selected)&&worker.status().accepted_inputs==2,
          "same immutable MAIN source repeatedly admitted");
    const auto sourcePair=selected.native->selected.frame;
    auto changedPair=std::make_shared<cpvr::StereoFrame>(*sourcePair);
    auto generated=std::make_shared<cpvr::EyeFrame>(*changedPair->eyes[0]);generated->generated=true;generated->color.resting_state=D3D12_RESOURCE_STATE_COMMON;
    changedPair->eyes[0]=generated;selected.native->selected.frame=changedPair;selected.synthetic=true;
    ComPtr<CpuGraphicsList> commands;commands.Attach(new CpuGraphicsList(c.cpu.Get()));
    {NativeReadScope scope(commands.Get(),selected,0,true);Check(bool(scope)&&(scope.ReadState()&D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE),"generated reconstruction input lacks pixel SRV state");}
    Check(commands->barriers.size()==2&&commands->barriers[0].Transition.StateBefore==D3D12_RESOURCE_STATE_COMMON&&
          commands->barriers[1].Transition.StateAfter==D3D12_RESOURCE_STATE_COMMON,"pixel read did not restore generated resting state");
    ComPtr<CpuFence> consumer;consumer.Attach(new CpuFence(c.cpu.Get()));
    bridge.Submitted(selected,consumer.Get(),9,true,true,true,Target.target);selected={};bridge.Collect();
    Check(bridge.Status().retainedConsumers==1,"in-flight alternating consumer released early");
    consumer->Signal(9);bridge.Collect();Check(bridge.Status().retainedConsumers==0,"alternating consumer did not retire at fence");
    const auto epoch=bridge.HistoryEpoch();Frame notReady;
    Check(!bridge.Select(c.device.Get(),c.queue.Get(),notReady,Target,selected)&&bridge.HistoryEpoch()!=epoch&&worker.status().history_reset_pending,
          "alternating-to-paired source-kind switch did not reset history");
    Check(cpvr::GenerationWorkerTestAccess::apply_history_reset(worker),"source-kind reset did not drain");
    const auto pairedEpoch=bridge.HistoryEpoch();
    Check(!bridge.SelectAlternating(c.device.Get(),c.queue.Get(),left.frame,cpvr::Eye::Left,Target,selected)&&bridge.HistoryEpoch()!=pairedEpoch,
          "paired-to-alternating source-kind switch did not reset history");
    Check(cpvr::GenerationWorkerTestAccess::apply_history_reset(worker),"second source-kind reset did not drain");
    bridge.SelectAlternating(c.device.Get(),c.queue.Get(),left.frame,cpvr::Eye::Left,Target,selected);
    const auto beforeWorld=bridge.HistoryEpoch();Capture next(c,2,9);next.frame.nativeStamp.eyes[0].mainEyeSource.epochs.gameWorld=10;
    Check(!bridge.SelectAlternating(c.device.Get(),c.queue.Get(),next.frame,cpvr::Eye::Left,Target,selected)&&bridge.HistoryEpoch()!=beforeWorld,
          "new game-world owner retained alternating history");
    Check(cpvr::GenerationWorkerTestAccess::apply_history_reset(worker),"world reset did not drain");
    const auto admittedBeforeDevice=worker.status().accepted_inputs;
    bridge.SelectAlternating(c.device.Get(),c.queue.Get(),next.frame,cpvr::Eye::Left,Target,selected);
    Check(worker.status().accepted_inputs==admittedBeforeDevice+1,"fresh world source did not enter after reset");
    Context other;
    Check(!bridge.SelectAlternating(other.device.Get(),other.queue.Get(),next.frame,cpvr::Eye::Left,Target,selected)&&
          bridge.Status().code==NativeBridgeCode::DeviceChanged,"changed device retained alternating bridge");
    Check(cpvr::GenerationWorkerTestAccess::apply_history_reset(worker),"device reset did not drain");
}
static float Half(uint16_t h){const auto e=(h>>10)&31,m=h&1023;return std::ldexp(float(e?1024+m:m),int(e?e:1)-25)*(h&0x8000?-1.f:1.f);}
static void WarpMotion(Context& c){
    Capture old(c,0,7),middle(c,1,8),current(c,2,9);
    auto a=old.Adapt(),b=middle.Adapt({},a),d=current.Adapt(a,b);Check(bool(d),"WARP adapter rejected input");
    auto request=Motion(a,d);Check(cpvr::validate_motion_request(request).valid,"WARP delta2 request invalid");
    cpvr::MotionRecorder recorder;Check(recorder.initialize(c.device.Get(),{}),"MotionRecorder initialization failed");
    Commands commands(c.device.Get());auto recorded=recorder.record(commands.list.Get(),request);Check(bool(recorded),"actual MotionRecorder rejected delta2");
    const auto desc=recorded->guides->motion.texture->GetDesc();D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};UINT rows{};UINT64 rowBytes{},bytes{};
    c.device->GetCopyableFootprints(&desc,0,1,0,&fp,&rows,&rowBytes,&bytes);auto readback=c.Buffer(bytes,D3D12_HEAP_TYPE_READBACK);
    D3D12_TEXTURE_COPY_LOCATION src{},dst{};src.pResource=recorded->guides->motion.texture.Get();src.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dst.pResource=readback.Get();dst.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;dst.PlacedFootprint=fp;
    commands.list->CopyTextureRegion(&dst,0,0,0,&src,nullptr);c.Submit(commands);
    void* mapped{};D3D12_RANGE range{0,SIZE_T(bytes)};Check(SUCCEEDED(readback->Map(0,&range,&mapped)),"motion readback failed");
    const auto* values=reinterpret_cast<const uint16_t*>(static_cast<const unsigned char*>(mapped)+fp.Offset+4*fp.Footprint.RowPitch+8*4);
    const float x=Half(values[0]),y=Half(values[1]);readback->Unmap(0,nullptr);
    Check(std::abs(x-.05f)<.0001f&&std::abs(y)<.0001f,"GPU did not compose actual current(.03)+middle(.02) motion");
    std::cout<<"WARP actual MotionRecorder delta2 center="<<x<<","<<y<<" expected=.05,0\n";
    c.CheckDebug();
}
static void WarpPixelRead(Context& c){
    for(const auto initial:{D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_COPY_SOURCE,CaptureColorReadState}){
        auto source=c.Texture(DXGI_FORMAT_R8G8B8A8_UNORM,32,16,initial);
        auto targetDesc=source->GetDesc();targetDesc.Flags=D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        D3D12_HEAP_PROPERTIES heap{};heap.Type=D3D12_HEAP_TYPE_DEFAULT;ComPtr<ID3D12Resource> target;
        Check(SUCCEEDED(c.device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&targetDesc,D3D12_RESOURCE_STATE_RENDER_TARGET,
            nullptr,IID_PPV_ARGS(&target))),"pixel-read target failed");
        auto eye=std::make_shared<cpvr::EyeFrame>();eye->eye=cpvr::Eye::Left;eye->generated=initial!=CaptureColorReadState;
        eye->color.texture=source;eye->color.resting_state=initial;
        auto pair=std::make_shared<cpvr::StereoFrame>();pair->eyes[0]=eye;
        Selection selection;selection.synthetic=eye->generated;selection.frame.color[0]=source.Get();
        selection.native=std::make_shared<NativeSelectionLease>();selection.native->device=c.device;selection.native->selected.frame=pair;
        SrgbToLinearPass pass;Check(pass.EnsureInitialized(c.device.Get(),DXGI_FORMAT_R8G8B8A8_UNORM,32,16),"pixel shader reader initialization failed");
        Commands commands(c.device.Get());
        {NativeReadScope read(commands.list.Get(),selection,0,true);
         Check(bool(read)&&(read.ReadState()&D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE),"pixel shader read state missing on WARP");
         Check(pass.Record(commands.list.Get(),source.Get(),target.Get(),0),"actual pixel shader reader failed");}
        Barrier(commands.list.Get(),target.Get(),D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_COPY_SOURCE);
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};UINT rows{};UINT64 rowBytes{},bytes{};
        c.device->GetCopyableFootprints(&targetDesc,0,1,0,&fp,&rows,&rowBytes,&bytes);auto readback=c.Buffer(bytes,D3D12_HEAP_TYPE_READBACK);
        D3D12_TEXTURE_COPY_LOCATION src{},dst{};src.pResource=target.Get();src.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        dst.pResource=readback.Get();dst.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;dst.PlacedFootprint=fp;
        commands.list->CopyTextureRegion(&dst,0,0,0,&src,nullptr);c.Submit(commands);
        void* mapped{};D3D12_RANGE range{0,SIZE_T(bytes)};Check(SUCCEEDED(readback->Map(0,&range,&mapped)),"pixel readback failed");
        const auto* pixel=static_cast<const unsigned char*>(mapped)+fp.Offset+8*fp.Footprint.RowPitch+16*4;
        const bool expected=std::abs(int(pixel[0])-55)<=1&&std::abs(int(pixel[1])-13)<=1&&std::abs(int(pixel[2])-4)<=1&&pixel[3]==255;
        readback->Unmap(0,nullptr);Check(expected,"actual pixel shader consumed wrong source pixels");c.CheckDebug();
    }
    std::cout<<"WARP pixel SRV read + restore verified for generated COMMON/COPY_SOURCE and real shared-read\n";
}
int main(int argc,char** argv){
    try{const bool warp=argc==2&&std::string_view(argv[1])=="warp";Context context(warp);Admission(context);History(context);GuideOnlyHistory(context);
        if(warp){WarpMotion(context);WarpPixelRead(context);}else{CpuGuards();BridgeLifecycle();}context.CheckDebug();
        std::cout<<"PASS "<<(warp?"WARP/resources/MotionRecorder":"CPU admission/history guards")<<" checks="<<checks<<"\n";return 0;
    }catch(const std::exception& e){std::cerr<<"FAIL "<<e.what()<<"\n";return std::string_view(e.what()).starts_with("NOT_RUN")?77:1;}
}
