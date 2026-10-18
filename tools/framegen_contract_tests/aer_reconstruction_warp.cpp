#include "Render/SharpenPass.hpp"
#if USE_AER_PASS
#include "Render/AerReconstructionPass.hpp"
#include "Framegen/ColorCopy.hpp"
#endif
#include <d3d12sdklayers.h>
#include <dxgi1_6.h>
#include <d3dcompiler.h>
#include <array>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <vector>
#include "luke_warp_reference.hpp"
using Microsoft::WRL::ComPtr;
void Log(const char*,...) {}
static void Check(bool ok,const char* text){if(!ok)throw std::runtime_error(text);}
static void Hr(HRESULT hr,const char* text){Check(SUCCEEDED(hr),text);}
static std::vector<char> Read(const std::string& path){std::ifstream f(path,std::ios::binary);Check(bool(f),"missing pinned oracle asset");return {std::istreambuf_iterator<char>(f),{}};}
static void Barrier(ID3D12GraphicsCommandList* c,ID3D12Resource* r,D3D12_RESOURCE_STATES a,D3D12_RESOURCE_STATES b){D3D12_RESOURCE_BARRIER x{};x.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;x.Transition={r,D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,a,b};c->ResourceBarrier(1,&x);}
static ComPtr<ID3D12Resource> Buffer(ID3D12Device* d,UINT64 n,D3D12_HEAP_TYPE t){D3D12_HEAP_PROPERTIES h{};h.Type=t;D3D12_RESOURCE_DESC x{};x.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;x.Width=n;x.Height=1;x.DepthOrArraySize=1;x.MipLevels=1;x.SampleDesc.Count=1;x.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;ComPtr<ID3D12Resource> r;Hr(d->CreateCommittedResource(&h,D3D12_HEAP_FLAG_NONE,&x,t==D3D12_HEAP_TYPE_UPLOAD?D3D12_RESOURCE_STATE_GENERIC_READ:D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&r)),"buffer");return r;}
static ComPtr<ID3D12Resource> Tex(ID3D12Device*d,DXGI_FORMAT fmt,UINT w,UINT h,D3D12_RESOURCE_STATES state){D3D12_HEAP_PROPERTIES heap{};heap.Type=D3D12_HEAP_TYPE_DEFAULT;D3D12_RESOURCE_DESC x{};x.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;x.Width=w;x.Height=h;x.DepthOrArraySize=1;x.MipLevels=1;x.SampleDesc.Count=1;x.Format=fmt;x.Flags=D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;ComPtr<ID3D12Resource> r;Hr(d->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&x,state,nullptr,IID_PPV_ARGS(&r)),"texture");return r;}
struct Commands{ComPtr<ID3D12CommandAllocator>a;ComPtr<ID3D12GraphicsCommandList>c;Commands(ID3D12Device*d){Hr(d->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&a)),"allocator");Hr(d->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,a.Get(),nullptr,IID_PPV_ARGS(&c)),"commands");}void Execute(ID3D12CommandQueue*q){Hr(c->Close(),"close");ID3D12CommandList*l[]={c.Get()};q->ExecuteCommandLists(1,l);}};
static void Complete(ID3D12Fence*f,UINT64 n){HANDLE e=CreateEventW(nullptr,FALSE,FALSE,nullptr);Check(e!=nullptr,"event");if(f->GetCompletedValue()<n){Hr(f->SetEventOnCompletion(n,e),"completion event");Check(WaitForSingleObject(e,10000)==WAIT_OBJECT_0,"WARP completion timeout");}CloseHandle(e);Check(f->GetCompletedValue()!=UINT64_MAX&&f->GetCompletedValue()>=n,"completion");}
struct Image{ComPtr<ID3D12Resource>r,upload,readback;D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};UINT64 size{};};
static Image ImageResources(ID3D12Device*d,DXGI_FORMAT f,UINT w,UINT h,D3D12_RESOURCE_STATES s){Image i;i.r=Tex(d,f,w,h,s);auto desc=i.r->GetDesc();d->GetCopyableFootprints(&desc,0,1,0,&i.fp,nullptr,nullptr,&i.size);i.upload=Buffer(d,i.size,D3D12_HEAP_TYPE_UPLOAD);i.readback=Buffer(d,i.size,D3D12_HEAP_TYPE_READBACK);return i;}
static D3D12_TEXTURE_COPY_LOCATION Location(ID3D12Resource*r){D3D12_TEXTURE_COPY_LOCATION l{};l.pResource=r;l.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;return l;}
static D3D12_TEXTURE_COPY_LOCATION Placed(ID3D12Resource*r,const D3D12_PLACED_SUBRESOURCE_FOOTPRINT& f){auto l=Location(r);l.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;l.PlacedFootprint=f;return l;}
static uint16_t UnitToHalf(float value) {
    Check(std::isfinite(value)&&value>=0&&value<=1,"fixture value outside SDR range");
    if(value==0)return 0;
    uint32_t bits{};std::memcpy(&bits,&value,4);
    const int exponent=int((bits>>23)&255)-127+15;
    if(exponent<=0){
        if(exponent < -10)return 0;
        const unsigned shift=unsigned(14-exponent);
        return uint16_t((((bits&0x7fffff)|0x800000)+(1u<<(shift-1)))>>shift);
    }
    return uint16_t((bits+0x1000u-0x38000000u)>>13);
}
static float HalfToFloat(uint16_t value) {
    const unsigned exponent=(value>>10)&31, fraction=value&1023;
    Check(exponent!=31,"nonfinite WARP FLOAT16 readback");
    const float magnitude=exponent?std::ldexp(1.0f+float(fraction)/1024,int(exponent)-15):std::ldexp(float(fraction),-24);
    return (value&0x8000)?-magnitude:magnitude;
}
static void Upload(ID3D12GraphicsCommandList*c,Image&i,UINT seed,bool dark){
    void*m{};D3D12_RANGE none{};Hr(i.upload->Map(0,&none,&m),"upload map");
    const bool half=i.r->GetDesc().Format==DXGI_FORMAT_R16G16B16A16_FLOAT;
    for(UINT y=0;y<i.fp.Footprint.Height;++y)for(UINT x=0;x<i.fp.Footprint.Width;++x){
        auto*p=static_cast<unsigned char*>(m)+y*i.fp.Footprint.RowPitch+x*(half?8:4);
        for(UINT k=0;k<3;++k){
            const unsigned code=dark?((x*3+y+seed+k*7)%64):((x*29+y*17+seed*71+k*43+(x>y?61:0))%256);
            if(half){
                // Normalized SDR-linear, including zero, one, subnormal half,
                // and values on either side of the sRGB linear-segment boundary.
                float value=float(code)/(dark?8192.0f:255.0f);
                if(x==0)value=0;
                if(x==1)value=std::ldexp(1.0f,-20);
                if(x==i.fp.Footprint.Width-1)value=1;
                const auto encoded=UnitToHalf(value);std::memcpy(p+k*2,&encoded,2);
            }else p[k]=static_cast<unsigned char>(code);
        }
        if(half){const uint16_t one=0x3c00;std::memcpy(p+6,&one,2);}else p[3]=255;
    }
    i.upload->Unmap(0,nullptr);auto s=Placed(i.upload.Get(),i.fp),t=Location(i.r.Get());
    c->CopyTextureRegion(&t,0,0,0,&s,nullptr);Barrier(c,i.r.Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
}
static void Readback(ID3D12GraphicsCommandList*c,Image&i){Barrier(c,i.r.Get(),D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_COPY_SOURCE);auto s=Location(i.r.Get()),t=Placed(i.readback.Get(),i.fp);c->CopyTextureRegion(&t,0,0,0,&s,nullptr);Barrier(c,i.r.Get(),D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_RENDER_TARGET);}
// Pinned original DXBC is the independent GPU oracle, not the reconstructed HLSL.
struct Oracle{ComPtr<ID3D12RootSignature>root;ComPtr<ID3D12PipelineState>pso;ComPtr<ID3D12DescriptorHeap>srv,rtv;ComPtr<ID3D12Resource>cb;
 Oracle(ID3D12Device*d,const std::string& audit,const luke_warp_reference::Constants& vs,std::array<float,4> ps,bool bypass,DXGI_FORMAT target){
  D3D12_DESCRIPTOR_HEAP_DESC hd{};hd.NumDescriptors=1;hd.Type=D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;hd.Flags=D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;Hr(d->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&srv)),"oracle srv");hd.Type=D3D12_DESCRIPTOR_HEAP_TYPE_RTV;hd.Flags=D3D12_DESCRIPTOR_HEAP_FLAG_NONE;Hr(d->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&rtv)),"oracle rtv");
  cb=Buffer(d,512,D3D12_HEAP_TYPE_UPLOAD);void*m{};Hr(cb->Map(0,nullptr,&m),"oracle cb");memcpy(m,&vs,sizeof(vs));memcpy((char*)m+256,ps.data(),16);cb->Unmap(0,nullptr);
  D3D12_DESCRIPTOR_RANGE range{};range.RangeType=D3D12_DESCRIPTOR_RANGE_TYPE_SRV;range.NumDescriptors=2;range.BaseShaderRegister=0;
  // Original t1 is declared but unreachable for Draw4; use two duplicated descriptors.
  srv.Reset();hd.Type=D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;hd.Flags=D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;hd.NumDescriptors=2;Hr(d->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&srv)),"oracle srv2");
  D3D12_ROOT_PARAMETER rp[3]{};rp[0].ParameterType=rp[1].ParameterType=D3D12_ROOT_PARAMETER_TYPE_CBV;rp[0].ShaderVisibility=D3D12_SHADER_VISIBILITY_VERTEX;rp[1].ShaderVisibility=D3D12_SHADER_VISIBILITY_PIXEL;rp[2].ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;rp[2].DescriptorTable={1,&range};rp[2].ShaderVisibility=D3D12_SHADER_VISIBILITY_PIXEL;
  D3D12_STATIC_SAMPLER_DESC sm{};sm.Filter=bypass?D3D12_FILTER_MIN_MAG_MIP_LINEAR:D3D12_FILTER_MIN_MAG_MIP_POINT;sm.AddressU=sm.AddressV=sm.AddressW=D3D12_TEXTURE_ADDRESS_MODE_BORDER;sm.BorderColor=D3D12_STATIC_BORDER_COLOR_TRANSPARENT_BLACK;sm.ShaderVisibility=D3D12_SHADER_VISIBILITY_PIXEL;sm.ComparisonFunc=D3D12_COMPARISON_FUNC_NEVER;sm.MaxAnisotropy=1;
  D3D12_ROOT_SIGNATURE_DESC rs{};rs.NumParameters=3;rs.pParameters=rp;rs.NumStaticSamplers=1;rs.pStaticSamplers=&sm;ComPtr<ID3DBlob> blob,err;Hr(D3D12SerializeRootSignature(&rs,D3D_ROOT_SIGNATURE_VERSION_1,&blob,&err),"oracle root serialize");Hr(d->CreateRootSignature(0,blob->GetBufferPointer(),blob->GetBufferSize(),IID_PPV_ARGS(&root)),"oracle root");
  auto v=Read(audit+"/pixel-vs-warp-5778e0.dxbc"),p=Read(audit+"/pixel-quality-20261004/shader-"+std::string(bypass?"578800":"573b30")+".dxbc");D3D12_GRAPHICS_PIPELINE_STATE_DESC pd{};pd.pRootSignature=root.Get();pd.VS={v.data(),v.size()};pd.PS={p.data(),p.size()};pd.PrimitiveTopologyType=D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;pd.NumRenderTargets=1;pd.RTVFormats[0]=target;pd.SampleDesc.Count=1;pd.SampleMask=UINT_MAX;pd.RasterizerState.FillMode=D3D12_FILL_MODE_SOLID;pd.RasterizerState.CullMode=D3D12_CULL_MODE_BACK;pd.BlendState.RenderTarget[0].RenderTargetWriteMask=15;Hr(d->CreateGraphicsPipelineState(&pd,IID_PPV_ARGS(&pso)),"oracle PSO");
 }
 void Record(ID3D12Device*d,ID3D12GraphicsCommandList*c,ID3D12Resource*s,ID3D12Resource*t,DXGI_FORMAT sourceView,DXGI_FORMAT targetView){D3D12_SHADER_RESOURCE_VIEW_DESC sv{};sv.Format=sourceView;sv.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D;sv.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;sv.Texture2D.MipLevels=1;auto sh=srv->GetCPUDescriptorHandleForHeapStart();d->CreateShaderResourceView(s,&sv,sh);sh.ptr+=d->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);d->CreateShaderResourceView(s,&sv,sh);D3D12_RENDER_TARGET_VIEW_DESC tv{};tv.Format=targetView;tv.ViewDimension=D3D12_RTV_DIMENSION_TEXTURE2D;auto th=rtv->GetCPUDescriptorHandleForHeapStart();d->CreateRenderTargetView(t,&tv,th);float black[]={0,0,0,1};c->ClearRenderTargetView(th,black,0,nullptr);auto td=t->GetDesc();D3D12_VIEWPORT vp{0,0,float(td.Width),float(td.Height),0,1};D3D12_RECT sc{0,0,LONG(td.Width),LONG(td.Height)};c->RSSetViewports(1,&vp);c->RSSetScissorRects(1,&sc);c->OMSetRenderTargets(1,&th,FALSE,nullptr);c->SetGraphicsRootSignature(root.Get());c->SetPipelineState(pso.Get());ID3D12DescriptorHeap* heaps[]={srv.Get()};c->SetDescriptorHeaps(1,heaps);c->SetGraphicsRootConstantBufferView(0,cb->GetGPUVirtualAddress());c->SetGraphicsRootConstantBufferView(1,cb->GetGPUVirtualAddress()+256);c->SetGraphicsRootDescriptorTable(2,srv->GetGPUDescriptorHandleForHeapStart());c->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);c->DrawInstanced(4,1,0,0);}
};
#if USE_AER_PASS
using namespace cvr::render;
struct PixelDifference {unsigned byteDelta=0,halfUlp=0,badChannels=0;float halfAbsolute=0;};
static PixelDifference ComparePixels(Image&expected,Image&actual,unsigned tolerance){
    void*a{},*b{};Hr(expected.readback->Map(0,nullptr,&a),"expected map");Hr(actual.readback->Map(0,nullptr,&b),"actual map");
    const bool half=actual.r->GetDesc().Format==DXGI_FORMAT_R16G16B16A16_FLOAT;
    PixelDifference result;
    for(UINT y=0;y<actual.fp.Footprint.Height;++y)for(UINT channel=0;channel<actual.fp.Footprint.Width*4;++channel){
        const auto*ea=static_cast<unsigned char*>(a)+y*expected.fp.Footprint.RowPitch+channel*(half?2:1);
        const auto*ac=static_cast<unsigned char*>(b)+y*actual.fp.Footprint.RowPitch+channel*(half?2:1);
        if(half){
            uint16_t e{},v{};std::memcpy(&e,ea,2);std::memcpy(&v,ac,2);
            const unsigned ulp=unsigned(std::abs(int(e)-int(v)));
            result.halfUlp=std::max(result.halfUlp,ulp);
            result.halfAbsolute=std::max(result.halfAbsolute,std::abs(HalfToFloat(e)-HalfToFloat(v)));
            result.badChannels+=ulp>1;
        }else{
            const unsigned delta=unsigned(std::abs(int(*ea)-int(*ac)));
            result.byteDelta=std::max(result.byteDelta,delta);result.badChannels+=delta>tolerance;
        }
    }
    expected.readback->Unmap(0,nullptr);actual.readback->Unmap(0,nullptr);return result;
}
static void GreenCases(ID3D12Device*d,ID3D12CommandQueue*q,ID3D12Fence*fence,const std::string&audit){
 D3D12_RESOURCE_DESC menu{},eye{};menu.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;menu.Width=menu.Height=1920;
 menu.DepthOrArraySize=1;menu.MipLevels=1;menu.SampleDesc.Count=1;menu.Format=DXGI_FORMAT_R8G8B8A8_UNORM;eye=menu;eye.Width=1782;eye.Format=DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
 const auto menuPlan=cvr::framegen::PlanColorCopy(menu,eye,true);
 Check(menuPlan.kind!=cvr::framegen::ColorCopyKind::Reject,"1920x1920 menu must reach 1782x1920 XR surface instead of an empty layer");
 Check(menuPlan.source.left==0&&menuPlan.source.right==1920,"menu scaling must preserve the complete image and edge controls");
 Check(cvr::framegen::PlanColorCopy(menu,eye,false).kind==cvr::framegen::ColorCopyKind::Reject,"world geometry must still require the proven reconstruction route");
 AerReconstructionPass pass;Check(pass.Initialize(d),"AER init");UINT64 sequence=0;unsigned captureCount=0,globalMax=0,globalHalfUlp=0;float globalHalfAbsolute=0;
 Check(!pass.Prepare(DXGI_FORMAT_UNKNOWN,DXGI_FORMAT_R8G8B8A8_UNORM_SRGB,{}),"Prepare accepted unknown format");
 struct Job {Image source,referenceSource,expected,actual;std::unique_ptr<Commands> commands;std::unique_ptr<Oracle> oracle;AerRecordTicket ticket;std::weak_ptr<void> owner;unsigned tolerance=1;};
 for(unsigned batch=0;batch<9;++batch){std::vector<Job> jobs;const UINT width=batch%3==1?48u:64u,height=batch%3==1?32u:48u;const UINT64 completion=sequence+2;
  for(unsigned i=0;i<6;++i){
   Job j;const bool floatSource=batch>=6,bgra=batch>=3&&batch<6;
   const bool linear=i==4||floatSource,manual=i==3,typeless=i==5;
   bool outputLinear=i==4;
   const DXGI_FORMAT unorm=bgra?DXGI_FORMAT_B8G8R8A8_UNORM:DXGI_FORMAT_R8G8B8A8_UNORM;
   const DXGI_FORMAT srgb=bgra?DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
   const DXGI_FORMAT type=bgra?DXGI_FORMAT_B8G8R8A8_TYPELESS:DXGI_FORMAT_R8G8B8A8_TYPELESS;
   DXGI_FORMAT source=linear||manual?unorm:typeless?type:srgb;
   DXGI_FORMAT sourceView=linear?unorm:srgb;
   DXGI_FORMAT target=manual||outputLinear?unorm:typeless?type:srgb;
   DXGI_FORMAT targetView=outputLinear?unorm:srgb;
   if(floatSource){
    source=sourceView=DXGI_FORMAT_R16G16B16A16_FLOAT;
    outputLinear=i<2||i==5;
    if(i<2)target=targetView=DXGI_FORMAT_R16G16B16A16_FLOAT;
    else if(i==4)target=targetView=DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
    else if(i==5)target=targetView=DXGI_FORMAT_R8G8B8A8_UNORM;
    else {target=i==3?DXGI_FORMAT_R8G8B8A8_UNORM:DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;targetView=DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;}
   }
   const UINT outputWidth=batch==0&&i==0?width-16:width;
   j.source=ImageResources(d,source,width,height,D3D12_RESOURCE_STATE_COPY_DEST);j.referenceSource=ImageResources(d,sourceView,width,height,D3D12_RESOURCE_STATE_COPY_DEST);j.actual=ImageResources(d,target,outputWidth,height,D3D12_RESOURCE_STATE_RENDER_TARGET);j.expected=ImageResources(d,targetView,outputWidth,height,D3D12_RESOURCE_STATE_RENDER_TARGET);j.commands=std::make_unique<Commands>(d);auto*c=j.commands->c.Get();Upload(c,j.source,i+batch*7,i==3||(floatSource&&batch%3==1));Upload(c,j.referenceSource,i+batch*7,i==3||(floatSource&&batch%3==1));
   AerRecordRequest r;r.source=j.source.r.Get();r.destination=j.actual.r.Get();r.eye=i&1;r.completion={fence,completion};r.warp.sourceEpoch=r.warp.referenceEpoch=batch+1;r.warp.sourceSpace=r.warp.referenceSpace=17;r.warp.tangents={1.1f,.9f,.8f,1.2f};r.warp.crop={0,0,float(width),float(height)};r.filter.deblur=(i==0||(i==3&&batch%3==2))?0:.5f;r.filter.emphasis=i==5?.8f:0;r.filter.contrast=i==5?.2f:0;r.filter.vibrance=i==5?.25f:0;r.color={linear?AerEncoding::Linear:AerEncoding::EncodedSrgb,outputLinear?AerEncoding::Linear:AerEncoding::EncodedSrgb};
   if(batch==0){r.evidenceSourceId=800+i;r.evidenceSourceFloor=800;r.evidencePairSerial=1;}
   if(i>=2||(floatSource&&i==1)){float angle=(i&1)?.047f:-.035f;r.warp.captured={0,std::sin(angle*.5f),0,std::cos(angle*.5f)};r.warp.crop={.25f,.5f,float(width)-.25f,float(height)-.5f};}
   auto vc=luke_warp_reference::SingleImageConstants({1.1f,.9f,.8f,1.2f},r.warp.reference,r.warp.captured,{r.warp.crop[0],r.warp.crop[1],r.warp.crop[2],r.warp.crop[3]},float(width),float(height));AerVertexConstants actualConstants{};Check(AerReconstructionPass::BuildVertexConstants(r.warp,width,height,actualConstants),"matrix build");const float*va=(const float*)&vc,*vb=(const float*)&actualConstants;for(unsigned k=0;k<44;++k)Check(std::abs(va[k]-vb[k])<1e-6f,"independent matrix mismatch");
   if(i==0){auto invalid=r;invalid.completion.value=sequence;Check(!pass.Record(c,invalid),"old completed fence accepted");invalid=r;invalid.completion.value=UINT64_MAX;Check(!pass.Record(c,invalid),"device-removed sentinel reserved as a completion");invalid=r;invalid.warp.sourceSpace++;Check(!pass.Record(c,invalid),"foreign pose space accepted");invalid=r;invalid.warp.sourceEpoch++;Check(!pass.Record(c,invalid),"foreign epoch accepted");invalid=r;invalid.warp.crop[2]=float(width)+1;Check(!pass.Record(c,invalid),"invalid crop accepted");invalid=r;invalid.color.input=linear?AerEncoding::EncodedSrgb:AerEncoding::Linear;Check(!pass.Record(c,invalid),"inconsistent encoding accepted");}
   std::array<float,4> filter{r.filter.emphasis,4*r.filter.deblur,r.filter.contrast,r.filter.vibrance};j.oracle=std::make_unique<Oracle>(d,audit,vc,filter,r.filter.deblur==0&&r.filter.contrast==0&&r.filter.vibrance==0,targetView);j.oracle->Record(d,c,j.referenceSource.r.Get(),j.expected.r.Get(),sourceView,targetView);
   if(batch==0&&i==0){auto before=pass.PipelineStats();auto missing=pass.Record(c,r);auto after=pass.PipelineStats();Check(!missing&&missing.status==AerRecordStatus::NotPrepared,"Record did not explicitly reject an unprepared route");Check(pass.BusySlots()==0&&before.shaderCompileCalls==after.shaderCompileCalls&&before.pipelineCreateCalls==after.pipelineCreateCalls,"Record compiled or reserved an unprepared route");}
   Check(pass.Prepare(source,target,r.color),"Prepare failed");const auto prepared=pass.PipelineStats();Check(pass.Prepare(source,target,r.color),"Repeat Prepare failed");const auto repeated=pass.PipelineStats();Check(prepared.shaderCompileCalls==repeated.shaderCompileCalls&&prepared.pipelineCreateCalls==repeated.pipelineCreateCalls&&prepared.cachedPipelines==repeated.cachedPipelines,"Repeat Prepare rebuilt shaders or PSOs");
   auto owner=std::make_shared<int>(int(i));j.owner=owner;r.sourceOwner=owner;j.ticket=pass.Record(c,r);Check(bool(j.ticket),"AER record rejected");const auto recorded=pass.PipelineStats();Check(recorded.shaderCompileCalls==prepared.shaderCompileCalls&&recorded.pipelineCreateCalls==prepared.pipelineCreateCalls,"Record performed compilation or PSO creation");owner.reset();r.sourceOwner.reset();Check(!j.owner.expired(),"capture cell lease not retained");j.tolerance=manual?2u:1u;
   if(i==5){Check(!pass.Record(c,r),"seventh record overwrote in-flight slot");Check(!pass.TryShutdown(),"busy shutdown released slots");}
   Readback(c,j.expected);Readback(c,j.actual);jobs.push_back(std::move(j));
  }
  Check(pass.BusySlots()==6,"six records not held");
  ComPtr<ID3D12Fence> gate,foreign;Hr(d->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&gate)),"gate fence");Hr(d->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&foreign)),"foreign fence");
  Hr(q->Wait(gate.Get(),1),"queue gate");for(auto&j:jobs)j.commands->Execute(q);Hr(q->Signal(fence,completion),"queued exact signal");
  // Half have a confirmed successful Signal; half deliberately retain an
  // unknown result to exercise the failure policy independently of fence age.
  for(unsigned i=0;i<jobs.size();++i){Check(pass.SubmissionReported(jobs[i].ticket,true,(i&1)==0),"execute report");Check(!pass.CancelNeverExecuted(jobs[i].ticket),"executed record canceled");}
  Hr(foreign->Signal(completion),"foreign signal");Check(pass.Collect()==0&&pass.BusySlots()==6,"foreign fence released records");
  Hr(fence->Signal(sequence+1),"injected lower fence value");Check(pass.Collect()==0&&pass.BusySlots()==6,"submitted record released before exact value");
  Hr(gate->Signal(1),"open queue gate");Complete(fence,completion);Check(pass.Collect()==3&&pass.BusySlots()==3,"unknown Signal released by fence alone");
  for(unsigned i=1;i<jobs.size();i+=2)Check(pass.SubmissionReported(jobs[i].ticket,true,true),"confirmed signal retry report");Check(pass.Collect()==3&&pass.BusySlots()==0,"exact retirement failed");
  for(unsigned i=0;i<jobs.size();++i){
   auto&j=jobs[i];Check(j.owner.expired(),"capture lease leaked after completion");Check(!pass.CancelNeverExecuted(j.ticket),"stale ticket canceled another record");
   const auto difference=ComparePixels(j.expected,j.actual,j.tolerance);
   std::cout<<"case batch="<<batch<<" eye="<<(i&1)<<" variant="<<i<<" source_format="<<unsigned(j.source.r->GetDesc().Format)<<" destination_format="<<unsigned(j.actual.r->GetDesc().Format)<<" max_byte_delta="<<difference.byteDelta<<" max_half_ulp="<<difference.halfUlp<<" max_half_absolute="<<difference.halfAbsolute<<" bad_channels="<<difference.badChannels<<'\n';
   Check(!difference.badChannels,"AER pixel mismatch against actual pinned DXBC");globalMax=std::max(globalMax,difference.byteDelta);globalHalfUlp=std::max(globalHalfUlp,difference.halfUlp);globalHalfAbsolute=std::max(globalHalfAbsolute,difference.halfAbsolute);++captureCount;
  }
  sequence=completion;
 }
 const auto finalPipelines=pass.PipelineStats();
 // A genuinely never-executed record has an explicit, nonblocking cancel path.
 auto source=ImageResources(d,DXGI_FORMAT_R8G8B8A8_UNORM_SRGB,16,16,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);auto target=ImageResources(d,DXGI_FORMAT_R8G8B8A8_UNORM_SRGB,16,16,D3D12_RESOURCE_STATE_RENDER_TARGET);Commands canceled(d);AerRecordRequest request;request.source=source.r.Get();request.destination=target.r.Get();request.completion={fence,sequence+1};request.warp.sourceEpoch=request.warp.referenceEpoch=9;request.warp.sourceSpace=request.warp.referenceSpace=1;request.warp.crop={0,0,16,16};auto ticket=pass.Record(canceled.c.Get(),request);Check(bool(ticket)&&pass.CancelNeverExecuted(ticket),"never-executed cancel failed");Check(pass.TryShutdown(),"clean shutdown failed");
 // Failure teardown must not wait on a never-signaled completion point and
 // must preserve the opaque source owner after this pass object is destroyed.
 std::weak_ptr<void> retained;auto failure=std::make_unique<AerReconstructionPass>();Check(failure->Initialize(d),"failure owner init");Commands failedCommands(d);request.completion.value=sequence+99;auto lease=std::make_shared<int>(42);retained=lease;request.sourceOwner=lease;Check(failure->Prepare(request.source->GetDesc().Format,request.destination->GetDesc().Format,request.color),"failure route prepare");auto failedTicket=failure->Record(failedCommands.c.Get(),request);Check(bool(failedTicket),"failure record");failedCommands.Execute(q);Check(failure->SubmissionReported(failedTicket,true,false),"unknown failure report");Hr(q->Signal(fence,sequence+1),"separate fixture drain");Complete(fence,sequence+1);lease.reset();request.sourceOwner.reset();auto start=GetTickCount64();failure.reset();Check(GetTickCount64()-start<1000,"destructor blocked on unknown fence");Check(!retained.expired(),"unknown teardown freed source owner");
 std::cout<<"PASS AER captures="<<captureCount
          <<" six_inflight=1 two_eyes=1 resize=1 exact_fence=1 unknown_retained=1 lease_retired=1"
          <<" not_prepared_checked=1 repeat_prepare_cached=1 record_compile_calls=0"
          <<" shader_compile_calls="<<finalPipelines.shaderCompileCalls
          <<" pipeline_create_calls="<<finalPipelines.pipelineCreateCalls
          <<" max_byte_delta="<<globalMax<<" max_half_ulp="<<globalHalfUlp
          <<" max_half_absolute="<<globalHalfAbsolute<<'\n';
}
#endif
int main(int argc,char**argv){try{Check(argc==2,"usage: probe auditPath");ComPtr<ID3D12Debug>debug;Hr(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)),"debug unavailable");debug->EnableDebugLayer();ComPtr<IDXGIFactory4> factory;ComPtr<IDXGIAdapter1> adapter;Hr(CreateDXGIFactory1(IID_PPV_ARGS(&factory)),"factory");Hr(factory->EnumWarpAdapter(IID_PPV_ARGS(&adapter)),"WARP");DXGI_ADAPTER_DESC1 ad{};adapter->GetDesc1(&ad);Check((ad.Flags&DXGI_ADAPTER_FLAG_SOFTWARE)!=0,"hardware prohibited");ComPtr<ID3D12Device>d;Hr(D3D12CreateDevice(adapter.Get(),D3D_FEATURE_LEVEL_11_0,IID_PPV_ARGS(&d)),"device");ComPtr<ID3D12InfoQueue>info;Hr(d.As(&info),"info");ComPtr<ID3D12CommandQueue>q;D3D12_COMMAND_QUEUE_DESC qd{};Hr(d->CreateCommandQueue(&qd,IID_PPV_ARGS(&q)),"queue");ComPtr<ID3D12Fence>fence;Hr(d->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&fence)),"fence");
#if USE_AER_PASS
 GreenCases(d.Get(),q.Get(),fence.Get(),argv[1]);
