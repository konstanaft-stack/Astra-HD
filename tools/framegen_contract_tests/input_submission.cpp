// Compile the actual input registry in this TU. Unused game-hook entrypoints
// abort if called. No game, driver hook or GPU is executed here.
#include "../../src/Framegen/Inputs.cpp"
#pragma warning(push)
#pragma warning(disable:4100)
#include "CpuD3d.hpp"
#include "Render/NativeObjectIdentity.hpp"
#pragma warning(pop)
#include <cstdio>
#include <cstdlib>
extern "C" {__declspec(dllexport) int CyberpunkVR_RuntimeDiagnostics=0;int32_t CyberpunkVR_VrcamDlssViewport=1;}
void Log(const char*,...){}
// None of these game/hook functions is part of this registry test. Abort if a
// regression starts calling them, rather than silently simulating a game.
namespace cvr::detail {uint8_t* g_exe_base{};bool paired_sl_frame_index(uint32_t*){std::abort();}}
namespace cvr::camera {PoseIdentity fixturePose;PoseIdentity CurrentNodePoseIdentity(){return fixturePose;}}
extern "C" {
MH_STATUS WINAPI MH_CreateHook(LPVOID,LPVOID,LPVOID*){std::abort();}
MH_STATUS WINAPI MH_RemoveHook(LPVOID){std::abort();}
MH_STATUS WINAPI MH_EnableHook(LPVOID){std::abort();}
MH_STATUS WINAPI MH_DisableHook(LPVOID){std::abort();}
}
namespace cvr::framegen {
Settings GetSettings(){Settings s;s.aerV2=true;return s;}
void OnInput(unsigned,uint32_t,uint32_t){}
void RecordProducerTrace(const ProducerTraceRecord&) noexcept{}
uint64_t SrTraceQpc() noexcept {std::abort();}
void SnapshotSrConstants(SrTraceRecord&,const sl::Constants*,const sl::FrameToken*,const sl::ViewportHandle*) noexcept {std::abort();}
void SnapshotSrTags(SrTraceRecord&,const void*,const void*,uint32_t,void*) noexcept {std::abort();}
void SnapshotSrEvaluate(SrTraceRecord&,uint32_t,const sl::FrameToken*,const sl::BaseStructure*const*,uint32_t,void*) noexcept {std::abort();}
void RecordSrIdentity(const SrTraceRecord&) noexcept {std::abort();}
bool InstallSrSlHooks(){std::abort();}bool EnsureSrNativeHooks(){return true;}
bool SrIdentityTraceRequested() noexcept{return false;}
bool CaptureProjectionEnabled(){return true;}
SrConstantsCall BeginSrConstants(const sl::Constants&,const sl::FrameToken&,const sl::ViewportHandle&) noexcept {std::abort();}
void EndSrConstants(SrConstantsCall&,uint32_t,bool) noexcept {std::abort();}
SrEvaluationCall BeginSrEvaluation(uint32_t,const sl::FrameToken&,const sl::BaseStructure*const*,uint32_t,void*) noexcept {return {};}
void EndSrEvaluation(SrEvaluationCall&,uint32_t,bool) noexcept {}
bool CopyInput(ID3D12GraphicsCommandList*,const InputTag&,Inputs&){std::abort();}
bool CaptureInputsEnabled(){return false;}void OnSkip(){}
// Extent validation is covered separately; this fixture tests ownership keys.
bool InspectInputTag(const InputTag& tag,InputExtent& out){out={tag.width,tag.height,DXGI_FORMAT_R32_FLOAT};return true;}
}
using namespace cvr::framegen;
static unsigned checks{},failures{};
#define CHECK(x) do{++checks;if(!(x)){++failures;std::printf("FAIL %d: %s\n",__LINE__,#x);}}while(false)
static auto Seed(ID3D12CommandList* list,unsigned slot,uint32_t frame,uint8_t mask=7,unsigned side=0){
    auto r=std::make_shared<Record>();r->generation=++nextGeneration;r->expected=mask;
    r->data.frameId=frame;r->data.view=side+1;r->data.poseId=frame+100;r->data.origin=1;
    r->data.evaluated=true;r->data.hasDepth=true;r->data.hasMotion=true;
    pools[side][slot]=r;pending[list].push_back({r,r->generation,mask});havePending=true;return r;
}
static void Clear(){pending.clear();for(auto&pool:pools)for(auto&r:pool)r.reset();havePending=false;}
struct ForwardedList:ID3D12CommandList {
    ULONG refs{1};ComPtr<ID3D12CommandList> inner;std::vector<unsigned char> bytes;
    explicit ForwardedList(ID3D12CommandList* native=nullptr):inner(native){}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id,void** out)override{
        if(!out)return E_POINTER;*out=nullptr;
        if(id!=__uuidof(IUnknown)&&id!=__uuidof(ID3D12Object)&&id!=__uuidof(ID3D12CommandList)&&id!=__uuidof(ID3D12GraphicsCommandList))return E_NOINTERFACE;
        *out=static_cast<ID3D12CommandList*>(this);AddRef();return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef()override{return ++refs;}
    ULONG STDMETHODCALLTYPE Release()override{const auto n=--refs;if(!n)delete this;return n;}
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID key,UINT* size,void* data)override{
        if(inner)return inner->GetPrivateData(key,size,data);
        if(key!=cvr::gpu::NativeObjectIdentityKey||bytes.empty())return DXGI_ERROR_NOT_FOUND;
        if(!size)return E_INVALIDARG;auto capacity=*size;*size=UINT(bytes.size());
        if(capacity<bytes.size())return DXGI_ERROR_MORE_DATA;
        if(data)std::memcpy(data,bytes.data(),bytes.size());return S_OK;
    }
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID key,UINT size,const void* data)override{
        if(inner)return inner->SetPrivateData(key,size,data);
        if(key!=cvr::gpu::NativeObjectIdentityKey)return E_INVALIDARG;
        bytes.assign(static_cast<const unsigned char*>(data),static_cast<const unsigned char*>(data)+size);return S_OK;
    }
    HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID,const IUnknown*)override{std::abort();}
    HRESULT STDMETHODCALLTYPE SetName(LPCWSTR)override{return S_OK;}
    HRESULT STDMETHODCALLTYPE GetDevice(REFIID,void**)override{std::abort();}
    D3D12_COMMAND_LIST_TYPE STDMETHODCALLTYPE GetType()override{return D3D12_COMMAND_LIST_TYPE_DIRECT;}
};
struct Token:sl::FrameToken{operator uint32_t()const override{return 100;}};
static void* expectedOriginalList{};
sl::Result OriginalEvaluate(sl::Feature,const sl::FrameToken&,const sl::BaseStructure*const*,uint32_t,void* list){CHECK(list==expectedOriginalList);return sl::Result::eOk;}
static void WrappedProjection(){
    Clear();ComPtr<ForwardedList> native,proxy;
    native.Attach(new ForwardedList);CHECK(cvr::gpu::RegisterNativeObject(native.Get()));
    proxy.Attach(new ForwardedList(native.Get()));
    auto& pose=cvr::camera::fixturePose;pose={};pose.poseId=71;pose.writeId=4;pose.view=1;
    pose.head.valid=true;pose.head.originSerial=9;pose.hasRenderFrameId=true;
    ComPtr<CpuResource> resource;resource.Attach(new CpuResource(nullptr));
    sl::Resource depth{},motion{};depth.native=resource.Get();depth.type=sl::ResourceType::eTex2d;depth.state=D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;motion=depth;
    sl::ResourceTag tags[2]{};tags[0].resource=&depth;tags[0].type=0;tags[0].extent.width=1280;tags[0].extent.height=720;
    tags[1]=tags[0];tags[1].resource=&motion;tags[1].type=1;
    originalEvaluate=&OriginalEvaluate;expectedOriginalList=proxy.Get();Token token;
    ID3D12CommandList* nativeLists[]={native.Get()};ComPtr<CpuQueue> queueOwner;queueOwner.Attach(new CpuQueue(nullptr));auto* queue=queueOwner.Get();
    for(uint32_t frame=100;frame<112;++frame){
        pose.renderFrameId=frame;auto& camera=cameras[0][frame%8];camera={};camera.frame=frame;camera.pose=71;camera.origin=9;camera.valid=true;
        RecordTags(nullptr,tags,2,proxy.Get());Evaluate(0,token,nullptr,0,proxy.Get());
        auto batch=PrepareInputSubmission(1,nativeLists);
        CHECK(batch.count==1);if(batch.count!=1)break;
        CHECK(batch.masks[0]==7);Submitted(batch,queue);
        CHECK(AcquireFrameProjection(1,frame,71,9,queue).has_value());
    }
    CHECK(native->refs==2);CHECK(pending.empty());Clear();
    // Re-recording an unsubmitted native list must revoke proxy-tagged data.
    pose.renderFrameId=200;auto& camera=cameras[0][0];camera={};camera.frame=200;camera.pose=71;camera.origin=9;camera.valid=true;
    RecordTags(nullptr,tags,2,proxy.Get());ResetList(reinterpret_cast<ID3D12GraphicsCommandList*>(native.Get()));
    CHECK(pending.empty());CHECK(pools[0][0]&&pools[0][0]->invalid);Clear();
}
int main(){
    WrappedProjection();
    auto list=reinterpret_cast<ID3D12GraphicsCommandList*>(uintptr_t{0x1110});ID3D12CommandList* lists[]={list};
    auto queue=reinterpret_cast<ID3D12CommandQueue*>(uintptr_t{0x2220});
    auto old=Seed(list,0,10);
    auto oldBatch=PrepareInputSubmission(1,lists);
    // Actual Execute consumed recording10. Reset and recording11 happen before
    // the observer's post-Execute callback; only10 may be acknowledged there.
    ResetList(list);auto fresh=Seed(list,1,11);
    Submitted(oldBatch,queue);
    CHECK(old->data.submitted&&!old->invalid);CHECK(!fresh->data.submitted);
    CHECK(oldBatch.count==0);Submitted(oldBatch,queue);CHECK(!fresh->data.submitted);
    auto nextBatch=PrepareInputSubmission(1,lists);Submitted(nextBatch,queue);CHECK(fresh->data.submitted&&!fresh->invalid);
    Clear();old.reset();fresh.reset();
    // Split depth, motion and Evaluate work may use different command lists.
    auto list2=reinterpret_cast<ID3D12GraphicsCommandList*>(uintptr_t{0x1120});ID3D12CommandList* lists2[]={list2};
    auto part=Seed(list,0,20,1);part->expected=7;pending[list2].push_back({part,part->generation,6});
    auto first=PrepareInputSubmission(1,lists);ResetList(list);Submitted(first,queue);
    CHECK(!part->invalid&&!part->data.submitted&&part->submitted==1);
    auto rest=PrepareInputSubmission(1,lists2);Submitted(rest,queue);
    CHECK(!part->invalid&&part->data.submitted&&part->submitted==7);
    Clear();part.reset();
    // Three contributions to one record become one immutable submission entry.
    part=Seed(list,0,30,1);part->expected=7;
    pending[list].push_back({part,part->generation,2});pending[list].push_back({part,part->generation,4});
    auto merged=PrepareInputSubmission(1,lists);CHECK(merged.count==1&&merged.masks[0]==7);
    CHECK(part->claimedMask==7);Submitted(merged,queue);CHECK(part->data.submitted);
    Clear();part.reset();
    // Bound is the actual two-by-four source pools, not a heap-grown batch.
    for(unsigned side=0;side<2;++side)for(unsigned slot=0;slot<4;++slot)Seed(list,slot,40+side*4+slot,7,side);
    auto all=PrepareInputSubmission(1,lists);CHECK(all.count==8);Submitted(all,queue);
    for(const auto&pool:pools)for(const auto&r:pool)CHECK(r->data.submitted&&!r->invalid);
    Clear();
    // Queue disagreement is unknown producer completion and cannot be recycled.
    part=Seed(list,0,60,1);part->expected=7;pending[list2].push_back({part,part->generation,6});
    first=PrepareInputSubmission(1,lists);Submitted(first,queue);
    rest=PrepareInputSubmission(1,lists2);Submitted(rest,reinterpret_cast<ID3D12CommandQueue*>(uintptr_t{0x3330}));
    CHECK(part->invalid&&part->unknownProducer);part->submitted=part->expected;
    std::weak_ptr<Record> weak=part;part.reset();ReleaseInputs();CHECK(!weak.expired());
    Clear();CHECK(weak.expired());
    // A removed device reports UINT64_MAX, never successful GPU completion.
    Microsoft::WRL::ComPtr<CpuFence> fence;fence.Attach(new CpuFence(nullptr));fence->completed=UINT64_MAX;
    part=Seed(list,0,70);pending.clear();havePending=false;part->submitted=part->expected;
    part->data.readyFence=fence;part->data.fenceValue=8;weak=part;part.reset();
    ReleaseInputs();CHECK(!weak.expired());fence->completed=8;ReleaseInputs();CHECK(weak.expired());
    std::printf("%u checks, %u failures\n",checks,failures);return failures?1:0;
}
