#include "MenuPreview/DescriptorProvenance.hpp"
#include "MenuPreview/PixelReadback.hpp"
#include "Utils/StereoLog.hpp"
#include <wrl/client.h>
#include <atomic>
#include <mutex>
#include <vector>
namespace cvr::menu_preview {
namespace {
struct Retire {uintptr_t address{};uint64_t generation{};bool heap{};};
struct State {std::recursive_mutex mutex;PreviewDescriptorLedger ledger;std::mutex pendingMutex;std::vector<Retire> pending;std::atomic<bool> failed{};uint32_t rows{};};
State& S(){static auto* s=new State;return *s;}
void Apply(Retire r){if(!r.heap)ForgetPreviewPixelObject(r.address,false);if(r.heap)S().ledger.ForgetHeap(r.address,r.generation);else S().ledger.ForgetResource(r.address,r.generation);}
void Drain(){if(S().failed.load())S().ledger.Fail();std::lock_guard pending(S().pendingMutex);for(auto r:S().pending)Apply(r);S().pending.clear();}
void RetireObject(Retire r)noexcept{try{std::unique_lock lock(S().mutex,std::try_to_lock);if(lock.owns_lock())Apply(r);else{std::lock_guard pending(S().pendingMutex);if(S().pending.size()<128)S().pending.push_back(r);else S().failed=true;}}catch(...){S().failed=true;}}
const GUID key={0x8ba79d28,0x1b10,0x4c42,{0xb8,0x53,0x15,0x72,0x16,0x9f,0xa3,0x41}};
class Lifetime final:public IUnknown {std::atomic<ULONG> refs{1};public:const Retire identity;explicit Lifetime(Retire r):identity(r){}
 HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id,void** out)override{if(!out)return E_POINTER;*out=nullptr;if(id!=__uuidof(IUnknown))return E_NOINTERFACE;*out=this;AddRef();return S_OK;}
 ULONG STDMETHODCALLTYPE AddRef()override{return ++refs;}
 ULONG STDMETHODCALLTYPE Release()override{auto remaining=--refs;if(!remaining){RetireObject(identity);delete this;}return remaining;}
};
uint64_t Track(ID3D12Object* object,Retire identity){static std::mutex lifecycleMutex;std::lock_guard lifecycle(lifecycleMutex);IUnknown* prior{};UINT bytes=sizeof(prior);
 if(SUCCEEDED(object->GetPrivateData(key,&bytes,&prior))&&prior){const auto old=static_cast<Lifetime*>(prior)->identity;prior->Release();return old.heap==identity.heap?old.generation:0;}
 auto* life=new Lifetime(identity);const auto hr=object->SetPrivateDataInterface(key,life);life->Release();return SUCCEEDED(hr)?identity.generation:0;
}
}
void PreviewDescriptorHeapCreated(ID3D12Device* device,IUnknown* object)noexcept{if(!PreviewMetadataRequested()||!device||!object)return;try{Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> heap;if(FAILED(object->QueryInterface(IID_PPV_ARGS(&heap))))return;const auto d=heap->GetDesc();if(d.Type!=D3D12_DESCRIPTOR_HEAP_TYPE_RTV)return;
 const auto id=reinterpret_cast<uintptr_t>(heap.Get());const auto start=heap->GetCPUDescriptorHandleForHeapStart().ptr;const auto increment=device->GetDescriptorHandleIncrementSize(d.Type);uint64_t generation{};
 {std::lock_guard lock(S().mutex);Drain();generation=S().ledger.RegisterHeap(id,reinterpret_cast<uintptr_t>(device),start,d.NumDescriptors,increment);}
 const bool tracked=generation&&Track(heap.Get(),{id,generation,true})==generation;
 {std::lock_guard lock(S().mutex);Drain();if(!tracked)S().ledger.ForgetHeap(id,generation);
 if(S().rows++<8)Log("[MenuPreviewDescriptor] heap=%p generation=%llu start=%llx count=%u increment=%u tracked=%u; metadata lifetime only, native calls unlocked\n",heap.Get(),generation,start,d.NumDescriptors,increment,unsigned(tracked));}
 }catch(...){S().failed=true;}}
