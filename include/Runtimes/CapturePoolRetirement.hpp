#pragma once
#include <d3d12.h>
#include <wrl/client.h>
#include <array>
#include <span>
#include "Runtimes/CapturePoolPolicy.hpp"

namespace cvr::capture {
struct PoolProducerUse {
    Microsoft::WRL::ComPtr<ID3D12Fence> fence;
    uint64_t value{};
    bool submitted{},unknown{};
    void Executed(ID3D12Fence* point,uint64_t at,bool signalSucceeded) {
        submitted=true;
        if(!signalSucceeded || !point || !at || at==UINT64_MAX) {unknown=true;return;}
        fence=point;value=at;
    }
    bool Complete()const {
        if(!submitted)return true;
        if(unknown || !fence || !value)return false;
        const auto done=fence->GetCompletedValue();return done!=UINT64_MAX && done>=value;
    }
};
// The producer's last queued write is an owner too, independently of XR/FG
// readers. A resize may not release its texture merely because no reader pinned
// it yet. Unknown Signal is sticky and is never retired by a later fence value.
class MainPoolRetirement {
public:
    static constexpr uint32_t MaximumGenerations=3;
    ~MainPoolRetirement() {
        // A failed teardown is not GPU completion. Preserve this bounded set
        // until process teardown rather than destroying in-flight resources.
        for(auto& item:retired_)if(item.used && !item.producer.Complete()) {
            for(auto& resource:item.resources)(void)resource.Detach();
            (void)item.producer.fence.Detach();
        }
    }
    uint32_t Collect() {
        uint32_t freed{};
        for(auto& item:retired_)if(item.used && item.producer.Complete()){item={};++freed;}
        return freed;
    }
    uint32_t Pending()const {uint32_t n{};for(const auto& item:retired_)n+=item.used?1u:0u;return n;}
    bool CanRetire(const PoolProducerUse& use) {
        Collect();return use.Complete() || Pending()<MaximumGenerations;
    }
    bool Retire(std::span<ID3D12Resource* const> pool,const PoolProducerUse& use) {
        if(pool.size()>MainPoolMaximum)return false;
        Collect();if(use.Complete())return true;
        for(auto& item:retired_)if(!item.used) {
            item.used=true;item.producer=use;
            for(size_t i=0;i<pool.size();++i)item.resources[i]=pool[i];
            return true;
        }
        return false;
    }
private:
    struct Retired {
        std::array<Microsoft::WRL::ComPtr<ID3D12Resource>,MainPoolMaximum> resources;
        PoolProducerUse producer;
        bool used{};
    };
    std::array<Retired,MaximumGenerations> retired_;
};
}
