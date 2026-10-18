#pragma once
#include <array>
#include <atomic>
#include <cstdint>
#include <limits>
#include <unordered_map>
#include <utility>
namespace cvr::menu_preview {
// A negative lookup filter around the original stable-node container. It never
// grants provenance or stores resource ownership. Map access retains the caller's
// mutex; only MayContain's conservative, non-owning hints may be read without it.
template<class Value,unsigned Bits> class DescriptorMap {
    static_assert(Bits>0&&Bits<=20);
    using Map=std::unordered_map<uintptr_t,Value>;
    Map map_;
    std::array<std::atomic<uint16_t>,size_t{1}<<Bits> counts_{};
    static size_t Bucket(uintptr_t key)noexcept{return size_t((uint64_t(key)*0x9E3779B97F4A7C15ull)>>(64-Bits));}
    void Add(uintptr_t key)noexcept{auto& cell=counts_[Bucket(key)];const auto count=cell.load(std::memory_order_relaxed);if(count!=UINT16_MAX)cell.store(uint16_t(count+1),std::memory_order_release);}
    // A saturated bucket stays positive until clear; wrapping/decrementing an
    // unknown true count could hide a key that is still in the map.
    void Remove(uintptr_t key)noexcept{auto& cell=counts_[Bucket(key)];const auto count=cell.load(std::memory_order_relaxed);if(count&&count!=UINT16_MAX)cell.store(uint16_t(count-1),std::memory_order_release);}
public:
    DescriptorMap()=default;
    DescriptorMap(const DescriptorMap&)=delete;
    DescriptorMap& operator=(const DescriptorMap&)=delete;
    using iterator=typename Map::iterator;
    using const_iterator=typename Map::const_iterator;
    using node_type=typename Map::node_type;
    using insert_return_type=typename Map::insert_return_type;
    bool MayContain(uintptr_t key)const noexcept{return counts_[Bucket(key)].load(std::memory_order_acquire)!=0;}
    iterator find(uintptr_t key){return MayContain(key)?map_.find(key):map_.end();}
    const_iterator find(uintptr_t key)const{return MayContain(key)?map_.find(key):map_.end();}
    iterator begin()noexcept{return map_.begin();}
    iterator end()noexcept{return map_.end();}
    const_iterator begin()const noexcept{return map_.begin();}
    const_iterator end()const noexcept{return map_.end();}
    size_t size()const noexcept{return map_.size();}
    void reserve(size_t count){map_.reserve(count);}
    template<class... Args> auto emplace(Args&&... args){auto result=map_.emplace(std::forward<Args>(args)...);if(result.second)Add(result.first->first);return result;}
    Value& operator[](uintptr_t key){auto result=map_.try_emplace(key);if(result.second)Add(key);return result.first->second;}
    iterator erase(iterator it){Remove(it->first);return map_.erase(it);}
    size_t erase(uintptr_t key){auto it=find(key);if(it==end())return 0;erase(it);return 1;}
    node_type extract(uintptr_t key){if(!MayContain(key))return {};auto node=map_.extract(key);if(!node.empty())Remove(key);return node;}
    insert_return_type insert(node_type&& node){auto result=map_.insert(std::move(node));if(result.inserted)Add(result.position->first);return result;}
    void clear()noexcept{map_.clear();for(auto& count:counts_)count.store(0,std::memory_order_release);}
};
}
