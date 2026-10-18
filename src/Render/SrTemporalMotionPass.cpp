#include "Render/SrTemporalMotionPass.hpp"
#include "SrTemporalMotionShader.hpp"
#include <d3dcompiler.h>
#include <cstring>
#include <limits>
extern void Log(const char*,...);
namespace cvr::render {
using Microsoft::WRL::ComPtr;
namespace {
constexpr uint32_t DescriptorCount=13,ConstantStride=512;
constexpr auto Nps=D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
constexpr auto BothRead=D3D12_RESOURCE_STATES(Nps|D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
enum class Role {Motion,Depth,Color};
bool InputState(D3D12_RESOURCE_STATES s){return s==Nps||s==BothRead||s==D3D12_RESOURCE_STATE_GENERIC_READ;}
bool OutputState(D3D12_RESOURCE_STATES s){return InputState(s)||s==D3D12_RESOURCE_STATE_UNORDERED_ACCESS||
    s==D3D12_RESOURCE_STATE_COMMON||s==D3D12_RESOURCE_STATE_COPY_SOURCE||s==D3D12_RESOURCE_STATE_COPY_DEST||s==D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;}
bool RoleFormat(DXGI_FORMAT f,Role role,bool output){
    if(role==Role::Motion)return f==DXGI_FORMAT_R16G16_FLOAT||f==DXGI_FORMAT_R32G32_FLOAT||
        (!output&&(f==DXGI_FORMAT_R16G16_UNORM||f==DXGI_FORMAT_R16G16_SNORM));
    if(role==Role::Depth)return f==DXGI_FORMAT_R32_FLOAT||f==DXGI_FORMAT_R16_FLOAT||f==DXGI_FORMAT_R16_UNORM||
        f==DXGI_FORMAT_R24_UNORM_X8_TYPELESS||f==DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
    switch(f){
    case DXGI_FORMAT_R32G32B32A32_FLOAT:case DXGI_FORMAT_R16G16B16A16_FLOAT:case DXGI_FORMAT_R16G16B16A16_UNORM:
    case DXGI_FORMAT_R16G16B16A16_SNORM:case DXGI_FORMAT_R8G8B8A8_UNORM:case DXGI_FORMAT_R8G8B8A8_SNORM:
    case DXGI_FORMAT_R10G10B10A2_UNORM:return true;
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:case DXGI_FORMAT_B8G8R8A8_UNORM:case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
    case DXGI_FORMAT_B8G8R8X8_UNORM:case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB:return !output;
    // This shader only stores to its color UAVs. Prepare still requires the
    // device's typed UAV/store capabilities; no typed UAV load is assumed.
    case DXGI_FORMAT_R11G11B10_FLOAT:return true;
    default:return false;
    }
}
bool Compatible(DXGI_FORMAT resource,DXGI_FORMAT view){
    if(resource==view)return true;
    switch(resource){
    case DXGI_FORMAT_R32G32B32A32_TYPELESS:return view==DXGI_FORMAT_R32G32B32A32_FLOAT;
    case DXGI_FORMAT_R16G16B16A16_TYPELESS:return view==DXGI_FORMAT_R16G16B16A16_FLOAT||view==DXGI_FORMAT_R16G16B16A16_UNORM||view==DXGI_FORMAT_R16G16B16A16_SNORM;
    case DXGI_FORMAT_R32G32_TYPELESS:return view==DXGI_FORMAT_R32G32_FLOAT;
    case DXGI_FORMAT_R16G16_TYPELESS:return view==DXGI_FORMAT_R16G16_FLOAT||view==DXGI_FORMAT_R16G16_UNORM||view==DXGI_FORMAT_R16G16_SNORM;
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:return view==DXGI_FORMAT_R8G8B8A8_UNORM||view==DXGI_FORMAT_R8G8B8A8_UNORM_SRGB||view==DXGI_FORMAT_R8G8B8A8_SNORM;
    case DXGI_FORMAT_B8G8R8A8_TYPELESS:return view==DXGI_FORMAT_B8G8R8A8_UNORM||view==DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
    case DXGI_FORMAT_B8G8R8X8_TYPELESS:return view==DXGI_FORMAT_B8G8R8X8_UNORM||view==DXGI_FORMAT_B8G8R8X8_UNORM_SRGB;
    case DXGI_FORMAT_R10G10B10A2_TYPELESS:return view==DXGI_FORMAT_R10G10B10A2_UNORM;
    case DXGI_FORMAT_R32_TYPELESS:return view==DXGI_FORMAT_R32_FLOAT;
    case DXGI_FORMAT_R16_TYPELESS:return view==DXGI_FORMAT_R16_FLOAT||view==DXGI_FORMAT_R16_UNORM;
    case DXGI_FORMAT_R24G8_TYPELESS:return view==DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
    case DXGI_FORMAT_R32G8X24_TYPELESS:return view==DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
    default:return false;
    }
}
bool SameDevice(ID3D12DeviceChild* resource,ID3D12Device* device){
    ComPtr<ID3D12Device> actual;return resource&&SUCCEEDED(resource->GetDevice(IID_PPV_ARGS(&actual)))&&actual.Get()==device;
}
void Transition(ID3D12GraphicsCommandList* list,ID3D12Resource* resource,D3D12_RESOURCE_STATES from,D3D12_RESOURCE_STATES to){
    D3D12_RESOURCE_BARRIER barrier{};
    if(from==to){barrier.Type=D3D12_RESOURCE_BARRIER_TYPE_UAV;barrier.UAV.pResource=resource;}
    else{barrier.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;barrier.Transition={resource,D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,from,to};}
    list->ResourceBarrier(1,&barrier);
}
}
struct SrTemporalMotionPass::Impl {
    enum class State {Free,Recorded,Unknown,Submitted};
    struct Slot {
        State state=State::Free;uint64_t serial{},value{};ComPtr<ID3D12Fence> fence;
        std::array<ComPtr<ID3D12Resource>,13> resources;
        std::array<std::shared_ptr<void>,13> owners;
    };
    ComPtr<ID3D12Device> device;ComPtr<ID3D12RootSignature> root;ComPtr<ID3D12PipelineState> pso;
    ComPtr<ID3D12DescriptorHeap> descriptors;ComPtr<ID3D12Resource> constants;
    std::array<Slot,SlotCount> slots;std::array<uint8_t,192> formatSupport{};
    struct Timeline {ComPtr<ID3D12Fence> fence;uint64_t reserved{};};
    std::array<Timeline,SlotCount> timelines;
    unsigned char* mapped{};uint32_t descriptorStride{};bool removed{};
    SrTemporalPipelineStats stats;
    ~Impl(){if(mapped&&constants)constants->Unmap(0,nullptr);}
    uint32_t Collect(){
        uint32_t count=0;
        for(auto& slot:slots)if(slot.state!=State::Free){
            const auto completed=slot.fence->GetCompletedValue();
            if(completed==UINT64_MAX){removed=true;continue;}
            if(slot.state==State::Submitted&&completed>=slot.value){slot=Slot{};++count;}
        }
        return count;
    }
    Slot* Find(SrTemporalTicket ticket){return ticket.slot<SlotCount&&ticket.serial&&slots[ticket.slot].state!=State::Free&&
        slots[ticket.slot].serial==ticket.serial?&slots[ticket.slot]:nullptr;}
    SrTemporalRecordStatus Validate(const SrTemporalBinding& b,Role role,bool output){
        if(!b.resource||!b.width||!b.height)return SrTemporalRecordStatus::InvalidDescriptor;
        if(!SameDevice(b.resource.Get(),device.Get()))return SrTemporalRecordStatus::WrongDevice;
        const auto d=b.resource->GetDesc();
        if(d.Dimension!=D3D12_RESOURCE_DIMENSION_TEXTURE2D||d.Width!=b.width||d.Height!=b.height||
           d.MipLevels!=1||d.DepthOrArraySize!=1||d.SampleDesc.Count!=1||d.SampleDesc.Quality||
           (d.Flags&D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE)||!Compatible(d.Format,b.viewFormat)||!RoleFormat(b.viewFormat,role,output)||
           uint32_t(b.viewFormat)>=formatSupport.size()||!(formatSupport[b.viewFormat]&(output?2:1))||
           (output&&!(d.Flags&D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS)))return SrTemporalRecordStatus::InvalidDescriptor;
        if(output?!OutputState(b.beforeState):!InputState(b.beforeState))return SrTemporalRecordStatus::InvalidState;
        return SrTemporalRecordStatus::Recorded;
    }
};
SrTemporalMotionPass::SrTemporalMotionPass()=default;
SrTemporalMotionPass::~SrTemporalMotionPass(){
    if(!TryShutdown()){
        // Bounded four-slot failure retention, with CB/heap/PSO and all owners.
        // Never wait forever on an unsubmitted list or failed/unknown Signal.
        Log("SR motion: retaining bounded four-slot pool; unresolved GPU completion\n");(void)m_impl.release();
    }
}
SrTemporalPrepareStatus SrTemporalMotionPass::Prepare(ID3D12Device* device){
    if(!device)return SrTemporalPrepareStatus::InvalidDevice;
    if(m_impl){
        if(m_impl->device.Get()==device){
            if(m_impl->removed||FAILED(device->GetDeviceRemovedReason())){m_impl->removed=true;return SrTemporalPrepareStatus::DeviceRemoved;}
            return SrTemporalPrepareStatus::Ready;
        }
        if(!TryShutdown())return SrTemporalPrepareStatus::Busy;
    }
    if(FAILED(device->GetDeviceRemovedReason()))return SrTemporalPrepareStatus::DeviceRemoved;
    auto p=std::make_unique<Impl>();p->device=device;
    for(unsigned i=0;i<p->formatSupport.size();++i){
        const auto format=DXGI_FORMAT(i);
        if(!RoleFormat(format,Role::Motion,false)&&!RoleFormat(format,Role::Depth,false)&&!RoleFormat(format,Role::Color,false))continue;
        D3D12_FEATURE_DATA_FORMAT_SUPPORT support{format};
        if(SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT,&support,sizeof(support)))){
            if((support.Support1&(D3D12_FORMAT_SUPPORT1_TEXTURE2D|D3D12_FORMAT_SUPPORT1_SHADER_SAMPLE))==(D3D12_FORMAT_SUPPORT1_TEXTURE2D|D3D12_FORMAT_SUPPORT1_SHADER_SAMPLE))p->formatSupport[i]|=1;
            if((support.Support1&D3D12_FORMAT_SUPPORT1_TYPED_UNORDERED_ACCESS_VIEW)&&(support.Support2&D3D12_FORMAT_SUPPORT2_UAV_TYPED_STORE))p->formatSupport[i]|=2;
        }
    }
    D3D12_DESCRIPTOR_RANGE ranges[2]{{D3D12_DESCRIPTOR_RANGE_TYPE_SRV,9,0,0,0},{D3D12_DESCRIPTOR_RANGE_TYPE_UAV,4,0,0,0}};
    D3D12_ROOT_PARAMETER parameters[3]{};parameters[0].ParameterType=D3D12_ROOT_PARAMETER_TYPE_CBV;
    for(unsigned i=1;i<3;++i){parameters[i].ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;parameters[i].DescriptorTable={1,&ranges[i-1]};}
    D3D12_STATIC_SAMPLER_DESC samplers[4]{};const unsigned filters[]{0,0x14,0x114,0x194};
    for(unsigned i=0;i<4;++i){auto& s=samplers[i];s.Filter=D3D12_FILTER(filters[i]);s.AddressU=s.AddressV=s.AddressW=D3D12_TEXTURE_ADDRESS_MODE_CLAMP;s.ComparisonFunc=D3D12_COMPARISON_FUNC_ALWAYS;s.ShaderRegister=i;}
    D3D12_ROOT_SIGNATURE_DESC root{3,parameters,4,samplers,D3D12_ROOT_SIGNATURE_FLAG_NONE};ComPtr<ID3DBlob> blob,error;
    if(FAILED(D3D12SerializeRootSignature(&root,D3D_ROOT_SIGNATURE_VERSION_1,&blob,&error))||
       FAILED(device->CreateRootSignature(0,blob->GetBufferPointer(),blob->GetBufferSize(),IID_PPV_ARGS(&p->root))))return SrTemporalPrepareStatus::CreateFailed;
    ComPtr<ID3DBlob> shader;++p->stats.shaderCompileCalls;
    if(FAILED(D3DCompile(sr_motion_shader::Source,sizeof(sr_motion_shader::Source)-1,nullptr,nullptr,nullptr,"main","cs_5_0",
        D3DCOMPILE_ENABLE_STRICTNESS|D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&shader,&error))){
        Log("SR motion shader compilation failed: %.*s\n",error?int(error->GetBufferSize()):0,error?static_cast<const char*>(error->GetBufferPointer()):"");return SrTemporalPrepareStatus::CompileFailed;
    }
    D3D12_COMPUTE_PIPELINE_STATE_DESC pipeline{};pipeline.pRootSignature=p->root.Get();pipeline.CS={shader->GetBufferPointer(),shader->GetBufferSize()};++p->stats.pipelineCreateCalls;
    if(FAILED(device->CreateComputePipelineState(&pipeline,IID_PPV_ARGS(&p->pso))))return SrTemporalPrepareStatus::CreateFailed;
    D3D12_DESCRIPTOR_HEAP_DESC heap{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,SlotCount*DescriptorCount,D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE,0};
    if(FAILED(device->CreateDescriptorHeap(&heap,IID_PPV_ARGS(&p->descriptors))))return SrTemporalPrepareStatus::CreateFailed;
    p->descriptorStride=device->GetDescriptorHandleIncrementSize(heap.Type);
    D3D12_HEAP_PROPERTIES hp{};hp.Type=D3D12_HEAP_TYPE_UPLOAD;D3D12_RESOURCE_DESC cb{};
    cb.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;cb.Width=SlotCount*ConstantStride;cb.Height=cb.DepthOrArraySize=cb.MipLevels=1;
    cb.SampleDesc.Count=1;cb.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    if(FAILED(device->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&cb,D3D12_RESOURCE_STATE_GENERIC_READ,nullptr,IID_PPV_ARGS(&p->constants))))return SrTemporalPrepareStatus::CreateFailed;
    D3D12_RANGE noRead{};if(FAILED(p->constants->Map(0,&noRead,reinterpret_cast<void**>(&p->mapped))))return SrTemporalPrepareStatus::CreateFailed;
    m_impl=std::move(p);return SrTemporalPrepareStatus::Ready;
}
SrTemporalPipelineStats SrTemporalMotionPass::PipelineStats()const{return m_impl?m_impl->stats:SrTemporalPipelineStats{};}
SrTemporalTicket SrTemporalMotionPass::Record(ID3D12GraphicsCommandList* list,const SrTemporalMotionRequest& request){
    auto fail=[](SrTemporalRecordStatus status){return SrTemporalTicket{UINT32_MAX,0,status};};
    if(!m_impl)return fail(SrTemporalRecordStatus::NotPrepared);auto& p=*m_impl;p.Collect();
    if(p.removed||FAILED(p.device->GetDeviceRemovedReason())){p.removed=true;return fail(SrTemporalRecordStatus::DeviceRemoved);}
    if(!list||(list->GetType()!=D3D12_COMMAND_LIST_TYPE_DIRECT&&list->GetType()!=D3D12_COMMAND_LIST_TYPE_COMPUTE))return fail(SrTemporalRecordStatus::InvalidRequest);
    if(!SameDevice(list,p.device.Get()))return fail(SrTemporalRecordStatus::WrongDevice);
    const uint32_t width=request.constants.words[64],height=request.constants.words[65];
    if(!width||!height||width>D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION||height>D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION)return fail(SrTemporalRecordStatus::InvalidConstants);
    const auto& point=request.completion;
    if(!point.fence||!point.value||point.value==UINT64_MAX)return fail(SrTemporalRecordStatus::InvalidCompletion);
    if(!SameDevice(point.fence,p.device.Get()))return fail(SrTemporalRecordStatus::WrongDevice);
    for(const auto& slot:p.slots)if(slot.state!=Impl::State::Free&&slot.fence.Get()==point.fence)return fail(SrTemporalRecordStatus::InvalidCompletion);
    uint32_t timeline=SlotCount;
    for(uint32_t i=0;i<SlotCount;++i)if(p.timelines[i].fence.Get()==point.fence){
        if(point.value<=p.timelines[i].reserved)return fail(SrTemporalRecordStatus::InvalidCompletion);timeline=i;break;
    }
    const auto completed=point.fence->GetCompletedValue();
    if(completed==UINT64_MAX){p.removed=true;return fail(SrTemporalRecordStatus::DeviceRemoved);}
    if(point.value<=completed)return fail(SrTemporalRecordStatus::InvalidCompletion);
    std::array<const SrTemporalBinding*,13> bindings{};std::array<Role,13> roles{};
    for(unsigned i=0;i<8;++i){bindings[i]=&request.inputs[i];roles[i]=i<2?Role::Motion:i<5?Role::Depth:Role::Color;}
    if(request.unusedT8){bindings[8]=&*request.unusedT8;roles[8]=Role::Color;}
    for(unsigned i=0;i<4;++i){bindings[9+i]=&request.outputs[i];roles[9+i]=i<2?Role::Motion:Role::Color;}
    for(unsigned i=0;i<13;++i)if(bindings[i]){
        const auto status=p.Validate(*bindings[i],roles[i],i>=9);if(status!=SrTemporalRecordStatus::Recorded)return fail(status);
        for(unsigned j=0;j<i;++j)if(bindings[j]&&bindings[j]->resource==bindings[i]->resource){
            if(i>=9||roles[j]!=roles[i]||bindings[j]->viewFormat!=bindings[i]->viewFormat||bindings[j]->beforeState!=bindings[i]->beforeState)return fail(SrTemporalRecordStatus::Aliasing);
        }
        // A lease pins storage, but cannot make a reused UAV immutable. Prevent
        // conflicting reuse of this pass's own live resources across host lists.
        // Completed slots were collected above; shared immutable read/read is OK.
        for(const auto& live:p.slots)if(live.state!=Impl::State::Free)
            for(unsigned j=0;j<13;++j)if((i>=9||j>=9)&&live.resources[j]==bindings[i]->resource)return fail(SrTemporalRecordStatus::Aliasing);
    }
    for(unsigned i=0;i<4;++i){const auto& out=request.outputs[i];
        if(i<2?(out.width!=width||out.height!=height):(!request.allowClippedColorStores&&(out.width<width||out.height<height)))return fail(SrTemporalRecordStatus::InvalidDescriptor);
    }
    uint32_t index=SlotCount;for(uint32_t i=0;i<SlotCount;++i)if(p.slots[i].state==Impl::State::Free){index=i;break;}
    if(index==SlotCount||m_serial==UINT64_MAX)return fail(SrTemporalRecordStatus::NoCapacity);
    if(timeline==SlotCount)for(uint32_t i=0;i<SlotCount;++i){
        bool live=false;for(const auto& s:p.slots)if(s.state!=Impl::State::Free&&s.fence==p.timelines[i].fence)live=true;
        if(!live){timeline=i;break;}
    }
    if(timeline==SlotCount)return fail(SrTemporalRecordStatus::NoCapacity);
    // Validation finished. No fallible driver creation/compile after this point.
    auto cpu=p.descriptors->GetCPUDescriptorHandleForHeapStart();cpu.ptr+=SIZE_T(index)*DescriptorCount*p.descriptorStride;
    for(unsigned i=0;i<9;++i){D3D12_SHADER_RESOURCE_VIEW_DESC d{};d.Format=bindings[i]?bindings[i]->viewFormat:DXGI_FORMAT_R32G32B32A32_FLOAT;
        d.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D;d.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;d.Texture2D.MipLevels=1;
        p.device->CreateShaderResourceView(bindings[i]?bindings[i]->resource.Get():nullptr,&d,cpu);cpu.ptr+=p.descriptorStride;
    }
    for(unsigned i=0;i<4;++i){D3D12_UNORDERED_ACCESS_VIEW_DESC d{};d.Format=request.outputs[i].viewFormat;d.ViewDimension=D3D12_UAV_DIMENSION_TEXTURE2D;
        p.device->CreateUnorderedAccessView(request.outputs[i].resource.Get(),nullptr,&d,cpu);cpu.ptr+=p.descriptorStride;
    }
    std::memcpy(p.mapped+index*ConstantStride,&request.constants,480);
    auto& slot=p.slots[index];slot.state=Impl::State::Recorded;slot.serial=++m_serial;slot.value=point.value;slot.fence=point.fence;
    p.timelines[timeline]={point.fence,point.value};
    for(unsigned i=0;i<13;++i)if(bindings[i]){slot.resources[i]=bindings[i]->resource;slot.owners[i]=bindings[i]->owner;}
    for(const auto& out:request.outputs)Transition(list,out.resource.Get(),out.beforeState,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    ID3D12DescriptorHeap* heaps[]{p.descriptors.Get()};list->SetDescriptorHeaps(1,heaps);list->SetComputeRootSignature(p.root.Get());list->SetPipelineState(p.pso.Get());
    list->SetComputeRootConstantBufferView(0,p.constants->GetGPUVirtualAddress()+index*ConstantStride);
    auto gpu=p.descriptors->GetGPUDescriptorHandleForHeapStart();gpu.ptr+=UINT64(index)*DescriptorCount*p.descriptorStride;list->SetComputeRootDescriptorTable(1,gpu);
    gpu.ptr+=UINT64(9)*p.descriptorStride;list->SetComputeRootDescriptorTable(2,gpu);list->Dispatch((width+7)/8,(height+7)/8,1);
    for(const auto& out:request.outputs)Transition(list,out.resource.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,Nps);
    return {index,slot.serial,SrTemporalRecordStatus::Recorded};
}
bool SrTemporalMotionPass::SubmissionReported(SrTemporalTicket ticket,bool executed,bool signalSucceeded){
    if(!m_impl||!executed)return false;auto* slot=m_impl->Find(ticket);
    if(!slot||slot->state==Impl::State::Submitted)return false;
    slot->state=signalSucceeded?Impl::State::Submitted:Impl::State::Unknown;return true;
}
bool SrTemporalMotionPass::CancelNeverExecuted(SrTemporalTicket ticket){
    if(!m_impl)return false;auto* slot=m_impl->Find(ticket);if(!slot||slot->state!=Impl::State::Recorded)return false;
    *slot=Impl::Slot{};return true;
}
uint32_t SrTemporalMotionPass::Collect(){return m_impl?m_impl->Collect():0;}
uint32_t SrTemporalMotionPass::BusySlots()const{uint32_t n=0;if(m_impl)for(const auto& s:m_impl->slots)n+=s.state!=Impl::State::Free;return n;}
bool SrTemporalMotionPass::DeviceRemoved()const{return m_impl&&m_impl->removed;}
bool SrTemporalMotionPass::TryShutdown(){if(!m_impl)return true;m_impl->Collect();if(BusySlots())return false;m_impl.reset();return true;}
}
