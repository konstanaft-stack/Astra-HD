#pragma once
#include <cstdint>
#include <mutex>
#include <optional>
#include <span>
#include <unordered_map>
#include <utility>
#include <vector>

namespace cvr::camera {
// Snapshot before Execute; publish that immutable snapshot after Execute. Reset
// can legally start another recording between those two points. It discards the
// live recording, never the values already prepared for the submitted work.
template<class Payload>
class ImagePoseLedger {
    struct Write { uintptr_t resource;Payload pose;bool stable;uint64_t lifetime; };
    struct Recording {uint64_t lifetime;std::vector<Write> writes;};
    struct PreparedWrite {uintptr_t list;uint64_t listLifetime;Write write;};
public:
    struct Image {
        uintptr_t resource{},queue{};
        uint64_t generation{},stampMs{};
        Payload pose{};
        explicit operator bool() const { return generation!=0; }
    };
    class Prepared {
        friend class ImagePoseLedger;
        const ImagePoseLedger* m_owner{};
        uintptr_t m_queue{};
        uint64_t m_epoch{};
        bool m_valid{};
        std::vector<PreparedWrite> m_writes;
    public:
        Prepared()=default;
        Prepared(const Prepared&)=delete;
        Prepared& operator=(const Prepared&)=delete;
        Prepared(Prepared&& other) noexcept { *this=std::move(other); }
        Prepared& operator=(Prepared&& other) noexcept {
            if(this!=&other){
                m_owner=std::exchange(other.m_owner,nullptr);m_queue=std::exchange(other.m_queue,0);
                m_epoch=std::exchange(other.m_epoch,0);m_valid=std::exchange(other.m_valid,false);
                m_writes=std::move(other.m_writes);
            }
            return *this;
        }
        explicit operator bool() const noexcept {return m_valid;}
    };
    void Reset(uintptr_t list) {
        if(!list)return;
        std::lock_guard lock(m_mutex);EnsureList(list).writes.clear();
    }
    void Record(uintptr_t list,uintptr_t resource,const Payload& pose,bool stable) {
        if(!list || !resource)return;
        std::lock_guard lock(m_mutex);
        const auto [life,created]=m_lifetimes.try_emplace(resource,0);
        if(created)life->second=++m_lifetime;
        auto& writes=EnsureList(list).writes;
        // Keep the last write to each target in this list. Unknown pose is
        // explicit: it must revoke an earlier label when pixels are replaced.
        for(auto it=writes.begin();it!=writes.end();++it)if(it->resource==resource) { writes.erase(it);break; }
        writes.push_back({resource,pose,stable,life->second});
    }
    Prepared Prepare(uintptr_t queue,std::span<const uintptr_t> lists) noexcept {
        return Prepare(queue,lists.size(),[&](size_t i){return lists[i];});
    }
    // The getter keeps the native wrapper allocation-free when its input is an
    // array of COM pointers. Snapshot copies/allocations are contained here.
    template<class ListAt> Prepared Prepare(uintptr_t queue,size_t count,ListAt&& listAt) noexcept {
        std::lock_guard lock(m_mutex);
        if(!queue){InvalidatePublishedLocked();return {};}
        try {
            Prepared batch;batch.m_owner=this;batch.m_queue=queue;batch.m_epoch=m_publicationEpoch;
            for(size_t i=0;i<count;++i){
                const uintptr_t list=listAt(i);const auto found=m_lists.find(list);
                if(found==m_lists.end())continue;
                for(const auto& write:found->second.writes)
                    batch.m_writes.push_back({list,found->second.lifetime,write});
            }
            // If Execute happens but its caller never reaches Commit, the old
            // pose must not label the newly written pixels. Capture holds the
            // outer ImageSubmissionMutex across Prepare/Execute/Commit.
            for(const auto& item:batch.m_writes)RevokeImageLocked(item.write);
            batch.m_valid=true;return batch;
        } catch(...) {
            // We may not know every affected target after a failed snapshot.
            // Revoke all publications and older prepared batches, not pixels.
            InvalidatePublishedLocked();return {};
        }
    }
    void Commit(uintptr_t queue,Prepared&& batch,uint64_t stampMs) noexcept {
        const bool valid=std::exchange(batch.m_valid,false);
        if(!valid || batch.m_owner!=this)return;
        std::lock_guard lock(m_mutex);
        if(batch.m_epoch!=m_publicationEpoch)return;
        if(queue!=batch.m_queue){InvalidatePublishedLocked();return;}
        try {
            for(const auto& item:batch.m_writes){
                const auto& write=item.write;
                const auto life=m_lifetimes.find(write.resource);
                if(life==m_lifetimes.end() || life->second!=write.lifetime)continue;
                const auto list=m_lists.find(item.list);
                if(list==m_lists.end() || list->second.lifetime!=item.listLifetime){
                    RevokeImageLocked(write);continue;
                }
                Image image{write.resource,queue,++m_generation,stampMs,write.pose};
                m_images.insert_or_assign(write.resource,image);
                if(write.stable)m_latestStable=image;
            }
        } catch(...) {
            InvalidatePublishedLocked();
        }
    }
    void InvalidatePublished() noexcept {std::lock_guard lock(m_mutex);InvalidatePublishedLocked();}
    Image Read(uintptr_t resource) const {
        std::lock_guard lock(m_mutex);const auto found=m_images.find(resource);
        return found==m_images.end() ? Image{} : found->second;
    }
    Image LatestStable() const { std::lock_guard lock(m_mutex);return m_latestStable ? *m_latestStable:Image{}; }
    void ForgetResource(uintptr_t resource) {
        std::lock_guard lock(m_mutex);m_images.erase(resource);
        m_lifetimes.erase(resource);
        if(m_latestStable && m_latestStable->resource==resource)m_latestStable.reset();
    }
    void ForgetList(uintptr_t list) { std::lock_guard lock(m_mutex);m_lists.erase(list); }
private:
    Recording& EnsureList(uintptr_t list) {
        const auto [entry,created]=m_lists.try_emplace(list);
        if(created)entry->second.lifetime=++m_lifetime;
        return entry->second;
    }
    void RevokeImageLocked(const Write& write) {
        const auto life=m_lifetimes.find(write.resource);
        if(life==m_lifetimes.end() || life->second!=write.lifetime)return;
        m_images.erase(write.resource);
        if(m_latestStable && m_latestStable->resource==write.resource)m_latestStable.reset();
    }
    void InvalidatePublishedLocked() {
        m_images.clear();m_latestStable.reset();++m_publicationEpoch;
    }
    mutable std::mutex m_mutex;
    std::unordered_map<uintptr_t,Recording> m_lists;
    std::unordered_map<uintptr_t,Image> m_images;
    std::unordered_map<uintptr_t,uint64_t> m_lifetimes;
    std::optional<Image> m_latestStable;
    uint64_t m_generation{},m_lifetime{},m_publicationEpoch=1;
};
}
