// The runner extracts the two consumer blocks verbatim from OpenXRFrameLoop.cpp.
// Real D3D12 WARP/pass objects validate ownership; pixel equivalence is covered
// separately by aer_reconstruction_warp.cpp against pinned original DXBC.
#include "Render/AerReconstructionPass.hpp"
#include <d3d12sdklayers.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <array>
#include <cmath>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <vector>
using Microsoft::WRL::ComPtr;
using namespace cvr::render;
void Log(const char*,...) {}
static unsigned checks=0;
static void Check(bool condition,const char* text){++checks;if(!condition)throw std::runtime_error(text);}
static void Hr(HRESULT result,const char* text){Check(SUCCEEDED(result),text);}
struct Fov {float angleLeft=-.8f,angleRight=.9f,angleUp=.7f,angleDown=-.6f;};
struct ProjectionView {Fov fov;};
struct FakeSelection {struct {std::array<void*,2> color{};} frame;};
struct PassProbe {
    AerReconstructionPass pass;
    unsigned cancelCalls=0,submitCalls=0,falseSubmitCalls=0;
    bool SubmissionReported(AerRecordTicket ticket,bool executed,bool signaled){
        ++submitCalls;falseSubmitCalls+=!executed;return pass.SubmissionReported(ticket,executed,signaled);
    }
    bool CancelNeverExecuted(AerRecordTicket ticket){++cancelCalls;return pass.CancelNeverExecuted(ticket);}
};
#include "aer-consumer-sites.hpp"
struct Device {
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12Fence> fence;
    ComPtr<ID3D12InfoQueue> info;
    Device(){
        ComPtr<ID3D12Debug> debug;Hr(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)),"debug layer unavailable");debug->EnableDebugLayer();
        ComPtr<IDXGIFactory4> factory;Hr(CreateDXGIFactory1(IID_PPV_ARGS(&factory)),"factory");
        ComPtr<IDXGIAdapter> adapter;Hr(factory->EnumWarpAdapter(IID_PPV_ARGS(&adapter)),"WARP adapter");
        Hr(D3D12CreateDevice(adapter.Get(),D3D_FEATURE_LEVEL_11_0,IID_PPV_ARGS(&device)),"WARP device");
        Hr(device.As(&info),"info queue");D3D12_COMMAND_QUEUE_DESC desc{};
        Hr(device->CreateCommandQueue(&desc,IID_PPV_ARGS(&queue)),"queue");
        Hr(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&fence)),"fence");
    }
    unsigned Errors(){
        unsigned errors=0;
        for(UINT64 i=0;i<info->GetNumStoredMessages();++i){
            SIZE_T size=0;Hr(info->GetMessage(i,nullptr,&size),"debug size");std::vector<unsigned char> bytes(size);
            auto* message=reinterpret_cast<D3D12_MESSAGE*>(bytes.data());Hr(info->GetMessage(i,message,&size),"debug message");
            if(message->Severity<=D3D12_MESSAGE_SEVERITY_ERROR){++errors;std::cout<<"DEBUG "<<message->pDescription<<'\n';}
        }
        return errors;
    }
};
struct Commands {
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> list;
    explicit Commands(ID3D12Device* device){
        Hr(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&allocator)),"allocator");
        Hr(device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,allocator.Get(),nullptr,IID_PPV_ARGS(&list)),"list");
    }
};
static ComPtr<ID3D12Resource> Texture(ID3D12Device* device,D3D12_RESOURCE_STATES state){
    D3D12_HEAP_PROPERTIES heap{};heap.Type=D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC desc{};desc.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;desc.Width=desc.Height=16;
    desc.DepthOrArraySize=desc.MipLevels=1;desc.SampleDesc.Count=1;desc.Format=DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    desc.Flags=D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;ComPtr<ID3D12Resource> resource;
    Hr(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&desc,state,nullptr,IID_PPV_ARGS(&resource)),"texture");return resource;
}
static void Complete(ID3D12Fence* fence,UINT64 value){
    HANDLE event=CreateEventW(nullptr,FALSE,FALSE,nullptr);Check(event!=nullptr,"event");
    Hr(fence->SetEventOnCompletion(value,event),"set event");const DWORD result=WaitForSingleObject(event,10000);CloseHandle(event);
    Check(result==WAIT_OBJECT_0 && fence->GetCompletedValue()==value,"exact fence did not complete");
}
struct Fixture {
    PassProbe probe;
    ComPtr<ID3D12Resource> source,target;
    explicit Fixture(Device& device){
        source=Texture(device.device.Get(),D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        target=Texture(device.device.Get(),D3D12_RESOURCE_STATE_RENDER_TARGET);
        Check(probe.pass.Initialize(device.device.Get()),"initialize");
        Check(probe.pass.Prepare(DXGI_FORMAT_R8G8B8A8_UNORM_SRGB,DXGI_FORMAT_R8G8B8A8_UNORM_SRGB,{}),"prepare");
    }
    std::array<AerRecordTicket,2> Record(Commands& commands,ID3D12Fence* fence,UINT64 value,std::weak_ptr<void>& retained){
        auto owner=std::make_shared<int>(42);retained=owner;std::array<AerRecordTicket,2> result;
        AerRecordRequest request;request.source=source.Get();request.destination=target.Get();request.completion={fence,value};
        request.sourceOwner=owner;request.warp.crop={0,0,16,16};
        request.warp.sourceEpoch=request.warp.referenceEpoch=request.warp.sourceSpace=request.warp.referenceSpace=1;
        for(unsigned eye=0;eye<2;++eye){request.eye=eye;result[eye]=probe.pass.Record(commands.list.Get(),request);Check(bool(result[eye]),"a six-slot pass was exhausted by earlier never-executed pairs");}
        return result;
    }
};
static void CloseFailureNeverConsumesSlots(Device& device){
    Fixture fixture(device);
    // Eight failures exceed the former three-pair/six-slot exhaustion boundary.
    for(unsigned cycle=0;cycle<8;++cycle){
        Commands commands(device.device.Get());std::weak_ptr<void> owner;
        auto tickets=fixture.Record(commands,device.fence.Get(),1,owner);
        const auto retainedTickets=tickets;Hr(commands.list->Close(),"discarded list close");
        Check(fixture.probe.pass.BusySlots()==2 && !owner.expired(),"recorded sources missing");
        ActualCompletion(&fixture.probe,tickets,false,false);
        const bool released=fixture.probe.pass.BusySlots()==0 && owner.expired();
        // Test cleanup is separate, so a RED run does not leak its software device.
        if(!released)for(auto ticket:retainedTickets)fixture.probe.pass.CancelNeverExecuted(ticket);
        Check(released,"Close failure lost recorded tickets and retained source/descriptor slots");
        Check(!tickets[0] && !tickets[1],"completion left caller tickets live");
    }
    Check(fixture.probe.cancelCalls==16 && fixture.probe.submitCalls==0,"unexecuted work reached SubmissionReported");
    Check(fixture.probe.pass.TryShutdown(),"discarded command lists prevented shutdown");
}
static void ExecutedSignalOutcome(Device& device,bool signalReported){
    Fixture fixture(device);Commands commands(device.device.Get());std::weak_ptr<void> owner;
    const UINT64 value=device.fence->GetCompletedValue()+1;
    auto tickets=fixture.Record(commands,device.fence.Get(),value,owner);const auto original=tickets;
    ComPtr<ID3D12Fence> gate;Hr(device.device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&gate)),"gate");
    Hr(device.queue->Wait(gate.Get(),1),"gate wait");Hr(commands.list->Close(),"executed list close");
    ID3D12CommandList* lists[]={commands.list.Get()};device.queue->ExecuteCommandLists(1,lists);
    Hr(device.queue->Signal(device.fence.Get(),value),"actual submission signal");
    ActualCompletion(&fixture.probe,tickets,true,signalReported);
    const bool retained=fixture.probe.pass.BusySlots()==2 && !owner.expired() && fixture.probe.cancelCalls==0 &&
        fixture.probe.submitCalls==2 && fixture.probe.falseSubmitCalls==0 && !tickets[0] && !tickets[1];
    // Unblock before assertions, even if the production fragment regresses.
    Hr(gate->Signal(1),"gate release");Complete(device.fence.Get(),value);
    Check(retained,"executed commands were cancelled or source lease released before GPU completion");
    if(!signalReported){
        Check(fixture.probe.pass.Collect()==0 && fixture.probe.pass.BusySlots()==2 && !owner.expired(),"unknown Signal outcome retired on numeric fence completion");
        for(auto ticket:original)Check(!fixture.probe.pass.CancelNeverExecuted(ticket),"unknown executed slot allowed cancellation");
        for(auto ticket:original)Check(fixture.probe.pass.SubmissionReported(ticket,true,true),"confirmed exact Signal retry rejected");
    }
    Check(fixture.probe.pass.Collect()==2 && owner.expired(),"exact completed fence did not release sources");
    Check(fixture.probe.pass.TryShutdown(),"completed pass cannot shutdown");
}
static void FovCases(){
    struct Case {const char* name;bool aer,selected,same,vrcam,shift;};
    const Case cases[]={
        {"AER distinct",true,false,false,false,false},
        {"AER native pair",true,true,false,false,false},
        {"non-AER native distinct",false,true,false,false,false},
        {"legacy mono",false,false,true,false,true},
        {"selected shared mono",false,true,true,false,true},
        {"legacy VRCAM stereo",false,false,false,true,false}
    };
    int source0=0,source1=1;
    for(const auto& test:cases)for(bool mainRight:{false,true})for(float distance:{0.f,1.8f}){
        std::array<ProjectionView,2> views;const auto original=views;FakeSelection selected;
        selected.frame.color={&source0,test.same?static_cast<void*>(&source0):static_cast<void*>(&source1)};
        for(unsigned eye=0;eye<2;++eye)ActualFov(test.aer,test.selected,selected,test.vrcam?&source1:nullptr,distance,mainRight,.064f,views,eye);
        for(unsigned eye=0;eye<2;++eye){
            const bool left=eye==(mainRight?1u:0u);
            const float shift=test.shift&&distance>.05f?(left?1.f:-1.f)*.5f*(.064f/distance):0;
            Check(views[eye].fov.angleLeft==original[eye].fov.angleLeft+shift,test.name);
            Check(views[eye].fov.angleRight==original[eye].fov.angleRight+shift,test.name);
            Check(views[eye].fov.angleUp==original[eye].fov.angleUp && views[eye].fov.angleDown==original[eye].fov.angleDown,"vertical FOV changed");
        }
    }
}
int main(){
    unsigned passed=0,failed=0;
    try{
        Device device;
        auto run=[&](const char* name,auto&& test){try{test();++passed;std::cout<<"PASS "<<name<<'\n';}catch(const std::exception& error){++failed;std::cout<<"FAIL "<<name<<": "<<error.what()<<'\n';}};
        run("never_executed_8_stereo_pairs",[&]{CloseFailureNeverConsumesSlots(device);});
        run("executed_confirmed_signal",[&]{ExecutedSignalOutcome(device,true);});
        run("executed_unknown_signal_retention",[&]{ExecutedSignalOutcome(device,false);});
        run("actual_FOV_gate_24_cases",FovCases);
        const unsigned errors=device.Errors();Check(errors==0,"D3D12 debug errors");
        std::cout<<"RESULT passed="<<passed<<" failed="<<failed<<" checks="<<checks<<" debug_errors="<<errors<<'\n';
    }catch(const std::exception& error){std::cout<<"FATAL "<<error.what()<<'\n';return 2;}
    return failed?1:0;
}