#else
 constexpr UINT w=64,h=48;auto s=ImageResources(d.Get(),DXGI_FORMAT_R8G8B8A8_UNORM_SRGB,w,h,D3D12_RESOURCE_STATE_COPY_DEST);auto expected=ImageResources(d.Get(),DXGI_FORMAT_R8G8B8A8_UNORM_SRGB,w,h,D3D12_RESOURCE_STATE_RENDER_TARGET),actual=ImageResources(d.Get(),DXGI_FORMAT_R8G8B8A8_UNORM_SRGB,w,h,D3D12_RESOURCE_STATE_RENDER_TARGET);Commands commands(d.Get());Upload(commands.c.Get(),s,1,false);
 const luke_warp_reference::V4 identity{0,0,0,1};auto vs=luke_warp_reference::SingleImageConstants({1.1f,.9f,.8f,1.2f},identity,identity,{0,0,float(w),float(h)},float(w),float(h));Oracle oracle(d.Get(),argv[1],vs,{0,2,0,0},false,DXGI_FORMAT_R8G8B8A8_UNORM_SRGB);oracle.Record(d.Get(),commands.c.Get(),s.r.Get(),expected.r.Get(),DXGI_FORMAT_R8G8B8A8_UNORM_SRGB,DXGI_FORMAT_R8G8B8A8_UNORM_SRGB);
 SharpenPass legacy;Check(legacy.EnsureInitialized(d.Get(),DXGI_FORMAT_R8G8B8A8_UNORM_SRGB,w,h),"legacy init");Check(legacy.RecordSharpen(commands.c.Get(),s.r.Get(),actual.r.Get(),0,2),"legacy record");Readback(commands.c.Get(),expected);Readback(commands.c.Get(),actual);commands.Execute(q.Get());Hr(q->Signal(fence.Get(),1),"signal");Complete(fence.Get(),1);
 void *a{},*b{};Hr(expected.readback->Map(0,nullptr,&a),"expected map");Hr(actual.readback->Map(0,nullptr,&b),"actual map");unsigned changed=0,maxDelta=0;for(UINT y=0;y<h;++y)for(UINT x=0;x<w*4;++x){int delta=std::abs(int(((unsigned char*)a)[y*expected.fp.Footprint.RowPitch+x])-int(((unsigned char*)b)[y*actual.fp.Footprint.RowPitch+x]));maxDelta=std::max(maxDelta,unsigned(delta));if(delta>1)++changed;}expected.readback->Unmap(0,nullptr);actual.readback->Unmap(0,nullptr);
#endif
 unsigned errors=0;for(UINT64 i=0;i<info->GetNumStoredMessages();++i){SIZE_T n{};info->GetMessage(i,nullptr,&n);std::vector<char>v(n);auto*m=(D3D12_MESSAGE*)v.data();info->GetMessage(i,m,&n);if(m->Severity<=D3D12_MESSAGE_SEVERITY_ERROR){std::cerr<<m->pDescription<<'\n';++errors;}}std::cout<<"debug_errors="<<errors<<'\n';Check(errors==0,"debug errors");
#if !USE_AER_PASS
std::cout<<"legacy_vs_pinned_reference differing_channels="<<changed<<" max_delta="<<maxDelta<<'\n';Check(changed==0,"RED expected: existing SharpenPass does not reproduce pinned AER PS");
#endif
return 0;
}catch(const std::exception&e){std::cerr<<"FAIL "<<e.what()<<'\n';return 1;}}
