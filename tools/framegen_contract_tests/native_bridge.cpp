#include "Framegen/NativeGenerationBridge.hpp"
#include "Framegen/ColorRead.hpp"
#include "Framegen/OwnedGenerationPolicy.hpp"
#include "Render/NativeObjectIdentity.hpp"
#include "CpuD3d.hpp"
#include "Runtimes/CaptureBufferLeases.hpp"
#include <cmath>
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <string_view>
#include <thread>
using namespace cvr::framegen;
using Microsoft::WRL::ComPtr;
static void Check(bool value,const char* why) {if(!value)throw std::runtime_error(why);}
struct Fixture {
    ComPtr<CpuDevice> device;ComPtr<CpuQueue> queue;ComPtr<CpuFence> producer,consumer;
    ComPtr<CpuResource> colors[2],depth[2],motion[2];std::shared_ptr<Inputs> inputs[2];
    Frame frame;unsigned pinsReleased{};
    static constexpr int64_t Time=1'000'000'000;
    Fixture() {
        device.Attach(new CpuDevice);queue.Attach(new CpuQueue(device.Get()));
        producer.Attach(new CpuFence(device.Get()));consumer.Attach(new CpuFence(device.Get()));producer->Signal(5);
        auto texture=[&](ComPtr<CpuResource>& target,DXGI_FORMAT format,unsigned width,unsigned height) {
            target.Attach(new CpuResource(device.Get()));auto& d=target->descriptor;
            d.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;d.Width=width;d.Height=height;
            d.DepthOrArraySize=d.MipLevels=1;d.SampleDesc.Count=1;d.Format=format;
        };
        frame.identity.serial=20;frame.identity.origin=7;frame.identity.nativeFrame=10;
        frame.identity.stereo=frame.identity.inputs=true;
        frame.colorCompletion=CaptureCompletion::AfterSignal(producer.Get(),5,S_OK,{900},1000);
        frame.nativeStamp.resourceEpoch=3;frame.nativeStamp.transfer=CaptureTransfer::SrgbEncoded;
        frame.nativeStamp.renderPublicationTimeNs=Time+2'000'000;frame.nativeStamp.publicationClockGeneration=11;
        frame.lease=std::make_shared<ReadLease>();frame.lease->owner=this;
        frame.lease->release=[](void* owner,ID3D12Resource* resource,uint64_t) {
            ++static_cast<Fixture*>(owner)->pinsReleased;resource->Release();
        };
        for(unsigned eye=0;eye<2;++eye) {
            texture(colors[eye],DXGI_FORMAT_R8G8B8A8_UNORM_SRGB,32,16);
            texture(depth[eye],DXGI_FORMAT_R32_FLOAT,16,8);texture(motion[eye],DXGI_FORMAT_R16G16B16A16_FLOAT,16,8);
            frame.color[eye]=colors[eye].Get();frame.engineView[eye]=eye+1;
            frame.lease->resources[eye]=colors[eye].Get();colors[eye]->AddRef();
            frame.pose[eye].orientation.w=1;frame.pose[eye].position.x=eye?.032f:-.032f;
            frame.fov[eye]={-.8f,.8f,.8f,-.8f};frame.identity.pose[eye]=100+eye;
            auto input=std::make_shared<Inputs>();inputs[eye]=input;frame.inputs[eye]=input;
            input->depth=depth[eye];input->motion=motion[eye];input->readyFence=producer;input->fenceValue=5;
            input->queue=queue.Get();input->poseId=100+eye;input->poseWriteId=200+eye;input->origin=7;input->frameId=10;input->view=eye+1;
            input->width=input->motionWidth=16;input->height=input->motionHeight=8;
            input->hasDepth=input->hasMotion=input->submitted=input->evaluated=true;
            input->clockGeneration=11;input->cameraApplicationQpc=1000+eye;
            input->contentTimeNs=Time+int64_t(eye)*1'000'000;input->posePredictionNs=Time+10'000'000;
            frame.nativeStamp.eyes[eye]={20,100+eye,200+eye,7,11,input->cameraApplicationQpc,
                input->contentTimeNs,input->posePredictionNs,eye+1,10,true};
            auto& camera=input->camera.constants;camera.source=CameraConstantsSource::Streamline;camera.version=2;
            camera.valuePresent.set();camera.flagPresent.set();camera.flags={0,1,0,0,0,0,1};
            auto& p=camera.values;
            p[0]=p[5]=1;p[10]=1.0001f;p[11]=1;p[14]=-.10001f;
            p[16]=p[21]=1;p[27]=-9.999f;p[30]=1;p[31]=10;
            for(unsigned base:{48u,64u})for(unsigned i=0;i<16;++i)p[base+i]=i%5==0?1.f:0.f;
            for(unsigned i=32;i<48;++i)p[i]=3.402823466e38f;
            p[80]=.25f;p[81]=-.125f;p[82]=p[83]=1;p[84]=p[85]=3.402823466e38f;
            p[86]=frame.pose[eye].position.x;p[90]=p[92]=p[97]=1;
            p[98]=.1f;p[99]=1000;p[100]=113.7f;p[101]=1;p[102]=3.402823466e38f;p[103]=40;
            frame.projection[eye]=SnapshotFrameProjection(*input);
        }
    }
    NativeInputResult Adapt(uint64_t epoch=0) {return AdaptNativeInputs(device->Native(),queue.Get(),frame,{},epoch);}
    void PrimePrevious(NativeGenerationBridge& bridge,bool changedProjection=false) {
        auto original=frame;--original.identity.serial;--original.identity.nativeFrame;original.nativeStamp.renderPublicationTimeNs-=20'000'000;
        for(unsigned eye=0;eye<2;++eye){
            auto input=std::make_shared<Inputs>(*inputs[eye]);--input->frameId;
            input->contentTimeNs-=20'000'000;input->posePredictionNs-=20'000'000;input->cameraApplicationQpc-=100;
            if(changedProjection)input->camera.constants.values[0]*=1.2f;
            original.inputs[eye]=input;original.projection[eye]=SnapshotFrameProjection(*input);
            auto& stamp=original.nativeStamp.eyes[eye];--stamp.sourceSerial;--stamp.nativeFrame;
            stamp.contentTimeNs=input->contentTimeNs;stamp.posePredictionNs=input->posePredictionNs;stamp.cameraApplicationQpc=input->cameraApplicationQpc;
        }
        Selection ignored;bridge.Select(device->Native(),queue.Get(),original,{Time+10'000'000,11'111'111},ignored);
    }
};
static void Seed(cpvr::GenerationWorker& worker,const NativeInputResult& adapted,bool generated=false,bool common=false) {
    cpvr::GenerationPolicy policy;policy.generated_per_interval=generated?1:0;policy.cadence=cpvr::Cadence::AlternateEyes;
    policy.presentation_pattern=cpvr::PresentationPattern::Custom;policy.custom_sequence=generated?"GG":"RR";
    std::array<cpvr::GenerationProduct,2> products;
    for(unsigned eye=0;eye<2;++eye) {
        auto frame=std::make_shared<cpvr::EyeFrame>(*adapted.inputs[eye].frame);
        if(common)frame->color.resting_state=D3D12_RESOURCE_STATE_COMMON;
        frame->generated=generated;if(generated)frame->stereo_pair_id=0;
        const auto t=frame->content_time_ns;if(generated)frame->content_time_ns-=10'000'000;products[eye].frame=frame;
        products[eye].origin=generated ? cpvr::GenerationOrigin{true,19,20,19,20,t-20'000'000,t,1,1} :
            cpvr::GenerationOrigin{false,20,20,20,20,t,t,0,0};
    }
    cpvr::GenerationWorkerTestAccess::seed(worker,policy,std::span(&products[0],1),std::span(&products[1],1));
}
static constexpr PresentationTime Target{Fixture::Time+10'000'000,11'111'111};
struct DecodeProbe {
    D3D12_RESOURCE_STATES initial{};bool shared{};unsigned calls{};bool succeed=true;
    bool Record(ID3D12GraphicsCommandList* commands,ID3D12Resource* source,ID3D12Resource*,uint32_t slot) {
        auto& list=*static_cast<CpuGraphicsList*>(commands);auto state=initial;
        for(const auto& barrier:list.barriers)if(barrier.Transition.pResource==source) {
            Check(barrier.Transition.StateBefore==state,"decode branch recorded a false source before-state");
            state=barrier.Transition.StateAfter;
        }
        Check(slot==3,"decode descriptor slot changed");
        Check((state&D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE)!=0,"decode source is not shader-readable");
        if(shared)Check((state&D3D12_RESOURCE_STATE_COPY_SOURCE)!=0 &&
            (state&D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE)!=0,
            "real decode narrowed shared source state while worker may copy");
        ++calls;return succeed;
    }
};
static void DecodeBranch() {
    for(bool native:{false,true})for(bool generated:{false,true}) {
        Fixture f;cpvr::GenerationWorker worker;NativeGenerationBridge bridge(worker);
        Selection selected;selected.frame=f.frame;selected.synthetic=generated;
        auto initial=generated?D3D12_RESOURCE_STATE_COPY_SOURCE:f.frame.colorCompletion.colorState;
        if(native) {
            if(generated)f.PrimePrevious(bridge);
            const auto adapted=f.Adapt(bridge.HistoryEpoch());Seed(worker,adapted,generated,generated);
            cpvr::GenerationWorkerTestAccess::enable_submission(worker,f.device->Native(),bridge.HistoryEpoch(),7,true);
            Check(bridge.Select(f.device->Native(),f.queue.Get(),f.frame,Target,selected),"decode fixture could not select actual worker product");
            if(generated)initial=D3D12_RESOURCE_STATE_COMMON;
        }
        ComPtr<CpuGraphicsList> list;list.Attach(new CpuGraphicsList(f.device.Get()));
        DecodeProbe pass{initial,!generated};
        {
            NativeReadScope read(list.Get(),selected,0);Check(bool(read),"decode read scope rejected selected source");
            Check(RecordColorDecode(pass,list.Get(),selected.frame.color[0],f.colors[1].Get(),3,read.ReadState()),
                "author decode branch rejected source");
        }
        Check(pass.calls==1 && list->unexpected==0,"decode pass was bypassed or executed unrelated commands");
        auto final=initial;
        for(const auto& barrier:list->barriers)if(barrier.Transition.pResource==selected.frame.color[0]) {
            Check(barrier.Transition.StateBefore==final,"restore before-state mismatched");final=barrier.Transition.StateAfter;
        }
        Check(final==initial,"decode branch did not restore exclusive generated source state");
        if(!generated)Check(list->barriers.empty(),"real capture read changed state across queues");
        list->barriers.clear();pass.calls=0;pass.succeed=false;
        {
            NativeReadScope read(list.Get(),selected,0);
            Check(!RecordColorDecode(pass,list.Get(),selected.frame.color[0],f.colors[1].Get(),3,read.ReadState()),
                "failed decode was reported successful");
        }
        final=initial;
        for(const auto& barrier:list->barriers)if(barrier.Transition.pResource==selected.frame.color[0]) {
            Check(barrier.Transition.StateBefore==final,"failed decode recorded a false restore before-state");final=barrier.Transition.StateAfter;
        }
        Check(pass.calls==1 && final==initial,"failed decode lost the source resting state");
        bridge.Submitted(selected,nullptr,0,false,false,false,0);
    }
}
static void OwnedPixelRead() {
    Fixture f;Selection selected;selected.frame=f.frame;selected.synthetic=true;
    ComPtr<CpuGraphicsList> list;list.Attach(new CpuGraphicsList(f.device.Get()));
    {
        NativeReadScope read(list.Get(),selected,0,true);
        Check(bool(read),"own generated shader read was rejected");
        Check((read.ReadState()&D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE)!=0,
            "own generated frame is not readable by paired reconstruction pixel shader");
    }
    Check(list->barriers.size()==2,"exclusive generated output needs a read and restore barrier");
    Check(list->barriers[0].Transition.StateBefore==D3D12_RESOURCE_STATE_COPY_SOURCE &&
        list->barriers[1].Transition.StateAfter==D3D12_RESOURCE_STATE_COPY_SOURCE &&
        list->barriers[0].Transition.StateAfter==list->barriers[1].Transition.StateBefore,
        "own generated output resting state was not restored");
    // ReShade's resource GetDevice can return a wrapper while the submit list
    // owns the native device (and vice versa). Exercise the actual read scope.
    Check(cvr::gpu::RegisterNativeObject(f.device.Get()),"native device registration failed");
    ComPtr<CpuDevice> wrapper;wrapper.Attach(new CpuDevice(f.device.Get()));
    for(bool wrappedCommands:{false,true}) {
        ComPtr<CpuGraphicsList> wrappedList;
        wrappedList.Attach(new CpuGraphicsList(wrappedCommands?wrapper.Get():f.device.Get()));
        f.colors[0]->device=wrappedCommands?f.device.Get():wrapper.Get();
        {
            NativeReadScope read(wrappedList.Get(),selected,0,true);
            Check(bool(read),"registered wrapper/native generated shader read rejected");
            Check((read.ReadState()&D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE)!=0,
                "wrapped generated output lacks pixel shader read state");
        }
        Check(wrappedList->barriers.size()==2 &&
            wrappedList->barriers[0].Transition.pResource==selected.frame.color[0] &&
            wrappedList->barriers[0].Transition.StateBefore==D3D12_RESOURCE_STATE_COPY_SOURCE &&
            wrappedList->barriers[1].Transition.StateAfter==D3D12_RESOURCE_STATE_COPY_SOURCE &&
            wrappedList->barriers[0].Transition.StateAfter==wrappedList->barriers[1].Transition.StateBefore,
            "wrapped generated output read/restore barriers differ");
    }
    f.colors[0]->device=f.device.Get();
    list->barriers.clear();selected.synthetic=false;
    {NativeReadScope read(list.Get(),selected,0,true);Check(bool(read),"shared real shader read rejected");}
    Check(list->barriers.empty(),"real capture must keep its shared read state");
    selected.synthetic=true;
    {NativeReadScope read(nullptr,selected,0,true);Check(!read,"null command list admitted for exclusive read");}
    {NativeReadScope read(list.Get(),selected,2,true);Check(!read,"invalid eye admitted");}
    selected.frame.color[0]=nullptr;
    {NativeReadScope read(list.Get(),selected,0,true);Check(!read,"missing generated output admitted");}
    selected.frame=f.frame;
    ComPtr<CpuDevice> other;other.Attach(new CpuDevice);ComPtr<CpuGraphicsList> foreign;foreign.Attach(new CpuGraphicsList(other.Get()));
    {NativeReadScope read(foreign.Get(),selected,0,true);Check(!read,"foreign-device generated output admitted");}
    Check(foreign->barriers.empty(),"invalid input recorded a barrier");
    Check(cvr::gpu::RegisterNativeObject(other.Get()),"foreign native registration failed");
    ComPtr<CpuDevice> foreignWrapper;foreignWrapper.Attach(new CpuDevice(other.Get()));
    foreign->device=foreignWrapper.Get();
    {NativeReadScope read(foreign.Get(),selected,0,true);Check(!read,"foreign wrapped device admitted");}
    Check(foreign->barriers.empty(),"foreign wrapped input recorded a barrier");
}
static void PairedOutputMetadata() {
    Fixture f;cpvr::GenerationWorker worker;NativeGenerationBridge bridge(worker);
    f.PrimePrevious(bridge);
    for(unsigned eye=0;eye<2;++eye)f.frame.projection[eye]=SnapshotFrameProjection(*f.inputs[eye]);
    auto adapted=f.Adapt(bridge.HistoryEpoch());Seed(worker,adapted,true,true);
    cpvr::GenerationWorkerTestAccess::enable_submission(worker,f.device->Native(),bridge.HistoryEpoch(),7,true);
    Selection first;Check(bridge.Select(f.device->Native(),f.queue.Get(),f.frame,Target,first),"generated metadata fixture not selected");
    Check(first.frame.nativeStamp.resourceEpoch==3 && first.frame.nativeStamp.transfer==CaptureTransfer::SrgbEncoded,
        "selected own product lost reconstruction epoch/transfer metadata");
    for(unsigned eye=0;eye<2;++eye)Check(first.frame.projection[eye] && first.frame.projection[eye]->frameId==10 &&
        first.frame.engineView[eye]==eye+1 && first.frame.nativeStamp.eyes[eye].sourceSerial==20 && first.frame.identity.pose[eye]==0,
        "generated output lacks original source projection/provenance or invents a real pose ID");
    ++f.frame.identity.serial;++f.frame.identity.nativeFrame;f.frame.nativeStamp.renderPublicationTimeNs+=20'000'000;
    for(unsigned eye=0;eye<2;++eye){
        f.inputs[eye]=std::make_shared<Inputs>(*f.inputs[eye]);f.frame.inputs[eye]=f.inputs[eye];++f.inputs[eye]->frameId;
        ++f.frame.nativeStamp.eyes[eye].sourceSerial;++f.frame.nativeStamp.eyes[eye].nativeFrame;
        f.frame.projection[eye]=SnapshotFrameProjection(*f.inputs[eye]);f.frame.projection[eye]->srInputWidth=99;
    }
    Selection repeated;Check(bridge.Select(f.device->Native(),f.queue.Get(),f.frame,Target,repeated),"pending product not retained");
    Check(repeated.frame.identity.serial==20 && repeated.frame.projection[0] && repeated.frame.projection[0]->frameId==10 &&
        repeated.frame.projection[0]->srInputWidth==16,"old product was relabelled with the newest input projection");
    bridge.Submitted(first,nullptr,0,false,false,false,0);bridge.Submitted(repeated,nullptr,0,false,false,false,0);
    worker.stop();
}
static void PairedGeometryGuard() {
    for(unsigned variant=0;variant<3;++variant){
        Fixture f;cpvr::GenerationWorker worker;NativeGenerationBridge bridge(worker);
        if(variant!=1)f.PrimePrevious(bridge,variant==2);
        auto adapted=f.Adapt(bridge.HistoryEpoch());Seed(worker,adapted,true,true);
        cpvr::GenerationWorkerTestAccess::enable_submission(worker,f.device->Native(),bridge.HistoryEpoch(),7,true);
        Selection result;const auto accepted=bridge.Select(f.device->Native(),f.queue.Get(),f.frame,Target,result);
        Check(accepted==(variant==0),variant==1?"generated product with missing A metadata admitted":
            variant==2?"generated product crossed a projection/FOV change without a reset":"stable-geometry generated product rejected");
        if(accepted)bridge.Submitted(result,nullptr,0,false,false,false,0);
        worker.stop();
    }
}
static void PairedHistoryDoesNotPinCapture() {
    Fixture f;cpvr::GenerationWorker worker;NativeGenerationBridge bridge(worker);
    cpvr::GenerationPolicy policy;policy.generated_per_interval=0;policy.cadence=cpvr::Cadence::StereoPairs;
    policy.presentation_pattern=cpvr::PresentationPattern::RealThenGeneratedStereo;
    Check(cpvr::GenerationWorkerTestAccess::configure(worker,policy),"fixture policy rejected");
    cpvr::GenerationWorkerTestAccess::enable_submission(worker,f.device->Native(),bridge.HistoryEpoch(),7,true);
    std::weak_ptr<ReadLease> capture=f.frame.lease;
    Selection result;bridge.Select(f.device->Native(),f.queue.Get(),f.frame,Target,result);
    if(result.native)bridge.Submitted(result,nullptr,0,false,false,false,0);
    result={};worker.request_history_reset();
    Check(cpvr::GenerationWorkerTestAccess::apply_history_reset(worker),"worker did not retire fixture inputs");
    f.frame={};f.inputs[0].reset();f.inputs[1].reset();
    Check(capture.expired(),"paired adjacency metadata keeps a retired capture slot pinned after worker and consumers release it");
    worker.stop();
}
static void RetireWithoutNewSource() {
    for(bool gpuPending:{false,true}){
        Fixture f;cpvr::GenerationWorker worker;NativeGenerationBridge bridge(worker);
        auto adapted=f.Adapt(bridge.HistoryEpoch());Seed(worker,adapted);adapted.inputs={};
        Selection selected;Check(bridge.Select(f.device->Native(),f.queue.Get(),f.frame,Target,selected),"maintenance fixture not selected");
        bridge.Submitted(selected,f.consumer.Get(),9,gpuPending,gpuPending,false,Target.target);selected={};
        std::weak_ptr<ReadLease> capture=f.frame.lease;
        f.frame={};f.inputs[0].reset();f.inputs[1].reset();
        bridge.Maintain({Fixture::Time+400'000'000,Target.period});
        Check(worker.status().selected_pairs==0,"maintenance falsely acknowledged an XR presentation");
        if(gpuPending){Check(!capture.expired(),"maintenance freed pending GPU input");f.consumer->Signal(9);bridge.Maintain({Fixture::Time+410'000'000,Target.period});}
        Check(worker.status().expired_products>=2,"stopped source did not retire the worker's expired real products");
        Check(capture.expired(),"stopped source retained a capture after all GPU consumers completed");
        worker.stop();
    }
}
static void MaintenanceDoesNotPropose() {
    Fixture f;cpvr::GenerationWorker worker;NativeGenerationBridge bridge(worker);
    auto input=f.Adapt(bridge.HistoryEpoch());Seed(worker,input);
    cpvr::GenerationWorkerTestAccess::enable_submission(worker,f.device->Native(),bridge.HistoryEpoch(),7,true);
    bridge.Maintain(Target);
    for(unsigned eye=0;eye<2;++eye){
        auto newer=input.inputs[eye];auto frame=std::make_shared<cpvr::EyeFrame>(*newer.frame);
        frame->source_id=frame->stereo_pair_id=21;frame->content_time_ns+=5'000'000;
        newer.frame=frame;++newer.native_frame_token;
        Check(worker.submit(newer),"new source after maintenance rejected");
    }
    auto selected=worker.select(Target.target+1,Target.period);
    Check(selected.frame && selected.frame->eyes[0]->source_id==21 && selected.frame->eyes[1]->source_id==21,
        "maintenance froze an old proposal before the normal selector saw newer data");
    Check(worker.status().selected_pairs==0,"unacknowledged maintenance counted an output");
    worker.stop();
}
static void OwnedPolicy() {
    Settings settings;settings.enabled=true;
    for(int count=1;count<=3;++count){
        settings.ownGeneratedFrames=count;settings.aerV2=false;
        const auto paired=MakeOwnedGenerationPolicy(settings);
        Check(paired.cadence==cpvr::Cadence::StereoPairs && paired.generated_per_interval==unsigned(count),"paired X2/X3/X4 source cadence or count changed");
        Check(paired.presentation_pattern==cpvr::PresentationPattern::RealThenGeneratedStereo,
            "two-camera FG must request complete RR/GG pairs, not a mixed real/generated-eye pattern");
        settings.aerV2=true;const auto aer=MakeOwnedGenerationPolicy(settings);
        Check(aer.cadence==cpvr::Cadence::AlternateEyes && aer.presentation_pattern==cpvr::PresentationPattern::Chessboard && aer.generated_per_interval==unsigned(count),
            "AER's existing alternating-eye policy must stay separate");
    }
    settings.enabled=false;Check(MakeOwnedGenerationPolicy(settings).generated_per_interval==0,"disabled FG must request zero generated outputs");
}
static void PairedPublicationClock() {
    Fixture f;auto first=f.Adapt();Check(bool(first),"initial paired source rejected");
    for(unsigned eye=0;eye<2;++eye)Check(first.inputs[eye].frame->content_time_ns==f.frame.nativeStamp.renderPublicationTimeNs,
        "paired image age must use measured render publication, not a possibly stationary camera application");
    f.frame.identity.serial++;f.frame.identity.nativeFrame++;f.frame.nativeStamp.renderPublicationTimeNs+=20'000'000;
    for(unsigned eye=0;eye<2;++eye){
        f.inputs[eye]=std::make_shared<Inputs>(*f.inputs[eye]);f.frame.inputs[eye]=f.inputs[eye];++f.inputs[eye]->frameId;
        ++f.frame.nativeStamp.eyes[eye].sourceSerial;++f.frame.nativeStamp.eyes[eye].nativeFrame;
    }
    auto next=AdaptNativeInputs(f.device->Native(),f.queue.Get(),f.frame,first.inputs);
    Check(bool(next),"new rendered pair with unchanged camera application rejected");
    for(unsigned eye=0;eye<2;++eye){
        Check(next.inputs[eye].frame->content_time_ns==f.frame.nativeStamp.renderPublicationTimeNs,"render clock did not advance");
        Check(next.inputs[eye].guides_previous_source_id==20 && next.inputs[eye].guides_previous_time_ns==Fixture::Time+2'000'000,
            "stationary camera must retain adjacent rendered-frame motion history");
        Check(f.inputs[eye]->contentTimeNs==Fixture::Time+int64_t(eye)*1'000'000,"camera provenance was overwritten");
    }
    f.frame.nativeStamp.renderPublicationTimeNs=0;
    Check(f.Adapt().code==NativeBridgeCode::MissingCameraTime,"missing measured publication must not fabricate a time");
    f.frame.nativeStamp.renderPublicationTimeNs=Fixture::Time+22'000'000;f.frame.nativeStamp.publicationClockGeneration=12;
    Check(f.Adapt().code==NativeBridgeCode::MissingCameraTime,"different clock generations must not be combined");
}
static void ThreeSlotSteadyState() {
    Fixture f;
    cvr::capture::BufferLeases pins;
    struct PinOwner {cvr::capture::BufferLeases* pins;} owner{&pins};
    std::array<std::array<ComPtr<CpuResource>,2>,3> colors;
    for(auto& slot:colors)for(unsigned eye=0;eye<2;++eye) {
        slot[eye].Attach(new CpuResource(f.device.Get()));slot[eye]->descriptor=f.colors[eye]->descriptor;
    }
    // These are the production ownership types: the current/previous source
    // frames and the two MotionRecorder requests reachable through a current
    // snapshot and recorded_work.old. No reset or fourth color slot is used.
    std::array<cpvr::GenerationInput,2> current,previous;
    std::array<cpvr::MotionRecordRequest,2> recording,oldRecording;
    std::array<std::weak_ptr<const Inputs>,2> firstGuides;
    f.frame.lease.reset();
    for(unsigned serial=0;serial<8;++serial) {
        const auto slot=serial%3;
        auto lease=std::make_shared<ReadLease>();lease->owner=&owner;
        lease->release=[](void* context,ID3D12Resource* resource,uint64_t) {
            static_cast<PinOwner*>(context)->pins->Unpin(reinterpret_cast<uintptr_t>(resource));resource->Release();
        };
        for(unsigned eye=0;eye<2;++eye) {
            auto* color=colors[slot][eye].Get();const auto key=reinterpret_cast<uintptr_t>(color);
            Check(pins.BeginWrite(key),("three-slot producer stalled at source "+std::to_string(serial+1)).c_str());
            pins.EndWrite(key);Check(pins.Pin(key),"published capture could not be reserved");
            color->AddRef();lease->resources[eye]=color;f.frame.color[eye]=color;
            auto input=std::make_shared<Inputs>(*f.inputs[eye]);
            input->frameId=10+serial;input->poseId=100+2*serial+eye;input->poseWriteId=200+2*serial+eye;
            input->cameraApplicationQpc=1000+10*serial+eye;input->contentTimeNs=Fixture::Time+10'000'000ll*serial+1'000'000ll*eye;
            input->posePredictionNs=input->contentTimeNs+10'000'000;f.inputs[eye]=input;f.frame.inputs[eye]=input;
            if(!serial)firstGuides[eye]=input;
            f.frame.identity.pose[eye]=input->poseId;
            f.frame.nativeStamp.eyes[eye]={20+serial,input->poseId,input->poseWriteId,7,11,input->cameraApplicationQpc,
                input->contentTimeNs,input->posePredictionNs,eye+1,input->frameId,true};
        }
        f.frame.lease=std::move(lease);f.frame.identity.serial=20+serial;f.frame.identity.nativeFrame=10+serial;
        f.frame.nativeStamp.renderPublicationTimeNs=Fixture::Time+2'000'000+10'000'000ll*serial;
        auto adapted=AdaptNativeInputs(f.device->Native(),f.queue.Get(),f.frame,current,3);
        Check(bool(adapted),"steady-state source rejected by actual adapter");
        oldRecording=recording;previous=std::move(current);current=std::move(adapted.inputs);
        for(unsigned eye=0;eye<2;++eye) {
            recording[eye].current.guides=current[eye].frame->guides;
            recording[eye].previous.guides=previous[eye].frame?previous[eye].frame->guides:current[eye].frame->guides;
        }
        f.frame.lease.reset();
        if(serial==2)for(unsigned eye=0;eye<2;++eye)
            Check(!firstGuides[eye].expired() && oldRecording[eye].previous.guides->motion.ready_fence.Get()==f.producer.Get(),
                "old motion request lost its guide resource/fence ownership");
    }
}
static void Ingress() {
    Fixture f;cpvr::GenerationWorker worker;NativeGenerationBridge bridge(worker);
    cpvr::GenerationPolicy policy;policy.generated_per_interval=0;policy.cadence=cpvr::Cadence::AlternateEyes;
    Check(cpvr::GenerationWorkerTestAccess::configure(worker,policy),"real-only fixture policy was rejected");
    cpvr::GenerationWorkerTestAccess::enable_submission(worker,f.device->Native(),bridge.HistoryEpoch(),7,true);
    Selection selected;
    Check(bridge.Select(f.device->Native(),f.queue.Get(),f.frame,Target,selected),"bridge did not submit coherent input to the actual worker");
    Check(worker.status().accepted_inputs==2 && selected.native && !selected.synthetic,"bridge skipped real worker ingress or mislabeled real output");
    for(unsigned eye=0;eye<2;++eye) {
        const auto& value=*selected.native->selected.frame->eyes[eye];
        Check(value.source_id==20 && value.eye==cpvr::Eye(eye) && value.epoch==bridge.HistoryEpoch() &&
            value.content_time_ns==f.frame.nativeStamp.renderPublicationTimeNs && value.color.texture.Get()==f.colors[eye].Get() &&
            value.color.ready_fence.Get()==f.producer.Get() && value.color.ready_value==5 && value.color.lifetime,
            "worker ingress changed source identity/time/fence/ownership");
    }
    bridge.Submitted(selected,nullptr,0,false,false,false,0);selected={};
    Check(bridge.Select(f.device->Native(),f.queue.Get(),f.frame,Target,selected) && worker.status().accepted_inputs==2,
        "the same author publication was repeatedly queued");
    bridge.Submitted(selected,nullptr,0,false,false,false,0);selected={};
    bridge.Reset();Check(cpvr::GenerationWorkerTestAccess::apply_history_reset(worker),"actual queued ingress did not retire on owner reset");
}
static void Admission() {
    Fixture f;auto adapted=f.Adapt();Check(bool(adapted),"coherent measured input rejected");
    const auto& image=adapted.inputs[0].frame->color;
    Check(image.texture.Get()==f.colors[0].Get() && image.source_format==DXGI_FORMAT_R8G8B8A8_UNORM_SRGB &&
        image.transfer==cpvr::ColorTransfer::SrgbEncoded,"source view/encoding lost or byte alias called linear");
    Check(adapted.inputs[1].frame->content_time_ns==f.frame.nativeStamp.renderPublicationTimeNs &&
        f.inputs[1]->contentTimeNs==Fixture::Time+1'000'000,"render publication or original camera provenance lost");
    const auto& guides=*adapted.inputs[0].frame->guides;
    Check(std::abs(guides.camera[100]-1.57079632679f)<1e-5f && guides.camera[32]==1 && guides.camera[33]==0,
        "existing camera canonicalization not applied");
    Check(f.inputs[0]->camera.constants.values[32]>3.4e38f && f.inputs[0]->camera.constants.values[100]==113.7f,
        "raw measured/sentinel camera was rewritten");
    Check(guides.motion.texture->GetDesc().Format==DXGI_FORMAT_R16G16B16A16_FLOAT,"RGBA16F guide channels reinterpreted on CPU");
    auto previous=guides;previous.camera[80]=previous.camera[81]=0;
    auto mapping=cpvr::motion_normalization(guides,previous,{16,8},{16,8},adapted.inputs[0].motion_convention);
    Check(mapping && mapping->scale[0]==1 && mapping->scale[1]==1 && mapping->bias[0]==.015625f && mapping->bias[1]==-.015625f,
        "existing backward UV scale/jitter contract changed");
    f.frame.nativeStamp.eyes[0].contentTimeNs=0;
    Check(f.Adapt().code==NativeBridgeCode::MissingCameraTime,"capture QPC substituted for missing camera time");
    f.frame.nativeStamp.eyes[0].contentTimeNs=Fixture::Time;f.frame.nativeStamp.eyes[1].engineView=1;
    Check(f.Adapt().code==NativeBridgeCode::MissingIdentity,"engine view conflated with logical eye");
    f.frame.nativeStamp.eyes[1].engineView=2;f.colors[0]->descriptor.Format=DXGI_FORMAT_B8G8R8A8_UNORM;
    Check(f.Adapt().code==NativeBridgeCode::UnsupportedResource,"BGRA silently accepted as RGBA");
    f.colors[0]->descriptor.Format=DXGI_FORMAT_R8G8B8A8_UNORM;f.frame.nativeStamp.transfer=CaptureTransfer::Unknown;
    Check(f.Adapt().code==NativeBridgeCode::UnsupportedResource,"UNORM alone was used to infer sRGB");
    f.frame.nativeStamp.transfer=CaptureTransfer::SrgbEncoded;f.frame.colorCompletion.colorState=D3D12_RESOURCE_STATE_COPY_SOURCE;
    Check(f.Adapt().code==NativeBridgeCode::UnsupportedResource,"exclusive COPY_SOURCE was accepted for shared author/worker reads");
}
static void QueueGuard() {
    Fixture f;f.queue->descriptor.Type=D3D12_COMMAND_LIST_TYPE_COMPUTE;
    Check(f.Adapt().code==NativeBridgeCode::WrongQueue,"non-author queue type admitted");
}
static void CommandGuard() {
    Fixture f;cpvr::GenerationWorker worker;NativeGenerationBridge bridge(worker);
    auto adapted=f.Adapt(bridge.HistoryEpoch());Check(bool(adapted),"fixture admission failed");Seed(worker,adapted);
    Selection selected;Check(bridge.Select(f.device->Native(),f.queue.Get(),f.frame,Target,selected),"real worker selection not connected");
    ComPtr<CpuDevice> other;other.Attach(new CpuDevice);ComPtr<CpuGraphicsList> list;list.Attach(new CpuGraphicsList(other.Get()));
    NativeReadScope read(list.Get(),selected,0);
    Check(!read && list->barriers.empty(),"selected texture was recorded on a foreign device");
}
static void Retirement() {
    Fixture f;cpvr::GenerationWorker worker;NativeGenerationBridge bridge(worker);
    auto adapted=f.Adapt(bridge.HistoryEpoch());Seed(worker,adapted);
    Selection selected;Check(bridge.Select(f.device->Native(),f.queue.Get(),f.frame,Target,selected),"worker output not selected");
    ComPtr<CpuGraphicsList> list;list.Attach(new CpuGraphicsList(f.device.Get()));
    {
        NativeReadScope left(list.Get(),selected,0),right(list.Get(),selected,1);
        Check(bool(left) && bool(right),"selected pair not admitted to author read");
        Check(list->barriers.empty() && left.ReadState()==CaptureColorReadState && right.ReadState()==CaptureColorReadState,
            "real capture read narrowed the shared state");
    }
    Check(list->barriers.empty() && list->unexpected==0,"real read scope changed source state");
    std::weak_ptr<ReadLease> pins=f.frame.lease;
    bridge.Submitted(selected,f.consumer.Get(),9,true,true,false,Target.target);
    selected={};adapted.inputs={};worker.stop();f.frame={};f.inputs[0].reset();f.inputs[1].reset();
    bridge.Collect();Check(!pins.expired() && f.pinsReleased==0 && bridge.Status().retainedConsumers==1,
        "source/pool lease released before author copy fence");
    f.consumer->Signal(9);bridge.Collect();
    Check(pins.expired() && f.pinsReleased==2 && bridge.Status().retainedConsumers==0,"completed author copy did not retire leases");
}
static void Acknowledge() {
    Fixture f;cpvr::GenerationWorker worker;NativeGenerationBridge bridge(worker);
    auto adapted=f.Adapt(bridge.HistoryEpoch());Seed(worker,adapted);
    Selection first;Check(bridge.Select(f.device->Native(),f.queue.Get(),f.frame,Target,first),"first real selection missing");
    f.consumer->Signal(9);
    bridge.Submitted(first,f.consumer.Get(),9,true,true,false,Target.target);
    Check(!cpvr::GenerationWorkerTestAccess::committed_cursor(worker).valid,"rejected projection acknowledged");
    Selection accepted;Check(bridge.Select(f.device->Native(),f.queue.Get(),f.frame,Target,accepted),"proposal retry missing");
    bridge.Submitted(accepted,f.consumer.Get(),9,true,true,true,Target.target);
    const auto cursor=cpvr::GenerationWorkerTestAccess::committed_cursor(worker);
    Check(cursor.valid && cursor.display_time_ns==Target.target,"accepted projection did not reach existing worker acknowledgement");
    const auto oldEpoch=bridge.HistoryEpoch();bridge.Reset();Check(bridge.HistoryEpoch()!=oldEpoch,"history reset reused a generation");
    Selection stale;Check(!bridge.Select(f.device->Native(),f.queue.Get(),f.frame,{Target.target+Target.period,Target.period},stale),
        "old-generation products survived bridge reset");
}
static void BackendReadiness() {
    Fixture f;cpvr::GenerationWorker worker;NativeGenerationBridge bridge(worker);
    Selection result;Check(!bridge.Select(f.device->Native(),f.queue.Get(),f.frame,Target,result) &&
        bridge.Status().code==NativeBridgeCode::BackendPending,"unstarted NVIDIA backend advertised as ready");
    f.PrimePrevious(bridge);auto adapted=f.Adapt(bridge.HistoryEpoch());Seed(worker,adapted,true);
    Check(!bridge.Select(f.device->Native(),f.queue.Get(),f.frame,Target,result) && bridge.Status().code==NativeBridgeCode::BackendPending,
        "generated products bypassed backend readiness");
}
static void ResetDuplicates() {
    cpvr::GenerationWorker worker;
    Check(worker.request_history_reset() && worker.request_history_reset(),"reset request was not accepted/coalesced");
    auto status=worker.status();Check(status.history_reset_pending && status.history_reset_requests==1,"duplicate requests created a reset storm");
    Check(cpvr::GenerationWorkerTestAccess::apply_history_reset(worker),"owner did not apply requested reset");
    status=worker.status();Check(!status.history_reset_pending && status.history_resets==1,"owner reset completion missing");
    Check(!cpvr::GenerationWorkerTestAccess::apply_history_reset(worker),"owner repeated a completed reset");
}
static void RevisionCancellation() {
    Fixture f;cpvr::GenerationWorker worker;NativeGenerationBridge bridge(worker);
    auto adapted=f.Adapt(bridge.HistoryEpoch());Seed(worker,adapted);
    cpvr::GenerationWorkerTestAccess::enable_submission(worker,f.device->Native(),bridge.HistoryEpoch(),7);
    auto frame=std::make_shared<cpvr::EyeFrame>(*adapted.inputs[0].frame);frame->generated=true;frame->stereo_pair_id=0;
    const auto publication=frame->content_time_ns;
    cpvr::GenerationProduct product{frame,{true,19,20,19,20,publication-20'000'000,publication+20'000'000,1,1}};
    const auto revision=cpvr::GenerationWorkerTestAccess::revision(worker);
    Check(cpvr::GenerationWorkerTestAccess::publish_generated(worker,product,revision,5),"ready fixture output was not publishable before reset");
    Check(worker.request_history_reset(),"reset request failed");
    Check(!worker.select(Target.target,Target.period).frame,"pending reset exposed old products/cursor");
    Check(!cpvr::GenerationWorkerTestAccess::publish_generated(worker,product,revision,5),"in-flight output published during reset");
    Check(cpvr::GenerationWorkerTestAccess::apply_history_reset(worker),"owner reset step failed");
    Check(cpvr::GenerationWorkerTestAccess::revision(worker)>revision &&
        !cpvr::GenerationWorkerTestAccess::publish_generated(worker,product,revision,5),"old revision revived cleared history");
}
static void DisableReleaseReenable() {
    Fixture f;
    cvr::capture::BufferLeases pins;
    struct PinOwner {cvr::capture::BufferLeases* pins;unsigned released{};} owner{&pins};
    cpvr::GenerationWorker worker;NativeGenerationBridge bridge(worker);
    std::array<std::array<ComPtr<CpuResource>,2>,3> textures;
    std::vector<cpvr::GenerationProduct> left,right;
    auto source=f.Adapt(bridge.HistoryEpoch());
    for(unsigned slot=0;slot<3;++slot) {
        // Admit the original metadata for each manufactured worker product.
        // The production bridge must not invent source-22 receipts from the
        // source-20 frame passed at the final selection below.
        Frame original=f.frame;original.identity.serial=20+slot;original.identity.nativeFrame=10+slot;
        original.nativeStamp.renderPublicationTimeNs+=slot*10'000'000;
        for(unsigned eye=0;eye<2;++eye){
            auto input=std::make_shared<Inputs>(*f.inputs[eye]);input->frameId=10+slot;original.inputs[eye]=input;
            original.nativeStamp.eyes[eye].sourceSerial=20+slot;original.nativeStamp.eyes[eye].nativeFrame=10+slot;
        }
        Selection ignored;bridge.Select(f.device->Native(),f.queue.Get(),original,Target,ignored);
        auto lease=std::make_shared<ReadLease>();lease->owner=&owner;
        lease->release=[](void* context,ID3D12Resource* resource,uint64_t) {
            auto& owner=*static_cast<PinOwner*>(context);owner.pins->Unpin(reinterpret_cast<uintptr_t>(resource));
            ++owner.released;resource->Release();
        };
        for(unsigned eye=0;eye<2;++eye) {
            textures[slot][eye].Attach(new CpuResource(f.device.Get()));textures[slot][eye]->descriptor=f.colors[eye]->descriptor;
            auto* texture=textures[slot][eye].Get();Check(pins.Pin(reinterpret_cast<uintptr_t>(texture)),"fixture capture pin failed");
            texture->AddRef();lease->resources[eye]=texture;
            auto frame=std::make_shared<cpvr::EyeFrame>(*source.inputs[eye].frame);
            frame->source_id=frame->stereo_pair_id=20+slot;frame->content_time_ns+=slot*10'000'000;frame->guides.reset();
            frame->color.texture=texture;frame->color.lifetime=lease;
            const auto id=frame->source_id;const auto time=frame->content_time_ns;
            (eye?right:left).push_back({frame,{false,id,id,id,id,time,time,0,0}});
        }
    }
    cpvr::GenerationPolicy policy;policy.generated_per_interval=0;policy.cadence=cpvr::Cadence::AlternateEyes;
    cpvr::GenerationWorkerTestAccess::seed(worker,policy,left,right);left.clear();right.clear();source.inputs={};
    const auto writable=[&](unsigned slot) {
        const auto id=reinterpret_cast<uintptr_t>(textures[slot][0].Get());
        if(!pins.BeginWrite(id))return false;pins.EndWrite(id);return true;
    };
    Check(!writable(0) && !writable(1) && !writable(2),"three author capture slots were not retained");
    Selection selected;const PresentationTime time{Fixture::Time+40'000'000,Target.period};
    Check(bridge.Select(f.device->Native(),f.queue.Get(),f.frame,time,selected),"ready source could not be selected");
    ComPtr<CpuGraphicsList> list;list.Attach(new CpuGraphicsList(f.device.Get()));
    {NativeReadScope l(list.Get(),selected,0),r(list.Get(),selected,1);Check(bool(l)&&bool(r),"read scope rejected pair");}
    bridge.Submitted(selected,f.consumer.Get(),9,true,true,true,time.target);selected={};
    bridge.Reset();const auto epoch=bridge.HistoryEpoch();bridge.Reset();bridge.Reset();
    Check(bridge.HistoryEpoch()==epoch && worker.status().history_reset_requests==1,"menu/disable repeats stormed history resets");
    Check(!worker.select(time.target+time.period,time.period).frame,"disabled pending-reset cursor remained selectable");
    Check(cpvr::GenerationWorkerTestAccess::apply_history_reset(worker),"owner reset did not clear source/history leases");
    bridge.Collect();Check(writable(0) && writable(1) && !writable(2),"reset released an in-flight consumer or retained unused slots");
    f.consumer->Signal(9);bridge.Collect();Check(writable(2) && owner.released==6,"copy fence did not release the final author capture slot");
    bridge.Reset();Check(worker.status().history_reset_requests==1,"completed reset repeated while still disabled");
    auto fresh=f.Adapt(bridge.HistoryEpoch());Seed(worker,fresh);
    Selection resumed;Check(bridge.Select(f.device->Native(),f.queue.Get(),f.frame,Target,resumed),"new epoch could not resume after disable");
    bridge.Reset();Check(worker.status().history_reset_requests==2,"new active-to-disabled transition was not reset");
    cpvr::GenerationWorkerTestAccess::apply_history_reset(worker);
}
static void DeferredStart() {
    Fixture f;cpvr::GenerationWorker worker;cpvr::GenerationConfig config;
    config.backend_library=L"C:\\__cpvr_missing_backend_682ca58eb5e44ae2__\\backend.dll";
    config.snippet_directory=L"C:\\__cpvr_missing_backend_682ca58eb5e44ae2__";
    Check(GetFileAttributesW(config.backend_library.c_str())==INVALID_FILE_ATTRIBUTES,"negative backend path unexpectedly exists");
    NativeGenerationBridge bridge(worker,config);Selection result;
    for(unsigned attempt=0;attempt<100 && !worker.status().failed;++attempt) {
        bridge.Select(f.device->Native(),f.queue.Get(),f.frame,Target,result);
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    Check(worker.status().failed,"explicit configuration never reached the existing worker loader");
    bridge.Select(f.device->Native(),f.queue.Get(),f.frame,Target,result);
    Check(bridge.Status().code==NativeBridgeCode::BackendFailed && !result.native,"failed backend was advertised as an output");
    worker.stop();
}
int main(int argc,char** argv) {
    try {
        const std::string_view name=argc>1?argv[1]:"";
        if(name=="maintenance_policy")MaintenanceDoesNotPropose();else if(name=="idle_source")RetireWithoutNewSource();else if(name=="paired_history")PairedHistoryDoesNotPinCapture();else if(name=="paired_geometry")PairedGeometryGuard();else if(name=="paired_metadata")PairedOutputMetadata();else if(name=="owned_policy")OwnedPolicy();else if(name=="publication")PairedPublicationClock();else if(name=="owned_pixel")OwnedPixelRead();else if(name=="decode")DecodeBranch();else if(name=="three_slots")ThreeSlotSteadyState();else if(name=="ingress")Ingress();else if(name=="admission")Admission();else if(name=="queue")QueueGuard();else if(name=="commands")CommandGuard();
        else if(name=="retirement")Retirement();else if(name=="ack")Acknowledge();else if(name=="backend")BackendReadiness();
        else if(name=="reset_duplicates")ResetDuplicates();else if(name=="revision")RevisionCancellation();
        else if(name=="disable")DisableReleaseReenable();else if(name=="deferred_start")DeferredStart();
        else throw std::runtime_error("unknown bridge case");
        std::cout<<"PASS "<<name<<'\n';return 0;
    } catch(const std::exception& e) {std::cerr<<"FAIL "<<e.what()<<'\n';return 1;}
}
