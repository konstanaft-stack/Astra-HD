#include "Render/AerReconstructionPass.hpp"
#include "Render/NativeObjectIdentity.hpp"
#include "AerReconstructionShaders.hpp"
#include "AerPixelEvidence.hpp"
#include <d3dcompiler.h>
#include <wrl.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <mutex>
#include <vector>
extern void Log(const char*,...);
namespace cvr::render {
    using Microsoft::WRL::ComPtr;
    namespace {
        using M4=std::array<float,16>;
        using V4=std::array<float,4>;
        bool Finite(float v){
            return std::isfinite(v);
        }
        M4 Multiply(const M4&a,const M4&b){
            M4 o{
            };
            for(unsigned r=0;r<4;++r)for(unsigned c=0;c<4;++c)for(unsigned k=0;k<4;++k)o[r*4+c]+=a[r*4+k]*b[k*4+c];
            return o;
        }
        bool Quaternion(const V4&q){
            float n=0;
            for(float v:q){
                if(!Finite(v))return false;
                n+=v*v;
            }
            return std::abs(n-1)<.002f;
        }
        struct View {
            DXGI_FORMAT format=DXGI_FORMAT_UNKNOWN;
            bool manual=false;
        };
        View ResolveView(DXGI_FORMAT f,AerEncoding e){
            if(e!=AerEncoding::Linear&&e!=AerEncoding::EncodedSrgb)return {
            };
            const bool encoded=e==AerEncoding::EncodedSrgb;
            switch(f){
                case DXGI_FORMAT_R8G8B8A8_TYPELESS:return {
                    encoded?DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:DXGI_FORMAT_R8G8B8A8_UNORM,false
                };
                case DXGI_FORMAT_B8G8R8A8_TYPELESS:return {
                    encoded?DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:DXGI_FORMAT_B8G8R8A8_UNORM,false
                };
                case DXGI_FORMAT_R8G8B8A8_UNORM:case DXGI_FORMAT_B8G8R8A8_UNORM:return {
                    f,encoded
                };
                case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:return encoded?View{
                    f,false
                }
                :View{
                };
                case DXGI_FORMAT_R16G16B16A16_FLOAT:case DXGI_FORMAT_R32G32B32A32_FLOAT:return !encoded?View{
                    f,false
                }
                :View{
                };
                default:return {
                };
            }
        }
        bool Compile(const char*s,const char*entry,const char*profile,bool decode,bool encode,bool bypass,ComPtr<ID3DBlob>&out){
            D3D_SHADER_MACRO macros[]={
                {
                    "AER_DECODE",decode?"1":"0"
                },{
                    "AER_ENCODE",encode?"1":"0"
                },{
                    "AER_BYPASS",bypass?"1":"0"
                },{
                    nullptr,nullptr
                }
            };
            ComPtr<ID3DBlob>errors;
            auto hr=D3DCompile(s,strlen(s),nullptr,macros,nullptr,entry,profile,D3DCOMPILE_ENABLE_STRICTNESS|D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&out,&errors);
            if(FAILED(hr))Log("AER shader %s failed hr=%08X: %.*s\n",entry,unsigned(hr),errors?int(errors->GetBufferSize()):0,errors?(const char*)errors->GetBufferPointer():"");
            return SUCCEEDED(hr);
        }
    }
    struct AerReconstructionPass::Impl {
        enum class Status{
            Free,Recorded,Unknown,Submitted
        };
        struct Slot{
            Status status=Status::Free;
            uint64_t serial=0,value=0;
            ComPtr<ID3D12Fence>fence;
            ComPtr<ID3D12Resource>source,destination;
            std::shared_ptr<void>owner;
            std::unique_ptr<pixel_evidence::Capture> evidence;
        };
        struct Pipeline{
            DXGI_FORMAT format;
            bool decode,encode,bypass;
            ComPtr<ID3D12PipelineState>pso;
        };
        mutable std::mutex mutex;
        ComPtr<ID3D12Device>device;
        ComPtr<ID3D12RootSignature>root;
        ComPtr<ID3D12DescriptorHeap>srv,rtv;
        ComPtr<ID3D12Resource>constants;
        ComPtr<ID3DBlob>vs;
        std::array<Slot,SlotCount>slots{
        };
        std::vector<Pipeline>pipelines;
        unsigned char*mapped=nullptr;
        UINT srvSize=0,rtvSize=0;
        bool removed=false;
        uint32_t shaderCompileCalls=0, pipelineCreateCalls=0;
        pixel_evidence::Configuration evidenceConfiguration;
        uint32_t CollectLocked(){
            uint32_t count=0;
            for(auto&s:slots)if(s.status!=Status::Free){
                const auto done=s.fence->GetCompletedValue();
                if(done==UINT64_MAX){
                    removed=true;
                    continue;
                }
                if(s.status==Status::Submitted&&done>=s.value){
                    if(s.evidence)Log("AER pixel evidence eye=%u source=%lld saved=%d\n",s.evidence->eye,
                        static_cast<long long>(s.evidence->sourceId),int(pixel_evidence::Save(*s.evidence)));
                    s=Slot{
                    };
                    ++count;
                }
            }
            return count;
        }
        Slot* Find(AerRecordTicket t){
            return t.slot<SlotCount&&t.serial&&slots[t.slot].serial==t.serial&&slots[t.slot].status!=Status::Free?&slots[t.slot]:nullptr;
        }
        ID3D12PipelineState* FindPipeline(View source,View target,bool bypass)const{
            for(auto&p:pipelines)if(p.format==target.format&&p.decode==source.manual&&p.encode==target.manual&&p.bypass==bypass)return p.pso.Get();
            return nullptr;
        }
        ID3D12PipelineState* PreparePipeline(View source,View target,bool bypass){
            if(auto* ready=FindPipeline(source,target,bypass))return ready;
            if(pipelines.size()>=48)return nullptr;
            ComPtr<ID3DBlob>ps;
            ++shaderCompileCalls;
            if(!Compile(aer_shaders::Pixel,"LukeCasWarpPS","ps_5_0",source.manual,target.manual,bypass,ps))return nullptr;
            D3D12_GRAPHICS_PIPELINE_STATE_DESC p{
            };
            p.pRootSignature=root.Get();
            p.VS={
                vs->GetBufferPointer(),vs->GetBufferSize()
            };
            p.PS={
                ps->GetBufferPointer(),ps->GetBufferSize()
            };
            p.PrimitiveTopologyType=D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
            p.NumRenderTargets=1;
            p.RTVFormats[0]=target.format;
            p.SampleDesc.Count=1;
            p.SampleMask=UINT_MAX;
            p.RasterizerState.FillMode=D3D12_FILL_MODE_SOLID;
            p.RasterizerState.CullMode=D3D12_CULL_MODE_BACK;
            p.BlendState.RenderTarget[0].RenderTargetWriteMask=15;
            Pipeline entry{
                target.format,source.manual,target.manual,bypass,{
                }
            };
            ++pipelineCreateCalls;
            if(FAILED(device->CreateGraphicsPipelineState(&p,IID_PPV_ARGS(&entry.pso))))return nullptr;
            pipelines.push_back(std::move(entry));
            return pipelines.back().pso.Get();
        }
    };
    AerReconstructionPass::AerReconstructionPass()=default;
    AerReconstructionPass::~AerReconstructionPass(){
        if(!TryShutdown()){
            // Failure-only process-lifetime retention. At most this fixed six-slot pool
            // is retained per destroyed owner. Never release possibly executing GPU data
            // and never hang teardown on a fence that was not successfully signaled.
            Log("AER: retaining bounded pool at teardown; outstanding/unknown GPU work\n");
            (void)m_impl.release();
        }
    }
    bool AerReconstructionPass::BuildVertexConstants(const AerWarpParams&p,uint32_t width,uint32_t height,AerVertexConstants&out){
        if(!width||!height||!p.sourceEpoch||p.sourceEpoch!=p.referenceEpoch||!p.sourceSpace||p.sourceSpace!=p.referenceSpace||!Quaternion(p.reference)||!Quaternion(p.captured))return false;
        for(float v:p.tangents)if(!Finite(v)||v<=0)return false;
        for(float v:p.crop)if(!Finite(v))return false;
        const auto&c=p.crop;
        if(c[0]<0||c[1]<0||c[2]<=c[0]||c[3]<=c[1]||c[2]>float(width)||c[3]>float(height))return false;
        const auto&a=p.reference;
        const auto&b=p.captured;
        const V4 inverse{
            -a[0],-a[1],-a[2],a[3]
        };
        const float x=inverse[0]*b[3]+b[0]*inverse[3]+inverse[1]*b[2]-inverse[2]*b[1],y=b[1]*inverse[3]-b[2]*inverse[0]+inverse[1]*b[3]+inverse[2]*b[0],z=b[1]*inverse[0]+b[2]*inverse[3]-inverse[1]*b[0]+inverse[2]*b[3],w=b[3]*inverse[3]-inverse[0]*b[0]-inverse[1]*b[1]-inverse[2]*b[2];
        const M4 rotation{
            x*x+w*w-y*y-z*z,2*(x*y-z*w),2*(x*z+y*w),0,2*(x*y+z*w),w*w-x*x+y*y-z*z,2*(y*z-x*w),0,2*(x*z-y*w),2*(y*z+x*w),w*w-x*x-y*y+z*z,0,0,0,0,1
        };
        const auto&t=p.tangents;
        const float hx=(t[2]+t[3])*.5f,hy=(t[0]+t[1])*.5f,ox=(t[3]-t[2])*.5f,oy=(t[0]-t[1])*.5f;
        if(!Finite(hx)||!Finite(hy))return false;
        const M4 project{
            1/hx,0,ox/hx,0,0,1/hy,oy/hy,0,0,0,0,.01f,0,0,-1,0
        },unproject{
            hx,0,0,ox,0,hy,0,oy,0,0,0,-1,0,0,100,0
        };
        auto m=Multiply(Multiply(project,rotation),unproject);
        AerVertexConstants next{
        };
        next.sourceViewport={
            (c[2]-c[0])/(2*float(width)),(c[1]-c[3])/(2*float(height)),(c[2]+c[0])/(2*float(width)),(c[3]+c[1])/(2*float(height))
        };
        next.color0={
            1,1,1,1
        };
        for(unsigned r=0;r<4;++r)for(unsigned col=0;col<4;++col){
            if(!Finite(m[r*4+col]))return false;
            next.matrix0[col*4+r]=m[r*4+col];
        }
        out=next;
        return true;
    }
    bool AerReconstructionPass::Initialize(ID3D12Device*d){
        if(!d)return false;
        if(m_impl){
            if(m_impl->device.Get()==d)return !m_impl->removed;
            if(!TryShutdown())return false;
        }
        auto p=std::make_unique<Impl>();
        p->device=d;
        D3D12_DESCRIPTOR_HEAP_DESC hd{
        };
        hd.NumDescriptors=SlotCount;
        hd.Type=D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        hd.Flags=D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        if(FAILED(d->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&p->srv))))return false;
        hd.Type=D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        hd.Flags=D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
        if(FAILED(d->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&p->rtv))))return false;
        p->srvSize=d->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        p->rtvSize=d->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
        D3D12_HEAP_PROPERTIES hp{
        };
        hp.Type=D3D12_HEAP_TYPE_UPLOAD;
        D3D12_RESOURCE_DESC rd{
        };
        rd.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;
        rd.Width=SlotCount*512;
        rd.Height=1;
        rd.DepthOrArraySize=1;
        rd.MipLevels=1;
        rd.SampleDesc.Count=1;
        rd.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        if(FAILED(d->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&rd,D3D12_RESOURCE_STATE_GENERIC_READ,nullptr,IID_PPV_ARGS(&p->constants))))return false;
        D3D12_RANGE noRead{
        };
        if(FAILED(p->constants->Map(0,&noRead,reinterpret_cast<void**>(&p->mapped))))return false;
        D3D12_DESCRIPTOR_RANGE range{
        };
        range.RangeType=D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        range.NumDescriptors=1;
        D3D12_ROOT_PARAMETER rp[3]{
        };
        rp[0].ParameterType=rp[1].ParameterType=D3D12_ROOT_PARAMETER_TYPE_CBV;
        rp[0].ShaderVisibility=D3D12_SHADER_VISIBILITY_VERTEX;
        rp[1].ShaderVisibility=D3D12_SHADER_VISIBILITY_PIXEL;
        rp[2].ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        rp[2].DescriptorTable={
            1,&range
        };
        rp[2].ShaderVisibility=D3D12_SHADER_VISIBILITY_PIXEL;
        D3D12_STATIC_SAMPLER_DESC sm[2]{
        };
        for(unsigned i=0;i<2;++i){
            sm[i].Filter=i?D3D12_FILTER_MIN_MAG_MIP_LINEAR:D3D12_FILTER_MIN_MAG_MIP_POINT;
            sm[i].AddressU=sm[i].AddressV=sm[i].AddressW=D3D12_TEXTURE_ADDRESS_MODE_BORDER;
            sm[i].ShaderRegister=i;
            sm[i].ShaderVisibility=D3D12_SHADER_VISIBILITY_PIXEL;
            sm[i].BorderColor=D3D12_STATIC_BORDER_COLOR_TRANSPARENT_BLACK;
            sm[i].ComparisonFunc=D3D12_COMPARISON_FUNC_NEVER;
            sm[i].MaxAnisotropy=1;
        }
        D3D12_ROOT_SIGNATURE_DESC rs{
        };
        rs.NumParameters=3;
        rs.pParameters=rp;
        rs.NumStaticSamplers=2;
        rs.pStaticSamplers=sm;
        ComPtr<ID3DBlob>blob,err;
        if(FAILED(D3D12SerializeRootSignature(&rs,D3D_ROOT_SIGNATURE_VERSION_1,&blob,&err))||FAILED(d->CreateRootSignature(0,blob->GetBufferPointer(),blob->GetBufferSize(),IID_PPV_ARGS(&p->root))))return false;
        ++p->shaderCompileCalls;
        if(!Compile(aer_shaders::Vertex,"WarpVS","vs_5_0",false,false,false,p->vs))return false;
        m_impl=std::move(p);
        return true;
    }
    bool AerReconstructionPass::Prepare(DXGI_FORMAT sourceFormat,DXGI_FORMAT destinationFormat,
                                        const AerColorRoute& route){
        if(!m_impl)return false;
        auto&p=*m_impl;
        std::lock_guard lock(p.mutex);
        if(p.removed||FAILED(p.device->GetDeviceRemovedReason()))return false;
        const View source=ResolveView(sourceFormat,route.input);
        const View target=ResolveView(destinationFormat,route.output);
        if(source.format==DXGI_FORMAT_UNKNOWN||target.format==DXGI_FORMAT_UNKNOWN)return false;
        return p.PreparePipeline(source,target,false)&&p.PreparePipeline(source,target,true);
    }
    AerPipelineStats AerReconstructionPass::PipelineStats()const{
        if(!m_impl)return {};
        std::lock_guard lock(m_impl->mutex);
        return {m_impl->shaderCompileCalls,m_impl->pipelineCreateCalls,
                static_cast<uint32_t>(m_impl->pipelines.size())};
    }
    AerRecordTicket AerReconstructionPass::Record(ID3D12GraphicsCommandList*c,const AerRecordRequest&r){
        if(!m_impl||!c||c->GetType()!=D3D12_COMMAND_LIST_TYPE_DIRECT||!r.source||!r.destination||r.source==r.destination||!r.completion.fence||!r.completion.value||r.completion.value==UINT64_MAX||r.eye>1||!(r.sourceState&D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE))return {
        };
        auto&p=*m_impl;
        std::lock_guard lock(p.mutex);
        p.CollectLocked();
        if(p.removed)return {
        };
        auto completed=r.completion.fence->GetCompletedValue();
        if(completed==UINT64_MAX){
            p.removed=true;
            return {
            };
        }
        if(r.completion.value<=completed)return {
        };
        for(ID3D12DeviceChild*child:{
            static_cast<ID3D12DeviceChild*>(r.source),static_cast<ID3D12DeviceChild*>(r.destination),static_cast<ID3D12DeviceChild*>(r.completion.fence),static_cast<ID3D12DeviceChild*>(c)
        }){
            ComPtr<ID3D12Device>d;
            if(FAILED(child->GetDevice(IID_PPV_ARGS(&d)))||!cvr::gpu::SameNativeDevice(d.Get(),p.device.Get()))return {
            };
        }
        const auto sd=r.source->GetDesc(),td=r.destination->GetDesc();
        if(sd.Dimension!=D3D12_RESOURCE_DIMENSION_TEXTURE2D||td.Dimension!=D3D12_RESOURCE_DIMENSION_TEXTURE2D||sd.DepthOrArraySize!=1||td.DepthOrArraySize!=1||sd.SampleDesc.Count!=1||td.SampleDesc.Count!=1||!sd.MipLevels||!td.MipLevels||sd.Width>UINT32_MAX||td.Width>LONG_MAX||td.Height>LONG_MAX||(sd.Flags&D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE)||!(td.Flags&D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET))return {
        };
        AerVertexConstants vc;
        if(!BuildVertexConstants(r.warp,uint32_t(sd.Width),sd.Height,vc))return {
        };
        const auto&f=r.filter;
        if(!Finite(f.emphasis)||!Finite(f.deblur)||!Finite(f.contrast)||!Finite(f.vibrance)||f.emphasis<0||f.emphasis>1||f.deblur<0||f.deblur>1||std::abs(f.contrast)>1||std::abs(f.vibrance)>1)return {
        };
        View sv=ResolveView(sd.Format,r.color.input),tv=ResolveView(td.Format,r.color.output);
        if(sv.format==DXGI_FORMAT_UNKNOWN||tv.format==DXGI_FORMAT_UNKNOWN)return {
        };
        uint32_t index=SlotCount;
        for(uint32_t i=0;i<SlotCount;++i)if(p.slots[i].status==Impl::Status::Free){
            index=i;
            break;
        }
        if(index==SlotCount||m_serial==UINT64_MAX)return {
        };
        auto*cas=p.FindPipeline(sv,tv,false);
        auto*bypass=p.FindPipeline(sv,tv,true);
        if(!cas||!bypass)return {UINT32_MAX,0,AerRecordStatus::NotPrepared};
        auto*pso=f.deblur==0&&f.contrast==0&&f.vibrance==0?bypass:cas;
        // Everything fallible precedes command-list mutation and slot publication.
        auto sh=p.srv->GetCPUDescriptorHandleForHeapStart();
        sh.ptr+=SIZE_T(index)*p.srvSize;
        auto th=p.rtv->GetCPUDescriptorHandleForHeapStart();
        th.ptr+=SIZE_T(index)*p.rtvSize;
        D3D12_SHADER_RESOURCE_VIEW_DESC srv{
        };
        srv.Format=sv.format;
        srv.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D;
        srv.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.Texture2D.MipLevels=1;
        p.device->CreateShaderResourceView(r.source,&srv,sh);
        D3D12_RENDER_TARGET_VIEW_DESC rtv{
        };
        rtv.Format=tv.format;
        rtv.ViewDimension=D3D12_RTV_DIMENSION_TEXTURE2D;
        p.device->CreateRenderTargetView(r.destination,&rtv,th);
        const std::array<float,4> pc{
            f.emphasis,4*f.deblur,f.contrast,f.vibrance
        };
        std::unique_ptr<pixel_evidence::Capture> evidence;
        try {evidence=pixel_evidence::Prepare(p.evidenceConfiguration,p.device.Get(),r,vc,pc);}catch(...){}
        memcpy(p.mapped+index*512,&vc,sizeof(vc));
        memcpy(p.mapped+index*512+256,pc.data(),16);
        auto&s=p.slots[index];
        s.status=Impl::Status::Recorded;
        s.serial=++m_serial;
        s.value=r.completion.value;
        s.fence=r.completion.fence;
        s.source=r.source;
        s.destination=r.destination;
        s.owner=r.sourceOwner;
        s.evidence=std::move(evidence);
        const float black[]={
            0,0,0,1
        };
        c->ClearRenderTargetView(th,black,0,nullptr);
        D3D12_VIEWPORT vp{
            0,0,float(td.Width),float(td.Height),0,1
        };
        D3D12_RECT sc{
            0,0,LONG(td.Width),LONG(td.Height)
        };
        c->RSSetViewports(1,&vp);
        c->RSSetScissorRects(1,&sc);
        c->OMSetRenderTargets(1,&th,FALSE,nullptr);
        c->SetGraphicsRootSignature(p.root.Get());
        c->SetPipelineState(pso);
        ID3D12DescriptorHeap*heaps[]={
            p.srv.Get()
        };
        c->SetDescriptorHeaps(1,heaps);
        c->SetGraphicsRootConstantBufferView(0,p.constants->GetGPUVirtualAddress()+index*512);
        c->SetGraphicsRootConstantBufferView(1,p.constants->GetGPUVirtualAddress()+index*512+256);
        auto gpu=p.srv->GetGPUDescriptorHandleForHeapStart();
        gpu.ptr+=UINT64(index)*p.srvSize;
        c->SetGraphicsRootDescriptorTable(2,gpu);
        c->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
        c->DrawInstanced(4,1,0,0);
        if(s.evidence) {
            pixel_evidence::Copy(c,r.source,r.sourceState,s.evidence->input);
            pixel_evidence::Copy(c,r.destination,D3D12_RESOURCE_STATE_RENDER_TARGET,s.evidence->output);
        }
        return {
            index,s.serial,AerRecordStatus::Recorded
        };
    }
    bool AerReconstructionPass::SubmissionReported(AerRecordTicket t,bool executed,bool signalSucceeded){
        if(!m_impl||!executed)return false;
        auto&p=*m_impl;
        std::lock_guard lock(p.mutex);
        auto*s=p.Find(t);
        if(!s||s->status==Impl::Status::Submitted)return false;
        s->status=signalSucceeded?Impl::Status::Submitted:Impl::Status::Unknown;
        return true;
    }
    bool AerReconstructionPass::CancelNeverExecuted(AerRecordTicket t){
        if(!m_impl)return false;
        auto&p=*m_impl;
        std::lock_guard lock(p.mutex);
        auto*s=p.Find(t);
        if(!s||s->status!=Impl::Status::Recorded)return false;
        *s=Impl::Slot{
        };
        return true;
    }
    uint32_t AerReconstructionPass::Collect(){
        if(!m_impl)return 0;
        std::lock_guard lock(m_impl->mutex);
        return m_impl->CollectLocked();
    }
    uint32_t AerReconstructionPass::BusySlots()const{
        if(!m_impl)return 0;
        std::lock_guard lock(m_impl->mutex);
        uint32_t n=0;
        for(auto&s:m_impl->slots)n+=s.status!=Impl::Status::Free;
        return n;
    }
    bool AerReconstructionPass::DeviceRemoved()const{
        if(!m_impl)return false;
        std::lock_guard lock(m_impl->mutex);
        return m_impl->removed;
    }
    bool AerReconstructionPass::TryShutdown(){
        if(!m_impl)return true;
        {
            std::lock_guard lock(m_impl->mutex);
            m_impl->CollectLocked();
            for(auto&s:m_impl->slots)if(s.status!=Impl::Status::Free)return false;
        }
        m_impl.reset();
        return true;
    }
}
// namespace cvr::render
