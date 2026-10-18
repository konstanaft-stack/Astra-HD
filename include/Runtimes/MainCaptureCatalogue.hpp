#pragma once
#include "Framegen/MainEyeSourceLatch.hpp"
#include <algorithm>
#include <array>
#include <cstdint>
#include <span>
#include <utility>

namespace cvr::capture {
inline bool SameMainCaptureScope(const framegen::MainEyeSourceReceipt& a,
                                 const framegen::MainEyeSourceReceipt& b) {
    return a && b && a.epochs==b.epochs && a.requestRevision==b.requestRevision &&
        a.latchRevision==b.latchRevision && a.clockGeneration==b.clockGeneration;
}

// Versioned live color cells. Ordered AER publications reserve their cell until
// a completed image is claimed into a real ReadLease. Legacy mode remains a
// latest-version catalogue. Caller serializes access with the publication mutex.
// Payload must contain metadata only: no color read lease or Inputs Record.
template<class Payload,size_t Capacity>
class MainCaptureCatalogue {
public:
    struct Key {
        uint64_t generation{},poolEpoch{},version{};
        uint32_t slot{};
        uintptr_t resource{};
        bool operator==(const Key&)const=default;
        explicit operator bool()const{return generation && poolEpoch && version && resource;}
    };
    struct Entry {
        Key key;
        framegen::MainEyeSourceReceipt receipt;
        uint64_t serial{},imageGeneration{};
        Payload payload;
        bool published{},claimed{};
    };
    struct Keys {std::array<Key,Capacity> keys{};size_t size{};};
    uint64_t Generation()const{return generation_;}
    void SetOrderedMode(bool enabled){if(ordered_!=enabled){Clear();ordered_=enabled;}}
    bool OrderedMode()const{return ordered_;}
    bool CanOverwrite(size_t slot)const{
        return slot<count_&&(!ordered_||!entries_[slot].published||entries_[slot].claimed);
    }
    void DiscardOutsideScope(const framegen::MainEyeSourceReceipt& scope){
        for(auto& entry:entries_)if(entry.published&&!SameMainCaptureScope(entry.receipt,scope))entry={};
    }
    bool HasSource(const framegen::MainEyeSourceReceipt& source)const{
        if(!ordered_||!source)return false;
        for(const auto& entry:entries_)if(entry.published&&SameMainCaptureScope(entry.receipt,source)&&
            entry.receipt.sourceId==source.sourceId)return true;
        return false;
    }
    bool Claim(const Key& key){
        if(!Matches(key)||!entries_[key.slot].published)return false;
        if(!ordered_)return true;
        auto& entry=entries_[key.slot];if(entry.claimed)return false;entry.claimed=true;return true;
    }
    void Clear() {
        if(generation_==UINT64_MAX)exhausted_=true;else ++generation_;
        entries_={};
    }
    void Reset(uint64_t poolEpoch,std::span<const uintptr_t> resources) {
        Clear();resources_={};count_=0;poolEpoch_=poolEpoch;
        if(!poolEpoch || resources.size()>Capacity)return;
        count_=resources.size();std::copy(resources.begin(),resources.end(),resources_.begin());
    }
    // Always invalidate on a successful writer reservation, before recording
    // any GPU write. A reset during this capture attempt makes its ticket stale
    // even if reservation itself happened after the reset.
    Key BeginWrite(uint32_t slot,uint64_t attemptGeneration) {
        if(slot>=count_||!CanOverwrite(slot))return {};
        entries_[slot]={};
        if(exhausted_ || nextVersion_==UINT64_MAX || !resources_[slot]){exhausted_=true;return {};}
        entries_[slot].key={generation_,poolEpoch_,++nextVersion_,slot,resources_[slot]};
        auto ticket=entries_[slot].key;ticket.generation=attemptGeneration;return ticket;
    }
    bool Publish(const Key& key,const framegen::MainEyeSourceReceipt& receipt,
                 uint64_t serial,Payload payload,uint64_t imageGeneration=0) {
        if(!Matches(key) || !receipt || !serial)return false;
        auto& entry=entries_[key.slot];entry.receipt=receipt;entry.serial=serial;
        entry.imageGeneration=imageGeneration;entry.payload=std::move(payload);entry.published=true;entry.claimed=false;return true;
    }
    const Entry* Find(const Key& key)const {
        return Matches(key) && entries_[key.slot].published?&entries_[key.slot]:nullptr;
    }
    template<class Ready>
    Keys ReadyKeys(const framegen::MainEyeSourceReceipt& scope,Ready ready)const {
        Keys out;if(!scope || exhausted_)return out;
        for(size_t i=0;i<count_;++i){const auto& entry=entries_[i];
            if(entry.published && SameMainCaptureScope(entry.receipt,scope) && (!ordered_||!entry.claimed))out.keys[out.size++]=entry.key;
        }
        std::sort(out.keys.begin(),out.keys.begin()+out.size,[&](const Key& a,const Key& b){
            const auto& x=entries_[a.slot];const auto& y=entries_[b.slot];
            return x.receipt.sourceId!=y.receipt.sourceId?(ordered_?x.receipt.sourceId<y.receipt.sourceId:x.receipt.sourceId>y.receipt.sourceId):x.serial>y.serial;
        });
        std::array<bool,2> blocked{};size_t retained=0;
        for(size_t i=0;i<out.size;++i){
            const auto& entry=entries_[out.keys[i].slot];const auto eye=uint32_t(entry.receipt.physicalEye);
            if(eye>1||(ordered_&&blocked[eye]))continue;
            if(!ready(entry)){if(ordered_)blocked[eye]=true;continue;}
            out.keys[retained++]=out.keys[i];
        }
        out.size=retained;
        return out;
    }
private:
    bool Matches(const Key& key)const {
        return !exhausted_ && key && key.generation==generation_ && key.poolEpoch==poolEpoch_ &&
            key.slot<count_ && key.resource==resources_[key.slot] && entries_[key.slot].key==key;
    }
    std::array<Entry,Capacity> entries_{};
    std::array<uintptr_t,Capacity> resources_{};
    size_t count_{};
    uint64_t generation_=1,poolEpoch_{},nextVersion_{};
    bool exhausted_{},ordered_{};
};
}
