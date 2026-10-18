#pragma once
#include <atomic>
#include <chrono>
#include <cstdint>
#include <limits>
#include <memory>
#include <mutex>
#include "MenuPreview/NativePreviewSource.hpp"

namespace cvr::menu_preview {
// One clock domain for publication timestamps and all freshness comparisons.
inline uint64_t PublicationNowMs() noexcept {
    return uint64_t(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}
struct Metadata {
    uint32_t screenEpoch{}, setupID{};
    int32_t screen{}; // 0 none; 1 Inventory; 2 selected character creation setup.
    uint64_t controllerIdentity{}, componentIdentity{}, entityIdentity{}, entityID{};
    uint64_t sceneNameHash{}, cameraRefHash{}, slotNameHash{}, virtualCameraNameHash{};
    uint64_t publicationSerial{}, publishedAtMs{};
};
struct Lease {
    Metadata metadata{};
    // Holds locked SDK handles. No scripting call or mutable component-array read
    // is needed in a render hook; keep this lease until the engine call returns.
    std::shared_ptr<const void> lifetime;
    // Optional exact native-view binding. Metadata remains the common editor
    // lifetime; an offscreen right camera is never relabelled as the UI camera.
    std::shared_ptr<const NativePreviewSource> source;
};
bool TryAcquireForComponent(uintptr_t component, Lease& out) noexcept;
bool TryAcquireForVirtualCamera(uint64_t key, Lease& out) noexcept;
bool TryAcquireCurrent(Lease& out) noexcept;
// Cheap observation gate only. Pixel consumers must still acquire a locked Lease.
uint32_t CurrentPreviewObservationEpoch() noexcept;
// Lifetime gate for an already accepted preview packet. Unlike acquisition or
// observation, a category/setup gap may have no current scope but stays open.
bool IsPreviewEpochOpen(std::uint32_t epoch) noexcept;

inline bool SupportedPreviewScreen(int32_t screen) noexcept { return screen==1 || screen==2; }
inline bool ValidPublication(int32_t screen,uint32_t epoch,bool ready,
    uint32_t setupCount,uint32_t actualSetup,uint32_t enabledRttCount) noexcept {
    return SupportedPreviewScreen(screen) && epoch!=0 && ready && setupCount!=0 &&
        actualSetup<setupCount && enabledRttCount==1;
}
inline bool FreshPublication(uint64_t published,uint64_t now) noexcept {
    return published!=0 && now>=published && now-published<=500;
}

// SDK-independent publication policy, shared by the native adapter and CPU tests.
// Begin allocates a process-lifetime token; Lua reload cannot recycle an epoch.
template<class Scope> class ScopeStore final {
public:
    uint32_t Begin() {
        std::lock_guard lock(writer_);
        if(epoch_==std::numeric_limits<uint32_t>::max()){
            closed_=true;current_.store({},std::memory_order_release);return 0;
        }
        ++epoch_;closed_=false;current_.store({},std::memory_order_release);return epoch_;
    }
    bool Publish(std::shared_ptr<const Scope> value) {
        if(!value)return false;
        std::lock_guard lock(writer_);
        if(closed_ || !epoch_ || value->metadata.screenEpoch!=epoch_ || !SupportedPreviewScreen(value->metadata.screen))return false;
        current_.store(std::move(value),std::memory_order_release);return true;
    }
    bool Invalidate(uint32_t epoch) {
        std::lock_guard lock(writer_);
        if(closed_ || !epoch || epoch!=epoch_)return false;
        current_.store({},std::memory_order_release);return true;
    }
    bool Clear(uint32_t epoch) {
        std::lock_guard lock(writer_);
        if(!epoch || epoch!=epoch_)return false;
        closed_=true;current_.store({},std::memory_order_release);return true;
    }
    bool IsEpochOpen(std::uint32_t epoch)const noexcept {
        try {
            std::lock_guard lock(writer_);
            return epoch!=0 && epoch==epoch_ && !closed_;
        }catch(...){return false;}
    }
    std::shared_ptr<const Scope> Read()const noexcept {return current_.load(std::memory_order_acquire);}
    uint32_t ObservationEpoch(uint64_t now)const noexcept {
        const auto value=Read();
        return value && FreshPublication(value->metadata.publishedAtMs,now)
            ? value->metadata.screenEpoch : 0;
    }
private:
    mutable std::mutex writer_;
    uint32_t epoch_{};
    bool closed_{true};
    std::atomic<std::shared_ptr<const Scope>> current_;
};
}
