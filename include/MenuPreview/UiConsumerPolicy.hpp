#pragma once
#include <cstdint>
#include <memory>
#include <vector>
#include <unordered_map>
namespace cvr::menu_preview {
template<class Source> struct UiOwnedMutationStorage {
 uint64_t token{};bool finished{};std::vector<uintptr_t> destinations;std::vector<Source> sources;std::vector<uint8_t> enlisted;
 UiOwnedMutationStorage(uint64_t serial,size_t count):token(serial),destinations(count),sources(count),enlisted(count){}
};
template<class Payload> std::shared_ptr<Payload> OwnUiMutation(uint64_t serial,size_t count){if(!serial||!count||count>128)return {};return std::make_shared<Payload>(serial,count);}
inline void UiBeginDescriptorMutation(uint64_t& version,uint32_t& active,bool& overlap,uint64_t token)noexcept{overlap=active?true:false;++active;version=token;}
inline bool UiFinishDescriptorMutation(uint64_t version,uint32_t& active,bool overlap,uint64_t token)noexcept{if(!active)return false;--active;return !active&&!overlap&&version==token;}
inline bool UiFinishEnlistedMutation(bool enlisted,uint64_t version,uint32_t& active,bool overlap,uint64_t token)noexcept{if(!enlisted)return false;return UiFinishDescriptorMutation(version,active,overlap,token);}
template<class Row> class UiLiveListStore {
 std::unordered_map<uintptr_t,Row> entries_;uint64_t recordingSerial_{};
public:
 Row* Find(uintptr_t id){auto i=entries_.find(id);return i==entries_.end()?nullptr:&i->second;}
 Row* Ensure(uintptr_t id,uint64_t life){if(!id||!life)return nullptr;auto [it,created]=entries_.try_emplace(id);auto& r=it->second;if(created||r.objectGeneration!=life){r={};r.identity=id;r.objectGeneration=life;r.generation=++recordingSerial_;}return &r;}
 void Retire(uintptr_t id,uint64_t life){auto* r=Find(id);if(r&&r->objectGeneration==life)entries_.erase(id);}
 void Reset(uintptr_t id,uint64_t life){if(auto* r=Ensure(id,life)){*r={};r->identity=id;r->objectGeneration=life;r->generation=++recordingSerial_;}}
 void Clear(){entries_.clear();}
 size_t Size()const noexcept{return entries_.size();}
 template<class Fn> void ForEach(Fn&& f){for(auto& pair:entries_)f(pair.second);}
};
struct UiOrderedWriter {uintptr_t resource{},queue{};uint64_t source{},revision{};uint32_t epoch{},setup{},eye{},frame{};};
inline bool UiCanConsume(const UiOrderedWriter& w,uintptr_t resource,uintptr_t queue,uint32_t epoch,uint32_t setup)noexcept{
    return resource&&queue&&w.resource==resource&&w.queue==queue&&w.source&&w.revision&&w.epoch==epoch&&w.setup==setup&&w.eye<2;
}
inline bool UiSelectedBinding(uintptr_t bindLayout,uint32_t bindTable,uint32_t bindRoot,uint32_t selected,uintptr_t flushLayout,uint32_t flushTable,uint32_t flushRoot,uint32_t count)noexcept{return bindLayout&&bindLayout==flushLayout&&bindTable==flushTable&&bindRoot==flushRoot&&count&&count<=128&&selected<count;}
inline bool UiRecordingValid(uint64_t generation,uint64_t expected,bool closed,bool overflow)noexcept{return generation&&generation==expected&&closed&&!overflow;}
inline bool UiTableContains(uintptr_t start,uint32_t count,uint32_t increment,uintptr_t handle,uint32_t& index)noexcept{
    index=0;if(!start||!count||count>128||!increment||handle<start)return false;
    const auto distance=handle-start;if(distance%increment||distance/increment>=count)return false;
    index=uint32_t(distance/increment);return true;
}
}
