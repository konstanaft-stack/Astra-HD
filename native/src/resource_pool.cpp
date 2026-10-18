#include "cpvr/resource_pool.hpp"
#include <algorithm>
namespace cpvr {
namespace {
bool same_identity(IUnknown* a,IUnknown* b){ComPtr<IUnknown> x,y;
    return a&&b&&SUCCEEDED(a->QueryInterface(IID_PPV_ARGS(&x)))&&SUCCEEDED(b->QueryInterface(IID_PPV_ARGS(&y)))&&x==y;}
bool same_desc(const D3D12_RESOURCE_DESC&a,const D3D12_RESOURCE_DESC&b){
    return a.Dimension==b.Dimension&&a.Alignment==b.Alignment&&a.Width==b.Width&&a.Height==b.Height&&
        a.DepthOrArraySize==b.DepthOrArraySize&&a.MipLevels==b.MipLevels&&a.Format==b.Format&&
        a.SampleDesc.Count==b.SampleDesc.Count&&a.SampleDesc.Quality==b.SampleDesc.Quality&&a.Layout==b.Layout&&a.Flags==b.Flags;
}
}
void PooledTexture::pending()noexcept{submitted=true;fence.Reset();value=0;}
bool PooledTexture::completion(ID3D12Fence* f,std::uint64_t v)noexcept{
    ComPtr<ID3D12Device> owner;
    if(!f||!v||v==UINT64_MAX||FAILED(f->GetDevice(IID_PPV_ARGS(&owner)))||!same_identity(device.Get(),owner.Get()))return false;
    fence=f;value=v;submitted=true;return true;
}
bool PooledTexture::ready()const noexcept{
    if(!submitted)return true;if(!fence||!value)return false;
    const auto completed=fence->GetCompletedValue();return completed!=UINT64_MAX&&completed>=value;
}
std::shared_ptr<PooledTexture> TexturePool::acquire(ID3D12Device* device,const D3D12_RESOURCE_DESC&desc,std::uint64_t epoch){
    return acquire(device,desc,epoch,D3D12_RESOURCE_STATE_COMMON);
}
std::shared_ptr<PooledTexture> TexturePool::acquire(ID3D12Device* device,const D3D12_RESOURCE_DESC&desc,std::uint64_t epoch,D3D12_RESOURCE_STATES resting){
    if(closed_||!device||!epoch||!capacity_||(resting!=D3D12_RESOURCE_STATE_COMMON&&resting!=D3D12_RESOURCE_STATE_GENERIC_READ))return {};
    for(const auto& entry:entries_)if(entry.use_count()==1&&entry->ready()&&entry->epoch==epoch&&
        entry->resting==resting&&same_identity(entry->device.Get(),device)&&same_desc(entry->desc,desc)){
        ++reuses_;return entry;
    }
    if(entries_.size()>=capacity_){
        const auto idle=std::find_if(entries_.begin(),entries_.end(),[](const auto&e){return e.use_count()==1&&e->ready();});
        if(idle==entries_.end()){++exhausted_;return {};}
        entries_.erase(idle);
    }
    auto entry=std::make_shared<PooledTexture>();D3D12_HEAP_PROPERTIES heap{};heap.Type=D3D12_HEAP_TYPE_DEFAULT;
    if(FAILED(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&desc,resting,nullptr,
        IID_PPV_ARGS(&entry->texture))))return {};
    if(!accounting_.track(device,entry->texture.Get()))return {};
    entry->device=device;entry->desc=desc;entry->epoch=epoch;entry->resting=resting;entries_.push_back(entry);return entry;
}
void TexturePool::trim(){std::erase_if(entries_,[](const auto&e){return e.use_count()==1&&e->ready();});}
bool TexturePool::close(){closed_=true;std::erase_if(entries_,[](const auto&e){return e->ready();});return entries_.empty();}
}