uint64_t BeginPreviewDescriptorCopies(ID3D12Device* device,UINT count,const D3D12_CPU_DESCRIPTOR_HANDLE* starts,const UINT* sizes,D3D12_DESCRIPTOR_HEAP_TYPE type)noexcept{if(!PreviewMetadataRequested()||type!=D3D12_DESCRIPTOR_HEAP_TYPE_RTV||!device)return 0;try{const auto increment=device->GetDescriptorHandleIncrementSize(type);std::lock_guard lock(S().mutex);Drain();if((count&&!starts)||count>128){S().ledger.Fail();return 0;}const auto token=S().ledger.BeginMutation(reinterpret_cast<uintptr_t>(device));for(UINT n=0;n<count;++n)S().ledger.AddDestination(token,starts[n].ptr,sizes?sizes[n]:1,increment);return token;}catch(...){S().failed=true;return 0;}}
uint64_t BeginPreviewDescriptorRtv(ID3D12Device* device,D3D12_CPU_DESCRIPTOR_HANDLE handle)noexcept{return BeginPreviewDescriptorCopies(device,1,&handle,nullptr,D3D12_DESCRIPTOR_HEAP_TYPE_RTV);}
void FinishPreviewDescriptorRtv(ID3D12Device* device,ID3D12Resource* resource,D3D12_CPU_DESCRIPTOR_HANDLE handle,uint64_t token,const D3D12_RENDER_TARGET_VIEW_DESC* view)noexcept{if(!token)return;try{const auto address=reinterpret_cast<uintptr_t>(resource);uint64_t generation{};
 static_assert(uint32_t(DXGI_FORMAT_R16G16B16A16_FLOAT)==10&&uint32_t(D3D12_RTV_DIMENSION_TEXTURE2D)==4);
 // Native resource/view reads remain outside the ledger mutex. Keep this exact
 // resource alive through the metadata mutation's final admission.
 Microsoft::WRL::ComPtr<ID3D12Resource> resourceOwner(resource);
 PreviewRtvViewEvidence evidence;
 if(resourceOwner){const auto desc=resourceOwner->GetDesc();PreviewRtvViewEvidence requested;
  if(view){requested.format=uint32_t(view->Format);requested.dimension=uint32_t(view->ViewDimension);
   if(view->ViewDimension==D3D12_RTV_DIMENSION_TEXTURE2D){requested.mip=view->Texture2D.MipSlice;requested.known=view->Texture2D.PlaneSlice==0;}}
  if(desc.SampleDesc.Quality==0)evidence=MakePreviewRtvViewEvidence(uint32_t(desc.Format),desc.DepthOrArraySize,desc.MipLevels,desc.SampleDesc.Count,desc.Dimension==D3D12_RESOURCE_DIMENSION_TEXTURE2D,view?&requested:nullptr);
 }
 {std::lock_guard lock(S().mutex);Drain();if(resource&&S().ledger.HasHeap(reinterpret_cast<uintptr_t>(device),handle.ptr))generation=S().ledger.RegisterResource(address);}
 const bool tracked=generation&&Track(resource,{address,generation,false})==generation;
 {std::lock_guard lock(S().mutex);Drain();S().ledger.FinishDirect(token,handle.ptr,tracked?address:0,tracked?generation:0,evidence);}
 }catch(...){S().failed=true;}}
void FinishPreviewDescriptorCopies(uint64_t token)noexcept{if(!token)return;try{std::lock_guard lock(S().mutex);Drain();S().ledger.FinishCopies(token);}catch(...){S().failed=true;}}
std::optional<PreviewDescriptorProof> ReadPreviewDescriptor(uintptr_t handle)noexcept{try{std::lock_guard lock(S().mutex);Drain();return S().ledger.Read(handle);}catch(...){S().failed=true;return {};}}
bool ValidatePreviewDescriptor(const PreviewDescriptorProof& proof)noexcept{try{std::lock_guard lock(S().mutex);Drain();return S().ledger.Validate(proof);}catch(...){S().failed=true;return false;}}
}
